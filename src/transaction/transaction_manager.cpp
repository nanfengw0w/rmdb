/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "transaction/version_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }

    // 使用会话的隔离级别
    int session_id = static_cast<int>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    IsolationLevel session_level = get_session_isolation_level(session_id);
    txn->set_isolation_level(session_level);

    txn->set_state(TransactionState::GROWING);

    // MVCC: 分配开始时间戳
    IsolationLevel level = txn->get_isolation_level();
    if (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE) {
        txn->set_start_ts(get_next_timestamp());
    }

    txn_map[txn->get_transaction_id()] = txn;

    // WAL: Write begin log record and flush
    if (log_manager != nullptr) {
        BeginLogRecord begin_log(txn->get_transaction_id());
        lsn_t lsn = log_manager->add_log_to_buffer(&begin_log);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
        log_manager->add_active_txn(txn->get_transaction_id());
    }

    return txn;
}

/**
 * @description: 事务的提交方法
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) return;
    if (txn->get_state() == TransactionState::ABORTED ||
        txn->get_state() == TransactionState::COMMITTED) {
        return;
    }

    IsolationLevel level = txn->get_isolation_level();
    if (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE) {
        // MVCC: 分配提交时间戳和提交顺序
        timestamp_t commit_ts = get_next_timestamp();
        txn->set_commit_ts(commit_ts);
        txn->commit_order_ = global_commit_order_++;

        // 提交版本管理器中的所有写操作
        auto& vm = VersionManager::instance();
        vm.commit_transaction(txn->get_transaction_id(), commit_ts);
    }

    txn->set_state(TransactionState::COMMITTED);

    // WAL: Write commit log record and flush
    if (log_manager != nullptr) {
        CommitLogRecord commit_log(txn->get_transaction_id(), txn->get_prev_lsn());
        lsn_t lsn = log_manager->add_log_to_buffer(&commit_log);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
        log_manager->remove_active_txn(txn->get_transaction_id());
    }

    // Release all locks held by this transaction
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
}

/**
 * @description: 事务的终止（回滚）方法
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) return;

    IsolationLevel level = txn->get_isolation_level();
    if (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE) {
        // MVCC: 回滚版本管理器中的所有写操作
        auto& vm = VersionManager::instance();
        vm.abort_transaction(txn->get_transaction_id());

        // 使用write_set恢复旧数据到磁盘（与非MVCC模式相同）
        auto write_set = txn->get_write_set();
        for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
            WriteRecord *wr = *it;
            auto &tab_name = wr->GetTableName();
            auto &rid = wr->GetRid();
            auto fh = sm_manager_->fhs_.at(tab_name).get();

            switch (wr->GetWriteType()) {
                case WType::INSERT_TUPLE: {
                    fh->delete_record(rid, nullptr);
                    break;
                }
                case WType::DELETE_TUPLE: {
                    fh->update_record(rid, wr->GetRecord().data, nullptr);
                    break;
                }
                case WType::UPDATE_TUPLE: {
                    fh->update_record(rid, wr->GetRecord().data, nullptr);
                    break;
                }
            }
        }
    } else {
        // 非MVCC模式：Undo all write operations in reverse order
        auto write_set = txn->get_write_set();
        for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
            WriteRecord *wr = *it;
            auto &tab_name = wr->GetTableName();
            auto &rid = wr->GetRid();
            auto fh = sm_manager_->fhs_.at(tab_name).get();

            switch (wr->GetWriteType()) {
                case WType::INSERT_TUPLE: {
                    fh->delete_record(rid, nullptr);
                    break;
                }
                case WType::DELETE_TUPLE: {
                    fh->insert_record(rid, wr->GetRecord().data);
                    break;
                }
                case WType::UPDATE_TUPLE: {
                    fh->update_record(rid, wr->GetRecord().data, nullptr);
                    break;
                }
            }
        }
    }

    // Release all locks
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }

    // 清理 SSI 状态
    clear_ssi_state(txn->get_transaction_id());
    txn->read_set_.clear();
    txn->predicate_reads_.clear();
    txn->ssi_writes_.clear();
    txn->rw_deps_.clear();

    // WAL: Write abort log record
    if (log_manager != nullptr) {
        AbortLogRecord abort_log(txn->get_transaction_id(), txn->get_prev_lsn());
        lsn_t lsn = log_manager->add_log_to_buffer(&abort_log);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
        log_manager->remove_active_txn(txn->get_transaction_id());
    }

    txn->set_state(TransactionState::ABORTED);
}

/**
 * @brief SSI: 检查危险结构
 */
bool TransactionManager::check_dangerous_structure(Transaction* txn) {
    // 检查是否存在两个事务互相有 rw 依赖
    for (auto& [tin_id, tout_id] : txn->rw_deps_) {
        // txn ->rw tout_id
        if (tin_id != txn->get_transaction_id()) continue;

        Transaction* tout = get_transaction(tout_id);
        if (tout == nullptr || tout->get_state() != TransactionState::GROWING) continue;

        // 检查 tout 是否也有依赖到 txn
        for (auto& [tin2_id, tout2_id] : tout->rw_deps_) {
            if (tin2_id == tout_id && tout2_id == txn->get_transaction_id()) {
                return true;  // 危险结构
            }
        }
    }

    return false;
}

// 辅助函数：从表名获取文件描述符
int TransactionManager::fd_from_tab_name(const std::string& tab_name) {
    auto it = sm_manager_->fhs_.find(tab_name);
    if (it != sm_manager_->fhs_.end()) {
        return it->second->GetFd();
    }
    return -1;
}
