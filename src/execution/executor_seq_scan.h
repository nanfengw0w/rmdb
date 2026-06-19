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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    bool is_end_;

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        fh_ = sm_manager_->get_table_fh(tab_name_);
        cols_ = sm_manager_->get_query_cols(tab_name_);
        len_ = cols_.back().offset + cols_.back().len;
        context_ = context;
        fed_conds_ = conds_;
        is_end_ = true;
    }

    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        is_end_ = false;
        // Skip to the first record that satisfies conditions
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_, context_);
            if (eval_conds(record.get(), conds_)) {
                return;
            }
            scan_->next();
        }
        is_end_ = true;
    }

    void nextTuple() override {
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_, context_);
            if (eval_conds(record.get(), conds_)) {
                return;
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
