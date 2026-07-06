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

namespace {

std::vector<char> build_abort_index_key(const IndexMeta &index, const char *record_data) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (const auto &col : index.cols) {
        memcpy(key.data() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

IxIndexHandle *get_abort_index_handle(SmManager *sm_manager, const std::string &tab_name,
                                      const IndexMeta &index) {
    auto ix_name = sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols);
    return sm_manager->ihs_.at(ix_name).get();
}

void delete_abort_index_entries(SmManager *sm_manager, const std::string &tab_name,
                                const TabMeta &tab, const char *record_data) {
    for (const auto &index : tab.indexes) {
        auto key = build_abort_index_key(index, record_data);
        get_abort_index_handle(sm_manager, tab_name, index)->delete_entry(key.data(), nullptr);
    }
}

void insert_abort_index_entries(SmManager *sm_manager, const std::string &tab_name,
                                const TabMeta &tab, const char *record_data, const Rid &rid) {
    for (const auto &index : tab.indexes) {
        auto key = build_abort_index_key(index, record_data);
        get_abort_index_handle(sm_manager, tab_name, index)->insert_entry(key.data(), rid, nullptr);
    }
}

void rollback_update_index_entries(SmManager *sm_manager, const std::string &tab_name,
                                   const TabMeta &tab, const RmRecord *current,
                                   const RmRecord &old_record, const Rid &rid,
                                   bool mvcc_abort) {
    if (current == nullptr) {
        if (!mvcc_abort) {
            insert_abort_index_entries(sm_manager, tab_name, tab, old_record.data, rid);
        }
        return;
    }

    for (const auto &index : tab.indexes) {
        auto current_key = build_abort_index_key(index, current->data);
        auto old_key = build_abort_index_key(index, old_record.data);
        if (current_key == old_key) {
            continue;
        }
        auto ih = get_abort_index_handle(sm_manager, tab_name, index);
        ih->delete_entry(current_key.data(), nullptr);
        if (!mvcc_abort) {
            ih->insert_entry(old_key.data(), rid, nullptr);
        }
    }
}

void clear_write_records(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    auto write_set = txn->get_write_set();
    for (auto *wr : *write_set) {
        delete wr;
    }
    write_set->clear();
}

}  // namespace

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

    // WAL: Write begin log record (flushed at commit or when buffer is full)
    if (log_manager != nullptr) {
        log_manager->add_active_txn(txn->get_transaction_id());
        BeginLogRecord begin_log(txn->get_transaction_id());
        lsn_t lsn = log_manager->add_log_to_buffer(&begin_log);
        txn->set_prev_lsn(lsn);
    }

    return txn;
}

void TransactionManager::acquire_explicit_txn_lock(Transaction* txn) {
    if (txn == nullptr || txn->get_serial_txn_lock_held()) {
        return;
    }
    explicit_txn_mutex_.lock();
    txn->set_serial_txn_lock_held(true);
}

void TransactionManager::release_explicit_txn_lock(Transaction* txn) {
    if (txn == nullptr || !txn->get_serial_txn_lock_held()) {
        return;
    }
    txn->set_serial_txn_lock_held(false);
    explicit_txn_mutex_.unlock();
}

bool TransactionManager::acquire_perf_write_lock(Transaction* txn, int fd, const Rid& rid) {
    if (txn == nullptr) {
        return false;
    }

    PerfWriteLockKey key{fd, rid.page_no, rid.slot_no};
    std::lock_guard<std::mutex> lock(perf_write_lock_mutex_);
    auto it = perf_write_locks_.find(key);
    if (it != perf_write_locks_.end()) {
        return it->second == txn->get_transaction_id();
    }

    perf_write_locks_.emplace(key, txn->get_transaction_id());
    txn_perf_write_locks_[txn->get_transaction_id()].push_back(key);
    return true;
}

void TransactionManager::acquire_perf_write_lock_wait(Transaction* txn, int fd, const Rid& rid) {
    if (txn == nullptr) {
        return;
    }

    PerfWriteLockKey key{fd, rid.page_no, rid.slot_no};
    std::unique_lock<std::mutex> lock(perf_write_lock_mutex_);
    perf_write_lock_cv_.wait(lock, [&] {
        auto it = perf_write_locks_.find(key);
        return it == perf_write_locks_.end() || it->second == txn->get_transaction_id();
    });

    if (perf_write_locks_.find(key) != perf_write_locks_.end()) {
        return;
    }
    perf_write_locks_.emplace(key, txn->get_transaction_id());
    txn_perf_write_locks_[txn->get_transaction_id()].push_back(key);
}

bool TransactionManager::acquire_perf_write_lock_wait_for(Transaction* txn, int fd, const Rid& rid,
                                                          std::chrono::milliseconds timeout) {
    if (txn == nullptr) {
        return false;
    }

    PerfWriteLockKey key{fd, rid.page_no, rid.slot_no};
    std::unique_lock<std::mutex> lock(perf_write_lock_mutex_);
    bool available = perf_write_lock_cv_.wait_for(lock, timeout, [&] {
        auto it = perf_write_locks_.find(key);
        return it == perf_write_locks_.end() || it->second == txn->get_transaction_id();
    });
    if (!available) {
        return false;
    }

    auto it = perf_write_locks_.find(key);
    if (it != perf_write_locks_.end()) {
        return it->second == txn->get_transaction_id();
    }
    perf_write_locks_.emplace(key, txn->get_transaction_id());
    txn_perf_write_locks_[txn->get_transaction_id()].push_back(key);
    return true;
}

bool TransactionManager::owns_perf_write_lock(Transaction* txn, int fd, const Rid& rid) {
    if (txn == nullptr) {
        return false;
    }

    PerfWriteLockKey key{fd, rid.page_no, rid.slot_no};
    std::lock_guard<std::mutex> lock(perf_write_lock_mutex_);
    auto it = perf_write_locks_.find(key);
    return it != perf_write_locks_.end() && it->second == txn->get_transaction_id();
}

bool TransactionManager::has_perf_write_locks(Transaction* txn) {
    if (txn == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(perf_write_lock_mutex_);
    auto it = txn_perf_write_locks_.find(txn->get_transaction_id());
    return it != txn_perf_write_locks_.end() && !it->second.empty();
}

void TransactionManager::release_perf_write_locks(Transaction* txn) {
    if (txn == nullptr) {
        return;
    }

    bool released = false;
    {
        std::lock_guard<std::mutex> lock(perf_write_lock_mutex_);
        auto it = txn_perf_write_locks_.find(txn->get_transaction_id());
        if (it == txn_perf_write_locks_.end()) {
            return;
        }

        for (const auto& key : it->second) {
            auto owner = perf_write_locks_.find(key);
            if (owner != perf_write_locks_.end() && owner->second == txn->get_transaction_id()) {
                perf_write_locks_.erase(owner);
                released = true;
            }
        }
        txn_perf_write_locks_.erase(it);
    }
    if (released) {
        perf_write_lock_cv_.notify_all();
    }
}

/**
 * @description: 事务的提交方法
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) return;
    if (txn->get_state() == TransactionState::ABORTED ||
        txn->get_state() == TransactionState::COMMITTED) {
        release_perf_write_locks(txn);
        release_explicit_txn_lock(txn);
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
    release_perf_write_locks(txn);
    release_explicit_txn_lock(txn);
}

/**
 * @description: 事务的终止（回滚）方法
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) return;

    IsolationLevel level = txn->get_isolation_level();
    bool mvcc_abort = (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE);
    if (mvcc_abort) {
        // Restore disk/index state before removing version entries, so concurrent
        // readers remain protected by the old-image version chain during abort.
        auto write_set = txn->get_write_set();
        for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
            WriteRecord *wr = *it;
            auto &tab_name = wr->GetTableName();
            auto &rid = wr->GetRid();
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            auto &tab = sm_manager_->db_.get_table(tab_name);

            switch (wr->GetWriteType()) {
                case WType::INSERT_TUPLE: {
                    auto current = fh->get_record(rid, nullptr);
                    if (current != nullptr) {
                        delete_abort_index_entries(sm_manager_, tab_name, tab, current->data);
                    }
                    fh->delete_record(rid, nullptr);
                    break;
                }
                case WType::DELETE_TUPLE: {
                    fh->update_record(rid, wr->GetRecord().data, nullptr);
                    break;
                }
                case WType::UPDATE_TUPLE: {
                    auto current = fh->get_record(rid, nullptr);
                    rollback_update_index_entries(sm_manager_, tab_name, tab, current.get(),
                                                  wr->GetRecord(), rid, true);
                    fh->update_record(rid, wr->GetRecord().data, nullptr);
                    break;
                }
            }
        }

        auto& vm = VersionManager::instance();
        vm.abort_transaction(txn->get_transaction_id());
    } else {
        // 非MVCC模式：Undo all write operations in reverse order
        auto write_set = txn->get_write_set();
        for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
            WriteRecord *wr = *it;
            auto &tab_name = wr->GetTableName();
            auto &rid = wr->GetRid();
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            auto &tab = sm_manager_->db_.get_table(tab_name);

            switch (wr->GetWriteType()) {
                case WType::INSERT_TUPLE: {
                    auto current = fh->get_record(rid, nullptr);
                    if (current != nullptr) {
                        delete_abort_index_entries(sm_manager_, tab_name, tab, current->data);
                    }
                    fh->delete_record(rid, nullptr);
                    break;
                }
                case WType::DELETE_TUPLE: {
                    insert_abort_index_entries(sm_manager_, tab_name, tab, wr->GetRecord().data, rid);
                    fh->insert_record(rid, wr->GetRecord().data);
                    break;
                }
                case WType::UPDATE_TUPLE: {
                    auto current = fh->get_record(rid, nullptr);
                    rollback_update_index_entries(sm_manager_, tab_name, tab, current.get(),
                                                  wr->GetRecord(), rid, false);
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
        log_manager->remove_active_txn(txn->get_transaction_id());
    }

    txn->set_state(TransactionState::ABORTED);
    clear_write_records(txn);
    release_perf_write_locks(txn);
    release_explicit_txn_lock(txn);
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
