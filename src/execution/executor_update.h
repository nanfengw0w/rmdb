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
#include <cstring>
#include <utility>

#include "execution_defs.h"
#include "execution_manager.h"
#include "index_maintenance.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "recovery/log_manager.h"

extern TransactionManager* g_txn_manager;

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    size_t cur_idx_;

    Value value_from_record(const ColMeta &col, const char *record_data) const {
        Value value;
        if (col.type == TYPE_INT) {
            value.set_int(*reinterpret_cast<const int *>(record_data + col.offset));
        } else if (col.type == TYPE_FLOAT) {
            value.set_float(*reinterpret_cast<const float *>(record_data + col.offset));
        } else if (col.type == TYPE_STRING) {
            std::string str(record_data + col.offset, col.len);
            str.resize(strlen(str.c_str()));
            value.set_str(str);
        }
        return value;
    }

    Value eval_set_value(const SetClause &set_clause, const ColMeta &col, const char *record_data) const {
        if (set_clause.op == ArithOp::NO_OP) {
            return set_clause.rhs;
        }
        if (col.type != TYPE_INT && col.type != TYPE_FLOAT) {
            throw IncompatibleTypeError(coltype2str(col.type), "numeric");
        }

        Value lhs = value_from_record(col, record_data);
        double lhs_num = lhs.type == TYPE_INT ? lhs.int_val : lhs.float_val;
        double rhs_num = set_clause.rhs.type == TYPE_INT ? set_clause.rhs.int_val : set_clause.rhs.float_val;
        double result = lhs_num;
        switch (set_clause.op) {
            case ArithOp::ADD:
                result = lhs_num + rhs_num;
                break;
            case ArithOp::SUB:
                result = lhs_num - rhs_num;
                break;
            case ArithOp::MUL:
                result = lhs_num * rhs_num;
                break;
            case ArithOp::DIV:
                result = lhs_num / rhs_num;
                break;
            case ArithOp::NO_OP:
                break;
        }

        Value out;
        if (col.type == TYPE_INT) {
            out.set_int(static_cast<int>(result));
        } else {
            out.set_float(static_cast<float>(result));
        }
        return out;
    }

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
        cur_idx_ = 0;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (cur_idx_ >= rids_.size()) {
            return nullptr;
        }

        Transaction *txn = context_ == nullptr ? nullptr : context_->txn_;
        bool mvcc_txn = index_maintenance::is_mvcc_txn(context_);
        if (mvcc_txn && txn != nullptr && g_txn_manager != nullptr &&
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

        struct PendingUpdate {
            Rid rid;
            std::unique_ptr<RmRecord> old_record;
            std::unique_ptr<RmRecord> new_record;
            std::vector<std::vector<char>> old_keys;
            std::vector<std::vector<char>> new_keys;
        };

        std::vector<PendingUpdate> pending_updates;
        pending_updates.reserve(rids_.size());

        for (auto &rid : rids_) {
            PendingUpdate pending;
            pending.rid = rid;
            pending.old_record = fh_->get_record(rid, context_);
            if (pending.old_record == nullptr) {
                if (mvcc_txn && context_ != nullptr && context_->txn_ != nullptr) {
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                        AbortReason::DEADLOCK_PREVENTION);
                }
                continue;
            }
            pending.new_record = std::make_unique<RmRecord>(*pending.old_record);

            for (auto set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                Value rhs = eval_set_value(set_clause, *col, pending.new_record->data);
                if (col->type != rhs.type) {
                    if (col->type == TYPE_FLOAT && rhs.type == TYPE_INT) {
                        rhs.set_float(static_cast<float>(rhs.int_val));
                    } else {
                        throw IncompatibleTypeError(coltype2str(col->type), coltype2str(rhs.type));
                    }
                }
                rhs.init_raw(col->len);
                memcpy(pending.new_record->data + col->offset, rhs.raw->data, col->len);
            }

            pending.old_keys.reserve(tab_.indexes.size());
            pending.new_keys.reserve(tab_.indexes.size());
            for (auto &index : tab_.indexes) {
                pending.old_keys.emplace_back(index_maintenance::build_key(index, pending.old_record->data));
                pending.new_keys.emplace_back(index_maintenance::build_key(index, pending.new_record->data));
            }
            pending_updates.emplace_back(std::move(pending));
        }

        for (auto &pending : pending_updates) {
            for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
                index_maintenance::check_unique_conflict(sm_manager_, tab_name_, tab_.indexes[index_no],
                                                         pending.new_keys[index_no].data(), pending.rid,
                                                         context_);
            }
            index_maintenance::check_logical_key_write_conflict(sm_manager_, tab_, tab_name_,
                                                                pending.new_record->data, pending.rid,
                                                                context_);
        }

        for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
            for (size_t i = 0; i < pending_updates.size(); ++i) {
                for (size_t j = i + 1; j < pending_updates.size(); ++j) {
                    if (pending_updates[i].new_keys[index_no] == pending_updates[j].new_keys[index_no]) {
                        throw UniqueIndexConflictError(tab_name_,
                                                       index_maintenance::index_col_names(tab_.indexes[index_no]));
                    }
                }
            }
        }

        for (auto &pending : pending_updates) {
            // SSI: 检查 rw 依赖
            if (txn != nullptr && g_txn_manager != nullptr) {
                auto deps = g_txn_manager->check_rw_on_write(txn, tab_name_, pending.rid,
                                                             pending.old_record.get(),
                                                             pending.new_record.get());
                for (auto& [from, to] : deps) {
                    if (g_txn_manager->add_rw_dependency_and_check(from, to)) {
                        throw TransactionAbortException(txn->get_transaction_id(),
                            AbortReason::DEADLOCK_PREVENTION);
                    }
                }
            }

            fh_->update_record(pending.rid, pending.new_record->data, context_);

            // WAL: Write update log record to buffer (flushed at commit or when buffer is full)
            if (txn != nullptr && context_->log_mgr_ != nullptr) {
                if (g_txn_manager != nullptr) {
                    g_txn_manager->ensure_txn_begin_logged(txn, context_->log_mgr_);
                }
                UpdateLogRecord update_log(txn->get_transaction_id(), *pending.old_record,
                                           *pending.new_record, pending.rid, tab_name_);
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&update_log);
                txn->set_prev_lsn(lsn);
            }

            if (txn != nullptr) {
                txn->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, pending.rid, *pending.old_record));
                if (g_txn_manager != nullptr) {
                    g_txn_manager->record_write(txn, tab_name_, pending.rid, WType::UPDATE_TUPLE,
                                                pending.old_record.get(), pending.new_record.get(),
                                                false, false);
                }
            }

            for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
                if (pending.old_keys[index_no] == pending.new_keys[index_no]) {
                    continue;
                }
                auto ih = index_maintenance::get_index_handle(sm_manager_, tab_name_, tab_.indexes[index_no]);
                if (!mvcc_txn) {
                    ih->delete_entry(pending.old_keys[index_no].data(), txn);
                }
                ih->insert_entry(pending.new_keys[index_no].data(), pending.rid, txn);
            }
        }

        cur_idx_ = rids_.size();
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return cur_idx_ >= rids_.size(); }
};
