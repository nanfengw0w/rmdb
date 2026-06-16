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
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
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
        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        if (sm_manager_->ihs_.count(ix_name)) {
            ih_ = sm_manager_->ihs_.at(ix_name).get();
        } else {
            ih_ = nullptr;
        }
    }

    void beginTuple() override {
        matched_rids_.clear();
        matched_pos_ = 0;
        is_end_ = true;

        if (ih_ == nullptr) {
            return;
        }

        auto exact_key = build_exact_match_key();
        if (exact_key.has_value()) {
            std::vector<Rid> result;
            if (ih_->get_value(exact_key->data(), &result, nullptr)) {
                for (auto &candidate_rid : result) {
                    auto record = fh_->get_record(candidate_rid, context_);
                    if (eval_conds(record.get(), fed_conds_)) {
                        matched_rids_.push_back(candidate_rid);
                    }
                }
            }
            finish_materialized_scan();
            return;
        }

        std::optional<Bound> lower_bound;
        std::optional<Bound> upper_bound;
        auto &first_col = index_meta_.cols[0];
        for (auto &cond : conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != first_col.name) {
                continue;
            }

            auto key = make_first_col_key(cond.rhs_val);
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
        Iid upper = upper_bound.has_value() && index_meta_.col_num == 1
                        ? (upper_bound->inclusive ? ih_->upper_bound(upper_bound->key.data())
                                                  : ih_->lower_bound(upper_bound->key.data()))
                        : ih_->leaf_end();

        IxScan index_scan(ih_, lower, upper, sm_manager_->get_bpm());
        while (!index_scan.is_end()) {
            if (!eval_index_key(index_scan.key())) {
                index_scan.next();
                continue;
            }
            auto candidate_rid = index_scan.rid();
            auto record = fh_->get_record(candidate_rid, context_);
            if (eval_conds(record.get(), fed_conds_)) {
                matched_rids_.push_back(candidate_rid);
            }
            index_scan.next();
        }
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

   private:
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

    std::vector<char> make_first_col_key(const Value &value) {
        std::vector<char> key(index_meta_.col_tot_len, 0);
        memcpy(key.data(), value.raw->data, index_meta_.cols[0].len);
        fill_suffix_with_min_key(key, 1);
        return key;
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

    int compare_first_col_key(const std::vector<char> &lhs, const std::vector<char> &rhs) {
        auto &col = index_meta_.cols[0];
        return compare_value(lhs.data(), rhs.data(), col.type, col.len);
    }

    void update_lower_bound(std::optional<Bound> &current, Bound candidate) {
        if (!current.has_value()) {
            current = std::move(candidate);
            return;
        }
        int cmp = compare_first_col_key(candidate.key, current->key);
        if (cmp > 0 || (cmp == 0 && current->inclusive && !candidate.inclusive)) {
            current = std::move(candidate);
        }
    }

    void update_upper_bound(std::optional<Bound> &current, Bound candidate) {
        if (!current.has_value()) {
            current = std::move(candidate);
            return;
        }
        int cmp = compare_first_col_key(candidate.key, current->key);
        if (cmp < 0 || (cmp == 0 && current->inclusive && !candidate.inclusive)) {
            current = std::move(candidate);
        }
    }

    bool bounds_are_empty(const std::optional<Bound> &lower, const std::optional<Bound> &upper) {
        if (!lower.has_value() || !upper.has_value()) {
            return false;
        }
        int cmp = compare_first_col_key(lower->key, upper->key);
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
