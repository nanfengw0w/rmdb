/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <algorithm>
#include <limits>
#include <optional>

#include "execution_defs.h"
#include "execution_manager.h"
#include "index_maintenance.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

extern TransactionManager* g_txn_manager;

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;
    IxIndexHandle *ih_;

    Rid rid_;
    std::vector<Rid> matched_rids_;
    size_t matched_pos_;
    bool is_end_;
    std::optional<std::vector<char>> runtime_eq_key_;

    SmManager *sm_manager_;

    struct Bound {
        std::vector<char> key;
        bool inclusive;
    };

    // 预计算：条件中列的元数据，避免每次 eval_conds 做 O(n) 线性搜索
    struct CondColInfo {
        size_t offset;
        ColType type;
        int len;
        size_t rhs_offset;
        bool is_rhs_val;
        CompOp op;
    };
    std::vector<CondColInfo> cond_col_infos_;
    bool cond_info_inited_ = false;

    // 预计算：索引键条件列偏移
    struct IndexKeyColInfo {
        int offset;  // 在索引键中的偏移
        ColType type;
        int len;
        // 对应的条件索引（-1 表示无 EQ 条件）
        int cond_idx;
    };
    std::vector<IndexKeyColInfo> index_key_col_infos_;
    bool index_key_info_inited_ = false;

    void init_cond_col_infos() {
        if (cond_info_inited_) return;
        cond_info_inited_ = true;
        cond_col_infos_.reserve(fed_conds_.size());
        for (auto &cond : fed_conds_) {
            CondColInfo info;
            auto lhs_pos = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta &col) {
                return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
            });
            if (lhs_pos == cols_.end()) {
                cond_col_infos_.push_back(info);
                continue;
            }
            info.offset = lhs_pos->offset;
            info.type = lhs_pos->type;
            info.len = lhs_pos->len;
            info.op = cond.op;
            info.is_rhs_val = cond.is_rhs_val;
            if (cond.is_rhs_val) {
                info.rhs_offset = 0;
            } else {
                auto rhs_pos = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta &col) {
                    return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
                });
                info.rhs_offset = (rhs_pos != cols_.end()) ? rhs_pos->offset : 0;
            }
            cond_col_infos_.push_back(info);
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        std::string real_tab_name = sm_manager_->resolve_table_name(tab_name_);
        tab_ = sm_manager_->db_.get_table(real_tab_name);
        conds_ = std::move(conds);
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->get_table_fh(tab_name_);
        cols_ = sm_manager_->get_query_cols(tab_name_);
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                if (cond.is_rhs_val || cond.rhs_col.tab_name != tab_name_) {
                    throw InternalError("Index scan condition does not reference target table");
                }
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
        is_end_ = true;
        matched_pos_ = 0;

        // Get the index handle
        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(real_tab_name, index_col_names_);
        if (sm_manager_->ihs_.count(ix_name)) {
            ih_ = sm_manager_->ihs_.at(ix_name).get();
        } else {
            ih_ = nullptr;
        }
    }

    void beginTuple() override {
        init_cond_col_infos();
        matched_rids_.clear();
        matched_pos_ = 0;
        is_end_ = true;
        track_predicate_read();

        if (ih_ == nullptr) {
            return;
        }

        if (runtime_eq_key_.has_value()) {
            std::vector<Rid> result;
            if (ih_->get_value(runtime_eq_key_->data(), &result, nullptr)) {
                for (auto &candidate_rid : result) {
                    auto record = fh_->get_record(candidate_rid, context_);
                    if (record != nullptr && eval_conds(record.get(), fed_conds_)) {
                        track_record_read(candidate_rid);
                        matched_rids_.push_back(candidate_rid);
                    }
                }
            }
            finish_materialized_scan();
            return;
        }

        // 1. 尝试全列精确匹配
        auto exact_key = build_exact_match_key();
        if (exact_key.has_value()) {
            std::vector<Rid> result;
            if (ih_->get_value(exact_key->data(), &result, nullptr)) {
                for (auto &candidate_rid : result) {
                    auto record = fh_->get_record(candidate_rid, context_);
                    if (record != nullptr && eval_conds(record.get(), fed_conds_)) {
                        track_record_read(candidate_rid);
                        matched_rids_.push_back(candidate_rid);
                    }
                }
            }
            finish_materialized_scan();
            return;
        }

        // 2. 复合索引：找最长等值前缀 + 下一列范围条件，构建精确上下界
        if (index_meta_.col_num > 1) {
            auto bounds = build_prefix_range_bounds();
            if (bounds.has_value()) {
                auto &[lower, upper] = *bounds;
                Iid lower_iid = lower.has_value()
                    ? (lower->inclusive ? ih_->lower_bound(lower->key.data())
                                       : ih_->upper_bound(lower->key.data()))
                    : ih_->leaf_begin();
                Iid upper_iid = upper.has_value()
                    ? (upper->inclusive ? ih_->upper_bound(upper->key.data())
                                       : ih_->lower_bound(upper->key.data()))
                    : ih_->leaf_end();
                materialize_index_scan(lower_iid, upper_iid);
                finish_materialized_scan();
                return;
            }
            // 无法构建有效范围，回退到全索引扫描
            materialize_index_scan(ih_->leaf_begin(), ih_->leaf_end());
            finish_materialized_scan();
            return;
        }

        // 3. 单列索引范围扫描
        std::optional<Bound> lower_bound;
        std::optional<Bound> upper_bound;
        auto &first_col = index_meta_.cols[0];
        for (auto &cond : conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != first_col.name) {
                continue;
            }

            auto key = make_col_key(cond.rhs_val, 0);
            switch (cond.op) {
                case OP_EQ:
                    update_lower_bound(lower_bound, Bound{key, true});
                    update_upper_bound(upper_bound, Bound{std::move(key), true});
                    break;
                case OP_GT:
                    update_lower_bound(lower_bound, Bound{std::move(key), false});
                    break;
                case OP_GE:
                    update_lower_bound(lower_bound, Bound{std::move(key), true});
                    break;
                case OP_LT:
                    update_upper_bound(upper_bound, Bound{std::move(key), false});
                    break;
                case OP_LE:
                    update_upper_bound(upper_bound, Bound{std::move(key), true});
                    break;
                case OP_NE:
                    break;
            }
        }

        if (bounds_are_empty(lower_bound, upper_bound)) {
            return;
        }

        Iid lower = lower_bound.has_value()
                        ? (lower_bound->inclusive ? ih_->lower_bound(lower_bound->key.data())
                                                  : ih_->upper_bound(lower_bound->key.data()))
                        : ih_->leaf_begin();
        Iid upper = upper_bound.has_value()
                        ? (upper_bound->inclusive ? ih_->upper_bound(upper_bound->key.data())
                                                  : ih_->lower_bound(upper_bound->key.data()))
                        : ih_->leaf_end();

        materialize_index_scan(lower, upper);
        finish_materialized_scan();
    }

    void nextTuple() override {
        if (is_end_) {
            return;
        }
        matched_pos_++;
        if (matched_pos_ >= matched_rids_.size()) {
            is_end_ = true;
            return;
        }
        rid_ = matched_rids_[matched_pos_];
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "IndexScanExecutor"; }

    bool can_runtime_lookup(const TabCol &col) const {
        return index_meta_.col_num == 1 &&
               col.tab_name == tab_name_ &&
               !index_meta_.cols.empty() &&
               col.col_name == index_meta_.cols[0].name;
    }

    void set_runtime_lookup_key(const TabCol &col, const char *value) {
        if (!can_runtime_lookup(col)) {
            runtime_eq_key_.reset();
            return;
        }
        runtime_eq_key_ = std::vector<char>(index_meta_.col_tot_len, 0);
        memcpy(runtime_eq_key_->data(), value, index_meta_.cols[0].len);
    }

    void clear_runtime_lookup_key() {
        runtime_eq_key_.reset();
    }

   private:
    void materialize_index_scan(const Iid &lower, const Iid &upper) {
        IxScan index_scan(ih_, lower, upper, sm_manager_->get_bpm());
        while (!index_scan.is_end()) {
            if (!eval_index_key(index_scan.key())) {
                index_scan.next();
                continue;
            }
            auto candidate_rid = index_scan.rid();
            auto record = fh_->get_record(candidate_rid, context_);
            if (record != nullptr && eval_conds(record.get(), fed_conds_)) {
                track_record_read(candidate_rid);
                matched_rids_.push_back(candidate_rid);
            }
            index_scan.next();
        }
    }

    void track_predicate_read() {
        if (context_ == nullptr || context_->txn_ == nullptr || g_txn_manager == nullptr ||
            context_->txn_->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return;
        }
        PredicateRead pred;
        pred.tab_name = tab_name_;
        pred.is_empty_result = false;
        pred.conds = fed_conds_;
        pred.cols = cols_;
        g_txn_manager->record_predicate_read(context_->txn_, pred);

        auto deps = g_txn_manager->check_rw_on_predicate_read(context_->txn_, tab_name_, pred);
        for (auto& [from, to] : deps) {
            if (g_txn_manager->add_rw_dependency_and_check(from, to)) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }

    void track_record_read(const Rid& rid) {
        if (context_ == nullptr || context_->txn_ == nullptr || g_txn_manager == nullptr ||
            context_->txn_->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return;
        }
        g_txn_manager->record_read(context_->txn_, tab_name_, rid);
        auto deps = g_txn_manager->check_rw_on_read(context_->txn_, tab_name_, rid);
        for (auto& [from, to] : deps) {
            if (g_txn_manager->add_rw_dependency_and_check(from, to)) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }

    bool eval_index_key(const char *key) {
        for (auto &cond : conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_) {
                continue;
            }
            int offset = 0;
            bool matched_index_col = false;
            for (auto &col : index_meta_.cols) {
                if (cond.lhs_col.col_name == col.name) {
                    int cmp = compare_value(key + offset, cond.rhs_val.raw->data, col.type, col.len);
                    if (!eval_cmp(cmp, cond.op)) {
                        return false;
                    }
                    matched_index_col = true;
                    break;
                }
                offset += col.len;
            }
            (void)matched_index_col;
        }
        return true;
    }

    std::optional<std::vector<char>> build_exact_match_key() {
        std::vector<char> key(index_meta_.col_tot_len, 0);
        int offset = 0;
        for (auto &col : index_meta_.cols) {
            bool found = false;
            for (auto &cond : conds_) {
                if (cond.is_rhs_val && cond.lhs_col.tab_name == tab_name_ &&
                    cond.lhs_col.col_name == col.name && cond.op == OP_EQ) {
                    memcpy(key.data() + offset, cond.rhs_val.raw->data, col.len);
                    found = true;
                    break;
                }
            }
            if (!found) {
                return std::nullopt;
            }
            offset += col.len;
        }
        return key;
    }

    std::vector<char> make_col_key(const Value &value, size_t col_idx) {
        std::vector<char> key(index_meta_.col_tot_len, 0);
        int offset = 0;
        for (size_t i = 0; i < col_idx; ++i) offset += index_meta_.cols[i].len;
        memcpy(key.data() + offset, value.raw->data, index_meta_.cols[col_idx].len);
        return key;
    }

    // 构建复合索引的前缀范围上下界
    // 策略：找最长等值前缀（前N列都有EQ），再用第N列的范围条件构建上下界
    std::optional<std::pair<std::optional<Bound>, std::optional<Bound>>> build_prefix_range_bounds() {
        // 第一步：找最长等值前缀
        int prefix_len = 0;
        std::vector<std::vector<char>> eq_keys;  // 每个前缀列的 EQ 值
        for (size_t ci = 0; ci < index_meta_.cols.size(); ++ci) {
            auto &col = index_meta_.cols[ci];
            bool found_eq = false;
            for (auto &cond : conds_) {
                if (cond.is_rhs_val && cond.lhs_col.tab_name == tab_name_ &&
                    cond.lhs_col.col_name == col.name && cond.op == OP_EQ) {
                    eq_keys.push_back(std::vector<char>(col.len, 0));
                    memcpy(eq_keys.back().data(), cond.rhs_val.raw->data, col.len);
                    found_eq = true;
                    break;
                }
            }
            if (!found_eq) break;
            prefix_len++;
        }

        // 第二步：检查第 prefix_len 列是否有范围条件
        if (prefix_len < static_cast<int>(index_meta_.cols.size())) {
            auto &range_col = index_meta_.cols[prefix_len];
            std::optional<Bound> lower, upper;

            // 先用前缀构建基础 key（前缀列 + min 填充）
            for (auto &cond : conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ ||
                    cond.lhs_col.col_name != range_col.name) {
                    continue;
                }

                // 构建完整 key：前缀 + 范围列值
                std::vector<char> key(index_meta_.col_tot_len, 0);
                int offset = 0;
                for (int i = 0; i < prefix_len; ++i) {
                    memcpy(key.data() + offset, eq_keys[i].data(), index_meta_.cols[i].len);
                    offset += index_meta_.cols[i].len;
                }
                memcpy(key.data() + offset, cond.rhs_val.raw->data, range_col.len);

                switch (cond.op) {
                    case OP_EQ:
                        update_lower_bound(lower, Bound{key, true});
                        update_upper_bound(upper, Bound{key, true});
                        break;
                    case OP_GT:
                        update_lower_bound(lower, Bound{key, false});
                        break;
                    case OP_GE:
                        update_lower_bound(lower, Bound{key, true});
                        break;
                    case OP_LT:
                        update_upper_bound(upper, Bound{key, false});
                        break;
                    case OP_LE:
                        update_upper_bound(upper, Bound{key, true});
                        break;
                    default:
                        break;
                }
            }

            if (lower.has_value() || upper.has_value() || prefix_len > 0) {
                // 如果只有前缀没有范围条件，用前缀作为精确下界和上界
                if (!lower.has_value() && !upper.has_value() && prefix_len > 0) {
                    // 构建前缀 + min 作为下界
                    std::vector<char> lower_key(index_meta_.col_tot_len, 0);
                    int offset = 0;
                    for (int i = 0; i < prefix_len; ++i) {
                        memcpy(lower_key.data() + offset, eq_keys[i].data(), index_meta_.cols[i].len);
                        offset += index_meta_.cols[i].len;
                    }
                    lower = Bound{lower_key, true};

                    // 构建前缀 + max 作为上界
                    std::vector<char> upper_key(index_meta_.col_tot_len, 0);
                    offset = 0;
                    for (int i = 0; i < prefix_len; ++i) {
                        memcpy(upper_key.data() + offset, eq_keys[i].data(), index_meta_.cols[i].len);
                        offset += index_meta_.cols[i].len;
                    }
                    // 填充 max 值
                    for (size_t i = prefix_len; i < index_meta_.cols.size(); ++i) {
                        int col_off = 0;
                        for (size_t j = 0; j < i; ++j) col_off += index_meta_.cols[j].len;
                        fill_max_key_part(upper_key.data() + col_off, index_meta_.cols[i]);
                    }
                    upper = Bound{upper_key, true};
                }

                if (!bounds_are_empty(lower, upper)) {
                    return std::make_pair(std::move(lower), std::move(upper));
                }
            }
        }

        // 如果有完整前缀但没有范围列条件（所有列都有EQ），应该已经被 exact_match 处理
        // 这里返回 nullopt 表示无法构建有效范围
        return std::nullopt;
    }

    void fill_suffix_with_min_key(std::vector<char> &key, size_t begin_col) {
        int offset = 0;
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            auto &col = index_meta_.cols[i];
            if (i >= begin_col) {
                fill_min_key_part(key.data() + offset, col);
            }
            offset += col.len;
        }
    }

    void fill_min_key_part(char *dest, const ColMeta &col) {
        switch (col.type) {
            case TYPE_INT: {
                int value = std::numeric_limits<int>::min();
                memcpy(dest, &value, sizeof(value));
                break;
            }
            case TYPE_FLOAT: {
                float value = std::numeric_limits<float>::lowest();
                memcpy(dest, &value, sizeof(value));
                break;
            }
            case TYPE_STRING:
                memset(dest, 0, col.len);
                break;
        }
    }

    void fill_max_key_part(char *dest, const ColMeta &col) {
        switch (col.type) {
            case TYPE_INT: {
                int value = std::numeric_limits<int>::max();
                memcpy(dest, &value, sizeof(value));
                break;
            }
            case TYPE_FLOAT: {
                float value = std::numeric_limits<float>::max();
                memcpy(dest, &value, sizeof(value));
                break;
            }
            case TYPE_STRING:
                memset(dest, 0xFF, col.len);
                break;
        }
    }

    // 比较完整复合 key（所有列按序比较），而不是只比较第一列
    int compare_full_key(const std::vector<char> &lhs, const std::vector<char> &rhs) {
        int offset = 0;
        for (auto &col : index_meta_.cols) {
            int cmp = compare_value(lhs.data() + offset, rhs.data() + offset, col.type, col.len);
            if (cmp != 0) return cmp;
            offset += col.len;
        }
        return 0;
    }

    void update_lower_bound(std::optional<Bound> &current, Bound candidate) {
        if (!current.has_value()) {
            current = std::move(candidate);
            return;
        }
        int cmp = compare_full_key(candidate.key, current->key);
        if (cmp > 0 || (cmp == 0 && current->inclusive && !candidate.inclusive)) {
            current = std::move(candidate);
        }
    }

    void update_upper_bound(std::optional<Bound> &current, Bound candidate) {
        if (!current.has_value()) {
            current = std::move(candidate);
            return;
        }
        int cmp = compare_full_key(candidate.key, current->key);
        if (cmp < 0 || (cmp == 0 && current->inclusive && !candidate.inclusive)) {
            current = std::move(candidate);
        }
    }

    bool bounds_are_empty(const std::optional<Bound> &lower, const std::optional<Bound> &upper) {
        if (!lower.has_value() || !upper.has_value()) {
            return false;
        }
        int cmp = compare_full_key(lower->key, upper->key);
        return cmp > 0 || (cmp == 0 && (!lower->inclusive || !upper->inclusive));
    }

    void finish_materialized_scan() {
        std::sort(matched_rids_.begin(), matched_rids_.end(), index_maintenance::rid_less);
        matched_rids_.erase(std::unique(matched_rids_.begin(), matched_rids_.end(),
                                        index_maintenance::same_rid),
                            matched_rids_.end());
        if (matched_rids_.empty()) {
            is_end_ = true;
            return;
        }
        matched_pos_ = 0;
        rid_ = matched_rids_[matched_pos_];
        is_end_ = false;
    }

    bool eval_conds(RmRecord *record, const std::vector<Condition> &conds) {
        // 使用预计算的列信息（如果可用）
        if (cond_info_inited_ && conds.size() == cond_col_infos_.size()) {
            for (size_t i = 0; i < conds.size(); ++i) {
                auto &info = cond_col_infos_[i];
                char *lhs_buf = record->data + info.offset;
                if (conds[i].is_rhs_val) {
                    char *rhs_buf = conds[i].rhs_val.raw->data;
                    int cmp = compare_value(lhs_buf, rhs_buf, info.type, info.len);
                    if (!eval_cmp(cmp, info.op)) return false;
                } else {
                    char *rhs_buf = record->data + info.rhs_offset;
                    int cmp = compare_value(lhs_buf, rhs_buf, info.type, info.len);
                    if (!eval_cmp(cmp, info.op)) return false;
                }
            }
            return true;
        }
        // 回退到原始实现
        for (auto &cond : conds) {
            auto &lhs_col_meta = find_col_meta(cols_, cond.lhs_col);
            char *lhs_buf = record->data + lhs_col_meta.offset;

            if (cond.is_rhs_val) {
                char *rhs_buf = cond.rhs_val.raw->data;
                int cmp = compare_value(lhs_buf, rhs_buf, lhs_col_meta.type, lhs_col_meta.len);
                if (!eval_cmp(cmp, cond.op)) return false;
            } else {
                auto &rhs_col_meta = find_col_meta(cols_, cond.rhs_col);
                char *rhs_buf = record->data + rhs_col_meta.offset;
                int cmp = compare_value(lhs_buf, rhs_buf, lhs_col_meta.type, lhs_col_meta.len);
                if (!eval_cmp(cmp, cond.op)) return false;
            }
        }
        return true;
    }

    ColMeta& find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) {
        auto pos = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == cols.end()) {
            throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
        }
        return const_cast<ColMeta&>(*pos);
    }

    int compare_value(const char *a, const char *b, ColType type, int len) {
        if (type == TYPE_INT) {
            int va = *(int *)a;
            int vb = *(int *)b;
            return (va > vb) ? 1 : ((va < vb) ? -1 : 0);
        } else if (type == TYPE_FLOAT) {
            float va = *(float *)a;
            float vb = *(float *)b;
            return (va > vb) ? 1 : ((va < vb) ? -1 : 0);
        } else if (type == TYPE_STRING) {
            return memcmp(a, b, len);
        }
        return 0;
    }

    bool eval_cmp(int cmp, CompOp op) {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return false;
        }
    }
};
