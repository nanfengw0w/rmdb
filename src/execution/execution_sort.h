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
#include <cstring>
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    ColMeta sort_col_;
    std::vector<ColMeta> cols_;
    size_t len_;
    size_t cursor_;
    bool is_desc_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, TabCol sel_cols, bool is_desc) {
        prev_ = std::move(prev);
        auto pos = get_col(prev_->cols(), sel_cols);
        sort_col_ = *pos;
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
        is_desc_ = is_desc;
        cursor_ = 0;
    }

    void beginTuple() override {
        tuples_.clear();
        cursor_ = 0;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            tuples_.push_back(prev_->Next());
        }
        std::stable_sort(tuples_.begin(), tuples_.end(),
            [&](const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b) {
                int cmp = compare_key(a->data + sort_col_.offset, b->data + sort_col_.offset);
                return is_desc_ ? (cmp > 0) : (cmp < 0);
            });
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return cursor_ >= tuples_.size(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

   private:
    int compare_key(const char *lhs, const char *rhs) const {
        if (sort_col_.type == TYPE_INT) {
            int a = *(int *)lhs;
            int b = *(int *)rhs;
            return (a > b) ? 1 : ((a < b) ? -1 : 0);
        }
        if (sort_col_.type == TYPE_FLOAT) {
            float a = *(float *)lhs;
            float b = *(float *)rhs;
            return (a > b) ? 1 : ((a < b) ? -1 : 0);
        }
        return memcmp(lhs, rhs, sort_col_.len);
    }
};
