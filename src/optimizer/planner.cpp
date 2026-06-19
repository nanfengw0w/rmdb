/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <memory>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

// 支持最左匹配原则：自动调整where条件顺序，支持单点查询和范围查询
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string>& index_col_names) {
    index_col_names.clear();
    std::string real_tab_name = sm_manager_->resolve_table_name(tab_name);
    TabMeta& tab = sm_manager_->db_.get_table(real_tab_name);

    for (auto& index : tab.indexes) {
        if (index.cols.empty()) {
            continue;
        }

        bool found_leftmost_col = false;
        for (auto& cond : curr_conds) {
            // 检查lhs或rhs是否匹配索引的第一列
            if (cond.lhs_col.tab_name == tab_name &&
                cond.lhs_col.col_name == index.cols[0].name) {
                found_leftmost_col = true;
                break;
            }
            if (!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name &&
                cond.rhs_col.col_name == index.cols[0].name) {
                found_leftmost_col = true;
                break;
            }
        }

        if (found_leftmost_col) {
            for (auto &col : index.cols) {
                index_col_names.push_back(col.name);
            }
            return true;
        }
    }

    return false;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if (it->lhs_col.tab_name == tab_names &&
            (it->is_rhs_val || it->rhs_col.tab_name == tab_names)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->tab_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->tab_name_.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    // 其他物理优化

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    return plan;
}



std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }
    std::vector<Condition> remaining_conds = std::move(query->conds);
    std::vector<std::string> joined_tables{tables[0]};
    std::shared_ptr<Plan> join_root = table_scan_executors[0];

    auto is_joined = [&](const std::string &tab_name) {
        return std::find(joined_tables.begin(), joined_tables.end(), tab_name) != joined_tables.end();
    };
    auto connects_to_joined = [&](const std::string &tab_name) {
        for (auto &cond : remaining_conds) {
            if (cond.is_rhs_val) {
                continue;
            }
            if (cond.lhs_col.tab_name == tab_name && is_joined(cond.rhs_col.tab_name)) {
                return true;
            }
            if (cond.rhs_col.tab_name == tab_name && is_joined(cond.lhs_col.tab_name)) {
                return true;
            }
        }
        return false;
    };

    std::vector<size_t> pending_tables;
    for (size_t i = 1; i < tables.size(); i++) {
        pending_tables.push_back(i);
    }

    while (!pending_tables.empty()) {
        size_t chosen_pos = 0;
        if (!connects_to_joined(tables[pending_tables[chosen_pos]])) {
            for (size_t pos = 1; pos < pending_tables.size(); pos++) {
                if (connects_to_joined(tables[pending_tables[pos]])) {
                    chosen_pos = pos;
                    break;
                }
            }
        }
        size_t i = pending_tables[chosen_pos];
        pending_tables.erase(pending_tables.begin() + chosen_pos);

        std::vector<Condition> join_conds;
        auto it = remaining_conds.begin();
        while (it != remaining_conds.end()) {
            bool lhs_ready = is_joined(it->lhs_col.tab_name) || it->lhs_col.tab_name == tables[i];
            bool rhs_ready = it->is_rhs_val || is_joined(it->rhs_col.tab_name) || it->rhs_col.tab_name == tables[i];
            bool touches_new_table = it->lhs_col.tab_name == tables[i] ||
                                     (!it->is_rhs_val && it->rhs_col.tab_name == tables[i]);

            if (lhs_ready && rhs_ready && touches_new_table) {
                join_conds.emplace_back(std::move(*it));
                it = remaining_conds.erase(it);
            } else {
                ++it;
            }
        }

        // 检查JOIN条件中是否有索引可用于内表
        auto right_scan = std::dynamic_pointer_cast<ScanPlan>(table_scan_executors[i]);
        if (right_scan && right_scan->tag == T_SeqScan) {
            std::vector<std::string> join_index_cols;
            bool has_join_index = get_index_cols(tables[i], join_conds, join_index_cols);
            if (has_join_index) {
                // 升级为IndexScan
                table_scan_executors[i] = std::make_shared<ScanPlan>(T_IndexScan, sm_manager_,
                    tables[i], right_scan->conds_, join_index_cols);
            }
        }

        join_root = std::make_shared<JoinPlan>(T_NestLoop, std::move(join_root),
                                               std::move(table_scan_executors[i]), std::move(join_conds));
        joined_tables.emplace_back(tables[i]);
    }

    for (auto &cond : remaining_conds) {
        push_conds(&cond, join_root);
    }

    return join_root;

}


std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if(!x->has_sort) {
        return plan;
    }
    std::vector<std::string> tables = query->tables;
    std::vector<ColMeta> all_cols;
    for (auto &sel_tab_name : tables) {
        auto sel_tab_cols = sm_manager_->get_query_cols(sel_tab_name);
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
    TabCol order_col = {.tab_name = x->order->cols->tab_name, .col_name = x->order->cols->col_name};
    TabCol sel_col;
    bool found = false;
    for (auto &col : all_cols) {
        bool matches = false;
        if (order_col.tab_name.empty()) {
            matches = col.name == order_col.col_name;
        } else {
            matches = col.tab_name == order_col.tab_name && col.name == order_col.col_name;
        }
        if (!matches) {
            continue;
        }
        if (found && order_col.tab_name.empty()) {
            throw AmbiguousColumnError(order_col.col_name);
        }
        sel_col = {.tab_name = col.tab_name, .col_name = col.name};
        found = true;
    }
    if (!found) {
        throw ColumnNotFoundError(order_col.tab_name.empty() ? order_col.col_name
                                                            : order_col.tab_name + "." + order_col.col_name);
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), sel_col, 
                                    x->order->orderby_dir == ast::OrderBy_DESC);
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);

    //物理优化
    auto sel_cols = query->cols;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);
    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), 
                                                        std::move(sel_cols));

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);
        
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,  
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
        index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, 
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
