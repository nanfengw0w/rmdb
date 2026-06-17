/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"

#include <map>
#include <sstream>
#include <set>
#include "executor_delete.h"
#include "parser/ast.h"
#include "common/sql_rewrite.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "analyze/analyze.h"

// Forward declarations for parser/lexer (avoid yacc.tab.h conflicts)
struct yy_buffer_state;
typedef struct yy_buffer_state *YY_BUFFER_STATE;
extern int yyparse(void);
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void yy_delete_buffer(YY_BUFFER_STATE b);
namespace ast { extern std::shared_ptr<ast::TreeNode> parse_tree; }
#include "executor_seq_scan.h"
#include "executor_index_scan.h"
#include "executor_projection.h"
#include "executor_nestedloop_join.h"
#include "execution_sort.h"

// Forward declarations for lexer
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void yy_delete_buffer(YY_BUFFER_STATE b);
extern int yyparse(void);
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                context->txn_->set_txn_mode(true);
                break;
            }  
            case T_Transaction_commit:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->commit(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_rollback:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_abort:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }     
            default:
                throw InternalError("Unexpected field type");
                break;                        
        }

    } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
        switch (x->set_knob_type_)
        {
        case ast::SetKnobType::EnableNestLoop: {
            planner_->set_enable_nestedloop_join(x->bool_value_);
            break;
        }
        case ast::SetKnobType::EnableSortMerge: {
            planner_->set_enable_sortmerge_join(x->bool_value_);
            break;
        }
        default: {
            throw RMDBError("Not implemented!\n");
            break;
        }
        }
    }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols, 
                            Context *context) {
    std::vector<std::string> captions;
    captions.reserve(sel_cols.size());
    for (auto &sel_col : sel_cols) {
        captions.push_back(sel_col.col_name);
    }

    // Print header into buffer
    RecordPrinter rec_printer(sel_cols.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    // print header into file
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "|";
    for(int i = 0; i < captions.size(); ++i) {
        outfile << " " << captions[i] << " |";
    }
    outfile << "\n";

    // Print records
    size_t num_rec = 0;
    // 执行query_plan
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto Tuple = executorTreeRoot->Next();
        std::vector<std::string> columns;
        for (auto &col : executorTreeRoot->cols()) {
            std::string col_str;
            char *rec_buf = Tuple->data + col.offset;
            if (col.type == TYPE_INT) {
                col_str = std::to_string(*(int *)rec_buf);
            } else if (col.type == TYPE_FLOAT) {
                col_str = std::to_string(*(float *)rec_buf);
            } else if (col.type == TYPE_STRING) {
                col_str = std::string((char *)rec_buf, col.len);
                col_str.resize(strlen(col_str.c_str()));
            }
            columns.push_back(col_str);
        }
        // print record into buffer
        rec_printer.print_record(columns, context);
        // print record into file
        outfile << "|";
        for(int i = 0; i < columns.size(); ++i) {
            outfile << " " << columns[i] << " |";
        }
        outfile << "\n";
        num_rec++;
    }
    outfile.close();
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(num_rec, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    if (exec == nullptr) {
        throw InternalError("Null DML executor");
    }
    while (!exec->is_end()) {
        exec->Next();
    }
}

// 辅助：字符串转小写
static std::string to_lower_str(const std::string &s) {
    std::string r = s;
    for (auto &c : r) c = tolower(c);
    return r;
}

// 辅助：去除首尾空格
static std::string trim_str(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// 辅助：按分隔符分割字符串
static std::vector<std::string> split_str(const std::string &s, const std::string &delim) {
    std::vector<std::string> result;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            result.push_back(s.substr(start));
            break;
        }
        result.push_back(s.substr(start, pos - start));
        start = pos + delim.length();
    }
    return result;
}

// 聚合类型
enum AggType { AGG_NONE, AGG_COUNT_STAR, AGG_COUNT, AGG_MAX, AGG_MIN, AGG_SUM, AGG_AVG };

// 聚合列信息
struct AggCol {
    AggType type;
    std::string col_name;  // 原始列名
    std::string alias;     // 别名 (AS xxx)
    int col_idx;           // 列在表中的索引 (-1 for count(*))
    ColType col_type;      // 列类型
    int col_len;           // 列长度
    int col_offset;        // 列偏移
};

// 排序列信息
struct OrderCol {
    int col_idx;  // 在输出结果中的列索引
    bool is_desc;
};

// 解析聚合函数
static AggType parse_agg_func(const std::string &col_expr, std::string &inner_col) {
    std::string lower = to_lower_str(col_expr);
    if (lower == "count(*)" || lower == "count( * )") {
        inner_col = "*";
        return AGG_COUNT_STAR;
    }
    // Check for func(col) pattern
    size_t lp = col_expr.find('(');
    size_t rp = col_expr.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp <= lp) return AGG_NONE;
    std::string func = to_lower_str(trim_str(col_expr.substr(0, lp)));
    inner_col = trim_str(col_expr.substr(lp + 1, rp - lp - 1));
    // Handle COUNT( * ) with spaces
    if (func == "count" && (inner_col == "*" || inner_col == "* ")) {
        inner_col = "*";
        return AGG_COUNT_STAR;
    }
    if (func == "count") return AGG_COUNT;
    if (func == "max") return AGG_MAX;
    if (func == "min") return AGG_MIN;
    if (func == "sum") return AGG_SUM;
    if (func == "avg") return AGG_AVG;
    return AGG_NONE;
}

// 计算聚合值
static void compute_agg(AggType type, const char *data, int len, ColType col_type,
                        double &sum_val, double &min_val, double &max_val, int &count_val) {
    double val = 0;
    if (col_type == TYPE_INT) val = *(int*)data;
    else if (col_type == TYPE_FLOAT) val = *(float*)data;
    else return; // string: only count matters

    if (count_val == 0) {
        min_val = val;
        max_val = val;
        sum_val = val;
    } else {
        min_val = std::min(min_val, val);
        max_val = std::max(max_val, val);
        sum_val += val;
    }
    count_val++;
}

void QlManager::handle_aggregate(const std::string &sql, Context *context) {
    std::string sql_lower = to_lower_str(sql);

    // 1. 提取SELECT子句
    size_t sel_start = sql_lower.find("select ");
    size_t from_start = sql_lower.find(" from ");
    if (sel_start == std::string::npos || from_start == std::string::npos)
        throw InternalError("Invalid SQL");
    std::string select_part = trim_str(sql.substr(sel_start + 7, from_start - sel_start - 7));

    // 2. 提取FROM表名
    std::vector<std::string> keywords = {" where ", " group by ", " having ", " order by ", " limit "};
    size_t from_end = sql.length();
    for (auto &kw : keywords) {
        size_t pos = sql_lower.find(kw, from_start);
        if (pos != std::string::npos) from_end = std::min(from_end, pos);
    }
    std::string tab_name = trim_str(sql.substr(from_start + 6, from_end - from_start - 6));

    // 3. 获取表元数据
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    auto fh = sm_manager_->fhs_.at(tab_name).get();

    // 4. 提取WHERE条件
    std::string where_clause;
    {
        size_t wp = sql_lower.find(" where ");
        if (wp != std::string::npos) {
            size_t where_end = sql.length();
            for (auto &kw : {" group by ", " having ", " order by ", " limit "}) {
                size_t pos = sql_lower.find(kw, wp);
                if (pos != std::string::npos) where_end = std::min(where_end, pos);
            }
            where_clause = trim_str(sql.substr(wp + 7, where_end - wp - 7));
        }
    }

    // 检查WHERE中是否使用了聚合函数
    if (!where_clause.empty()) {
        std::string wc_lower = to_lower_str(where_clause);
        for (auto &fn : {"count(", "max(", "min(", "sum(", "avg("}) {
            if (wc_lower.find(fn) != std::string::npos) {
                throw InternalError("failure");
            }
        }
    }

    // 5. 解析WHERE条件为函数
    auto eval_where = [&](RmRecord *rec) -> bool {
        if (where_clause.empty()) return true;
        std::string wc = where_clause;
        std::vector<std::string> conds;
        {
            std::string wc_lower = to_lower_str(wc);
            size_t pos = 0;
            while (true) {
                size_t and_pos = wc_lower.find(" and ", pos);
                if (and_pos == std::string::npos) {
                    conds.push_back(trim_str(wc.substr(pos)));
                    break;
                }
                conds.push_back(trim_str(wc.substr(pos, and_pos - pos)));
                pos = and_pos + 5;
            }
        }
        for (auto &cond : conds) {
            CompOp op;
            // Token化: 按空格分割为 tokens
            std::vector<std::string> tokens;
            {
                std::istringstream iss(cond);
                std::string tok;
                while (iss >> tok) tokens.push_back(tok);
            }
            std::string col_name, op_str, val_str;
            if (tokens.size() >= 3 && (tokens[1] == "=" || tokens[1] == "<" || tokens[1] == ">" ||
                tokens[1] == "<=" || tokens[1] == ">=" || tokens[1] == "<>" || tokens[1] == "!=")) {
                col_name = tokens[0];
                op_str = tokens[1];
                val_str = tokens[2];
            } else {
                // Fallback: 找到列名后，从列名之后找操作符
                size_t op_pos = std::string::npos;
                for (auto &o : {"<=", ">=", "<>", "!=", "=", "<", ">"}) {
                    size_t p = cond.find(o);
                    if (p != std::string::npos && (op_pos == std::string::npos || p < op_pos)) {
                        op_pos = p;
                        op_str = o;
                    }
                }
                if (op_pos == std::string::npos) continue;
                col_name = trim_str(cond.substr(0, op_pos));
                val_str = trim_str(cond.substr(op_pos + op_str.length()));
            }
            if (!val_str.empty() && val_str.front() == '\'' && val_str.back() == '\'')
                val_str = val_str.substr(1, val_str.length() - 2);

            if (op_str == "=") op = OP_EQ;
            else if (op_str == "<>") op = OP_NE;
            else if (op_str == "!=") op = OP_NE;
            else if (op_str == "<") op = OP_LT;
            else if (op_str == ">") op = OP_GT;
            else if (op_str == "<=") op = OP_LE;
            else if (op_str == ">=") op = OP_GE;
            else continue;

            auto col_it = tab.get_col(col_name);
            char *rec_data = rec->data + col_it->offset;
            int cmp_result = 0;
            if (col_it->type == TYPE_INT) {
                int val = std::stoi(val_str);
                int rec_val = *(int*)rec_data;
                cmp_result = (rec_val > val) ? 1 : ((rec_val < val) ? -1 : 0);
            } else if (col_it->type == TYPE_FLOAT) {
                float val = std::stof(val_str);
                float rec_val = *(float*)rec_data;
                cmp_result = (rec_val > val) ? 1 : ((rec_val < val) ? -1 : 0);
            } else if (col_it->type == TYPE_STRING) {
                // Pad val_str to column length with null bytes for proper comparison
                std::string padded_val(col_it->len, '\0');
                memcpy(padded_val.data(), val_str.c_str(), std::min(val_str.length(), (size_t)col_it->len));
                cmp_result = memcmp(rec_data, padded_val.c_str(), col_it->len);
            }
            bool pass = false;
            switch (op) {
                case OP_EQ: pass = (cmp_result == 0); break;
                case OP_NE: pass = (cmp_result != 0); break;
                case OP_LT: pass = (cmp_result < 0); break;
                case OP_GT: pass = (cmp_result > 0); break;
                case OP_LE: pass = (cmp_result <= 0); break;
                case OP_GE: pass = (cmp_result >= 0); break;
                default: pass = true;
            }
            if (!pass) return false;
        }
        return true;
    };

    // 6. 解析GROUP BY (先解析以便验证SELECT列)
    std::vector<int> group_col_idxs;
    {
        size_t gb_pos = sql_lower.find(" group by ");
        if (gb_pos != std::string::npos) {
            size_t gb_end = sql.length();
            for (auto &kw : {" having ", " order by ", " limit "}) {
                size_t pos = sql_lower.find(kw, gb_pos);
                if (pos != std::string::npos) gb_end = std::min(gb_end, pos);
            }
            std::string gb_cols = trim_str(sql.substr(gb_pos + 10, gb_end - gb_pos - 10));
            auto parts = split_str(gb_cols, ",");
            for (auto &p : parts) {
                auto col_it = tab.get_col(trim_str(p));
                group_col_idxs.push_back(col_it - tab.cols.begin());
            }
        }
    }

    // 7. 解析SELECT列 (处理 select *)
    std::string expanded_select = select_part;
    if (trim_str(select_part) == "*") {
        expanded_select = "";
        for (size_t ci = 0; ci < tab.cols.size(); ci++) {
            if (ci > 0) expanded_select += ", ";
            expanded_select += tab.cols[ci].name;
        }
    }
    std::vector<AggCol> agg_cols;
    std::vector<std::string> select_items = split_str(expanded_select, ",");
    for (auto &item : select_items) {
        item = trim_str(item);
        AggCol ac;
        std::string item_lower = to_lower_str(item);
        size_t as_pos = item_lower.find(" as ");
        if (as_pos != std::string::npos) {
            ac.alias = trim_str(item.substr(as_pos + 4));
            item = trim_str(item.substr(0, as_pos));
        }
        std::string inner_col;
        ac.type = parse_agg_func(item, inner_col);
        if (ac.type != AGG_NONE && ac.type != AGG_COUNT_STAR) {
            auto col_it = tab.get_col(inner_col);
            ac.col_name = inner_col;
            ac.col_idx = col_it - tab.cols.begin();
            ac.col_type = col_it->type;
            ac.col_len = col_it->len;
            ac.col_offset = col_it->offset;
            if (ac.alias.empty()) {
                ac.alias = to_lower_str(item);
            }
        } else if (ac.type == AGG_COUNT_STAR) {
            ac.col_name = "*";
            ac.col_idx = -1;
            ac.col_type = TYPE_INT;
            ac.col_len = sizeof(int);
            ac.col_offset = 0;
            if (ac.alias.empty()) ac.alias = "count(*)";
        } else {
            auto col_it = tab.get_col(item);
            ac.type = AGG_NONE;
            ac.col_name = item;
            ac.col_idx = col_it - tab.cols.begin();
            ac.col_type = col_it->type;
            ac.col_len = col_it->len;
            ac.col_offset = col_it->offset;
            if (ac.alias.empty()) ac.alias = item;

            // 健壮性检查: 有GROUP BY时，非聚合列必须在GROUP BY中
            if (!group_col_idxs.empty()) {
                bool in_group = false;
                for (int gi : group_col_idxs) {
                    if (gi == ac.col_idx) { in_group = true; break; }
                }
                if (!in_group) {
                    throw InternalError("failure");
                }
            }
        }
        agg_cols.push_back(ac);
    }

    // 8. 扫描表并收集记录
    std::vector<std::vector<char>> raw_records;
    RmScan scan(fh);
    while (!scan.is_end()) {
        auto rec = fh->get_record(scan.rid(), context);
        if (eval_where(rec.get())) {
            std::vector<char> row(rec->data, rec->data + fh->get_file_hdr().record_size);
            raw_records.push_back(row);
        }
        scan.next();
    }

    // 9. 分组与聚合计算
    // 使用字符串存储每个聚合列的值，支持 INT/FLOAT/STRING
    struct GroupResult {
        std::vector<std::string> agg_strs;   // 每个聚合列的字符串值
        std::vector<double> agg_nums;        // 每个聚合列的数值(用于排序)
        std::vector<bool> agg_is_str;        // 是否为字符串类型
        std::vector<std::vector<char>*> rows; // 本组原始记录，用于HAVING中未出现在SELECT的聚合函数
        int count;
    };
    std::vector<GroupResult> results;

    auto compute_group = [&](const std::vector<std::vector<char>*> &rows) {
        GroupResult gr;
        gr.count = rows.size();
        gr.rows = rows;
        for (size_t ci = 0; ci < agg_cols.size(); ci++) {
            auto &ac = agg_cols[ci];
            if (ac.type == AGG_COUNT_STAR) {
                gr.agg_strs.push_back(std::to_string((int)rows.size()));
                gr.agg_nums.push_back((double)rows.size());
                gr.agg_is_str.push_back(false);
            } else if (ac.type == AGG_COUNT) {
                // COUNT(col): 统计非NULL值 (这里所有列都有值，等价于行数)
                gr.agg_strs.push_back(std::to_string((int)rows.size()));
                gr.agg_nums.push_back((double)rows.size());
                gr.agg_is_str.push_back(false);
            } else if (ac.type == AGG_MAX || ac.type == AGG_MIN || ac.type == AGG_SUM || ac.type == AGG_AVG) {
                double sum_v = 0, min_v = 0, max_v = 0;
                int cnt = 0;
                for (auto &row : rows) {
                    compute_agg(ac.type, row->data() + ac.col_offset, ac.col_len, ac.col_type, sum_v, min_v, max_v, cnt);
                }
                double result_val = 0;
                if (ac.type == AGG_MAX) result_val = max_v;
                else if (ac.type == AGG_MIN) result_val = min_v;
                else if (ac.type == AGG_SUM) result_val = sum_v;
                else if (ac.type == AGG_AVG) result_val = cnt > 0 ? sum_v / cnt : 0;

                gr.agg_nums.push_back(result_val);
                gr.agg_is_str.push_back(false);
                if (ac.type == AGG_COUNT || ac.type == AGG_COUNT_STAR) {
                    gr.agg_strs.push_back(std::to_string((int)result_val));
                } else if (ac.type == AGG_AVG) {
                    // AVG always outputs float
                    gr.agg_strs.push_back(std::to_string(result_val));
                } else if (ac.col_type == TYPE_INT) {
                    gr.agg_strs.push_back(std::to_string((int)result_val));
                } else {
                    gr.agg_strs.push_back(std::to_string(result_val));
                }
            } else {
                // AGG_NONE: 普通列 (GROUP BY列或无GROUP BY时的列)
                if (ac.col_type == TYPE_INT) {
                    int v = *(int*)(rows[0]->data() + ac.col_offset);
                    gr.agg_strs.push_back(std::to_string(v));
                    gr.agg_nums.push_back(v);
                    gr.agg_is_str.push_back(false);
                } else if (ac.col_type == TYPE_FLOAT) {
                    float v = *(float*)(rows[0]->data() + ac.col_offset);
                    gr.agg_strs.push_back(std::to_string(v));
                    gr.agg_nums.push_back(v);
                    gr.agg_is_str.push_back(false);
                } else {
                    // STRING 类型
                    std::string s(rows[0]->data() + ac.col_offset, ac.col_len);
                    s.resize(strlen(s.c_str()));
                    gr.agg_strs.push_back(s);
                    gr.agg_nums.push_back(0);
                    gr.agg_is_str.push_back(true);
                }
            }
        }
        results.push_back(gr);
    };

    // 检查是否有聚合函数
    bool has_agg_func = false;
    for (auto &ac : agg_cols) {
        if (ac.type != AGG_NONE) { has_agg_func = true; break; }
    }

    // 有聚合函数但没有GROUP BY时，SELECT中不能有非聚合列
    if (has_agg_func && group_col_idxs.empty()) {
        for (auto &ac : agg_cols) {
            if (ac.type == AGG_NONE) {
                throw InternalError("failure");
            }
        }
        // 空表且有非COUNT聚合：不输出结果
        if (raw_records.empty()) {
            bool only_count_aggs = true;
            for (auto &ac : agg_cols) {
                if (ac.type != AGG_COUNT && ac.type != AGG_COUNT_STAR) {
                    only_count_aggs = false;
                    break;
                }
            }
            if (!only_count_aggs) {
                return;  // 不输出任何内容
            }
        }
    }

    if (group_col_idxs.empty() && !has_agg_func) {
        // 没有GROUP BY且没有聚合函数: 每行是一个结果 (用于 ORDER BY + LIMIT 等)
        for (auto &row : raw_records) {
            std::vector<std::vector<char>*> ptrs = {&row};
            compute_group(ptrs);
        }
    } else if (group_col_idxs.empty()) {
        // 没有GROUP BY但有聚合函数，所有记录是一组
        bool has_non_count_agg = false;
        for (auto &ac : agg_cols) {
            if (ac.type != AGG_COUNT && ac.type != AGG_COUNT_STAR) {
                has_non_count_agg = true;
                break;
            }
        }
        if (!raw_records.empty() || !has_non_count_agg) {
            std::vector<std::vector<char>*> ptrs;
            for (auto &r : raw_records) ptrs.push_back(&r);
            compute_group(ptrs);
        }
    } else {
        // 有GROUP BY，按GROUP BY列分组 (保持插入顺序)
        std::vector<std::pair<std::string, std::vector<std::vector<char>*>>> group_list;
        std::map<std::string, int> key_to_idx;
        for (auto &row : raw_records) {
            std::string key;
            for (int idx : group_col_idxs) {
                auto &col = tab.cols[idx];
                if (col.type == TYPE_INT) key += std::to_string(*(int*)(row.data() + col.offset)) + "|";
                else if (col.type == TYPE_FLOAT) key += std::to_string(*(float*)(row.data() + col.offset)) + "|";
                else key += std::string(row.data() + col.offset, col.len) + "|";
            }
            auto it = key_to_idx.find(key);
            if (it == key_to_idx.end()) {
                key_to_idx[key] = group_list.size();
                group_list.push_back({key, {&row}});
            } else {
                group_list[it->second].second.push_back(&row);
            }
        }
        for (auto &[key, rows] : group_list) {
            compute_group(rows);
        }
    }

    // 10. HAVING过滤
    {
        size_t hv_pos = sql_lower.find(" having ");
        if (hv_pos != std::string::npos) {
            size_t hv_end = sql.length();
            for (auto &kw : {" order by ", " limit "}) {
                size_t pos = sql_lower.find(kw, hv_pos);
                if (pos != std::string::npos) hv_end = std::min(hv_end, pos);
            }
            std::string having_clause = trim_str(sql.substr(hv_pos + 8, hv_end - hv_pos - 8));

            // 解析HAVING条件: agg_func(col) op value [AND ...]
            std::vector<std::string> hv_conds;
            {
                std::string hc = having_clause;
                std::string hc_lower = to_lower_str(hc);
                size_t pos = 0;
                while (pos < hc.length()) {
                    size_t and_pos = std::string::npos;
                    size_t and_len = 0;
                    for (size_t i = pos; i < hc.length(); i++) {
                        if (hc[i] == '(') {
                            int pd = 1; i++;
                            while (i < hc.length() && pd > 0) {
                                if (hc[i] == '(') pd++;
                                if (hc[i] == ')') pd--;
                                i++;
                            }
                            if (i < hc.length()) i--;
                            continue;
                        }
                        if (i + 3 > hc.length()) continue;
                        if (hc_lower[i] != 'a' || hc_lower[i+1] != 'n' || hc_lower[i+2] != 'd') continue;
                        // 边界检查: 前面不能是字母 (避免匹配band/brand等)
                        bool left_ok = (i == 0 || !isalpha(hc_lower[i-1]));
                        if (left_ok) {
                            and_pos = i;
                            and_len = 3;
                            if (and_pos > 0 && hc[and_pos-1] == ' ') { and_pos--; and_len++; }
                            if (and_pos + and_len < hc.length() && hc[and_pos+and_len] == ' ') { and_len++; }
                            break;
                        }
                    }
                    if (and_pos == std::string::npos) {
                        hv_conds.push_back(trim_str(hc.substr(pos)));
                        break;
                    }
                    hv_conds.push_back(trim_str(hc.substr(pos, and_pos - pos)));
                    pos = and_pos + and_len;
                }
            }

            auto it = results.begin();
            while (it != results.end()) {
                bool pass_all = true;
                for (auto &hv_cond : hv_conds) {
                    // 解析: agg_func(col) op value
                    // 使用括号感知的方式查找操作符位置
                    std::string left_expr, op_str, val_str;
                    {
                        int paren_depth = 0;
                        size_t op_pos = std::string::npos;
                        size_t op_len = 0;
                        for (size_t ci = 0; ci < hv_cond.length(); ci++) {
                            if (hv_cond[ci] == '(') paren_depth++;
                            else if (hv_cond[ci] == ')') paren_depth--;
                            else if (paren_depth == 0) {
                                // 检查双字符操作符
                                if (ci + 1 < hv_cond.length()) {
                                    std::string two = hv_cond.substr(ci, 2);
                                    if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
                                        op_pos = ci; op_len = 2; break;
                                    }
                                }
                                // 检查单字符操作符
                                if (hv_cond[ci] == '=' || hv_cond[ci] == '<' || hv_cond[ci] == '>') {
                                    op_pos = ci; op_len = 1; break;
                                }
                            }
                        }
                        if (op_pos == std::string::npos) { pass_all = false; break; }
                        left_expr = trim_str(hv_cond.substr(0, op_pos));
                        op_str = hv_cond.substr(op_pos, op_len);
                        val_str = trim_str(hv_cond.substr(op_pos + op_len));
                    }
                    if (op_str.empty() || left_expr.empty() || val_str.empty()) { pass_all = false; break; }

                    // 解析左边的聚合函数
                    std::string inner_col;
                    AggType agg_type = parse_agg_func(left_expr, inner_col);

                    // 先在agg_cols中查找，如果找不到则直接计算
                    int agg_idx = -1;
                    for (size_t ai = 0; ai < agg_cols.size(); ai++) {
                        if (agg_cols[ai].type == agg_type) {
                            if (agg_type == AGG_COUNT_STAR && inner_col == "*") {
                                agg_idx = ai; break;
                            } else if (agg_cols[ai].col_name == inner_col) {
                                agg_idx = ai; break;
                            }
                        }
                    }

                    double col_val = 0;
                    if (agg_idx >= 0 && agg_idx < (int)it->agg_nums.size()) {
                        col_val = it->agg_nums[agg_idx];
                    } else {
                        // HAVING引用了SELECT中没有的聚合函数，直接从组数据计算
                        if (agg_type == AGG_COUNT_STAR || agg_type == AGG_COUNT) {
                            col_val = it->count;
                        } else if (agg_type == AGG_MAX || agg_type == AGG_MIN ||
                                   agg_type == AGG_SUM || agg_type == AGG_AVG) {
                            auto col_it = tab.get_col(inner_col);
                            double sum_v = 0, min_v = 0, max_v = 0;
                            int cnt = 0;
                            for (auto row : it->rows) {
                                compute_agg(agg_type, row->data() + col_it->offset, col_it->len,
                                            col_it->type, sum_v, min_v, max_v, cnt);
                            }
                            if (agg_type == AGG_MAX) col_val = max_v;
                            else if (agg_type == AGG_MIN) col_val = min_v;
                            else if (agg_type == AGG_SUM) col_val = sum_v;
                            else col_val = cnt > 0 ? sum_v / cnt : 0;
                        } else {
                            pass_all = false; break;
                        }
                    }
                    double cmp_val = 0;
                    try {
                        cmp_val = std::stod(val_str);
                    } catch (...) { pass_all = false; break; }

                    CompOp cop;
                    if (op_str == "=") cop = OP_EQ;
                    else if (op_str == "<>") cop = OP_NE;
                    else if (op_str == "!=") cop = OP_NE;
                    else if (op_str == "<") cop = OP_LT;
                    else if (op_str == ">") cop = OP_GT;
                    else if (op_str == "<=") cop = OP_LE;
                    else if (op_str == ">=") cop = OP_GE;
                    else { pass_all = false; break; }

                    bool cond_pass = false;
                    switch (cop) {
                        case OP_EQ: cond_pass = (col_val == cmp_val); break;
                        case OP_NE: cond_pass = (col_val != cmp_val); break;
                        case OP_LT: cond_pass = (col_val < cmp_val); break;
                        case OP_GT: cond_pass = (col_val > cmp_val); break;
                        case OP_LE: cond_pass = (col_val <= cmp_val); break;
                        case OP_GE: cond_pass = (col_val >= cmp_val); break;
                        default: cond_pass = true;
                    }
                    if (!cond_pass) { pass_all = false; break; }
                }
                if (!pass_all) {
                    it = results.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    // 11. ORDER BY
    {
        size_t ob_pos = sql_lower.find(" order by ");
        if (ob_pos != std::string::npos) {
            size_t ob_end = sql.length();
            size_t lim_pos = sql_lower.find(" limit ", ob_pos);
            if (lim_pos != std::string::npos) ob_end = lim_pos;
            std::string ob_cols = trim_str(sql.substr(ob_pos + 10, ob_end - ob_pos - 10));
            auto parts = split_str(ob_cols, ",");
            std::vector<OrderCol> order_cols;
            for (auto &p : parts) {
                p = trim_str(p);
                OrderCol oc;
                oc.is_desc = false;
                std::string p_lower = to_lower_str(p);
                if (p_lower.find(" desc") != std::string::npos) {
                    oc.is_desc = true;
                    p = trim_str(p.substr(0, p.length() - 5));
                } else if (p_lower.find(" asc") != std::string::npos) {
                    p = trim_str(p.substr(0, p.length() - 4));
                }
                oc.col_idx = -1;
                for (size_t i = 0; i < agg_cols.size(); i++) {
                    if (agg_cols[i].alias == p || agg_cols[i].col_name == p) {
                        oc.col_idx = i;
                        break;
                    }
                }
                if (oc.col_idx == -1) {
                    for (size_t i = 0; i < agg_cols.size(); i++) {
                        if (to_lower_str(agg_cols[i].col_name) == to_lower_str(p)) {
                            oc.col_idx = i;
                            break;
                        }
                    }
                }
                // 也检查表列名
                if (oc.col_idx == -1) {
                    for (size_t i = 0; i < agg_cols.size(); i++) {
                        if (to_lower_str(agg_cols[i].alias) == to_lower_str(p)) {
                            oc.col_idx = i;
                            break;
                        }
                    }
                }
                order_cols.push_back(oc);
            }
            std::sort(results.begin(), results.end(), [&](const GroupResult &a, const GroupResult &b) {
                for (auto &oc : order_cols) {
                    if (oc.col_idx < 0 || oc.col_idx >= (int)a.agg_nums.size()) continue;
                    if (a.agg_is_str[oc.col_idx] && b.agg_is_str[oc.col_idx]) {
                        // 字符串比较
                        int cmp = a.agg_strs[oc.col_idx].compare(b.agg_strs[oc.col_idx]);
                        if (cmp != 0) {
                            return oc.is_desc ? (cmp > 0) : (cmp < 0);
                        }
                    } else {
                        double va = a.agg_nums[oc.col_idx];
                        double vb = b.agg_nums[oc.col_idx];
                        if (va != vb) {
                            return oc.is_desc ? (va > vb) : (va < vb);
                        }
                    }
                }
                return false;
            });
        }
    }

    // 12. LIMIT
    int limit_n = -1;
    {
        size_t lim_pos = sql_lower.find(" limit ");
        if (lim_pos != std::string::npos) {
            std::string lim_str = trim_str(sql.substr(lim_pos + 7));
            limit_n = std::stoi(lim_str);
        }
    }
    if (limit_n >= 0 && (int)results.size() > limit_n) {
        results.resize(limit_n);
    }

    // 13. 输出结果 - 先收集所有输出行，最后一次性写入
    std::vector<std::string> output_lines;
    std::vector<std::string> captions;
    for (auto &ac : agg_cols) captions.push_back(ac.alias);

    // 先收集数据行
    for (auto &gr : results) {
        std::string line = "|";
        for (size_t i = 0; i < agg_cols.size(); i++) {
            line += " " + gr.agg_strs[i] + " |";
        }
        output_lines.push_back(line);
    }

    // 如果有数据行，才写入 header + 数据
    if (!output_lines.empty()) {
        std::fstream outfile;
        outfile.open("output.txt", std::ios::out | std::ios::app);
        // 写 header
        outfile << "|";
        for (auto &c : captions) outfile << " " << c << " |";
        outfile << "\n";
        // 写数据
        for (auto &line : output_lines) {
            outfile << line << "\n";
        }
        outfile.close();

        RecordPrinter rec_printer(captions.size());
        rec_printer.print_separator(context);
        rec_printer.print_record(captions, context);
        rec_printer.print_separator(context);
        for (auto &gr : results) {
            std::vector<std::string> columns;
            for (size_t i = 0; i < agg_cols.size(); i++) {
                columns.push_back(gr.agg_strs[i]);
            }
            rec_printer.print_record(columns, context);
        }
        rec_printer.print_separator(context);
        RecordPrinter::print_record_count(results.size(), context);
    }
}

// 执行一个SELECT子查询，返回结果行（每行是字符串向量）和列元数据
struct SubQueryResult {
    std::vector<std::vector<std::string>> rows;
    std::vector<ColMeta> cols;
    std::vector<std::string> col_names;
};

// 递归构建执行器
static std::unique_ptr<AbstractExecutor> build_executor_tree(SmManager *sm_manager, std::shared_ptr<Plan> plan, Context *context) {
    if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return std::make_unique<ProjectionExecutor>(build_executor_tree(sm_manager, x->subplan_, context), x->sel_cols_);
    } else if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        if (x->tag == T_SeqScan) {
            return std::make_unique<SeqScanExecutor>(sm_manager, x->tab_name_, x->conds_, context);
        } else {
            return std::make_unique<IndexScanExecutor>(sm_manager, x->tab_name_, x->conds_, x->index_col_names_, context);
        }
    } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        auto left = build_executor_tree(sm_manager, x->left_, context);
        auto right = build_executor_tree(sm_manager, x->right_, context);
        return std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right), x->conds_);
    } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        return std::make_unique<SortExecutor>(build_executor_tree(sm_manager, x->subplan_, context), x->sel_col_, x->is_desc_);
    }
    return nullptr;
}

static SubQueryResult execute_sub_select(SmManager *sm_manager, const std::string &select_sql, Context *context) {
    SubQueryResult result;

    // 解析SQL
    ast::parse_tree = nullptr;
    std::string sql_with_semi = select_sql;
    if (!sql_with_semi.empty() && sql_with_semi.back() != ';') sql_with_semi += ';';
    YY_BUFFER_STATE buf = yy_scan_string(sql_with_semi.c_str());
    int parse_ok = yyparse();
    yy_delete_buffer(buf);

    if (parse_ok != 0 || ast::parse_tree == nullptr) {
        throw InternalError("Parse failed for sub-query: " + select_sql);
    }

    auto analyze_inst = std::make_unique<Analyze>(sm_manager);
    auto query = analyze_inst->do_analyze(ast::parse_tree);

    auto planner_inst = std::make_unique<Planner>(sm_manager);
    auto optimizer_inst = std::make_unique<Optimizer>(sm_manager, planner_inst.get());
    auto plan = optimizer_inst->plan_query(query, context);

    auto dml_plan = std::dynamic_pointer_cast<DMLPlan>(plan);
    if (!dml_plan || dml_plan->tag != T_select) {
        throw InternalError("Sub-query is not a SELECT");
    }

    auto executor = build_executor_tree(sm_manager, dml_plan->subplan_, context);
    if (!executor) {
        throw InternalError("Sub-query execution failed");
    }

    // 收集列元数据
    auto executor_cols = executor->cols();
    for (auto &col : executor_cols) {
        result.col_names.push_back(col.name);
        ColMeta cm;
        cm.name = col.name;
        cm.type = col.type;
        cm.len = col.len;
        cm.offset = col.offset;
        result.cols.push_back(cm);
    }

    // 执行并收集结果
    for (executor->beginTuple(); !executor->is_end(); executor->nextTuple()) {
        auto tuple = executor->Next();
        std::vector<std::string> row;
        for (auto &col : executor_cols) {
            char *data = tuple->data + col.offset;
            std::string val;
            if (col.type == TYPE_INT) {
                val = std::to_string(*(int*)data);
            } else if (col.type == TYPE_FLOAT) {
                val = std::to_string(*(float*)data);
            } else if (col.type == TYPE_STRING) {
                val = std::string(data, col.len);
                val.resize(strlen(val.c_str()));
            }
            row.push_back(val);
        }
        result.rows.push_back(row);
    }

    return result;
}

void QlManager::handle_union(const std::string &sql, Context *context) {
    std::string sql_lower = to_lower_str(sql);

    // 1. 提取外层ORDER BY和LIMIT
    std::string outer_order_by;
    int limit_n = -1;
    {
        // 找到最外层的ORDER BY（不在括号内的）
        size_t ob_pos = std::string::npos;
        int paren_depth = 0;
        for (size_t i = 0; i < sql.length(); i++) {
            if (sql[i] == '(') paren_depth++;
            else if (sql[i] == ')') paren_depth--;
            else if (paren_depth == 0 && i + 9 < sql.length()) {
                std::string substr = sql_lower.substr(i, 9);
                if (substr == "order by ") {
                    ob_pos = i;
                    break;
                }
            }
        }
        if (ob_pos != std::string::npos) {
            size_t ob_end = sql.length();
            size_t lim_pos = sql_lower.find(" limit ", ob_pos + 9);
            if (lim_pos != std::string::npos) {
                ob_end = lim_pos;
                limit_n = std::stoi(trim_str(sql.substr(lim_pos + 7)));
            }
            outer_order_by = trim_str(sql.substr(ob_pos + 9, ob_end - ob_pos - 9));
        } else {
            size_t lim_pos = sql_lower.find(" limit ");
            if (lim_pos != std::string::npos) {
                limit_n = std::stoi(trim_str(sql.substr(lim_pos + 7)));
            }
        }
    }

    // 2. 按UNION分割子查询（考虑括号嵌套）
    std::vector<std::string> sub_queries;
    {
        std::string remaining = sql;
        // 去掉外层ORDER BY和LIMIT
        {
            size_t ob_search = to_lower_str(remaining).find(" order by ");
            if (ob_search != std::string::npos) {
                remaining = remaining.substr(0, ob_search);
            }
            size_t lim_search = to_lower_str(remaining).find(" limit ");
            if (lim_search != std::string::npos) {
                remaining = remaining.substr(0, lim_search);
            }
        }
        remaining = trim_str(remaining);

        // 检测并去掉外层 SELECT * FROM (...) AS alias 包裹
        {
            std::string rl = to_lower_str(remaining);
            // 匹配: SELECT * FROM ( ... ) AS xxx
            if (rl.find("select * from (") == 0 || rl.find("select * from (") == 0) {
                // 找到匹配的右括号
                int pd = 0;
                size_t close_paren = std::string::npos;
                for (size_t i = 15; i < remaining.length(); i++) {
                    if (remaining[i] == '(') pd++;
                    else if (remaining[i] == ')') {
                        if (pd == 0) { close_paren = i; break; }
                        pd--;
                    }
                }
                if (close_paren != std::string::npos) {
                    // 检查括号后面是否是 AS alias
                    std::string after = trim_str(remaining.substr(close_paren + 1));
                    std::string after_lower = to_lower_str(after);
                    if (after_lower.find("as ") == 0 || after.empty()) {
                        // 去掉外层包裹，只保留括号内的内容
                        remaining = trim_str(remaining.substr(15, close_paren - 15));
                    }
                }
            }
        }

        // 按UNION分割，考虑括号
        std::string remaining_lower = to_lower_str(remaining);
        size_t pos = 0;
        while (pos < remaining.length()) {
            // 找到下一个不在括号内的UNION
            int pd = 0;
            size_t union_pos = std::string::npos;
            for (size_t i = pos; i < remaining.length(); i++) {
                if (remaining[i] == '(') pd++;
                else if (remaining[i] == ')') pd--;
                else if (pd == 0 && i + 6 <= remaining.length()) {
                    if (remaining_lower.substr(i, 6) == "union ") {
                        union_pos = i;
                        break;
                    }
                    // 也检查 UNION ALL
                    if (i + 10 <= remaining.length() && remaining_lower.substr(i, 10) == "union all ") {
                        union_pos = i;
                        break;
                    }
                }
            }
            if (union_pos == std::string::npos) {
                sub_queries.push_back(trim_str(remaining.substr(pos)));
                break;
            }
            sub_queries.push_back(trim_str(remaining.substr(pos, union_pos - pos)));
            // 跳过UNION [ALL]
            size_t next_pos = union_pos + 6;
            if (remaining_lower.substr(union_pos, 10) == "union all ") {
                next_pos = union_pos + 10;
            }
            // 跳过空格
            while (next_pos < remaining.length() && remaining[next_pos] == ' ') next_pos++;
            pos = next_pos;
        }
    }

    if (sub_queries.size() < 2) {
        throw InternalError("UNION requires at least two sub-queries");
    }

    // 去掉子查询外层括号
    for (auto &sq : sub_queries) {
        sq = trim_str(sq);
        while (sq.size() >= 2 && sq.front() == '(' && sq.back() == ')') {
            sq = trim_str(sq.substr(1, sq.size() - 2));
        }
    }

    // 3. 执行每个子查询
    std::vector<SubQueryResult> sub_results;
    for (auto &sq : sub_queries) {
        sub_results.push_back(execute_sub_select(sm_manager_, sq, context));
    }

    // 4. 验证列数一致
    int num_cols = sub_results[0].col_names.size();
    for (size_t i = 1; i < sub_results.size(); i++) {
        if ((int)sub_results[i].col_names.size() != num_cols) {
            throw InternalError("failure");
        }
    }

    // 5. 确定输出列类型（类型提升）
    std::vector<ColType> out_types(num_cols, TYPE_INT);
    std::vector<int> out_lens(num_cols, 0);
    std::vector<std::string> out_names = sub_results[0].col_names;

    for (int c = 0; c < num_cols; c++) {
        ColType promoted = sub_results[0].cols[c].type;
        int max_len = sub_results[0].cols[c].len;
        for (size_t si = 1; si < sub_results.size(); si++) {
            ColType cur = sub_results[si].cols[c].type;
            int cur_len = sub_results[si].cols[c].len;
            // 类型兼容性检查
            if (promoted == TYPE_STRING && cur != TYPE_STRING) {
                throw InternalError("failure");
            }
            if (cur == TYPE_STRING && promoted != TYPE_STRING) {
                throw InternalError("failure");
            }
            // 类型提升
            if (promoted == TYPE_INT && cur == TYPE_FLOAT) {
                promoted = TYPE_FLOAT;
            }
            if (cur == TYPE_INT && promoted == TYPE_FLOAT) {
                // 已经是float
            }
            max_len = std::max(max_len, cur_len);
        }
        out_types[c] = promoted;
        out_lens[c] = max_len;
    }

    // 6. 合并结果并去重
    // 将每行转为统一格式的字符串
    struct UnifiedRow {
        std::vector<std::string> vals;
        std::string key; // 用于去重
    };
    std::vector<UnifiedRow> all_rows;
    std::set<std::string> seen_keys;

    for (auto &sr : sub_results) {
        for (auto &row : sr.rows) {
            UnifiedRow ur;
            ur.vals.resize(num_cols);
            std::string key;
            for (int c = 0; c < num_cols; c++) {
                // 类型提升：INT -> FLOAT
                if (out_types[c] == TYPE_FLOAT && sr.cols[c].type == TYPE_INT) {
                    int int_val = std::stoi(row[c]);
                    float float_val = static_cast<float>(int_val);
                    ur.vals[c] = std::to_string(float_val);
                } else {
                    ur.vals[c] = row[c];
                }
            }
            // 构建去重key（使用提升后的类型进行比较）
            std::string dedup_key;
            for (int c = 0; c < num_cols; c++) {
                if (out_types[c] == TYPE_INT) {
                    dedup_key += std::to_string(std::stoi(ur.vals[c])) + "|";
                } else if (out_types[c] == TYPE_FLOAT) {
                    dedup_key += std::to_string(std::stof(ur.vals[c])) + "|";
                } else {
                    dedup_key += ur.vals[c] + "|";
                }
            }
            if (seen_keys.find(dedup_key) == seen_keys.end()) {
                seen_keys.insert(dedup_key);
                all_rows.push_back(ur);
            }
        }
    }

    // 7. ORDER BY
    if (!outer_order_by.empty()) {
        // 解析ORDER BY列
        auto ob_parts = split_str(outer_order_by, ",");
        struct OrderSpec { int col_idx; bool is_desc; };
        std::vector<OrderSpec> order_specs;
        for (auto &p : ob_parts) {
            p = trim_str(p);
            OrderSpec os;
            os.is_desc = false;
            std::string p_lower = to_lower_str(p);
            if (p_lower.find(" desc") != std::string::npos) {
                os.is_desc = true;
                p = trim_str(p.substr(0, p.length() - 5));
            } else if (p_lower.find(" asc") != std::string::npos) {
                p = trim_str(p.substr(0, p.length() - 4));
            }
            // 查找列索引（按列名）
            os.col_idx = -1;
            for (int c = 0; c < num_cols; c++) {
                if (to_lower_str(out_names[c]) == to_lower_str(p)) {
                    os.col_idx = c;
                    break;
                }
            }
            if (os.col_idx == -1) {
                // 尝试按列号（1-based）
                try {
                    os.col_idx = std::stoi(p) - 1;
                } catch (...) {}
            }
            if (os.col_idx < 0 || os.col_idx >= num_cols) {
                throw InternalError("failure");
            }
            order_specs.push_back(os);
        }

        std::sort(all_rows.begin(), all_rows.end(), [&](const UnifiedRow &a, const UnifiedRow &b) {
            for (auto &os : order_specs) {
                if (os.col_idx < 0 || os.col_idx >= num_cols) continue;
                if (out_types[os.col_idx] == TYPE_STRING) {
                    int cmp = a.vals[os.col_idx].compare(b.vals[os.col_idx]);
                    if (cmp != 0) return os.is_desc ? (cmp > 0) : (cmp < 0);
                } else {
                    double va = std::stod(a.vals[os.col_idx]);
                    double vb = std::stod(b.vals[os.col_idx]);
                    if (va != vb) return os.is_desc ? (va > vb) : (va < vb);
                }
            }
            return false;
        });
    }

    // 8. LIMIT
    if (limit_n >= 0 && (int)all_rows.size() > limit_n) {
        all_rows.resize(limit_n);
    }

    // 9. 输出
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "|";
    for (auto &name : out_names) outfile << " " << name << " |";
    outfile << "\n";

    RecordPrinter rec_printer(num_cols);
    rec_printer.print_separator(context);
    rec_printer.print_record(out_names, context);
    rec_printer.print_separator(context);

    size_t num_rec = 0;
    for (auto &ur : all_rows) {
        rec_printer.print_record(ur.vals, context);
        outfile << "|";
        for (auto &v : ur.vals) outfile << " " << v << " |";
        outfile << "\n";
        num_rec++;
    }
    outfile.close();
    rec_printer.print_separator(context);
    RecordPrinter::print_record_count(num_rec, context);
}

// EXPLAIN ANALYZE 输出格式的执行计划节点
struct ExplainNode {
    std::string type;
    std::vector<std::string> attrs;
    int rows = 0;
    std::vector<std::shared_ptr<ExplainNode>> children;
};

static std::string op_to_string(CompOp op) {
    switch (op) {
        case OP_EQ: return "=";
        case OP_NE: return "<>";
        case OP_LT: return "<";
        case OP_GT: return ">";
        case OP_LE: return "<=";
        case OP_GE: return ">=";
        default: return "";
    }
}

static std::string value_to_explain_string(const Value &val) {
    if (val.type == TYPE_INT) {
        return std::to_string(val.int_val);
    }
    if (val.type == TYPE_FLOAT) {
        // Use std::to_string for consistent formatting, trailing zeros will be cleaned up later
        std::string s = std::to_string(val.float_val);
        return s;
    }
    return "'" + val.str_val + "'";
}

static std::string col_to_explain_string(const TabCol &col) {
    if (col.tab_name.empty()) {
        return col.col_name;
    }
    return col.tab_name + "." + col.col_name;
}

static std::string condition_to_explain_string(const Condition &cond) {
    std::string out = col_to_explain_string(cond.lhs_col) + op_to_string(cond.op);
    if (cond.is_rhs_val) {
        out += value_to_explain_string(cond.rhs_val);
    } else {
        out += col_to_explain_string(cond.rhs_col);
    }
    return out;
}

static std::string list_attr(const std::string &name, std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::string out = name + "=[";
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0) out += ", ";
        out += values[i];
    }
    out += "]";
    return out;
}

static void add_required_col(std::map<std::string, std::vector<std::string>> &required, const TabCol &col) {
    if (col.tab_name.empty() || col.col_name.empty()) {
        return;
    }
    auto &cols = required[col.tab_name];
    if (std::find(cols.begin(), cols.end(), col.col_name) == cols.end()) {
        cols.push_back(col.col_name);
    }
}

static void collect_join_cols(std::shared_ptr<Plan> plan, std::map<std::string, std::vector<std::string>> &required) {
    if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        for (auto &cond : x->conds_) {
            add_required_col(required, cond.lhs_col);
            if (!cond.is_rhs_val) {
                add_required_col(required, cond.rhs_col);
            }
        }
        collect_join_cols(x->left_, required);
        collect_join_cols(x->right_, required);
    } else if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        collect_join_cols(x->subplan_, required);
    } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        collect_join_cols(x->subplan_, required);
    }
}

static std::shared_ptr<ExplainNode> wrap_pushdown_project(const std::string &tab_name,
                                                          const std::map<std::string, std::vector<std::string>> *required,
                                                          std::shared_ptr<ExplainNode> child) {
    if (required == nullptr) {
        return child;
    }
    auto it = required->find(tab_name);
    if (it == required->end() || it->second.empty()) {
        return child;
    }
    auto node = std::make_shared<ExplainNode>();
    node->type = "Project";
    std::vector<std::string> cols;
    for (size_t i = 0; i < it->second.size(); i++) {
        cols.push_back(tab_name + "." + it->second[i]);
    }
    node->attrs.push_back(list_attr("columns", cols));
    node->children.push_back(child);
    return node;
}

static void multiply_rows(std::shared_ptr<ExplainNode> node, int factor) {
    if (!node) return;
    node->rows *= factor;
    for (auto &child : node->children) {
        multiply_rows(child, factor);
    }
}

static void set_rows_recursive(std::shared_ptr<ExplainNode> node, int rows) {
    if (!node) return;
    node->rows = rows;
    for (auto &child : node->children) {
        set_rows_recursive(child, rows);
    }
}

static std::shared_ptr<ExplainNode> build_explain_tree(SmManager *sm_manager, std::shared_ptr<Plan> plan,
                                                       bool is_select_star = false,
                                                       const std::map<std::string, std::vector<std::string>> *required = nullptr,
                                                       bool enable_pushdown_project = false) {
    if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        auto node = std::make_shared<ExplainNode>();
        node->type = "Project";
        if (is_select_star) {
            node->attrs.push_back("columns=[*]");
        } else {
            std::vector<std::string> cols;
            for (size_t i = 0; i < x->sel_cols_.size(); i++) {
                cols.push_back(col_to_explain_string(x->sel_cols_[i]));
            }
            node->attrs.push_back(list_attr("columns", cols));
        }
        if (!is_select_star && std::dynamic_pointer_cast<JoinPlan>(x->subplan_)) {
            std::map<std::string, std::vector<std::string>> pushed_cols;
            collect_join_cols(x->subplan_, pushed_cols);
            for (auto &sel_col : x->sel_cols_) {
                add_required_col(pushed_cols, sel_col);
            }
            node->children.push_back(build_explain_tree(sm_manager, x->subplan_, false, &pushed_cols, true));
        } else {
            node->children.push_back(build_explain_tree(sm_manager, x->subplan_, false, required, enable_pushdown_project));
        }
        return node;
    } else if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        auto make_scan_node = [&](std::shared_ptr<ScanPlan> sp) {
            auto sn = std::make_shared<ExplainNode>();
            sn->type = "Scan";
            sn->attrs.push_back("table=" + sp->tab_name_);
            sn->attrs.push_back(std::string("type=") + (sp->tag == T_SeqScan ? "SeqScan" : "IndexScan"));
            if (sp->tag == T_IndexScan && !sp->index_col_names_.empty()) {
                std::string index_attr = "using_index=(";
                for (size_t i = 0; i < sp->index_col_names_.size(); i++) {
                    if (i > 0) index_attr += ", ";
                    index_attr += sp->index_col_names_[i];
                }
                index_attr += ")";
                sn->attrs.push_back(index_attr);
            }
            auto fh = sm_manager->fhs_.at(sp->tab_name_).get();
            int count = 0; RmScan s(fh); while (!s.is_end()) { count++; s.next(); }
            sn->rows = count;
            return sn;
        };
        std::shared_ptr<ExplainNode> result;
        if (!x->conds_.empty()) {
            auto filter_node = std::make_shared<ExplainNode>();
            filter_node->type = "Filter";
            std::vector<std::string> conds;
            for (size_t i = 0; i < x->conds_.size(); i++) {
                conds.push_back(condition_to_explain_string(x->conds_[i]));
            }
            filter_node->attrs.push_back(list_attr("condition", conds));
            filter_node->children.push_back(make_scan_node(x));
            result = filter_node;
        } else {
            result = make_scan_node(x);
        }
        if (enable_pushdown_project) {
            result = wrap_pushdown_project(x->tab_name_, required, result);
        }
        return result;
    } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        auto node = std::make_shared<ExplainNode>();
        node->type = "Join";
        std::function<void(std::shared_ptr<Plan>, std::vector<std::string>&)> collect_tabs;
        collect_tabs = [&](std::shared_ptr<Plan> p, std::vector<std::string> &tabs) {
            if (auto s = std::dynamic_pointer_cast<ScanPlan>(p)) tabs.push_back(s->tab_name_);
            else if (auto j = std::dynamic_pointer_cast<JoinPlan>(p)) { collect_tabs(j->left_, tabs); collect_tabs(j->right_, tabs); }
            else if (auto pr = std::dynamic_pointer_cast<ProjectionPlan>(p)) collect_tabs(pr->subplan_, tabs);
        };
        std::vector<std::string> tabs;
        collect_tabs(x, tabs);
        node->attrs.push_back(list_attr("tables", tabs));
        if (!x->conds_.empty()) {
            std::vector<std::string> conds;
            for (size_t i = 0; i < x->conds_.size(); i++) {
                conds.push_back(condition_to_explain_string(x->conds_[i]));
            }
            node->attrs.push_back(list_attr("condition", conds));
        }
        node->children.push_back(build_explain_tree(sm_manager, x->left_, false, required, enable_pushdown_project));
        node->children.push_back(build_explain_tree(sm_manager, x->right_, false, required, enable_pushdown_project));
        return node;
    } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        return build_explain_tree(sm_manager, x->subplan_, false, required, enable_pushdown_project);
    }
    return nullptr;
}

static void format_explain_tree(std::shared_ptr<ExplainNode> node, std::string &output, int depth = 0) {
    if (!node) return;
    output += std::string(depth, '\t');
    output += node->type + "(";
    for (size_t i = 0; i < node->attrs.size(); i++) { if (i > 0) output += ", "; output += node->attrs[i]; }
    output += ", rows=" + std::to_string(node->rows) + ")\n";
    for (auto &child : node->children) format_explain_tree(child, output, depth + 1);
}

static void sort_explain_attr_lists(std::string &output, const std::string &attr) {
    size_t pos = 0;
    while ((pos = output.find(attr, pos)) != std::string::npos) {
        size_t start = pos + attr.length();
        size_t end = output.find(']', start);
        if (end == std::string::npos) {
            break;
        }
        std::string content = output.substr(start, end - start);
        if (!content.empty() && content != "*") {
            auto items = split_str(content, ",");
            for (auto &item : items) {
                item = trim_str(item);
            }
            std::sort(items.begin(), items.end());
            std::string sorted;
            for (size_t i = 0; i < items.size(); i++) {
                if (i > 0) sorted += ", ";
                sorted += items[i];
            }
            output.replace(start, end - start, sorted);
            pos = start + sorted.length();
        } else {
            pos = end + 1;
        }
    }
}

static int count_executor_rows(AbstractExecutor *executor) {
    int count = 0;
    for (executor->beginTuple(); !executor->is_end(); executor->nextTuple()) count++;
    return count;
}

static bool plan_uses_index_scan(std::shared_ptr<Plan> plan) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        return x->tag == T_IndexScan;
    }
    if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        return plan_uses_index_scan(x->subplan_);
    }
    if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
        return plan_uses_index_scan(x->subplan_);
    }
    return false;
}

void QlManager::handle_explain_analyze(const std::string &sql, Context *context) {
    std::string sql_lower = to_lower_str(sql);
    size_t pos = sql_lower.find("explain analyze ");
    if (pos == std::string::npos) throw InternalError("Invalid EXPLAIN ANALYZE syntax");
    std::string inner_sql = trim_str(sql.substr(pos + 16));
    if (!inner_sql.empty() && inner_sql.back() == ';') inner_sql.pop_back();
    inner_sql = trim_str(inner_sql);

    // 使用共享的SQL预处理函数
    auto rewrite_result = rewrite_sql_for_parser(inner_sql);
    inner_sql = rewrite_result.sql;

    ast::parse_tree = nullptr;
    std::string sql_semi = inner_sql;
    if (!sql_semi.empty() && sql_semi.back() != ';') sql_semi += ';';
    YY_BUFFER_STATE buf = yy_scan_string(sql_semi.c_str());
    int parse_ok = yyparse();
    yy_delete_buffer(buf);
    if (parse_ok != 0 || ast::parse_tree == nullptr) throw InternalError("Parse failed for EXPLAIN ANALYZE");

    auto analyze_inst = std::make_unique<Analyze>(sm_manager_);
    auto query = analyze_inst->do_analyze(ast::parse_tree);
    auto planner_inst = std::make_unique<Planner>(sm_manager_);
    auto optimizer_inst = std::make_unique<Optimizer>(sm_manager_, planner_inst.get());
    auto plan = optimizer_inst->plan_query(query, context);

    auto dml_plan = std::dynamic_pointer_cast<DMLPlan>(plan);
    if (!dml_plan || dml_plan->tag != T_select) throw InternalError("EXPLAIN ANALYZE only supports SELECT");

    auto explain_root = build_explain_tree(sm_manager_, dml_plan->subplan_, rewrite_result.is_select_star);

    // 填充行数 - 通过执行器包装器统计每层行数
    // 对于简单SELECT（单表），直接执行获取各层行数
    auto root_executor = build_executor_tree(sm_manager_, dml_plan->subplan_, context);
    int total_rows = count_executor_rows(root_executor.get());

    // 递归填充行数
    std::function<void(std::shared_ptr<ExplainNode>, std::shared_ptr<Plan>)> fill_rows;
    fill_rows = [&](std::shared_ptr<ExplainNode> node, std::shared_ptr<Plan> p) {
        if (!node || !p) return;
        if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(p)) {
            node->rows = total_rows;
            if (!node->children.empty()) fill_rows(node->children[0], x->subplan_);
        } else if (auto x = std::dynamic_pointer_cast<ScanPlan>(p)) {
            // ScanPlan可能产生Filter+Scan或纯Scan
            if (node->type == "Project") {
                if (!node->children.empty()) {
                    fill_rows(node->children[0], p);
                    node->rows = node->children[0]->rows;
                }
            } else if (node->type == "Filter") {
                // Filter行数 = 执行带条件的Scan后的行数
                auto exec = build_executor_tree(sm_manager_, p, context);
                node->rows = count_executor_rows(exec.get());
                if (!node->children.empty()) fill_rows(node->children[0], p);
            } else if (node->type == "Scan") {
                // 行数已在build_explain_tree中设置
            }
        } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(p)) {
            auto exec = build_executor_tree(sm_manager_, p, context);
            node->rows = count_executor_rows(exec.get());
            if (node->children.size() >= 2) {
                fill_rows(node->children[0], x->left_);
                int left_rows = node->children[0]->rows;
                fill_rows(node->children[1], x->right_);
                if (plan_uses_index_scan(x->right_)) {
                    set_rows_recursive(node->children[1], total_rows);
                } else {
                    multiply_rows(node->children[1], left_rows);
                }
            }
        } else if (auto x = std::dynamic_pointer_cast<SortPlan>(p)) {
            fill_rows(node, x->subplan_);
        }
    };
    fill_rows(explain_root, dml_plan->subplan_);

    std::string output;
    format_explain_tree(explain_root, output);

    // 将表名替换回别名（用于condition和columns显示）
    for (auto &[alias, table] : rewrite_result.alias_to_table) {
        // 替换 "table." 为 "alias." （仅在condition和columns中）
        std::string from = table + ".";
        std::string to = alias + ".";
        size_t pos = 0;
        while ((pos = output.find(from, pos)) != std::string::npos) {
            output.replace(pos, from.length(), to);
            pos += to.length();
        }
    }

    sort_explain_attr_lists(output, "columns=[");
    sort_explain_attr_lists(output, "condition=[");
    sort_explain_attr_lists(output, "tables=[");

    // 修复浮点数显示: 去掉多余的0
    // 1000.000000 -> 1000, 700.500000 -> 700.5
    {
        size_t pos = 0;
        while (pos < output.size()) {
            // 找到小数点
            size_t dot_pos = output.find('.', pos);
            if (dot_pos == std::string::npos) break;
            // 检查小数点前后是否是数字
            if (dot_pos == 0 || !isdigit(output[dot_pos - 1])) {
                pos = dot_pos + 1;
                continue;
            }
            // 找到小数部分的结尾
            size_t end = dot_pos + 1;
            while (end < output.size() && isdigit(output[end])) end++;
            // 检查小数部分是否全是0
            std::string frac = output.substr(dot_pos + 1, end - dot_pos - 1);
            bool all_zeros = true;
            for (char c : frac) {
                if (c != '0') { all_zeros = false; break; }
            }
            if (all_zeros) {
                // 去掉 ".000000" 部分
                output.erase(dot_pos, end - dot_pos);
                pos = dot_pos;
            } else {
                // 去掉尾部的0: 700.500000 -> 700.5
                size_t last_nonzero = dot_pos;
                for (size_t k = dot_pos + 1; k < end; k++) {
                    if (output[k] != '0') last_nonzero = k;
                }
                if (last_nonzero > dot_pos) {
                    // 保留到最后一个非零位
                    output.erase(last_nonzero + 1, end - last_nonzero - 1);
                    pos = last_nonzero + 1;
                } else {
                    // 全是0，去掉整个小数部分
                    output.erase(dot_pos, end - dot_pos);
                    pos = dot_pos;
                }
            }
        }
    }

    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << output;
    outfile.close();

    memcpy(context->data_send_ + *(context->offset_), output.c_str(), output.size());
    *(context->offset_) += output.size();
}
