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

#include "errors.h"
#include "index/ix.h"
#include "system/sm.h"

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

inline void check_unique_conflict(SmManager *sm_manager, const std::string &tab_name, const IndexMeta &index,
                                  const char *key, std::optional<Rid> self = std::nullopt) {
    auto ih = get_index_handle(sm_manager, tab_name, index);
    std::vector<Rid> existing;
    if (!ih->get_value(key, &existing, nullptr)) {
        return;
    }
    for (const auto &rid : existing) {
        if (!self.has_value() || !same_rid(rid, *self)) {
            throw UniqueIndexConflictError(tab_name, index_col_names(index));
        }
    }
}

}  // namespace index_maintenance
