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
#include <optional>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_index_scan.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;

    std::vector<Condition> fed_conds_;
    bool isend;
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> right_rec_;
    IndexScanExecutor *right_index_scan_;

    struct RuntimeLookup {
        TabCol right_col;
        ColMeta left_col;
    };
    std::optional<RuntimeLookup> runtime_lookup_;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
        right_index_scan_ = dynamic_cast<IndexScanExecutor *>(right_.get());
        init_runtime_lookup();
    }

    void beginTuple() override {
        isend = false;
        left_->beginTuple();
        if (left_->is_end()) {
            isend = true;
            return;
        }
        left_rec_ = left_->Next();
        restart_right();
        if (!find_next_match()) {
            isend = true;
        }
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        right_->nextTuple();
        if (!find_next_match()) {
            isend = true;
        }
    }

    std::string getType() override { return "NestedLoopJoinExecutor"; }

    bool find_next_match() {
        while (true) {
            while (right_->is_end()) {
                left_->nextTuple();
                if (left_->is_end()) {
                    return false;
                }
                left_rec_ = left_->Next();
                restart_right();
            }
            right_rec_ = right_->Next();
            if (eval_conds()) {
                return true;
            }
            right_->nextTuple();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        auto result = std::make_unique<RmRecord>(len_);
        memcpy(result->data, left_rec_->data, left_->tupleLen());
        memcpy(result->data + left_->tupleLen(), right_rec_->data, right_->tupleLen());
        return result;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
    void init_runtime_lookup() {
        if (right_index_scan_ == nullptr) {
            return;
        }
        for (auto &cond : fed_conds_) {
            if (cond.is_rhs_val || cond.op != OP_EQ) {
                continue;
            }
            if (right_index_scan_->can_runtime_lookup(cond.lhs_col)) {
                auto left_col = find_col_meta_if_exists(left_->cols(), cond.rhs_col);
                if (left_col.has_value()) {
                    runtime_lookup_ = RuntimeLookup{cond.lhs_col, left_col.value()};
                    return;
                }
            }
            if (right_index_scan_->can_runtime_lookup(cond.rhs_col)) {
                auto left_col = find_col_meta_if_exists(left_->cols(), cond.lhs_col);
                if (left_col.has_value()) {
                    runtime_lookup_ = RuntimeLookup{cond.rhs_col, left_col.value()};
                    return;
                }
            }
        }
    }

    std::optional<ColMeta> find_col_meta_if_exists(const std::vector<ColMeta> &cols, const TabCol &target) {
        auto pos = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == cols.end()) {
            return std::nullopt;
        }
        return *pos;
    }

    void restart_right() {
        if (right_index_scan_ != nullptr && runtime_lookup_.has_value()) {
            right_index_scan_->set_runtime_lookup_key(runtime_lookup_->right_col,
                                                      left_rec_->data + runtime_lookup_->left_col.offset);
        }
        right_->beginTuple();
    }

    bool eval_conds() {
        for (auto &cond : fed_conds_) {
            auto &lhs_meta = find_col_meta(cols_, cond.lhs_col);
            char *lhs_buf = value_addr(lhs_meta);

            if (cond.is_rhs_val) {
                char *rhs_buf = cond.rhs_val.raw->data;
                int cmp = compare_value(lhs_buf, rhs_buf, lhs_meta.type, lhs_meta.len);
                if (!eval_cmp(cmp, cond.op)) return false;
            } else {
                auto &rhs_meta = find_col_meta(cols_, cond.rhs_col);
                char *rhs_buf = value_addr(rhs_meta);
                int cmp = compare_value(lhs_buf, rhs_buf, lhs_meta.type, lhs_meta.len);
                if (!eval_cmp(cmp, cond.op)) return false;
            }
        }
        return true;
    }

    char *value_addr(const ColMeta &meta) {
        int left_len = static_cast<int>(left_->tupleLen());
        if (meta.offset < left_len) {
            return left_rec_->data + meta.offset;
        }
        return right_rec_->data + (meta.offset - left_len);
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
