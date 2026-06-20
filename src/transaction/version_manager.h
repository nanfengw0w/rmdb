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

// 前向声明
class Transaction;

/**
 * @brief 版本条目 - 存储每个记录版本的元数据
 */
struct VersionEntry {
    txn_id_t txn_id_{INVALID_TXN_ID};  // 创建此版本的事务ID
    timestamp_t commit_ts_{0};          // 提交时间戳（0表示未提交）
    bool is_deleted_{false};            // 是否为删除标记
    std::shared_ptr<RmRecord> data_;    // 此版本的数据（旧版本）
};

/**
 * @brief 版本管理器 - 管理所有记录的版本链
 *
 * 核心设计：
 * 1. 版本链存储的是旧版本数据
 * 2. 磁盘上存储的是最新数据
 * 3. 写操作时，将旧数据保存到版本链，然后写入新数据
 * 4. 读操作时，先检查版本链找到可见版本，如果没有则读磁盘
 */
class VersionManager {
public:
    static VersionManager& instance() {
        static VersionManager instance;
        return instance;
    }

    // 版本链的key
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
     * 1. 如果记录被另一个未提交事务修改，返回 false（写写冲突）
     * 2. 如果记录在本事务开始后被其他事务提交修改，返回 false（first-updater-wins）
     */
    bool check_write_conflict(int fd, const Rid& rid, Transaction* txn) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end()) {
            return true;  // 没有版本信息，可以写
        }

        auto& chain = it->second;
        if (chain.empty()) {
            return true;
        }

        // 检查最新版本
        auto& latest = chain.back();

        // 如果最新版本是自己创建的，可以写
        if (latest.txn_id_ == txn->get_transaction_id()) {
            return true;
        }

        // 如果最新版本未提交，写写冲突
        if (latest.commit_ts_ == 0) {
            return false;
        }

        // 如果最新版本在本事务开始后提交，写写冲突（first-updater-wins）
        if (latest.commit_ts_ > txn->get_start_ts()) {
            return false;
        }

        return true;
    }

    /**
     * @brief 保存旧版本（在写操作前调用）
     * @param old_data 当前磁盘上的数据
     */
    void save_old_version(int fd, const Rid& rid, Transaction* txn,
                          const RmRecord* old_data, bool was_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto& chain = version_chains_[key];

        // 保存旧版本
        VersionEntry entry;
        entry.txn_id_ = txn->get_transaction_id();
        entry.commit_ts_ = 0;  // 未提交
        entry.is_deleted_ = was_deleted;
        if (old_data && !was_deleted) {
            entry.data_ = std::make_shared<RmRecord>(*old_data);
        }

        chain.push_back(std::move(entry));
    }

    /**
     * @brief 提交事务的所有写操作
     */
    void commit_transaction(txn_id_t txn_id, timestamp_t commit_ts) {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& [key, chain] : version_chains_) {
            for (auto& entry : chain) {
                if (entry.txn_id_ == txn_id && entry.commit_ts_ == 0) {
                    entry.commit_ts_ = commit_ts;
                }
            }
        }
    }

    /**
     * @brief 回滚事务的所有写操作
     */
    void abort_transaction(txn_id_t txn_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto it = version_chains_.begin(); it != version_chains_.end(); ) {
            auto& chain = it->second;
            // 移除该事务的所有版本
            chain.erase(
                std::remove_if(chain.begin(), chain.end(),
                    [txn_id](const VersionEntry& entry) {
                        return entry.txn_id_ == txn_id;
                    }),
                chain.end()
            );
            // 如果版本链为空，移除整个条目
            if (chain.empty()) {
                it = version_chains_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief 获取记录的可见版本数据
     *
     * @param fd 文件描述符
     * @param rid 记录ID
     * @param txn 当前事务
     * @param disk_data 磁盘上的当前数据
     * @param[out] is_deleted 如果记录被删除，设置为 true
     * @return 可见版本的数据，如果使用磁盘数据返回 nullptr
     */
    RmRecord* get_visible_version(int fd, const Rid& rid, Transaction* txn,
                                  const RmRecord* disk_data, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end()) {
            // 没有版本信息，使用磁盘数据
            is_deleted = false;
            return nullptr;
        }

        auto& chain = it->second;

        // 从最新版本开始查找
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            auto& entry = *rit;

            // 自己写的数据总是可见
            if (entry.txn_id_ == txn->get_transaction_id()) {
                is_deleted = entry.is_deleted_;
                return entry.data_.get();
            }

            // 未提交的数据不可见
            if (entry.commit_ts_ == 0) {
                continue;
            }

            // 已提交的数据，检查时间戳
            if (entry.commit_ts_ <= txn->get_start_ts()) {
                is_deleted = entry.is_deleted_;
                return entry.data_.get();
            }
        }

        // 没有找到可见版本，使用磁盘数据
        is_deleted = false;
        return nullptr;
    }

    /**
     * @brief 检查是否有未提交的版本（用于判断记录是否被锁定）
     */
    bool has_uncommitted_version(int fd, const Rid& rid, txn_id_t exclude_txn) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end()) {
            return false;
        }

        auto& chain = it->second;
        if (chain.empty()) {
            return false;
        }

        auto& latest = chain.back();
        return latest.txn_id_ != exclude_txn && latest.commit_ts_ == 0;
    }

private:
    VersionManager() = default;

    std::mutex mutex_;
    // 版本链：每个记录有一个版本列表，按时间从旧到新排列
    std::unordered_map<VersionKey, std::vector<VersionEntry>, VersionKeyHash> version_chains_;
};
