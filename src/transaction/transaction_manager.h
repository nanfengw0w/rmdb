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
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <functional>
#include <shared_mutex>
#include <cstring>
#include <limits>
#include <condition_variable>
#include <chrono>

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
        current_instance_ = this;
    }
    
    ~TransactionManager() = default;

    Transaction* begin(Transaction* txn, LogManager* log_manager);

    void ensure_txn_begin_logged(Transaction* txn, LogManager* log_manager);

    void commit(Transaction* txn, LogManager* log_manager);

    void abort(Transaction* txn, LogManager* log_manager);

    void acquire_explicit_txn_lock(Transaction* txn);

    void release_explicit_txn_lock(Transaction* txn);

    bool acquire_perf_write_lock(Transaction* txn, int fd, const Rid& rid);

    void acquire_perf_write_lock_wait(Transaction* txn, int fd, const Rid& rid);

    bool acquire_perf_write_lock_wait_for(Transaction* txn, int fd, const Rid& rid,
                                          std::chrono::milliseconds timeout);

    bool owns_perf_write_lock(Transaction* txn, int fd, const Rid& rid);

    bool has_perf_write_locks(Transaction* txn);

    void release_perf_write_locks(Transaction* txn);

    static TransactionManager* current() { return current_instance_; }

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
        return IsolationLevel::READ_UNCOMMITTED;
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

    void record_scan_predicate(Transaction* txn, const std::string& tab_name,
                               const std::vector<Condition>& conds,
                               const std::vector<ColMeta>& cols) {
        if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return;
        }
        PredicateRead pred;
        pred.tab_name = tab_name;
        pred.is_empty_result = false;
        pred.conds = conds;
        pred.cols = cols;
        record_predicate_read(txn, pred);
    }

    void record_write(Transaction* txn, const std::string& tab_name, const Rid& rid,
                      WType type, const RmRecord* before, const RmRecord* after,
                      bool before_deleted, bool after_deleted) {
        if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return;
        }
        std::lock_guard<std::mutex> lock(ssi_mutex_);
        TxnWriteInfo info;
        info.tab_name = tab_name;
        info.rid = rid;
        info.type = type;
        info.before_deleted = before_deleted;
        info.after_deleted = after_deleted;
        if (before != nullptr && !before_deleted) {
            info.before = std::make_shared<RmRecord>(*before);
        }
        if (after != nullptr && !after_deleted) {
            info.after = std::make_shared<RmRecord>(*after);
        }
        txn->ssi_writes_.push_back(std::move(info));
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

    std::vector<std::pair<txn_id_t, txn_id_t>> check_rw_on_write(
        Transaction* writer, const std::string& tab_name, const Rid& rid,
        const RmRecord* before, const RmRecord* after) {

        std::vector<std::pair<txn_id_t, txn_id_t>> new_deps;
        if (writer == nullptr || writer->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return new_deps;
        }

        std::lock_guard<std::mutex> lock(ssi_mutex_);

        for (auto& [id, reader] : txn_map) {
            if (reader == nullptr || reader == writer) continue;
            if (reader->get_state() == TransactionState::ABORTED) continue;
            if (reader->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;
            if (!write_invisible_to_reader_locked(reader, writer)) continue;

            bool conflicts = reader->read_set_.count({tab_name, rid}) > 0;
            if (!conflicts) {
                for (auto& pred : reader->predicate_reads_) {
                    if (pred.tab_name != tab_name) continue;
                    if (record_matches_predicate_locked(before, pred) ||
                        record_matches_predicate_locked(after, pred)) {
                        conflicts = true;
                        break;
                    }
                }
            }
            if (conflicts) {
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

    std::vector<std::pair<txn_id_t, txn_id_t>> check_rw_on_predicate_read(
        Transaction* reader, const std::string& tab_name, const PredicateRead& pred) {

        std::vector<std::pair<txn_id_t, txn_id_t>> new_deps;
        if (reader == nullptr || reader->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
            return new_deps;
        }

        std::lock_guard<std::mutex> lock(ssi_mutex_);

        for (auto& [id, writer] : txn_map) {
            if (writer == nullptr || writer == reader) continue;
            if (writer->get_state() == TransactionState::ABORTED) continue;
            if (writer->get_isolation_level() != IsolationLevel::SERIALIZABLE) continue;
            if (!write_invisible_to_reader_locked(reader, writer)) continue;

            for (auto& write : writer->ssi_writes_) {
                if (write.tab_name != tab_name) continue;
                if (record_matches_predicate_locked(write.before.get(), pred) ||
                    record_matches_predicate_locked(write.after.get(), pred)) {
                    new_deps.push_back({reader->get_transaction_id(), writer->get_transaction_id()});
                    break;
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
        if (from_txn->get_state() == TransactionState::ABORTED ||
            to_txn->get_state() == TransactionState::ABORTED ||
            from == to) {
            return false;
        }

        // 检查依赖是否已存在
        for (auto& [f, t] : rw_edges_) {
            if (f == from && t == to) return false;  // 已存在
        }

        // 添加依赖
        rw_edges_.push_back({from, to});
        from_txn->rw_deps_.push_back({from, to});

        auto dangerous = [&](txn_id_t tin, txn_id_t tpivot, txn_id_t tout) -> bool {
            Transaction* tin_txn = get_transaction(tin, false);
            Transaction* tpivot_txn = get_transaction(tpivot, false);
            Transaction* tout_txn = get_transaction(tout, false);
            if (tin_txn == nullptr || tpivot_txn == nullptr || tout_txn == nullptr) return false;
            if (tin_txn->get_state() == TransactionState::ABORTED ||
                tpivot_txn->get_state() == TransactionState::ABORTED ||
                tout_txn->get_state() == TransactionState::ABORTED) {
                return false;
            }
            if (tin == tout) return true;
            if (tout_txn->get_state() == TransactionState::COMMITTED) {
                if (tin_txn->get_state() == TransactionState::COMMITTED) {
                    return tout_txn->commit_order_ >= 0 &&
                           tin_txn->commit_order_ >= 0 &&
                           tout_txn->commit_order_ < tin_txn->commit_order_;
                }
                return tout_txn->commit_order_ >= 0;
            }
            return false;
        };

        for (auto& [f, t] : rw_edges_) {
            if (f == to && dangerous(from, to, t)) return true;
            if (t == from && dangerous(f, from, to)) return true;
        }

        return false;
    }

    void clear_ssi_state(txn_id_t txn_id) {
        std::lock_guard<std::mutex> lock(ssi_mutex_);
        rw_edges_.erase(
            std::remove_if(rw_edges_.begin(), rw_edges_.end(),
                [txn_id](const std::pair<txn_id_t, txn_id_t>& edge) {
                    return edge.first == txn_id || edge.second == txn_id;
                }),
            rw_edges_.end());

        for (auto& [id, txn] : txn_map) {
            if (txn == nullptr) continue;
            auto& deps = txn->rw_deps_;
            deps.erase(
                std::remove_if(deps.begin(), deps.end(),
                    [txn_id](const std::pair<txn_id_t, txn_id_t>& edge) {
                        return edge.first == txn_id || edge.second == txn_id;
                    }),
                deps.end());
        }
    }

    // 辅助函数：从表名获取文件描述符
    int fd_from_tab_name(const std::string& tab_name);

    // 全局提交顺序计数器
    std::atomic<int64_t> global_commit_order_{0};
    // 全局互斥锁
    std::mutex mvcc_mutex_;
    // SSI 互斥锁
    std::mutex ssi_mutex_;
    std::vector<std::pair<txn_id_t, txn_id_t>> rw_edges_;

    struct PerfWriteLockKey {
        int fd;
        int page_no;
        int slot_no;

        bool operator==(const PerfWriteLockKey& other) const {
            return fd == other.fd && page_no == other.page_no && slot_no == other.slot_no;
        }
    };

    struct PerfWriteLockKeyHash {
        size_t operator()(const PerfWriteLockKey& key) const {
            size_t h1 = std::hash<int>()(key.fd);
            size_t h2 = std::hash<int>()(key.page_no);
            size_t h3 = std::hash<int>()(key.slot_no);
            return h1 ^ (h2 << 16) ^ (h3 << 32);
        }
    };

    std::mutex perf_write_lock_mutex_;
    std::condition_variable perf_write_lock_cv_;
    std::unordered_map<PerfWriteLockKey, txn_id_t, PerfWriteLockKeyHash> perf_write_locks_;
    std::unordered_map<txn_id_t, std::vector<PerfWriteLockKey>> txn_perf_write_locks_;
    inline static TransactionManager* current_instance_{nullptr};

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
    void retire_transaction(Transaction *txn);
    timestamp_t oldest_active_start_ts();

    ConcurrencyMode concurrency_mode_;      // 事务使用的并发控制算法，目前只需要考虑2PL
    std::atomic<txn_id_t> next_txn_id_{0};  // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{0};    // 用于分发事务时间戳
    std::atomic<int64_t> next_commit_order_{0};  // 用于分发提交顺序
    std::mutex latch_;  // 用于txn_map的并发
    std::mutex explicit_txn_mutex_;
    SmManager *sm_manager_;
    LockManager *lock_manager_;
    std::mutex session_mutex_;  // 用于会话隔离级别映射的并发
    std::unordered_map<int, IsolationLevel> session_isolation_levels_;  // 会话隔离级别映射

    std::atomic<timestamp_t> last_commit_ts_{0};    // 最后提交的时间戳,仅用于MVCC
    Watermark running_txns_{0};             // 存储所有正在运行事务的读取时间戳，以便于垃圾回收，仅用于MVCC

    bool write_invisible_to_reader_locked(Transaction* reader, Transaction* writer) {
        if (reader == nullptr || writer == nullptr || reader == writer) return false;
        if (writer->get_state() == TransactionState::ABORTED) return false;
        if (!transactions_overlap_locked(reader, writer)) return false;
        if (writer->get_state() == TransactionState::GROWING) return true;
        if (writer->get_commit_ts() == INVALID_TS) return true;
        return writer->get_commit_ts() > reader->get_start_ts();
    }

    bool transactions_overlap_locked(Transaction* lhs, Transaction* rhs) {
        if (lhs == nullptr || rhs == nullptr) return false;
        if (lhs->get_state() == TransactionState::ABORTED ||
            rhs->get_state() == TransactionState::ABORTED) {
            return false;
        }
        timestamp_t lhs_end = lhs->get_state() == TransactionState::COMMITTED
                                  ? lhs->get_commit_ts()
                                  : std::numeric_limits<timestamp_t>::max();
        timestamp_t rhs_end = rhs->get_state() == TransactionState::COMMITTED
                                  ? rhs->get_commit_ts()
                                  : std::numeric_limits<timestamp_t>::max();
        return lhs->get_start_ts() < rhs_end && rhs->get_start_ts() < lhs_end;
    }

    bool record_matches_predicate_locked(const RmRecord* record, const PredicateRead& pred) {
        if (record == nullptr) return false;
        for (auto& cond : pred.conds) {
            const ColMeta* lhs = find_col_locked(pred.cols, cond.lhs_col);
            if (lhs == nullptr) continue;
            const char* lhs_buf = record->data + lhs->offset;
            const char* rhs_buf = nullptr;
            ColType rhs_type = lhs->type;
            int rhs_len = lhs->len;
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) continue;
                rhs_buf = cond.rhs_val.raw->data;
            } else {
                const ColMeta* rhs = find_col_locked(pred.cols, cond.rhs_col);
                if (rhs == nullptr) continue;
                rhs_buf = record->data + rhs->offset;
                rhs_type = rhs->type;
                rhs_len = rhs->len;
            }
            (void)rhs_type;
            (void)rhs_len;
            int cmp = compare_value_locked(lhs_buf, rhs_buf, lhs->type, lhs->len);
            if (!eval_cmp_locked(cmp, cond.op)) return false;
        }
        return true;
    }

    const ColMeta* find_col_locked(const std::vector<ColMeta>& cols, const TabCol& target) {
        for (auto& col : cols) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                return &col;
            }
        }
        return nullptr;
    }

    int compare_value_locked(const char *a, const char *b, ColType type, int len) {
        if (type == TYPE_INT) {
            int va = *(int *)a;
            int vb = *(int *)b;
            return (va > vb) ? 1 : ((va < vb) ? -1 : 0);
        }
        if (type == TYPE_FLOAT) {
            float va = *(float *)a;
            float vb = *(float *)b;
            return (va > vb) ? 1 : ((va < vb) ? -1 : 0);
        }
        if (type == TYPE_STRING) {
            return memcmp(a, b, len);
        }
        return 0;
    }

    bool eval_cmp_locked(int cmp, CompOp op) {
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
