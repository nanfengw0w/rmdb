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
     * @brief 记录写操作前的旧数据
     * @param old_data 写操作前的数据
     * @param was_deleted 写操作前记录是否已删除
     */
    void save_old_data(int fd, const Rid& rid, Transaction* txn,
                       const RmRecord* old_data, bool was_deleted,
                       bool new_deleted = false) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto& chain = version_chains_[key];

        VersionEntry entry;
        entry.txn_id_ = txn->get_transaction_id();
        entry.commit_ts_ = 0;  // 未提交
        entry.is_deleted_ = was_deleted;
        entry.new_deleted_ = new_deleted;
        if (old_data && !was_deleted) {
            entry.old_data_ = std::make_shared<RmRecord>(*old_data);
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
     * @return 需要恢复的记录列表 (fd, rid, old_data, is_deleted)
     */
    std::vector<std::tuple<int, Rid, std::shared_ptr<RmRecord>, bool>> abort_transaction(txn_id_t txn_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::tuple<int, Rid, std::shared_ptr<RmRecord>, bool>> to_restore;

        for (auto it = version_chains_.begin(); it != version_chains_.end(); ) {
            auto& chain = it->second;

            // 找到该事务的版本条目，恢复旧数据
            for (auto& entry : chain) {
                if (entry.txn_id_ == txn_id) {
                    to_restore.push_back({it->first.fd,
                                          Rid{it->first.page_no, it->first.slot_no},
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
                it = version_chains_.erase(it);
            } else {
                ++it;
            }
        }

        return to_restore;
    }

    /**
     * @brief 检查记录是否对事务可见
     *
     * @return -1: 从磁盘读取, 0: 记录不存在, 1: 从old_data读取
     */
    int get_visible_data(int fd, const Rid& rid, Transaction* txn, RmRecord*& result, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end() || it->second.empty()) {
            is_deleted = false;
            result = nullptr;
            return -1;  // 没有版本信息，从磁盘读取
        }

        auto& chain = it->second;

        bool has_invisible_before_image = false;
        bool invisible_before_deleted = false;
        RmRecord* invisible_before_data = nullptr;

        // 从最新版本开始查找。磁盘保存最新物理值，版本链保存每次写入前的旧值；
        // 如果快照看不到多个较新的写入，需要一直回退到最早的不可见写入前。
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            auto& entry = *rit;

            if (entry.txn_id_ == txn->get_transaction_id()) {
                if (entry.new_deleted_) {
                    is_deleted = true;
                    result = nullptr;
                    return 0;
                }
                is_deleted = false;
                result = nullptr;
                return -1;
            }

            bool invisible = (entry.commit_ts_ == 0 || entry.commit_ts_ > txn->get_start_ts());
            if (invisible) {
                has_invisible_before_image = true;
                invisible_before_deleted = entry.is_deleted_;
                invisible_before_data = entry.old_data_.get();
                continue;
            }

            // commit_ts <= start_ts，这个写入在快照中可见。若它是删除，
            // 且没有更晚的不可见写入需要回退，则该记录对本事务不存在。
            if (!has_invisible_before_image && entry.new_deleted_) {
                is_deleted = true;
                result = nullptr;
                return 0;
            }
            break;
        }

        if (has_invisible_before_image) {
            if (invisible_before_deleted) {
                is_deleted = true;
                result = nullptr;
                return 0;
            }
            is_deleted = false;
            result = invisible_before_data;
            return 1;
        }

        is_deleted = false;
        result = nullptr;
        return -1;  // 从磁盘读取
    }

    bool latest_is_deleted_for_txn(int fd, const Rid& rid, txn_id_t txn_id) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end() || it->second.empty()) {
            return false;
        }
        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
            if (rit->txn_id_ == txn_id) {
                return rit->new_deleted_;
            }
        }
        return false;
    }

private:
    VersionManager() = default;
    std::mutex mutex_;
    std::unordered_map<VersionKey, std::vector<VersionEntry>, VersionKeyHash> version_chains_;
};
