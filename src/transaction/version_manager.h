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
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include "common/config.h"
#include "defs.h"
#include "record/rm_defs.h"

class Transaction;

/**
 * @brief 版本条目 - 保存写操作前的旧数据
 *
 * 当事务修改记录时：
 * 1. 将旧数据保存到版本链
 * 2. 将新数据写入磁盘
 *
 * 当其他事务读取时：
 * 1. 检查是否有未提交的版本（其他事务的写操作）
 * 2. 如果有，从版本链读取旧数据（避免脏读）
 * 3. 如果没有，从磁盘读取（最新数据）
 */
struct VersionEntry {
    txn_id_t txn_id_{INVALID_TXN_ID};  // 执行此写操作的事务ID
    timestamp_t commit_ts_{0};          // 提交时间戳（0表示未提交）
    bool is_deleted_{false};            // 写操作前该记录是否已删除
    bool new_deleted_{false};           // 写操作后该记录是否被删除
    std::shared_ptr<RmRecord> old_data_;// 写操作前的旧数据
};

class VersionManager {
public:
    static VersionManager& instance() {
        static VersionManager instance;
        return instance;
    }

    struct VersionKey {
        int fd;
        int page_no;
        int slot_no;
        bool operator==(const VersionKey& other) const {
            return fd == other.fd && page_no == other.page_no && slot_no == other.slot_no;
        }
    };

    struct VersionKeyHash {
        size_t operator()(const VersionKey& k) const {
            size_t h1 = std::hash<int>()(k.fd);
            size_t h2 = std::hash<int>()(k.page_no);
            size_t h3 = std::hash<int>()(k.slot_no);
            return h1 ^ (h2 << 16) ^ (h3 << 32);
        }
    };

    /**
     * @brief 检查写操作是否可以执行（写写冲突检测）
     *
     * 规则：
     * 1. 如果记录有另一个未提交事务的版本，冲突
     * 2. 如果记录的最新已提交版本在本事务开始之后提交，冲突
     */
    bool check_write_conflict(int fd, const Rid& rid, Transaction* txn) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);

        auto it = shard.version_chains.find(key);
        if (it == shard.version_chains.end() || it->second.empty()) {
            return true;  // 没有版本信息，可以写
        }

        auto& chain = it->second;
        auto& latest = chain.back();

        // 如果最新版本是自己创建的，可以写（重复写）
        if (latest.txn_id_ == txn->get_transaction_id()) {
            return true;
        }

        // 如果最新版本未提交（由其他事务创建），冲突
        if (latest.commit_ts_ == 0) {
            return false;
        }

        // 如果最新已提交版本在本事务开始之后提交，冲突
        if (latest.commit_ts_ > txn->get_start_ts()) {
            return false;
        }

        return true;
    }

    bool save_old_data_if_no_conflict(int fd, const Rid& rid, Transaction* txn,
                                       const RmRecord* old_data, bool was_deleted,
                                       bool new_deleted = false) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);

        auto& chain = shard.version_chains[key];
        if (!chain.empty()) {
            auto& latest = chain.back();
            if (latest.txn_id_ != txn->get_transaction_id()) {
                if (latest.commit_ts_ == 0 || latest.commit_ts_ > txn->get_start_ts()) {
                    return false;
                }
            }
        }

        VersionEntry entry;
        entry.txn_id_ = txn->get_transaction_id();
        entry.commit_ts_ = 0;
        entry.is_deleted_ = was_deleted;
        entry.new_deleted_ = new_deleted;
        if (old_data && !was_deleted) {
            entry.old_data_ = std::make_shared<RmRecord>(*old_data);
        }

        chain.push_back(std::move(entry));
        {
            std::lock_guard<std::mutex> txn_lock(txn_mutex_);
            txn_versions_[txn->get_transaction_id()].push_back(key);
        }
        return true;
    }

    /**
     * @brief 记录写操作前的旧数据
     * @param old_data 写操作前的数据
     * @param was_deleted 写操作前记录是否已删除
     */
    void save_old_data(int fd, const Rid& rid, Transaction* txn,
                        const RmRecord* old_data, bool was_deleted,
                        bool new_deleted = false) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);

        auto& chain = shard.version_chains[key];

        VersionEntry entry;
        entry.txn_id_ = txn->get_transaction_id();
        entry.commit_ts_ = 0;  // 未提交
        entry.is_deleted_ = was_deleted;
        entry.new_deleted_ = new_deleted;
        if (old_data && !was_deleted) {
            entry.old_data_ = std::make_shared<RmRecord>(*old_data);
        }

        chain.push_back(std::move(entry));
        {
            std::lock_guard<std::mutex> txn_lock(txn_mutex_);
            txn_versions_[txn->get_transaction_id()].push_back(key);
        }
    }

    /**
     * @brief 提交事务的所有写操作
     */
    void commit_transaction(txn_id_t txn_id, timestamp_t commit_ts) {
        std::vector<VersionKey> keys;
        {
            std::lock_guard<std::mutex> txn_lock(txn_mutex_);
            auto txn_it = txn_versions_.find(txn_id);
            if (txn_it == txn_versions_.end()) {
                return;
            }
            keys = std::move(txn_it->second);
            txn_versions_.erase(txn_it);
        }

        // 按分片分组处理
        std::vector<std::vector<VersionKey>> shard_keys(NUM_SHARDS);
        for (const auto& key : keys) {
            shard_keys[get_shard_index(key)].push_back(key);
        }

        for (int i = 0; i < NUM_SHARDS; ++i) {
            if (shard_keys[i].empty()) continue;
            std::lock_guard<std::mutex> lock(shards_[i].mutex);
            for (const auto& key : shard_keys[i]) {
                auto chain_it = shards_[i].version_chains.find(key);
                if (chain_it == shards_[i].version_chains.end()) {
                    continue;
                }
                auto& chain = chain_it->second;
                for (auto& entry : chain) {
                    if (entry.txn_id_ == txn_id && entry.commit_ts_ == 0) {
                        entry.commit_ts_ = commit_ts;
                    }
                }
            }
        }
    }

    /**
     * @brief 回滚事务的所有写操作
     * @return 需要恢复的记录列表 (fd, rid, old_data, is_deleted)
     */
    std::vector<std::tuple<int, Rid, std::shared_ptr<RmRecord>, bool>> abort_transaction(txn_id_t txn_id) {
        std::vector<VersionKey> keys;
        {
            std::lock_guard<std::mutex> txn_lock(txn_mutex_);
            auto txn_it = txn_versions_.find(txn_id);
            if (txn_it == txn_versions_.end()) {
                return {};
            }
            keys = std::move(txn_it->second);
            txn_versions_.erase(txn_it);
        }

        std::vector<std::tuple<int, Rid, std::shared_ptr<RmRecord>, bool>> to_restore;

        // 按分片分组处理
        std::vector<std::vector<VersionKey>> shard_keys(NUM_SHARDS);
        for (const auto& key : keys) {
            shard_keys[get_shard_index(key)].push_back(key);
        }

        for (int i = 0; i < NUM_SHARDS; ++i) {
            if (shard_keys[i].empty()) continue;
            std::lock_guard<std::mutex> lock(shards_[i].mutex);
            for (const auto& key : shard_keys[i]) {
                auto it = shards_[i].version_chains.find(key);
                if (it == shards_[i].version_chains.end()) {
                    continue;
                }
                auto& chain = it->second;

                // 找到该事务的版本条目，恢复旧数据
                for (auto& entry : chain) {
                    if (entry.txn_id_ == txn_id) {
                        to_restore.push_back({key.fd,
                                              Rid{key.page_no, key.slot_no},
                                              entry.old_data_,
                                              entry.is_deleted_});
                    }
                }

                // 移除该事务的版本条目
                chain.erase(
                    std::remove_if(chain.begin(), chain.end(),
                        [txn_id](const VersionEntry& entry) {
                            return entry.txn_id_ == txn_id;
                        }),
                    chain.end()
                );

                if (chain.empty()) {
                    shards_[i].version_chains.erase(it);
                }
            }
        }

        return to_restore;
    }

    /**
     * @brief 检查记录是否对事务可见
     *
     * @return -1: 从磁盘读取, 0: 记录不存在, 1: 从old_data读取
     */
    int get_visible_data(int fd, const Rid& rid, Transaction* txn,
                          std::unique_ptr<RmRecord>& result, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        result.reset();

        auto it = shard.version_chains.find(key);
        if (it == shard.version_chains.end() || it->second.empty()) {
            is_deleted = false;
            return -1;  // 没有版本信息，从磁盘读取
        }

        auto& chain = it->second;

        const VersionEntry* newest_visible = nullptr;
        bool has_invisible_after_newest = false;
        bool invisible_after_deleted = false;
        std::shared_ptr<RmRecord> invisible_after_data;

        // 从最新版本开始查找。磁盘保存最新物理值，版本链保存每次写入前的旧值；
        // 如果快照看不到较新的写入，需要回退到这些写入之前的旧值。
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            auto& entry = *rit;

            if (entry.txn_id_ == txn->get_transaction_id()) {
                if (entry.new_deleted_) {
                    is_deleted = true;
                    return 0;
                }
                is_deleted = false;
                return -1;
            }

            bool invisible = (entry.commit_ts_ == 0 || entry.commit_ts_ > txn->get_start_ts());
            if (invisible) {
                if (newest_visible == nullptr) {
                    has_invisible_after_newest = true;
                    invisible_after_deleted = entry.is_deleted_;
                    invisible_after_data = entry.old_data_;
                }
            } else if (newest_visible == nullptr) {
                newest_visible = &entry;
            }
        }

        if (has_invisible_after_newest) {
            if (invisible_after_deleted) {
                is_deleted = true;
                return 0;
            }
            is_deleted = false;
            if (invisible_after_data == nullptr) {
                return 0;
            }
            result = std::make_unique<RmRecord>(*invisible_after_data);
            return 1;
        }

        if (newest_visible != nullptr && newest_visible->new_deleted_) {
            is_deleted = true;
            return 0;
        }

        is_deleted = false;
        return -1;  // 从磁盘读取
    }

    bool latest_is_deleted_for_txn(int fd, const Rid& rid, txn_id_t txn_id) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);

        auto it = shard.version_chains.find(key);
        if (it == shard.version_chains.end() || it->second.empty()) {
            return false;
        }
        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
            if (rit->txn_id_ == txn_id) {
                return rit->new_deleted_;
            }
        }
        return false;
    }

    int get_read_committed_data(int fd, const Rid& rid,
                                std::unique_ptr<RmRecord>& result, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& shard = get_shard(key);
        std::lock_guard<std::mutex> lock(shard.mutex);
        result.reset();

        auto it = shard.version_chains.find(key);
        if (it == shard.version_chains.end() || it->second.empty()) {
            is_deleted = false;
            return -1;
        }

        bool has_uncommitted_before_image = false;
        bool uncommitted_before_deleted = false;
        std::shared_ptr<RmRecord> uncommitted_before_data;

        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
            auto& entry = *rit;

            if (entry.commit_ts_ == 0) {
                has_uncommitted_before_image = true;
                uncommitted_before_deleted = entry.is_deleted_;
                uncommitted_before_data = entry.old_data_;
                continue;
            }

            if (!has_uncommitted_before_image && entry.new_deleted_) {
                is_deleted = true;
                return 0;
            }

            break;
        }

        if (has_uncommitted_before_image) {
            if (uncommitted_before_deleted) {
                is_deleted = true;
                return 0;
            }
            is_deleted = false;
            if (uncommitted_before_data == nullptr) {
                return 0;
            }
            result = std::make_unique<RmRecord>(*uncommitted_before_data);
            return 1;
        }

        is_deleted = false;
        return -1;
    }

    void clear_fd(int fd) {
        // 清理所有分片中的version_chains
        for (int i = 0; i < NUM_SHARDS; ++i) {
            std::lock_guard<std::mutex> lock(shards_[i].mutex);
            auto& chains = shards_[i].version_chains;
            for (auto it = chains.begin(); it != chains.end(); ) {
                if (it->first.fd == fd) {
                    it = chains.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 清理txn_versions_
        {
            std::lock_guard<std::mutex> txn_lock(txn_mutex_);
            for (auto txn_it = txn_versions_.begin(); txn_it != txn_versions_.end(); ) {
                auto& keys = txn_it->second;
                keys.erase(std::remove_if(keys.begin(), keys.end(),
                                          [fd](const VersionKey& key) { return key.fd == fd; }),
                           keys.end());
                if (keys.empty()) {
                    txn_it = txn_versions_.erase(txn_it);
                } else {
                    ++txn_it;
                }
            }
        }
    }

    void clear_all() {
        for (int i = 0; i < NUM_SHARDS; ++i) {
            std::lock_guard<std::mutex> lock(shards_[i].mutex);
            shards_[i].version_chains.clear();
        }
        {
            std::lock_guard<std::mutex> txn_lock(txn_mutex_);
            txn_versions_.clear();
        }
    }

private:
    VersionManager() = default;

    // 分片锁配置
    static constexpr int NUM_SHARDS = 64;
    static constexpr int SHARD_MASK = NUM_SHARDS - 1;

    struct Shard {
        std::mutex mutex;
        std::unordered_map<VersionKey, std::vector<VersionEntry>, VersionKeyHash> version_chains;
    };

    // 根据key计算分片索引
    static int get_shard_index(const VersionKey& key) {
        size_t hash = VersionKeyHash{}(key);
        return hash & SHARD_MASK;
    }

    // 获取分片引用
    Shard& get_shard(const VersionKey& key) {
        return shards_[get_shard_index(key)];
    }

    Shard& get_shard(int fd, const Rid& rid) {
        return get_shard(VersionKey{fd, rid.page_no, rid.slot_no});
    }

    // 分片数组
    Shard shards_[NUM_SHARDS];

    // txn_versions_ 使用单独的锁保护
    std::mutex txn_mutex_;
    std::unordered_map<txn_id_t, std::vector<VersionKey>> txn_versions_;
};
