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

#include "execution_defs.h"
#include "execution_manager.h"
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
    std::unique_ptr<RecScan> scan_;
    bool is_end_;

    SmManager *sm_manager_;

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

        // Get the index handle
        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        if (sm_manager_->ihs_.count(ix_name)) {
            ih_ = sm_manager_->ihs_.at(ix_name).get();
        } else {
            ih_ = nullptr;
        }
    }

    void beginTuple() override {
        if (ih_ == nullptr) {
            is_end_ = true;
            return;
        }

        // Build search key from conditions
        char *key = new char[index_meta_.col_tot_len];
        memset(key, 0, index_meta_.col_tot_len);

        bool has_eq = true;
        int offset = 0;
        for (auto &col : index_meta_.cols) {
            bool found = false;
            for (auto &cond : conds_) {
                if (cond.is_rhs_val && cond.lhs_col.col_name == col.name && cond.op == OP_EQ) {
                    memcpy(key + offset, cond.rhs_val.raw->data, col.len);
                    found = true;
                    break;
                }
            }
            if (!found) {
                has_eq = false;
                break;
            }
            offset += col.len;
        }

        if (has_eq && index_col_names_.size() == (size_t)index_meta_.col_num) {
            // Exact match on all index columns - use point lookup
            std::vector<Rid> result;
            if (ih_->get_value(key, &result, nullptr) && !result.empty()) {
                rid_ = result[0];
                auto record = fh_->get_record(rid_, context_);
                if (eval_conds(record.get(), fed_conds_)) {
                    is_end_ = false;
                    delete[] key;
                    return;
                }
            }
            is_end_ = true;
            delete[] key;
            return;
        }

        // Range scan using lower_bound
        Iid lower = ih_->lower_bound(key);
        Iid upper = ih_->leaf_end();
        delete[] key;

        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        is_end_ = false;

        // Find first matching tuple
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            try {
                auto record = fh_->get_record(rid_, context_);
                if (eval_conds(record.get(), fed_conds_)) {
                    return;
                }
            } catch (...) {
                // Skip invalid records
            }
            scan_->next();
        }
        is_end_ = true;
    }

    void nextTuple() override {
        // If scan_ is null (point lookup mode), there's only one result
        if (!scan_) {
            is_end_ = true;
            return;
        }
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            try {
                auto record = fh_->get_record(rid_, context_);
                if (eval_conds(record.get(), fed_conds_)) {
                    return;
                }
            } catch (...) {
                // Skip invalid records
            }
            scan_->next();
        }
        is_end_ = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return is_end_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
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
