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
#include <map>
#include <optional>
#include <set>

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
    std::unique_ptr<IxScan> scan_;
    std::unique_ptr<RmRecord> current_record_;
    std::set<Rid> seen_rids_;
    bool is_end_;
    std::optional<std::vector<char>> runtime_first_col_value_;

    SmManager *sm_manager_;

    struct Bound {
        std::vector<char> key;
        bool inclusive;
    };

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

        // Get the index handle
        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(real_tab_name, index_col_names_);
        if (sm_manager_->ihs_.count(ix_name)) {
            ih_ = sm_manager_->ihs_.at(ix_name).get();
        } else {
            ih_ = nullptr;
        }
    }

    void beginTuple() override {
        scan_.reset();
        current_record_.reset();
        seen_rids_.clear();
        is_end_ = true;
        track_predicate_read();

        if (ih_ == nullptr) {
            return;
        }

        Iid lower;
        Iid upper;
        auto exact_key = build_exact_match_key();
        if (exact_key.has_value()) {
            lower = ih_->lower_bound(exact_key->data());
            upper = ih_->upper_bound(exact_key->data());
        } else {
            auto range = build_prefix_range_bounds();
            if (!range.has_value()) {
                lower = ih_->leaf_begin();
                upper = ih_->leaf_end();
            } else {
                if (bounds_are_empty(range->first, range->second)) {
                    return;
                }
                lower = range->first.inclusive ? ih_->lower_bound(range->first.key.data())
                                               : ih_->upper_bound(range->first.key.data());
                upper = range->second.inclusive ? ih_->upper_bound(range->second.key.data())
                                                : ih_->lower_bound(range->second.key.data());
            }
        }

        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        nextTuple();
    }

    void nextTuple() override {
        current_record_.reset();
        is_end_ = true;
        if (scan_ == nullptr) {
            return;
        }

        while (!scan_->is_end()) {
            if (!eval_index_key(scan_->key())) {
                scan_->next();
                continue;
            }
            Rid candidate_rid = scan_->rid();
            // Advance the index scan before touching the heap.  This keeps
            // the leaf latch held for the shortest possible interval.
            scan_->next();
            auto record = fh_->get_record(candidate_rid, context_);
            if (record == nullptr || !eval_conds(record.get(), fed_conds_)) {
                continue;
            }
            if (!seen_rids_.insert(candidate_rid).second) {
                continue;
            }
            track_record_read(candidate_rid);
            rid_ = candidate_rid;
            current_record_ = std::move(record);
            is_end_ = false;
            return;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_ || current_record_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_record_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "IndexScanExecutor"; }

    bool can_runtime_lookup(const TabCol &col) const {
        return col.tab_name == tab_name_ &&
               !index_meta_.cols.empty() &&
               col.col_name == index_meta_.cols[0].name;
    }

    void set_runtime_lookup_key(const TabCol &col, const char *value) {
        if (!can_runtime_lookup(col)) {
            runtime_first_col_value_.reset();
            return;
        }
        runtime_first_col_value_ = std::vector<char>(index_meta_.cols[0].len, 0);
        memcpy(runtime_first_col_value_->data(), value, index_meta_.cols[0].len);
    }

    void clear_runtime_lookup_key() {
        runtime_first_col_value_.reset();
    }

   private:
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
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            auto &col = index_meta_.cols[i];
            auto value = equality_value_for_col(i);
            if (!value.has_value()) {
                return std::nullopt;
            }
            memcpy(key.data() + offset, value.value(), col.len);
            offset += col.len;
        }
        return key;
    }

    std::optional<const char *> equality_value_for_col(size_t col_idx) {
        auto &col = index_meta_.cols[col_idx];
        if (col_idx == 0 && runtime_first_col_value_.has_value()) {
            return runtime_first_col_value_->data();
        }
        for (auto &cond : conds_) {
            if (cond.is_rhs_val && cond.lhs_col.tab_name == tab_name_ &&
                cond.lhs_col.col_name == col.name && cond.op == OP_EQ) {
                return cond.rhs_val.raw->data;
            }
        }
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

    void fill_suffix_with_max_key(std::vector<char> &key, size_t begin_col) {
        int offset = 0;
        for (size_t i = 0; i < index_meta_.cols.size(); ++i) {
            auto &col = index_meta_.cols[i];
            if (i >= begin_col) {
                fill_max_key_part(key.data() + offset, col);
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
                memset(dest, 0xff, col.len);
                break;
        }
    }

    int col_offset(size_t col_idx) {
        int offset = 0;
        for (size_t i = 0; i < col_idx; ++i) {
            offset += index_meta_.cols[i].len;
        }
        return offset;
    }

    int compare_full_key(const std::vector<char> &lhs, const std::vector<char> &rhs) {
        int offset = 0;
        for (auto &col : index_meta_.cols) {
            int cmp = compare_value(lhs.data() + offset, rhs.data() + offset, col.type, col.len);
            if (cmp != 0) {
                return cmp;
            }
            offset += col.len;
        }
        return 0;
    }

    std::optional<std::pair<Bound, Bound>> build_prefix_range_bounds() {
        if (index_meta_.cols.empty()) {
            return std::nullopt;
        }

        std::vector<char> prefix(index_meta_.col_tot_len, 0);
        size_t first_unbound = 0;
        int offset = 0;
        bool has_prefix = false;
        for (; first_unbound < index_meta_.cols.size(); ++first_unbound) {
            auto value = equality_value_for_col(first_unbound);
            if (!value.has_value()) {
                break;
            }
            memcpy(prefix.data() + offset, value.value(), index_meta_.cols[first_unbound].len);
            offset += index_meta_.cols[first_unbound].len;
            has_prefix = true;
        }

        std::optional<Bound> lower_bound;
        std::optional<Bound> upper_bound;
        bool has_range = false;

        auto make_prefix_key = [&](size_t begin_fill_col, bool fill_max) {
            std::vector<char> key = prefix;
            if (fill_max) {
                fill_suffix_with_max_key(key, begin_fill_col);
            } else {
                fill_suffix_with_min_key(key, begin_fill_col);
            }
            return key;
        };

        if (has_prefix) {
            update_lower_bound(lower_bound, Bound{make_prefix_key(first_unbound, false), true});
            update_upper_bound(upper_bound, Bound{make_prefix_key(first_unbound, true), true});
        }

        if (first_unbound < index_meta_.cols.size()) {
            auto &range_col = index_meta_.cols[first_unbound];
            int range_offset = col_offset(first_unbound);
            for (auto &cond : conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ ||
                    cond.lhs_col.col_name != range_col.name) {
                    continue;
                }
                switch (cond.op) {
                    case OP_GT:
                    case OP_GE: {
                        std::vector<char> key = prefix;
                        memcpy(key.data() + range_offset, cond.rhs_val.raw->data, range_col.len);
                        fill_suffix_with_min_key(key, first_unbound + 1);
                        if (cond.op == OP_GT) {
                            fill_suffix_with_max_key(key, first_unbound + 1);
                        }
                        update_lower_bound(lower_bound, Bound{std::move(key), cond.op == OP_GE});
                        has_range = true;
                        break;
                    }
                    case OP_LT:
                    case OP_LE: {
                        std::vector<char> key = prefix;
                        memcpy(key.data() + range_offset, cond.rhs_val.raw->data, range_col.len);
                        fill_suffix_with_min_key(key, first_unbound + 1);
                        if (cond.op == OP_LE) {
                            fill_suffix_with_max_key(key, first_unbound + 1);
                        }
                        update_upper_bound(upper_bound, Bound{std::move(key), cond.op == OP_LE});
                        has_range = true;
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        if (!has_prefix && !has_range) {
            return std::nullopt;
        }
        if (!lower_bound.has_value()) {
            lower_bound = Bound{make_prefix_key(first_unbound, false), true};
        }
        if (!upper_bound.has_value()) {
            upper_bound = Bound{make_prefix_key(first_unbound, true), true};
        }
        return std::make_pair(std::move(*lower_bound), std::move(*upper_bound));
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

    bool eval_conds(RmRecord *record, const std::vector<Condition> &conds) {
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
