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
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include "common/config.h"
#include "defs.h"
#include "record/rm_defs.h"

class Transaction;

struct VersionEntry {
    txn_id_t txn_id_{INVALID_TXN_ID};
    timestamp_t commit_ts_{0};
    bool is_deleted_{false};
    bool new_deleted_{false};
    std::shared_ptr<RmRecord> old_data_;
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

    bool check_write_conflict(int fd, const Rid& rid, Transaction* txn) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& bucket = get_bucket(key);
        std::lock_guard<std::shared_mutex> lock(bucket.mutex_);

        auto it = bucket.chains_.find(key);
        if (it == bucket.chains_.end() || it->second.empty()) {
            return true;
        }

        auto& chain = it->second;
        auto& latest = chain.back();

        if (latest.txn_id_ == txn->get_transaction_id()) {
            return true;
        }

        if (latest.commit_ts_ == 0) {
            return false;
        }

        if (latest.commit_ts_ > txn->get_start_ts()) {
            return false;
        }

        return true;
    }

    void save_old_data(int fd, const Rid& rid, Transaction* txn,
                       const RmRecord* old_data, bool was_deleted,
                       bool new_deleted = false) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& bucket = get_bucket(key);
        std::lock_guard<std::shared_mutex> lock(bucket.mutex_);

        auto& chain = bucket.chains_[key];

        VersionEntry entry;
        entry.txn_id_ = txn->get_transaction_id();
        entry.commit_ts_ = 0;
        entry.is_deleted_ = was_deleted;
        entry.new_deleted_ = new_deleted;
        if (old_data && !was_deleted) {
            entry.old_data_ = std::make_shared<RmRecord>(*old_data);
        }

        chain.push_back(std::move(entry));

        // Track which keys this transaction touched
        txn_keys_[txn->get_transaction_id()].push_back(key);
    }

    // Optimized commit: only iterate keys touched by this transaction
    void commit_transaction(txn_id_t txn_id, timestamp_t commit_ts) {
        std::vector<VersionKey> keys;
        {
            std::lock_guard<std::mutex> lock(txn_keys_mutex_);
            auto it = txn_keys_.find(txn_id);
            if (it == txn_keys_.end()) return;
            keys = std::move(it->second);
            txn_keys_.erase(it);
        }

        for (auto& key : keys) {
            auto& bucket = get_bucket(key);
            std::lock_guard<std::shared_mutex> lock(bucket.mutex_);
            auto chain_it = bucket.chains_.find(key);
            if (chain_it == bucket.chains_.end()) continue;
            for (auto& entry : chain_it->second) {
                if (entry.txn_id_ == txn_id && entry.commit_ts_ == 0) {
                    entry.commit_ts_ = commit_ts;
                }
            }
        }
    }

    // Optimized abort: only iterate keys touched by this transaction
    std::vector<std::tuple<int, Rid, std::shared_ptr<RmRecord>, bool>> abort_transaction(txn_id_t txn_id) {
        std::vector<std::tuple<int, Rid, std::shared_ptr<RmRecord>, bool>> to_restore;
        std::vector<VersionKey> keys;
        {
            std::lock_guard<std::mutex> lock(txn_keys_mutex_);
            auto it = txn_keys_.find(txn_id);
            if (it == txn_keys_.end()) return to_restore;
            keys = std::move(it->second);
            txn_keys_.erase(it);
        }

        for (auto& key : keys) {
            auto& bucket = get_bucket(key);
            std::lock_guard<std::shared_mutex> lock(bucket.mutex_);
            auto chain_it = bucket.chains_.find(key);
            if (chain_it == bucket.chains_.end()) continue;

            auto& chain = chain_it->second;
            for (auto& entry : chain) {
                if (entry.txn_id_ == txn_id) {
                    to_restore.push_back({key.fd,
                                          Rid{key.page_no, key.slot_no},
                                          entry.old_data_,
                                          entry.is_deleted_});
                }
            }

            chain.erase(
                std::remove_if(chain.begin(), chain.end(),
                    [txn_id](const VersionEntry& entry) {
                        return entry.txn_id_ == txn_id;
                    }),
                chain.end()
            );

            if (chain.empty()) {
                bucket.chains_.erase(chain_it);
            }
        }

        return to_restore;
    }

    int get_visible_data(int fd, const Rid& rid, Transaction* txn, RmRecord*& result, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& bucket = get_bucket(key);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex_);

        auto it = bucket.chains_.find(key);
        if (it == bucket.chains_.end() || it->second.empty()) {
            is_deleted = false;
            result = nullptr;
            return -1;
        }

        auto& chain = it->second;

        bool has_invisible_before_image = false;
        bool invisible_before_deleted = false;
        RmRecord* invisible_before_data = nullptr;

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
        return -1;
    }

    int get_read_committed_data(int fd, const Rid& rid, RmRecord*& result, bool& is_deleted) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& bucket = get_bucket(key);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex_);

        auto it = bucket.chains_.find(key);
        if (it == bucket.chains_.end() || it->second.empty()) {
            is_deleted = false;
            result = nullptr;
            return -1;
        }

        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
            auto& entry = *rit;

            if (entry.commit_ts_ == 0) {
                if (entry.is_deleted_) {
                    is_deleted = true;
                    result = nullptr;
                    return 0;
                }
                is_deleted = false;
                result = entry.old_data_.get();
                return 1;
            }

            if (entry.new_deleted_) {
                is_deleted = true;
                result = nullptr;
                return 0;
            }

            is_deleted = false;
            result = nullptr;
            return -1;
        }

        is_deleted = false;
        result = nullptr;
        return -1;
    }

    bool latest_is_deleted_for_txn(int fd, const Rid& rid, txn_id_t txn_id) {
        VersionKey key{fd, rid.page_no, rid.slot_no};
        auto& bucket = get_bucket(key);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex_);

        auto it = bucket.chains_.find(key);
        if (it == bucket.chains_.end() || it->second.empty()) {
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

    static constexpr size_t NUM_SHARDS = 64;

    struct Shard {
        std::shared_mutex mutex_;
        std::unordered_map<VersionKey, std::vector<VersionEntry>, VersionKeyHash> chains_;
    };

    Shard shards_[NUM_SHARDS];

    // Per-transaction tracking of version keys for fast commit/abort
    std::mutex txn_keys_mutex_;
    std::unordered_map<txn_id_t, std::vector<VersionKey>> txn_keys_;

    Shard& get_bucket(const VersionKey& key) {
        size_t hash = VersionKeyHash{}(key);
        return shards_[hash % NUM_SHARDS];
    }
};
