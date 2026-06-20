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
 * @brief 版本条目 - 存储每次写操作的信息
 *
 * 版本链存储的是每个事务对该记录的写操作结果。
 * 磁盘上存储的是原始数据（在任何MVCC事务之前的状态）。
 */
struct VersionEntry {
    txn_id_t txn_id_{INVALID_TXN_ID};  // 创建此版本的事务ID
    timestamp_t commit_ts_{0};          // 提交时间戳（0表示未提交）
    bool is_deleted_{false};            // 是否为删除标记
    std::shared_ptr<RmRecord> data_;    // 此版本的数据（写入后的数据）
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
     * 1. 如果记录的最新版本是另一个未提交事务创建的，冲突
     * 2. 如果记录的最新已提交版本的commit_ts > 当前事务的start_ts，冲突
     */
    bool check_write_conflict(int fd, const Rid& rid, Transaction* txn) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end() || it->second.empty()) {
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

    /**
     * @brief 记录写操作
     * @param new_data 写入后的数据（对于delete为nullptr）
     * @param is_delete 是否是删除操作
     */
    void record_write(int fd, const Rid& rid, Transaction* txn,
                      const RmRecord* new_data, bool is_delete) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto& chain = version_chains_[key];

        VersionEntry entry;
        entry.txn_id_ = txn->get_transaction_id();
        entry.commit_ts_ = 0;  // 未提交
        entry.is_deleted_ = is_delete;
        if (new_data && !is_delete) {
            entry.data_ = std::make_shared<RmRecord>(*new_data);
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
            chain.erase(
                std::remove_if(chain.begin(), chain.end(),
                    [txn_id](const VersionEntry& entry) {
                        return entry.txn_id_ == txn_id;
                    }),
                chain.end()
            );
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
     * @param disk_data 磁盘上的原始数据
     * @param[out] found 是否找到了可见版本
     * @param[out] is_deleted 如果找到的版本是删除标记
     * @return 可见版本的数据，如果使用磁盘数据返回nullptr
     */
    RmRecord* get_visible_version(int fd, const Rid& rid, Transaction* txn,
                                  bool& found, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end() || it->second.empty()) {
            found = false;
            is_deleted = false;
            return nullptr;  // 没有版本信息，使用磁盘数据
        }

        auto& chain = it->second;

        // 从最新版本开始查找可见版本
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            auto& entry = *rit;

            // 自己的写操作总是可见
            if (entry.txn_id_ == txn->get_transaction_id()) {
                found = true;
                is_deleted = entry.is_deleted_;
                return entry.data_.get();
            }

            // 跳过其他事务的未提交版本
            if (entry.commit_ts_ == 0) {
                continue;
            }

            // 已提交版本：检查时间戳
            if (entry.commit_ts_ <= txn->get_start_ts()) {
                found = true;
                is_deleted = entry.is_deleted_;
                return entry.data_.get();
            }

            // commit_ts > start_ts，不可见，继续查找更旧的版本
        }

        // 没有找到可见版本
        found = false;
        is_deleted = false;
        return nullptr;
    }

    /**
     * @brief 检查记录是否有任何已提交的版本
     */
    bool has_committed_version(int fd, const Rid& rid) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end() || it->second.empty()) {
            return false;
        }

        for (auto& entry : it->second) {
            if (entry.commit_ts_ > 0) {
                return true;
            }
        }
        return false;
    }

private:
    VersionManager() = default;
    std::mutex mutex_;
    std::unordered_map<VersionKey, std::vector<VersionEntry>, VersionKeyHash> version_chains_;
};
