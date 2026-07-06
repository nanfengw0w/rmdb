/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include "execution/index_maintenance.h"
#include "optimizer/plan.h"
#include "system/sm.h"

namespace write_index_probe {

inline bool is_perf_txn(Context *context) {
    return context != nullptr && context->txn_ != nullptr && context->txn_->get_perf_mode();
}

inline int compare_value(const char *a, const char *b, ColType type, int len) {
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

inline bool eval_cmp(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        case OP_LT:
            return cmp < 0;
        case OP_GT:
            return cmp > 0;
        case OP_LE:
            return cmp <= 0;
        case OP_GE:
            return cmp >= 0;
        default:
            return false;
    }
}

inline ColMeta find_col_meta(const std::vector<ColMeta> &cols, const TabCol &target) {
    auto pos = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
    if (pos == cols.end()) {
        throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
    }
    return *pos;
}

inline bool normalize_condition_for_table(Condition &cond, const std::string &tab_name) {
    if (cond.lhs_col.tab_name == tab_name) {
        return true;
    }
    if (cond.is_rhs_val || cond.rhs_col.tab_name != tab_name) {
        return false;
    }
    static const std::map<CompOp, CompOp> swap_op = {
        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT},
        {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
    };
    std::swap(cond.lhs_col, cond.rhs_col);
    cond.op = swap_op.at(cond.op);
    return true;
}

inline bool eval_conds(const RmRecord *record, const std::vector<Condition> &conds,
                       const std::vector<ColMeta> &cols) {
    for (const auto &cond : conds) {
        auto lhs_col = find_col_meta(cols, cond.lhs_col);
        const char *lhs_buf = record->data + lhs_col.offset;

        if (cond.is_rhs_val) {
            int cmp = compare_value(lhs_buf, cond.rhs_val.raw->data, lhs_col.type, lhs_col.len);
            if (!eval_cmp(cmp, cond.op)) {
                return false;
            }
        } else {
            auto rhs_col = find_col_meta(cols, cond.rhs_col);
            const char *rhs_buf = record->data + rhs_col.offset;
            int cmp = compare_value(lhs_buf, rhs_buf, lhs_col.type, lhs_col.len);
            if (!eval_cmp(cmp, cond.op)) {
                return false;
            }
        }
    }
    return true;
}

inline bool update_changes_index_column(const DMLPlan &plan, const TabMeta &tab) {
    if (plan.tag != T_Update) {
        return false;
    }
    for (const auto &set_clause : plan.set_clauses_) {
        for (const auto &index : tab.indexes) {
            for (const auto &col : index.cols) {
                if (set_clause.lhs.col_name == col.name) {
                    return true;
                }
            }
        }
    }
    return false;
}

inline std::optional<std::vector<char>> build_full_equal_key(const std::string &tab_name,
                                                             const IndexMeta &index_meta,
                                                             const std::vector<Condition> &conds) {
    std::vector<char> key(index_meta.col_tot_len, 0);
    int key_offset = 0;
    for (const auto &index_col : index_meta.cols) {
        const Condition *eq_cond = nullptr;
        for (const auto &cond : conds) {
            if (cond.is_rhs_val && cond.op == OP_EQ &&
                cond.lhs_col.tab_name == tab_name &&
                cond.lhs_col.col_name == index_col.name) {
                eq_cond = &cond;
                break;
            }
        }
        if (eq_cond == nullptr) {
            return std::nullopt;
        }
        memcpy(key.data() + key_offset, eq_cond->rhs_val.raw->data, index_col.len);
        key_offset += index_col.len;
    }
    return key;
}

inline const IndexMeta *find_exact_index(const TabMeta &tab, const std::string &tab_name,
                                         const std::vector<Condition> &conds,
                                         std::vector<char> &key) {
    for (const auto &index : tab.indexes) {
        auto exact_key = build_full_equal_key(tab_name, index, conds);
        if (exact_key.has_value()) {
            key = std::move(*exact_key);
            return &index;
        }
    }
    return nullptr;
}

inline bool collect_exact_write_rids(SmManager *sm_manager, const std::shared_ptr<DMLPlan> &plan,
                                     Context *context, std::vector<Rid> &rids) {
    rids.clear();

    if (sm_manager == nullptr || plan == nullptr || !is_perf_txn(context)) {
        return false;
    }

    auto scan = std::dynamic_pointer_cast<ScanPlan>(plan->subplan_);
    if (scan == nullptr) {
        return false;
    }

    std::string real_tab_name = sm_manager->resolve_table_name(plan->tab_name_);
    auto tab = sm_manager->db_.get_table(real_tab_name);

    // 如果没有索引，尝试重新加载元数据（可能是其他连接创建的索引）
    if (tab.indexes.empty()) {
        sm_manager->reload_meta();
        tab = sm_manager->db_.get_table(real_tab_name);
    }

    if (update_changes_index_column(*plan, tab)) {
        return false;
    }

    std::vector<Condition> normalized_conds = scan->conds_;
    for (auto &cond : normalized_conds) {
        if (!normalize_condition_for_table(cond, scan->tab_name_)) {
            return false;
        }
    }

    std::vector<char> key;
    const IndexMeta *index_meta = find_exact_index(tab, scan->tab_name_, normalized_conds, key);
    if (index_meta == nullptr) {
        return false;
    }

    std::vector<Rid> candidates;
    auto ih = index_maintenance::get_index_handle(sm_manager, real_tab_name, *index_meta);
    if (!ih->get_value(key.data(), &candidates, nullptr)) {
        return true;
    }

    auto fh = sm_manager->get_table_fh(plan->tab_name_);
    auto cols = sm_manager->get_query_cols(plan->tab_name_);

    // 【关键修复】验证索引列值是否仍然匹配
    // MVCC 下索引可能包含已修改的旧条目，必须验证当前可见记录的索引列值
    for (const auto &rid : candidates) {
        auto visible = fh->get_record(rid, context);
        if (visible == nullptr) {
            continue;
        }

        // 验证索引列值是否匹配
        bool index_cols_match = true;
        for (const auto &index_col : index_meta->cols) {
            // 找到该索引列的元数据
            const ColMeta *col_meta = nullptr;
            for (const auto &col : cols) {
                if (col.tab_name == scan->tab_name_ && col.name == index_col.name) {
                    col_meta = &col;
                    break;
                }
            }

            if (col_meta == nullptr) {
                index_cols_match = false;
                break;
            }

            // 从可见记录中读取索引列的实际值
            const char *record_val = visible->data + col_meta->offset;

            // 从 WHERE 条件中找到该索引列的等值条件
            const Condition *eq_cond = nullptr;
            for (const auto &cond : normalized_conds) {
                if (cond.is_rhs_val && cond.op == OP_EQ &&
                    cond.lhs_col.tab_name == scan->tab_name_ &&
                    cond.lhs_col.col_name == index_col.name) {
                    eq_cond = &cond;
                    break;
                }
            }

            if (eq_cond == nullptr) {
                // 索引列没有等值条件，理论上不应该发生
                index_cols_match = false;
                break;
            }

            // 比较记录中的索引列值与查询条件的值
            int cmp = compare_value(record_val, eq_cond->rhs_val.raw->data,
                                   col_meta->type, col_meta->len);
            if (cmp != 0) {
                // 索引列值已改变，跳过此 RID
                index_cols_match = false;
                break;
            }
        }

        // 只有索引列值匹配且满足所有条件时，才加入候选列表
        if (index_cols_match && eval_conds(visible.get(), normalized_conds, cols)) {
            rids.push_back(rid);
        }
    }

    std::sort(rids.begin(), rids.end(), index_maintenance::rid_less);
    rids.erase(std::unique(rids.begin(), rids.end(), index_maintenance::same_rid), rids.end());
    return true;
}

}  // namespace write_index_probe
