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
 * @brief 版本信息 - 存储每个记录版本的元数据
 */
struct VersionEntry {
    txn_id_t txn_id_{INVALID_TXN_ID};  // 创建此版本的事务ID
    timestamp_t commit_ts_{0};          // 提交时间戳（0表示未提交）
    bool is_deleted_{false};            // 是否为删除标记
    std::shared_ptr<RmRecord> data_;    // 此版本的数据
};

/**
 * @brief 版本管理器 - 管理所有记录的版本链
 *
 * 核心设计：
 * 1. 每条记录有一个版本链，存储该记录的所有历史版本
 * 2. 写操作前检查写写冲突
 * 3. 读操作时沿版本链找到可见版本
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
     *
     * @param fd 文件描述符
     * @param rid 记录ID
     * @param txn 当前事务
     * @param old_data 输出参数：如果可以写，返回当前可见版本的数据（用于保存旧版本）
     * @return true 如果可以写，false 如果有冲突
     */
    bool check_write_conflict(int fd, const Rid& rid, Transaction* txn, RmRecord*& old_data) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto& chain = version_chains_[key];

        // 找到对当前事务可见的最新版本
        VersionEntry* visible_version = nullptr;
        for (auto& entry : chain) {
            // 自己写的数据总是可见
            if (entry.txn_id_ == txn->get_transaction_id()) {
                visible_version = &entry;
                break;
            }
            // 未提交的数据不可见
            if (entry.commit_ts_ == 0) {
                continue;
            }
            // 已提交的数据，检查时间戳
            if (entry.commit_ts_ <= txn->get_start_ts()) {
                visible_version = &entry;
                break;
            }
        }

        // 检查是否有其他未提交事务正在修改此记录
        if (!chain.empty()) {
            auto& latest = chain.front();
            if (latest.txn_id_ != txn->get_transaction_id() && latest.commit_ts_ == 0) {
                // 另一个事务正在修改此记录，写写冲突
                return false;
            }
        }

        // 检查是否有其他事务在本事务开始后提交了修改
        if (!chain.empty()) {
            auto& latest = chain.front();
            if (latest.txn_id_ != txn->get_transaction_id() &&
                latest.commit_ts_ > 0 &&
                latest.commit_ts_ > txn->get_start_ts()) {
                // 此记录已被其他事务在本事务开始后提交修改
                return false;
            }
        }

        // 返回可见版本的数据
        if (visible_version && !visible_version->is_deleted_) {
            old_data = visible_version->data_.get();
        } else {
            old_data = nullptr;
        }

        return true;
    }

    /**
     * @brief 记录写操作（在写操作成功后调用）
     */
    void record_write(int fd, const Rid& rid, Transaction* txn,
                      const RmRecord* new_data, bool is_delete) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto& chain = version_chains_[key];

        // 创建新版本
        VersionEntry new_entry;
        new_entry.txn_id_ = txn->get_transaction_id();
        new_entry.commit_ts_ = 0;  // 未提交
        new_entry.is_deleted_ = is_delete;
        if (new_data && !is_delete) {
            new_entry.data_ = std::make_shared<RmRecord>(*new_data);
        }

        // 插入到版本链头部
        chain.insert(chain.begin(), std::move(new_entry));
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
     * @param[out] is_deleted 如果可见版本是删除标记，设置为 true
     * @return 可见版本的数据，如果记录不存在返回 nullptr
     */
    RmRecord* get_visible_version(int fd, const Rid& rid, Transaction* txn, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end()) {
            is_deleted = false;
            return nullptr;  // 没有版本信息，使用原始数据
        }

        auto& chain = it->second;

        // 沿版本链找到可见版本
        for (auto& entry : chain) {
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

        is_deleted = false;
        return nullptr;  // 没有可见版本，使用原始数据
    }

    /**
     * @brief 检查记录是否有可见版本
     */
    bool has_visible_version(int fd, const Rid& rid, Transaction* txn) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = version_chains_.find(key);
        if (it == version_chains_.end()) {
            return false;  // 没有版本信息
        }

        auto& chain = it->second;

        for (auto& entry : chain) {
            if (entry.txn_id_ == txn->get_transaction_id()) {
                return true;
            }
            if (entry.commit_ts_ == 0) {
                continue;
            }
            if (entry.commit_ts_ <= txn->get_start_ts()) {
                return true;
            }
        }

        return false;
    }

private:
    VersionManager() = default;

    std::mutex mutex_;
    // 版本链：每个记录有一个版本列表，按时间从新到旧排列
    std::unordered_map<VersionKey, std::vector<VersionEntry>, VersionKeyHash> version_chains_;
};
