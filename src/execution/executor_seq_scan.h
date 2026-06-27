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

// 前向声明
extern TransactionManager* g_txn_manager;

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

    // 缓存：避免 beginTuple/nextTuple 和 Next() 重复调用 get_record
    std::unique_ptr<RmRecord> cached_record_;

    // 预计算：条件中列的元数据，避免每次 eval_conds 做 O(n) 线性搜索
    struct CondColInfo {
        size_t offset;
        ColType type;
        int len;
        size_t rhs_offset;  // 对于列-列比较
        bool is_rhs_val;
        ColType rhs_type;
        int rhs_len;
        CompOp op;
    };
    std::vector<CondColInfo> cond_col_infos_;
    bool cond_info_inited_ = false;

    TransactionManager* get_txn_manager() { return g_txn_manager; }

    void init_cond_col_infos() {
        if (cond_info_inited_) return;
        cond_info_inited_ = true;
        cond_col_infos_.reserve(conds_.size());
        for (auto &cond : conds_) {
            CondColInfo info;
            auto lhs_pos = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta &col) {
                return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
            });
            if (lhs_pos == cols_.end()) {
                cond_col_infos_.push_back(info);  // placeholder, eval will fail gracefully
                continue;
            }
            info.offset = lhs_pos->offset;
            info.type = lhs_pos->type;
            info.len = lhs_pos->len;
            info.op = cond.op;
            info.is_rhs_val = cond.is_rhs_val;
            if (cond.is_rhs_val) {
                info.rhs_offset = 0;
                info.rhs_type = lhs_pos->type;
                info.rhs_len = lhs_pos->len;
            } else {
                auto rhs_pos = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta &col) {
                    return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
                });
                if (rhs_pos == cols_.end()) {
                    cond_col_infos_.push_back(info);
                    continue;
                }
                info.rhs_offset = rhs_pos->offset;
                info.rhs_type = rhs_pos->type;
                info.rhs_len = rhs_pos->len;
            }
            cond_col_infos_.push_back(info);
        }
    }

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
        init_cond_col_infos();
        scan_ = std::make_unique<RmScan>(fh_);
        is_end_ = false;
        cached_record_.reset();
        track_predicate_read();
        // Skip to the first record that satisfies conditions
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_, context_);
            if (record != nullptr && eval_conds_fast(record.get())) {
                // SSI: 记录读操作
                if (context_ != nullptr && context_->txn_ != nullptr) {
                    auto* txn_mgr = get_txn_manager();
                    if (txn_mgr != nullptr) {
                        txn_mgr->record_read(context_->txn_, tab_name_, rid_);
                        auto deps = txn_mgr->check_rw_on_read(context_->txn_, tab_name_, rid_);
                        for (auto& [from, to] : deps) {
                            if (txn_mgr->add_rw_dependency_and_check(from, to)) {
                                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                    AbortReason::DEADLOCK_PREVENTION);
                            }
                        }
                    }
                }
                cached_record_ = std::move(record);
                return;
            }
            scan_->next();
        }
        is_end_ = true;
    }

    void nextTuple() override {
        cached_record_.reset();
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_, context_);
            if (record != nullptr && eval_conds_fast(record.get())) {
                // SSI: 记录读操作
                if (context_ != nullptr && context_->txn_ != nullptr) {
                    auto* txn_mgr = get_txn_manager();
                    if (txn_mgr != nullptr) {
                        txn_mgr->record_read(context_->txn_, tab_name_, rid_);
                        auto deps = txn_mgr->check_rw_on_read(context_->txn_, tab_name_, rid_);
                        for (auto& [from, to] : deps) {
                            if (txn_mgr->add_rw_dependency_and_check(from, to)) {
                                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                    AbortReason::DEADLOCK_PREVENTION);
                            }
                        }
                    }
                }
                cached_record_ = std::move(record);
                return;
            }
            scan_->next();
        }
        is_end_ = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (cached_record_) {
            return std::move(cached_record_);
        }
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

    // 使用预计算列信息的快速条件评估，避免 find_col_meta 线性搜索
    bool eval_conds_fast(RmRecord *record) {
        for (size_t i = 0; i < conds_.size(); ++i) {
            auto &cond = conds_[i];
            if (i >= cond_col_infos_.size()) break;
            auto &info = cond_col_infos_[i];
            char *lhs_buf = record->data + info.offset;

            if (cond.is_rhs_val) {
                char *rhs_buf = cond.rhs_val.raw->data;
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

    void track_predicate_read() {
        if (context_ == nullptr || context_->txn_ == nullptr) {
            return;
        }
        auto* txn_mgr = get_txn_manager();
        if (txn_mgr == nullptr ||
            context_->txn_->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return;
        }

        PredicateRead pred;
        pred.tab_name = tab_name_;
        pred.is_empty_result = false;
        pred.conds = conds_;
        pred.cols = cols_;
        txn_mgr->record_predicate_read(context_->txn_, pred);

        auto deps = txn_mgr->check_rw_on_predicate_read(context_->txn_, tab_name_, pred);
        for (auto& [from, to] : deps) {
            if (txn_mgr->add_rw_dependency_and_check(from, to)) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }
        }
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
