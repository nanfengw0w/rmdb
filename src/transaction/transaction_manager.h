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

#include <atomic>
#include <unordered_map>
#include <optional>
#include <functional>
#include <shared_mutex>

#include "transaction.h"
#include "watermark.h"
#include "recovery/log_manager.h"
#include "concurrency/lock_manager.h"
#include "system/sm_manager.h"
#include "common/exception.h"

/* 系统采用的并发控制算法，当前题目中要求两阶段封锁并发控制算法 */
enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0, BASIC_TO, MVCC };

/// 版本链中的第一个撤销链接，将表堆元组链接到撤销日志。
struct VersionUndoLink {
    /** 版本链中的下一个版本。 */
    UndoLink prev_;
    bool in_progress_{false};
    /** 创建此版本的事务ID */
    txn_id_t creator_txn_{INVALID_TXN_ID};
    /** 此版本的提交时间戳（0表示未提交） */
    timestamp_t commit_ts_{0};

    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_;
    }

    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }

    inline static std::optional<VersionUndoLink> FromOptionalUndoLink(std::optional<UndoLink> undo_link) {
        if (undo_link.has_value()) {
            return VersionUndoLink{*undo_link};
        }
        return std::nullopt;
    }
};

class TransactionManager{
public:
    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager,
                             ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING) {
        sm_manager_ = sm_manager;
        lock_manager_ = lock_manager;
        concurrency_mode_ = concurrency_mode;
    }
    
    ~TransactionManager() = default;

    Transaction* begin(Transaction* txn, LogManager* log_manager);

    void commit(Transaction* txn, LogManager* log_manager);

    void abort(Transaction* txn, LogManager* log_manager);

    ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }

    void set_concurrency_mode(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }

    LockManager* get_lock_manager() { return lock_manager_; }

    // MVCC: 获取下一个时间戳
    timestamp_t get_next_timestamp() { return next_timestamp_++; }
    // MVCC: 获取下一个提交顺序
    int64_t get_next_commit_order() { return next_commit_order_++; }

    // 会话隔离级别管理
    void set_session_isolation_level(int session_id, IsolationLevel level) {
        std::unique_lock<std::mutex> lock(session_mutex_);
        session_isolation_levels_[session_id] = level;
    }
    IsolationLevel get_session_isolation_level(int session_id) {
        std::unique_lock<std::mutex> lock(session_mutex_);
        auto it = session_isolation_levels_.find(session_id);
        if (it != session_isolation_levels_.end()) return it->second;
        return IsolationLevel::SERIALIZABLE;
    }

    // 获取所有活跃事务
    std::vector<Transaction*> get_active_transactions() {
        std::vector<Transaction*> result;
        std::unique_lock<std::mutex> lock(latch_);
        for (auto& [id, txn] : txn_map) {
            if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
                result.push_back(txn);
            }
        }
        return result;
    }

    // SSI: 检查危险结构
    bool check_dangerous_structure(Transaction* txn);

    // SSI: 记录读操作
    void record_read(Transaction* txn, const std::string& tab_name, const Rid& rid) {
        if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            std::lock_guard<std::mutex> lock(ssi_mutex_);
            txn->read_set_.insert({tab_name, rid});
        }
    }

    // SSI: 记录谓词读
    void record_predicate_read(Transaction* txn, const PredicateRead& pred) {
        if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            std::lock_guard<std::mutex> lock(ssi_mutex_);
            txn->predicate_reads_.push_back(pred);
        }
    }

    // SSI: 检查写操作是否与读集合冲突，返回需要创建的 rw 依赖
    std::vector<std::pair<txn_id_t, txn_id_t>> check_rw_on_write(
        Transaction* writer, const std::string& tab_name, const Rid& rid) {

        std::vector<std::pair<txn_id_t, txn_id_t>> new_deps;
        if (writer->get_isolation_level() != IsolationLevel::SERIALIZABLE) return new_deps;

        std::lock_guard<std::mutex> lock(ssi_mutex_);

        // 检查所有活跃的 SER 事务
        for (auto& [id, reader] : txn_map) {
            if (reader == nullptr || reader == writer) continue;
            if (reader->get_state() != TransactionState::GROWING) continue;
            if (reader->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;

            // 检查 reader 的读集合
            if (reader->read_set_.count({tab_name, rid})) {
                // reader 读过这条记录，writer 要修改它
                // 形成 reader ->rw writer 依赖
                new_deps.push_back({reader->get_transaction_id(), writer->get_transaction_id()});
            }
        }

        return new_deps;
    }

    // SSI: 检查读操作是否与写集合冲突，返回需要创建的 rw 依赖
    // 当 reader 读取的记录被 writer 写过，且 writer 的写入对 reader 不可见时，形成 reader ->rw writer
    std::vector<std::pair<txn_id_t, txn_id_t>> check_rw_on_read(
        Transaction* reader, const std::string& tab_name, const Rid& rid) {

        std::vector<std::pair<txn_id_t, txn_id_t>> new_deps;
        if (reader->get_isolation_level() != IsolationLevel::SERIALIZABLE) return new_deps;

        std::lock_guard<std::mutex> lock(ssi_mutex_);

        // 检查所有活跃的 SER 事务
        for (auto& [id, writer] : txn_map) {
            if (writer == nullptr || writer == reader) continue;
            if (writer->get_state() != TransactionState::GROWING) continue;
            if (writer->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;

            // 检查 writer 的写集合
            auto write_set = writer->get_write_set();
            for (auto& wr : *write_set) {
                if (wr->GetTableName() == tab_name && wr->GetRid() == rid) {
                    // writer 写过这条记录，reader 要读它
                    // 如果 writer 的写入在 reader 的快照中不可见，形成 reader ->rw writer
                    new_deps.push_back({reader->get_transaction_id(), writer->get_transaction_id()});
                }
            }
        }

        return new_deps;
    }

    // SSI: 添加 rw 依赖并检查危险结构
    bool add_rw_dependency_and_check(txn_id_t from, txn_id_t to) {
        std::lock_guard<std::mutex> lock(ssi_mutex_);

        Transaction* from_txn = get_transaction(from, false);
        Transaction* to_txn = get_transaction(to, false);

        if (from_txn == nullptr || to_txn == nullptr) return false;

        // 检查依赖是否已存在
        for (auto& [f, t] : from_txn->rw_deps_) {
            if (f == from && t == to) return false;  // 已存在
        }

        // 添加依赖
        from_txn->rw_deps_.push_back({from, to});

        // 检查危险结构
        // 检查是否存在 to ->rw from 的依赖
        for (auto& [tin, tout] : to_txn->rw_deps_) {
            if (tin == to && tout == from) {
                // 形成危险结构
                return true;
            }
        }

        return false;
    }

    // 辅助函数：从表名获取文件描述符
    int fd_from_tab_name(const std::string& tab_name);

    // 全局提交顺序计数器
    std::atomic<int64_t> global_commit_order_{0};
    // 全局互斥锁
    std::mutex mvcc_mutex_;
    // SSI 互斥锁
    std::mutex ssi_mutex_;

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     * @param {bool} check_thread 是否检查线程ID（SSI检查时需要设为false）
     */
    Transaction* get_transaction(txn_id_t txn_id, bool check_thread = true) {
        if(txn_id == INVALID_TXN_ID) return nullptr;

        std::unique_lock<std::mutex> lock(latch_);
        auto it = TransactionManager::txn_map.find(txn_id);
        if (it == TransactionManager::txn_map.end()) {
            return nullptr;
        }
        auto *res = it->second;
        lock.unlock();
        if (check_thread && (res == nullptr || res->get_thread_id() != std::this_thread::get_id())) {
            return nullptr;
        }

        return res;
    }

    static std::unordered_map<txn_id_t, Transaction *> txn_map;     // 全局事务表，存放事务ID与事务对象的映射关系
    std::shared_mutex txn_map_mutex_;
    /** ------------------------以下函数仅可能在MVCC当中使用------------------------------------------*/

    /**
    * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
    * 在更新之前，将调用 `check` 函数以确保有效性。
    */
    bool UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr);

    /**
     * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
     * 在更新之前，将调用 `check` 函数以确保有效性。
     */
    bool UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr);

    /** @brief 获取表堆元组的第一个撤销日志。 */
    std::optional<UndoLink> GetUndoLink(Rid rid);

    /** @brief 获取表堆元组的第一个撤销日志。*/
    std::optional<VersionUndoLink> GetVersionLink(Rid rid);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
     * 如果索引超出范围仍然会抛出异常。 */
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
     * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
    UndoLog GetUndoLog(UndoLink link);

    /** @brief 获取系统中的最低读时间戳。 */
    timestamp_t GetWatermark();

    /** @brief 垃圾回收。仅在所有事务都未访问时调用。 */
    void GarbageCollection();

    struct PageVersionInfo {
        std::shared_mutex mutex_;
        /** 存储所有槽的先前版本信息。注意：不要使用 `[x]` 来访问它，因为
         * 即使不存在也会创建新元素。请使用 `find` 来代替。
         */
        std::unordered_map<slot_offset_t, VersionUndoLink> prev_version_;
    };

    /** 保护版本信息 */
    std::shared_mutex version_info_mutex_;
    /** 存储表堆中每个元组的先前版本。 */
    std::unordered_map<page_id_t, std::shared_ptr<PageVersionInfo>> version_info_;


private:
    ConcurrencyMode concurrency_mode_;      // 事务使用的并发控制算法，目前只需要考虑2PL
    std::atomic<txn_id_t> next_txn_id_{0};  // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{0};    // 用于分发事务时间戳
    std::atomic<int64_t> next_commit_order_{0};  // 用于分发提交顺序
    std::mutex latch_;  // 用于txn_map的并发
    SmManager *sm_manager_;
    LockManager *lock_manager_;
    std::mutex session_mutex_;  // 用于会话隔离级别映射的并发
    std::unordered_map<int, IsolationLevel> session_isolation_levels_;  // 会话隔离级别映射

    std::atomic<timestamp_t> last_commit_ts_{0};    // 最后提交的时间戳,仅用于MVCC
    Watermark running_txns_{0};             // 存储所有正在运行事务的读取时间戳，以便于垃圾回收，仅用于MVCC
};
