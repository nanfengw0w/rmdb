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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;

    std::vector<Condition> fed_conds_;
    bool isend;
    bool right_valid_;
    std::unique_ptr<RmRecord> left_rec_;
    std::unique_ptr<RmRecord> right_rec_;

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
        right_valid_ = false;
    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            isend = true;
            return;
        }
        left_rec_ = left_->Next();
        right_->beginTuple();
        advance_to_match();
    }

    void nextTuple() override {
        right_->nextTuple();
        advance_to_match();
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
    void advance_to_match() {
        while (!left_->is_end()) {
            right_->beginTuple();
            while (!right_->is_end()) {
                right_rec_ = right_->Next();
                if (eval_conds()) {
                    return;
                }
                right_->nextTuple();
            }
            // Move to next left tuple
            left_->nextTuple();
            if (left_->is_end()) {
                isend = true;
                return;
            }
            left_rec_ = left_->Next();
        }
        isend = true;
    }

    bool eval_conds() {
        for (auto &cond : fed_conds_) {
            auto &lhs_meta = find_col_meta(cols_, cond.lhs_col);
            char *lhs_buf;
            if (lhs_meta.tab_name == left_->cols().front().tab_name) {
                lhs_buf = left_rec_->data + lhs_meta.offset;
            } else {
                lhs_buf = right_rec_->data + (lhs_meta.offset - (int)left_->tupleLen());
            }

            if (cond.is_rhs_val) {
                char *rhs_buf = cond.rhs_val.raw->data;
                int cmp = compare_value(lhs_buf, rhs_buf, lhs_meta.type, lhs_meta.len);
                if (!eval_cmp(cmp, cond.op)) return false;
            } else {
                auto &rhs_meta = find_col_meta(cols_, cond.rhs_col);
                char *rhs_buf;
                if (rhs_meta.tab_name == left_->cols().front().tab_name) {
                    rhs_buf = left_rec_->data + rhs_meta.offset;
                } else {
                    rhs_buf = right_rec_->data + (rhs_meta.offset - (int)left_->tupleLen());
                }
                int cmp = compare_value(lhs_buf, rhs_buf, lhs_meta.type, lhs_meta.len);
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