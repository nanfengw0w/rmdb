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

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "common/context.h"
#include "errors.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"
#include "transaction/version_manager.h"

namespace index_maintenance {

inline std::vector<std::string> index_col_names(const IndexMeta &index) {
    std::vector<std::string> col_names;
    col_names.reserve(index.cols.size());
    for (const auto &col : index.cols) {
        col_names.push_back(col.name);
    }
    return col_names;
}

inline std::vector<char> build_key(const IndexMeta &index, const char *record_data) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (const auto &col : index.cols) {
        memcpy(key.data() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

inline bool same_rid(const Rid &lhs, const Rid &rhs) {
    return lhs.page_no == rhs.page_no && lhs.slot_no == rhs.slot_no;
}

inline bool rid_less(const Rid &lhs, const Rid &rhs) {
    if (lhs.page_no != rhs.page_no) {
        return lhs.page_no < rhs.page_no;
    }
    return lhs.slot_no < rhs.slot_no;
}

inline IxIndexHandle *get_index_handle(SmManager *sm_manager, const std::string &tab_name, const IndexMeta &index) {
    auto ix_name = sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols);
    return sm_manager->ihs_.at(ix_name).get();
}

inline bool is_mvcc_txn(Context *context) {
    if (context == nullptr || context->txn_ == nullptr) {
        return false;
    }
    auto level = context->txn_->get_isolation_level();
    return level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE;
}

inline bool is_snapshot_txn(Context *context) {
    return context != nullptr && context->txn_ != nullptr &&
           context->txn_->get_isolation_level() == IsolationLevel::SNAPSHOT_ISOLATION;
}

inline bool is_perf_txn(Context *context) {
    return context != nullptr && context->txn_ != nullptr && context->txn_->get_perf_mode();
}

inline bool visible_record_matches_key(const IndexMeta &index, const RmRecord *record, const char *key) {
    if (record == nullptr) {
        return false;
    }
    auto actual_key = build_key(index, record->data);
    return memcmp(actual_key.data(), key, index.col_tot_len) == 0;
}

inline bool record_matches_col_key(const ColMeta &col, const RmRecord *record, const char *key) {
    return record != nullptr && memcmp(record->data + col.offset, key, col.len) == 0;
}

inline void check_logical_key_write_conflict(SmManager *sm_manager, const TabMeta &tab,
                                             const std::string &tab_name, const char *record_data,
                                             std::optional<Rid> self, Context *context) {
    if (!is_snapshot_txn(context) || tab.cols.empty()) {
        return;
    }
    if (is_perf_txn(context)) {
        return;
    }

    const auto &key_col = tab.cols.front();
    const char *key = record_data + key_col.offset;
    auto fh = sm_manager->get_table_fh(tab_name);
    auto &vm = VersionManager::instance();

    RmScan scan(fh);
    while (!scan.is_end()) {
        Rid rid = scan.rid();
        scan.next();

        if (self.has_value() && same_rid(rid, *self)) {
            continue;
        }

        auto physical = fh->get_record(rid, nullptr);
        if (physical == nullptr) {
            continue;
        }

        auto visible = fh->get_record(rid, context);
        if (!record_matches_col_key(key_col, physical.get(), key) &&
            !record_matches_col_key(key_col, visible.get(), key)) {
            continue;
        }

        if (!vm.check_write_conflict(fh->GetFd(), rid, context->txn_)) {
            throw TransactionAbortException(context->txn_->get_transaction_id(),
                AbortReason::DEADLOCK_PREVENTION);
        }
    }
}

inline void check_unique_conflict_by_scan(SmManager *sm_manager, const std::string &tab_name,
                                          const IndexMeta &index, const char *key,
                                          std::optional<Rid> self, Context *context) {
    auto fh = sm_manager->get_table_fh(tab_name);
    auto &vm = VersionManager::instance();
    RmScan scan(fh);
    while (!scan.is_end()) {
        Rid rid = scan.rid();
        scan.next();

        if (self.has_value() && same_rid(rid, *self)) {
            continue;
        }

        auto physical = fh->get_record(rid, nullptr);
        if (physical == nullptr) {
            continue;
        }

        auto visible = fh->get_record(rid, context);
        bool physical_matches = visible_record_matches_key(index, physical.get(), key);
        bool visible_matches = visible_record_matches_key(index, visible.get(), key);
        bool write_conflict = !vm.check_write_conflict(fh->GetFd(), rid, context->txn_);

        if (visible_matches) {
            if (write_conflict) {
                throw TransactionAbortException(context->txn_->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }
            throw UniqueIndexConflictError(tab_name, index_col_names(index));
        }

        if (physical_matches && write_conflict) {
            throw TransactionAbortException(context->txn_->get_transaction_id(),
                AbortReason::DEADLOCK_PREVENTION);
        }
    }
}

inline void check_unique_conflict(SmManager *sm_manager, const std::string &tab_name, const IndexMeta &index,
                                  const char *key, std::optional<Rid> self = std::nullopt,
                                  Context *context = nullptr) {
    if (is_snapshot_txn(context) && !is_perf_txn(context)) {
        check_unique_conflict_by_scan(sm_manager, tab_name, index, key, self, context);
        return;
    }

    auto ih = get_index_handle(sm_manager, tab_name, index);
    std::vector<Rid> existing;
    if (!ih->get_value(key, &existing, nullptr)) {
        return;
    }

    RmFileHandle *fh = nullptr;
    bool mvcc = is_mvcc_txn(context);
    if (mvcc) {
        fh = sm_manager->get_table_fh(tab_name);
    }

    for (const auto &rid : existing) {
        if (self.has_value() && same_rid(rid, *self)) {
            continue;
        }

        if (!mvcc) {
            throw UniqueIndexConflictError(tab_name, index_col_names(index));
        }

        auto &vm = VersionManager::instance();
        bool write_conflict = !vm.check_write_conflict(fh->GetFd(), rid, context->txn_);
        auto record = fh->get_record(rid, context);

        if (record == nullptr) {
            if (write_conflict) {
                throw TransactionAbortException(context->txn_->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }
            continue;
        }

        if (!visible_record_matches_key(index, record.get(), key)) {
            continue;
        }

        if (write_conflict) {
            throw TransactionAbortException(context->txn_->get_transaction_id(),
                AbortReason::DEADLOCK_PREVENTION);
        }
        throw UniqueIndexConflictError(tab_name, index_col_names(index));
    }
}

}  // namespace index_maintenance
