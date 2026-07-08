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
#include "index_maintenance.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "recovery/log_manager.h"

extern TransactionManager* g_txn_manager;

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    SmManager *sm_manager_;
    size_t cur_idx_;

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
        cur_idx_ = 0;
    }

    std::unique_ptr<RmRecord> Next() override {
        Transaction *txn = context_ == nullptr ? nullptr : context_->txn_;
        if (index_maintenance::is_mvcc_txn(context_) && txn != nullptr && g_txn_manager != nullptr &&
            txn->get_perf_mode() && rids_.size() == 1 &&
            !g_txn_manager->owns_perf_write_lock(txn, fh_->GetFd(), rids_[0]) &&
            !g_txn_manager->has_perf_write_locks(txn)) {
            if (!g_txn_manager->acquire_perf_write_lock_wait_for(txn, fh_->GetFd(), rids_[0],
                                                                 std::chrono::milliseconds(20))) {
                throw TransactionAbortException(txn->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }
            txn->set_start_ts(g_txn_manager->get_next_timestamp());
        }

        while (cur_idx_ < rids_.size()) {
            // Get the record before deletion
            auto record = fh_->get_record(rids_[cur_idx_], context_);
            if (record == nullptr) {
                if (index_maintenance::is_mvcc_txn(context_) && txn != nullptr) {
                    throw TransactionAbortException(txn->get_transaction_id(),
                        AbortReason::DEADLOCK_PREVENTION);
                }
                cur_idx_++;
                continue;
            }

            // SSI: 检查 rw 依赖
            if (txn != nullptr && g_txn_manager != nullptr) {
                auto deps = g_txn_manager->check_rw_on_write(txn, tab_name_, rids_[cur_idx_],
                                                             record.get(), nullptr);
                for (auto& [from, to] : deps) {
                    if (g_txn_manager->add_rw_dependency_and_check(from, to)) {
                        throw TransactionAbortException(txn->get_transaction_id(),
                            AbortReason::DEADLOCK_PREVENTION);
                    }
                }
            }

            fh_->delete_record(rids_[cur_idx_], context_);

            // WAL: Write delete log record to buffer (flushed at commit or when buffer is full)
            if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
                DeleteLogRecord delete_log(context_->txn_->get_transaction_id(), *record,
                                           rids_[cur_idx_], tab_name_);
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&delete_log);
                context_->txn_->set_prev_lsn(lsn);
            }

            // Record write operation for transaction abort (save old value)
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(new WriteRecord(WType::DELETE_TUPLE, tab_name_, rids_[cur_idx_], *record));
                if (g_txn_manager != nullptr) {
                    g_txn_manager->record_write(context_->txn_, tab_name_, rids_[cur_idx_],
                                                WType::DELETE_TUPLE, record.get(), nullptr,
                                                false, true);
                }
            }

            // MVCC keeps old index entries as visibility-filtered candidates for older snapshots.
            if (!index_maintenance::is_mvcc_txn(context_)) {
                for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                    auto& index = tab_.indexes[i];
                    auto ih = index_maintenance::get_index_handle(sm_manager_, tab_name_, index);
                    auto key = index_maintenance::build_key(index, record->data);
                    ih->delete_entry(key.data(), txn);
                }
            }
            cur_idx_++;
            break;
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return cur_idx_ >= rids_.size(); }
};
