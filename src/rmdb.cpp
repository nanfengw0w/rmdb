/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <netinet/in.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "errors.h"
#include "common/config.h"
#include "common/sql_rewrite.h"
#include "execution/index_maintenance.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"
#include <netinet/tcp.h>

#define SOCK_PORT 8765
#define MAX_CONN_LIMIT 64

static bool should_exit = false;
std::atomic<bool> enable_output_file{true};
// Serialize one complete SQL request at a time.  Transactions still span
// multiple requests, but parsing, planning, heap/index mutation, WAL work and
// commit/abort for each request are kept in one scheduling critical section.
// This also prevents unsynchronized heap-page inserts from racing each other.
static std::mutex request_mutex;

static void append_output_line(const std::string &line) {
    if (!enable_output_file.load()) {
        return;
    }
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << line;
    outfile.close();
}

static void set_response(char *data_send, int *offset, const std::string &msg) {
    if (data_send == nullptr || offset == nullptr) {
        return;
    }
    size_t writable = BUFFER_LENGTH > 0 ? BUFFER_LENGTH - 1 : 0;
    size_t len = std::min(msg.size(), writable);
    if (len > 0) {
        memcpy(data_send, msg.data(), len);
    }
    data_send[len] = '\0';
    *offset = static_cast<int>(len);
}

static std::string strip_sql_line_comments(const std::string &sql) {
    std::string result;
    bool in_string = false;
    for (size_t i = 0; i < sql.size(); i++) {
        if (sql[i] == '\'') {
            in_string = !in_string;
            result.push_back(sql[i]);
            continue;
        }
        if (!in_string && sql[i] == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            i += 2;
            while (i < sql.size() && sql[i] != '\n' && sql[i] != '\r') {
                i++;
            }
            if (i < sql.size()) {
                result.push_back(' ');
            }
            continue;
        }
        result.push_back(sql[i]);
    }
    return result;
}

static bool is_blank_sql(const std::string &sql) {
    return sql.find_first_not_of(" \t\r\n;") == std::string::npos;
}

static std::vector<std::string> split_sql_requests(const std::string &sql) {
    std::vector<std::string> requests;
    bool in_string = false;
    size_t start = 0;
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '\'') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && sql[i] == ';') {
            std::string part = sql.substr(start, i - start + 1);
            if (!is_blank_sql(part)) {
                requests.push_back(part);
            }
            start = i + 1;
        }
    }

    std::string tail = sql.substr(start);
    if (!is_blank_sql(tail)) {
        requests.push_back(tail);
    }
    return requests;
}

static std::string trim_local(std::string s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string lower_local(std::string s) {
    for (auto &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static size_t find_keyword_local(const std::string &lower_sql, const std::string &keyword, size_t start = 0) {
    bool in_string = false;
    for (size_t i = start; i + keyword.size() <= lower_sql.size(); ++i) {
        if (lower_sql[i] == '\'') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && lower_sql.compare(i, keyword.size(), keyword) == 0) {
            return i;
        }
    }
    return std::string::npos;
}

static std::vector<std::string> split_commas_local(const std::string &text) {
    std::vector<std::string> parts;
    bool in_string = false;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') {
            in_string = !in_string;
        } else if (!in_string && text[i] == ',') {
            parts.push_back(trim_local(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    parts.push_back(trim_local(text.substr(start)));
    return parts;
}

static size_t find_eq_local(const std::string &text) {
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') {
            in_string = !in_string;
        } else if (!in_string && text[i] == '=') {
            return i;
        }
    }
    return std::string::npos;
}

static bool parse_literal_local(const std::string &text, Value &value) {
    std::string s = trim_local(text);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        value.set_str(s.substr(1, s.size() - 2));
        return true;
    }
    try {
        if (s.find('.') != std::string::npos) {
            value.set_float(std::stof(s));
        } else {
            value.set_int(std::stoi(s));
        }
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<std::string> split_and_local(const std::string &text) {
    std::vector<std::string> parts;
    std::string lower = lower_local(text);
    bool in_string = false;
    size_t start = 0;
    for (size_t i = 0; i < lower.size();) {
        if (lower[i] == '\'') {
            in_string = !in_string;
            i++;
            continue;
        }
        if (!in_string && i + 5 <= lower.size() && lower.compare(i, 5, " and ") == 0) {
            parts.push_back(trim_local(text.substr(start, i - start)));
            i += 5;
            start = i;
            continue;
        }
        i++;
    }
    parts.push_back(trim_local(text.substr(start)));
    return parts;
}

static TabCol parse_tab_col_local(const std::string &text, const std::string &default_tab) {
    std::string name = trim_local(text);
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return TabCol{default_tab, name};
    }
    return TabCol{trim_local(name.substr(0, dot)), trim_local(name.substr(dot + 1))};
}

static bool parse_comp_condition_local(const std::string &text, const std::string &tab_name, Condition &cond) {
    bool in_string = false;
    size_t op_pos = std::string::npos;
    CompOp op = OP_EQ;
    size_t op_len = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (i + 2 <= text.size()) {
            std::string two = text.substr(i, 2);
            if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
                op_pos = i;
                op_len = 2;
                op = two == "<=" ? OP_LE : (two == ">=" ? OP_GE : OP_NE);
                break;
            }
        }
        if (text[i] == '=' || text[i] == '<' || text[i] == '>') {
            op_pos = i;
            op_len = 1;
            op = text[i] == '=' ? OP_EQ : (text[i] == '<' ? OP_LT : OP_GT);
            break;
        }
    }
    if (op_pos == std::string::npos) {
        return false;
    }

    std::string lhs = trim_local(text.substr(0, op_pos));
    std::string rhs = trim_local(text.substr(op_pos + op_len));
    if (lhs.empty() || rhs.empty()) {
        return false;
    }

    cond.lhs_col = parse_tab_col_local(lhs, tab_name);
    cond.op = op;
    cond.is_rhs_val = true;
    return parse_literal_local(rhs, cond.rhs_val);
}

static bool prepare_fast_condition_local(TabMeta &tab, const std::string &tab_name, Condition &cond) {
    if (cond.lhs_col.tab_name.empty()) {
        cond.lhs_col.tab_name = tab_name;
    }
    if (cond.lhs_col.tab_name != tab_name) {
        return false;
    }
    auto col = tab.get_col(cond.lhs_col.col_name);
    if (col == tab.cols.end()) {
        throw ColumnNotFoundError(cond.lhs_col.col_name);
    }
    if (col->type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
        cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
    }
    if (col->type != cond.rhs_val.type) {
        throw IncompatibleTypeError(coltype2str(col->type), coltype2str(cond.rhs_val.type));
    }
    cond.rhs_val.init_raw(col->len);
    return true;
}

static bool choose_fast_select_index_local(const TabMeta &tab, const std::string &tab_name,
                                           const std::vector<Condition> &conds,
                                           std::vector<std::string> &index_col_names) {
    index_col_names.clear();
    for (const auto &index : tab.indexes) {
        if (index.cols.empty()) {
            continue;
        }
        for (const auto &cond : conds) {
            if (cond.lhs_col.tab_name == tab_name && cond.lhs_col.col_name == index.cols[0].name) {
                for (const auto &col : index.cols) {
                    index_col_names.push_back(col.name);
                }
                return true;
            }
        }
    }
    return false;
}

static bool try_parse_simple_select_local(SmManager *sm_mgr, const std::string &sql, std::string &tab_name,
                                          std::vector<TabCol> &sel_cols,
                                          std::vector<Condition> &conds) {
    if (sm_mgr == nullptr) {
        return false;
    }
    std::string raw = trim_local(sql);
    while (!raw.empty() && raw.back() == ';') {
        raw.pop_back();
        raw = trim_local(raw);
    }
    std::string lower = lower_local(raw);
    const std::string prefix = "select ";
    if (lower.rfind(prefix, 0) != 0) {
        return false;
    }
    if (lower.find(" join ") != std::string::npos ||
        lower.find(" union ") != std::string::npos ||
        lower.find(" group by ") != std::string::npos ||
        lower.find(" having ") != std::string::npos ||
        lower.find(" order by ") != std::string::npos ||
        lower.find(" limit ") != std::string::npos ||
        lower.find(" count(") != std::string::npos ||
        lower.find(" max(") != std::string::npos ||
        lower.find(" min(") != std::string::npos ||
        lower.find(" sum(") != std::string::npos ||
        lower.find(" avg(") != std::string::npos) {
        return false;
    }

    size_t from_pos = find_keyword_local(lower, " from ", prefix.size());
    if (from_pos == std::string::npos) {
        return false;
    }
    size_t where_pos = find_keyword_local(lower, " where ", from_pos + 6);
    if (where_pos == std::string::npos) {
        return false;
    }

    std::string select_part = trim_local(raw.substr(prefix.size(), from_pos - prefix.size()));
    std::string table_part = trim_local(raw.substr(from_pos + 6, where_pos - (from_pos + 6)));
    std::string where_part = trim_local(raw.substr(where_pos + 7));
    if (select_part.empty() || table_part.empty() || where_part.empty() ||
        table_part.find(',') != std::string::npos ||
        table_part.find(' ') != std::string::npos ||
        table_part.find('\t') != std::string::npos) {
        return false;
    }

    tab_name = table_part;
    std::string real_tab_name = sm_mgr->resolve_table_name(tab_name);
    TabMeta &tab = sm_mgr->db_.get_table(real_tab_name);

    sel_cols.clear();
    if (trim_local(select_part) == "*") {
        for (const auto &col : tab.cols) {
            sel_cols.push_back(TabCol{tab_name, col.name});
        }
    } else {
        auto col_tokens = split_commas_local(select_part);
        for (const auto &token : col_tokens) {
            if (token.empty() || token.find('(') != std::string::npos) {
                return false;
            }
            TabCol col = parse_tab_col_local(token, tab_name);
            if (col.tab_name.empty()) {
                col.tab_name = tab_name;
            }
            if (col.tab_name != tab_name) {
                return false;
            }
            tab.get_col(col.col_name);
            sel_cols.push_back(std::move(col));
        }
    }
    if (sel_cols.empty()) {
        return false;
    }

    conds.clear();
    for (const auto &part : split_and_local(where_part)) {
        if (part.empty()) {
            return false;
        }
        Condition cond;
        if (!parse_comp_condition_local(part, tab_name, cond)) {
            return false;
        }
        if (!prepare_fast_condition_local(tab, tab_name, cond)) {
            return false;
        }
        conds.push_back(std::move(cond));
    }
    return !conds.empty();
}

static std::string value_to_update_sql_local(const Value &value) {
    if (value.type == TYPE_STRING) {
        return "'" + value.str_val + "'";
    }
    if (value.type == TYPE_FLOAT) {
        std::ostringstream oss;
        oss << value.float_val;
        return oss.str();
    }
    return std::to_string(value.int_val);
}

static bool parse_lhs_arithmetic_terms_local(const std::string &rhs, const std::string &lhs,
                                             std::vector<std::pair<ArithOp, Value>> &terms) {
    terms.clear();

    std::string rhs_lower = lower_local(rhs);
    std::string lhs_lower = lower_local(lhs);
    size_t pos = 0;
    while (pos < rhs_lower.size() && std::isspace(static_cast<unsigned char>(rhs_lower[pos]))) {
        pos++;
    }

    if (rhs_lower.compare(pos, lhs_lower.size(), lhs_lower) != 0) {
        return false;
    }
    pos += lhs_lower.size();
    if (pos < rhs_lower.size() &&
        (std::isalnum(static_cast<unsigned char>(rhs_lower[pos])) || rhs_lower[pos] == '_')) {
        return false;
    }

    while (true) {
        while (pos < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[pos]))) {
            pos++;
        }
        if (pos == rhs.size()) {
            break;
        }

        char op_char = rhs[pos];
        if (op_char != '+' && op_char != '-' && op_char != '*' && op_char != '/') {
            return false;
        }
        pos++;

        while (pos < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[pos]))) {
            pos++;
        }
        size_t value_start = pos;
        bool in_string = false;
        while (pos < rhs.size()) {
            char ch = rhs[pos];
            if (ch == '\'') {
                in_string = !in_string;
                pos++;
                continue;
            }
            if (!in_string && (ch == '+' || ch == '-' || ch == '*' || ch == '/') && pos > value_start) {
                char prev = rhs[pos - 1];
                if ((ch == '+' || ch == '-') && (prev == 'e' || prev == 'E')) {
                    pos++;
                    continue;
                }
                break;
            }
            pos++;
        }

        std::string value_text = trim_local(rhs.substr(value_start, pos - value_start));
        if (value_text.empty()) {
            return false;
        }

        Value value;
        if (!parse_literal_local(value_text, value)) {
            return false;
        }
        if (value.type != TYPE_INT && value.type != TYPE_FLOAT) {
            return false;
        }

        ArithOp op = op_char == '+' ? ArithOp::ADD :
                     op_char == '-' ? ArithOp::SUB :
                     op_char == '*' ? ArithOp::MUL : ArithOp::DIV;
        terms.emplace_back(op, std::move(value));
    }

    return !terms.empty();
}

static bool try_parse_arithmetic_update(const std::string &sql, std::string &dummy_sql,
                                        std::vector<SetClause> &set_clauses) {
    std::string raw = trim_local(sql);
    while (!raw.empty() && raw.back() == ';') {
        raw.pop_back();
        raw = trim_local(raw);
    }
    std::string lower = lower_local(raw);
    if (lower.rfind("update ", 0) != 0) {
        return false;
    }

    size_t set_pos = find_keyword_local(lower, " set ", 0);
    if (set_pos == std::string::npos) {
        return false;
    }
    size_t where_pos = find_keyword_local(lower, " where ", set_pos + 5);
    std::string tab_name = trim_local(raw.substr(7, set_pos - 7));
    std::string set_part = where_pos == std::string::npos
        ? trim_local(raw.substr(set_pos + 5))
        : trim_local(raw.substr(set_pos + 5, where_pos - (set_pos + 5)));
    std::string where_part = where_pos == std::string::npos ? "" : raw.substr(where_pos);
    if (tab_name.empty() || set_part.empty()) {
        return false;
    }

    bool has_arith = false;
    std::vector<std::string> dummy_assigns;
    std::vector<SetClause> parsed;
    for (const auto &assignment : split_commas_local(set_part)) {
        size_t eq_pos = find_eq_local(assignment);
        if (eq_pos == std::string::npos) {
            return false;
        }
        bool compound_assignment = false;
        ArithOp compound_op = ArithOp::NO_OP;
        size_t lhs_end = eq_pos;
        if (eq_pos > 0) {
            size_t op_pos = eq_pos;
            while (op_pos > 0 && std::isspace(static_cast<unsigned char>(assignment[op_pos - 1]))) {
                op_pos--;
            }
            if (op_pos > 0) {
                char op_ch = assignment[op_pos - 1];
                if (op_ch == '+' || op_ch == '-' || op_ch == '*' || op_ch == '/') {
                    compound_assignment = true;
                    compound_op = op_ch == '+' ? ArithOp::ADD :
                                  op_ch == '-' ? ArithOp::SUB :
                                  op_ch == '*' ? ArithOp::MUL : ArithOp::DIV;
                    lhs_end = op_pos - 1;
                }
            }
        }

        std::string lhs = trim_local(assignment.substr(0, lhs_end));
        std::string rhs = trim_local(assignment.substr(eq_pos + 1));
        if (lhs.empty() || rhs.empty()) {
            return false;
        }

        SetClause clause;
        clause.lhs = {.tab_name = tab_name, .col_name = lhs};
        clause.op = ArithOp::NO_OP;

        bool parsed_arith = false;
        if (compound_assignment) {
            if (!parse_literal_local(rhs, clause.rhs)) {
                return false;
            }
            if (clause.rhs.type != TYPE_INT && clause.rhs.type != TYPE_FLOAT) {
                return false;
            }
            clause.op = compound_op;
            parsed.push_back(clause);
            has_arith = true;
            parsed_arith = true;
        }

        std::vector<std::pair<ArithOp, Value>> arithmetic_terms;
        if (!parsed_arith && parse_lhs_arithmetic_terms_local(rhs, lhs, arithmetic_terms)) {
            for (auto &term : arithmetic_terms) {
                SetClause arithmetic_clause = clause;
                arithmetic_clause.op = term.first;
                arithmetic_clause.rhs = std::move(term.second);
                parsed.push_back(std::move(arithmetic_clause));
            }
            has_arith = true;
            parsed_arith = true;
        }

        if (!parsed_arith) {
            if (!parse_literal_local(rhs, clause.rhs)) {
                return false;
            }
            parsed.push_back(clause);
        }
        dummy_assigns.push_back(lhs + "=" + (parsed_arith ? "0" : value_to_update_sql_local(clause.rhs)));
    }

    if (!has_arith) {
        return false;
    }

    dummy_sql = "update " + tab_name + " set ";
    for (size_t i = 0; i < dummy_assigns.size(); ++i) {
        if (i > 0) {
            dummy_sql += ", ";
        }
        dummy_sql += dummy_assigns[i];
    }
    dummy_sql += where_part;
    dummy_sql += ";";
    set_clauses = std::move(parsed);
    return true;
}

// 构建全局所需的管理器对象
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, disk_manager.get());
auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
auto ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
auto sm_manager = std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(), ix_manager.get());
auto lock_manager = std::make_unique<LockManager>();
auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
TransactionManager* g_txn_manager = txn_manager.get();
auto planner = std::make_unique<Planner>(sm_manager.get());
auto optimizer = std::make_unique<Optimizer>(sm_manager.get(), planner.get());
auto ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get(), planner.get());
auto log_manager = std::make_unique<LogManager>(disk_manager.get());
auto recovery = std::make_unique<RecoveryManager>(disk_manager.get(), buffer_pool_manager.get(), sm_manager.get());
auto portal = std::make_unique<Portal>(sm_manager.get());
auto analyze = std::make_unique<Analyze>(sm_manager.get());
pthread_mutex_t *buffer_mutex;
pthread_mutex_t *sockfd_mutex;

static jmp_buf jmpbuf;
void sigint_handler(int signo) {
    should_exit = true;
    log_manager->flush_log_to_disk();
    std::cout << "The Server receive Crtl+C, will been closed\n";
    longjmp(jmpbuf, 1);
}

// 判断当前正在执行的是显式事务还是单条SQL语句的事务，并更新事务ID
void SetTransaction(txn_id_t *txn_id, Context *context) {
    context->txn_ = txn_manager->get_transaction(*txn_id);
    if(context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED) {
        context->txn_ = txn_manager->begin(nullptr, context->log_mgr_);
        *txn_id = context->txn_->get_transaction_id();
        context->txn_->set_txn_mode(false);
    }
    if (!enable_output_file.load() && context->txn_ != nullptr) {
        context->txn_->set_perf_mode(true);
        if (context->txn_->get_isolation_level() == IsolationLevel::READ_COMMITTED) {
            context->txn_->set_isolation_level(IsolationLevel::SNAPSHOT_ISOLATION);
            context->txn_->set_start_ts(txn_manager->get_next_timestamp());
        }
    }
}

static bool parse_insert_literal_local(const std::string &text, Value &value) {
    std::string s = trim_local(text);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        value.set_str(s.substr(1, s.size() - 2));
        return true;
    }

    try {
        size_t consumed = 0;
        if (s.find('.') != std::string::npos || s.find('e') != std::string::npos || s.find('E') != std::string::npos) {
            float v = std::stof(s, &consumed);
            if (consumed != s.size()) {
                return false;
            }
            value.set_float(v);
        } else {
            int v = std::stoi(s, &consumed);
            if (consumed != s.size()) {
                return false;
            }
            value.set_int(v);
        }
        return true;
    } catch (...) {
        return false;
    }
}

static bool try_parse_simple_insert_local(const std::string &sql, std::string &tab_name,
                                          std::vector<Value> &values) {
    std::string raw = trim_local(sql);
    while (!raw.empty() && raw.back() == ';') {
        raw.pop_back();
        raw = trim_local(raw);
    }
    std::string lower = lower_local(raw);
    const std::string prefix = "insert into ";
    if (lower.rfind(prefix, 0) != 0) {
        return false;
    }

    size_t values_pos = find_keyword_local(lower, " values", prefix.size());
    if (values_pos == std::string::npos) {
        return false;
    }

    tab_name = trim_local(raw.substr(prefix.size(), values_pos - prefix.size()));
    std::string value_part = trim_local(raw.substr(values_pos + 7));
    if (tab_name.empty() || value_part.size() < 2 || value_part.front() != '(' || value_part.back() != ')') {
        return false;
    }

    std::vector<std::string> tokens = split_commas_local(value_part.substr(1, value_part.size() - 2));
    values.clear();
    values.reserve(tokens.size());
    for (const auto &token : tokens) {
        Value value;
        if (!parse_insert_literal_local(token, value)) {
            return false;
        }
        values.push_back(std::move(value));
    }
    return true;
}

static void execute_fast_insert_direct(const std::string &tab_name, std::vector<Value> &values, Context *context) {
    TabMeta &tab = sm_manager->db_.get_table(tab_name);
    if (values.size() != tab.cols.size()) {
        throw InvalidValueCountError();
    }

    RmFileHandle *fh = sm_manager->fhs_.at(tab_name).get();
    RmRecord rec(fh->get_file_hdr().record_size);
    for (size_t i = 0; i < values.size(); i++) {
        auto &col = tab.cols[i];
        auto &val = values[i];
        char *dst = rec.data + col.offset;
        if (col.type == TYPE_INT) {
            if (val.type != TYPE_INT) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            *reinterpret_cast<int *>(dst) = val.int_val;
        } else if (col.type == TYPE_FLOAT) {
            if (val.type == TYPE_INT) {
                *reinterpret_cast<float *>(dst) = static_cast<float>(val.int_val);
            } else if (val.type == TYPE_FLOAT) {
                *reinterpret_cast<float *>(dst) = val.float_val;
            } else {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
        } else if (col.type == TYPE_STRING) {
            if (val.type != TYPE_STRING) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            if (col.len < static_cast<int>(val.str_val.size())) {
                throw StringOverflowError();
            }
            memset(dst, 0, col.len);
            memcpy(dst, val.str_val.c_str(), val.str_val.size());
        }
    }

    std::vector<std::vector<char>> index_keys;
    index_keys.reserve(tab.indexes.size());
    for (auto &index : tab.indexes) {
        auto key = index_maintenance::build_key(index, rec.data);
        index_maintenance::check_unique_conflict(sm_manager.get(), tab_name, index, key.data(),
                                                 std::nullopt, context);
        index_keys.emplace_back(std::move(key));
    }
    index_maintenance::check_logical_key_write_conflict(sm_manager.get(), tab, tab_name,
                                                        rec.data, std::nullopt, context);

    Rid rid = fh->insert_record(rec.data, context);

    if (context != nullptr && context->txn_ != nullptr && context->log_mgr_ != nullptr) {
        if (g_txn_manager != nullptr) {
            g_txn_manager->ensure_txn_begin_logged(context->txn_, context->log_mgr_);
        }
        InsertLogRecord insert_log(context->txn_->get_transaction_id(), rec, rid, tab_name);
        lsn_t lsn = context->log_mgr_->add_log_to_buffer(&insert_log);
        context->txn_->set_prev_lsn(lsn);
    }

    if (context != nullptr && context->txn_ != nullptr) {
        context->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name, rid));
    }

    Transaction *txn = context == nullptr ? nullptr : context->txn_;
    if (txn != nullptr && g_txn_manager != nullptr) {
        g_txn_manager->record_write(txn, tab_name, rid, WType::INSERT_TUPLE,
                                    nullptr, &rec, true, false);
        auto deps = g_txn_manager->check_rw_on_write(txn, tab_name, rid, nullptr, &rec);
        for (auto& [from, to] : deps) {
            if (g_txn_manager->add_rw_dependency_and_check(from, to)) {
                throw TransactionAbortException(txn->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }

    for (size_t i = 0; i < tab.indexes.size(); ++i) {
        auto &index = tab.indexes[i];
        auto ih = index_maintenance::get_index_handle(sm_manager.get(), tab_name, index);
        ih->insert_entry(index_keys[i].data(), rid, txn);
    }
}

static bool prepare_fast_set_clause_local(TabMeta &tab, const std::string &tab_name, SetClause &clause) {
    if (clause.lhs.tab_name.empty()) {
        clause.lhs.tab_name = tab_name;
    }
    if (clause.lhs.tab_name != tab_name) {
        return false;
    }

    auto col = tab.get_col(clause.lhs.col_name);
    if (col == tab.cols.end()) {
        throw ColumnNotFoundError(clause.lhs.col_name);
    }

    if (clause.op == ArithOp::NO_OP) {
        if (col->type == TYPE_FLOAT && clause.rhs.type == TYPE_INT) {
            clause.rhs.set_float(static_cast<float>(clause.rhs.int_val));
        }
        if (col->type != clause.rhs.type) {
            throw IncompatibleTypeError(coltype2str(col->type), coltype2str(clause.rhs.type));
        }
    } else {
        if (col->type != TYPE_INT && col->type != TYPE_FLOAT) {
            throw IncompatibleTypeError(coltype2str(col->type), "numeric");
        }
        if (clause.rhs.type != TYPE_INT && clause.rhs.type != TYPE_FLOAT) {
            throw IncompatibleTypeError(coltype2str(clause.rhs.type), "numeric");
        }
    }
    return true;
}

static bool try_parse_simple_update_local(SmManager *sm_mgr, const std::string &sql,
                                          std::string &tab_name,
                                          std::vector<SetClause> &set_clauses,
                                          std::vector<Condition> &conds) {
    if (sm_mgr == nullptr) {
        return false;
    }
    std::string raw = trim_local(sql);
    while (!raw.empty() && raw.back() == ';') {
        raw.pop_back();
        raw = trim_local(raw);
    }
    std::string lower = lower_local(raw);
    if (lower.rfind("update ", 0) != 0) {
        return false;
    }

    size_t set_pos = find_keyword_local(lower, " set ", 0);
    size_t where_pos = set_pos == std::string::npos ? std::string::npos
                                                    : find_keyword_local(lower, " where ", set_pos + 5);
    if (set_pos == std::string::npos || where_pos == std::string::npos) {
        return false;
    }

    tab_name = trim_local(raw.substr(7, set_pos - 7));
    std::string set_part = trim_local(raw.substr(set_pos + 5, where_pos - (set_pos + 5)));
    std::string where_part = trim_local(raw.substr(where_pos + 7));
    if (tab_name.empty() || set_part.empty() || where_part.empty() ||
        tab_name.find(',') != std::string::npos ||
        tab_name.find(' ') != std::string::npos ||
        tab_name.find('\t') != std::string::npos) {
        return false;
    }

    std::string real_tab_name = sm_mgr->resolve_table_name(tab_name);
    TabMeta &tab = sm_mgr->db_.get_table(real_tab_name);

    set_clauses.clear();
    for (const auto &assignment : split_commas_local(set_part)) {
        size_t eq_pos = find_eq_local(assignment);
        if (eq_pos == std::string::npos) {
            return false;
        }

        bool compound_assignment = false;
        ArithOp compound_op = ArithOp::NO_OP;
        size_t lhs_end = eq_pos;
        if (eq_pos > 0) {
            size_t op_pos = eq_pos;
            while (op_pos > 0 && std::isspace(static_cast<unsigned char>(assignment[op_pos - 1]))) {
                op_pos--;
            }
            if (op_pos > 0) {
                char op_ch = assignment[op_pos - 1];
                if (op_ch == '+' || op_ch == '-' || op_ch == '*' || op_ch == '/') {
                    compound_assignment = true;
                    compound_op = op_ch == '+' ? ArithOp::ADD :
                                  op_ch == '-' ? ArithOp::SUB :
                                  op_ch == '*' ? ArithOp::MUL : ArithOp::DIV;
                    lhs_end = op_pos - 1;
                }
            }
        }

        std::string lhs_text = trim_local(assignment.substr(0, lhs_end));
        std::string rhs_text = trim_local(assignment.substr(eq_pos + 1));
        if (lhs_text.empty() || rhs_text.empty()) {
            return false;
        }

        TabCol lhs_col = parse_tab_col_local(lhs_text, tab_name);
        if (lhs_col.tab_name.empty()) {
            lhs_col.tab_name = tab_name;
        }
        if (lhs_col.tab_name != tab_name) {
            return false;
        }

        SetClause base_clause;
        base_clause.lhs = lhs_col;
        base_clause.op = ArithOp::NO_OP;

        if (compound_assignment) {
            if (!parse_literal_local(rhs_text, base_clause.rhs)) {
                return false;
            }
            base_clause.op = compound_op;
            if (!prepare_fast_set_clause_local(tab, tab_name, base_clause)) {
                return false;
            }
            set_clauses.push_back(std::move(base_clause));
            continue;
        }

        std::vector<std::pair<ArithOp, Value>> arithmetic_terms;
        if (parse_lhs_arithmetic_terms_local(rhs_text, lhs_col.col_name, arithmetic_terms)) {
            for (auto &term : arithmetic_terms) {
                SetClause clause = base_clause;
                clause.op = term.first;
                clause.rhs = std::move(term.second);
                if (!prepare_fast_set_clause_local(tab, tab_name, clause)) {
                    return false;
                }
                set_clauses.push_back(std::move(clause));
            }
            continue;
        }

        if (!parse_literal_local(rhs_text, base_clause.rhs)) {
            return false;
        }
        if (!prepare_fast_set_clause_local(tab, tab_name, base_clause)) {
            return false;
        }
        set_clauses.push_back(std::move(base_clause));
    }
    if (set_clauses.empty()) {
        return false;
    }

    conds.clear();
    for (const auto &part : split_and_local(where_part)) {
        if (part.empty()) {
            return false;
        }
        Condition cond;
        if (!parse_comp_condition_local(part, tab_name, cond)) {
            return false;
        }
        if (!prepare_fast_condition_local(tab, tab_name, cond)) {
            return false;
        }
        conds.push_back(std::move(cond));
    }
    return !conds.empty();
}

static void execute_fast_update_direct(const std::string &tab_name,
                                       std::vector<SetClause> &set_clauses,
                                       std::vector<Condition> &conds,
                                       Context *context) {
    std::string real_tab_name = sm_manager->resolve_table_name(tab_name);
    TabMeta &tab = sm_manager->db_.get_table(real_tab_name);
    std::vector<std::string> index_col_names;
    bool index_exist = choose_fast_select_index_local(tab, tab_name, conds, index_col_names);
    auto scan = std::make_shared<ScanPlan>(index_exist ? T_IndexScan : T_SeqScan,
                                           sm_manager.get(), tab_name, conds, index_col_names);
    auto dml_plan = std::make_shared<DMLPlan>(T_Update, scan, tab_name,
                                              std::vector<Value>(), conds, set_clauses);

    std::vector<Rid> rids;
    if (!write_index_probe::collect_exact_write_rids(sm_manager.get(), dml_plan, context, rids)) {
        auto scan_exec = portal->convert_plan_executor(scan, context);
        for (scan_exec->beginTuple(); !scan_exec->is_end(); scan_exec->nextTuple()) {
            rids.push_back(scan_exec->rid());
        }
    }

    UpdateExecutor executor(sm_manager.get(), tab_name, set_clauses, conds, rids, context);
    executor.Next();
}

static bool try_parse_simple_delete_local(SmManager *sm_mgr, const std::string &sql,
                                          std::string &tab_name,
                                          std::vector<Condition> &conds) {
    if (sm_mgr == nullptr) {
        return false;
    }
    std::string raw = trim_local(sql);
    while (!raw.empty() && raw.back() == ';') {
        raw.pop_back();
        raw = trim_local(raw);
    }
    std::string lower = lower_local(raw);
    const std::string prefix = "delete from ";
    if (lower.rfind(prefix, 0) != 0) {
        return false;
    }

    size_t where_pos = find_keyword_local(lower, " where ", prefix.size());
    if (where_pos == std::string::npos) {
        return false;
    }

    tab_name = trim_local(raw.substr(prefix.size(), where_pos - prefix.size()));
    std::string where_part = trim_local(raw.substr(where_pos + 7));
    if (tab_name.empty() || where_part.empty() ||
        tab_name.find(',') != std::string::npos ||
        tab_name.find(' ') != std::string::npos ||
        tab_name.find('\t') != std::string::npos) {
        return false;
    }

    std::string real_tab_name = sm_mgr->resolve_table_name(tab_name);
    TabMeta &tab = sm_mgr->db_.get_table(real_tab_name);
    conds.clear();
    for (const auto &part : split_and_local(where_part)) {
        if (part.empty()) {
            return false;
        }
        Condition cond;
        if (!parse_comp_condition_local(part, tab_name, cond)) {
            return false;
        }
        if (!prepare_fast_condition_local(tab, tab_name, cond)) {
            return false;
        }
        conds.push_back(std::move(cond));
    }
    return !conds.empty();
}

static void execute_fast_delete_direct(const std::string &tab_name,
                                       std::vector<Condition> &conds,
                                       Context *context) {
    std::string real_tab_name = sm_manager->resolve_table_name(tab_name);
    TabMeta &tab = sm_manager->db_.get_table(real_tab_name);
    std::vector<std::string> index_col_names;
    bool index_exist = choose_fast_select_index_local(tab, tab_name, conds, index_col_names);
    auto scan = std::make_shared<ScanPlan>(index_exist ? T_IndexScan : T_SeqScan,
                                           sm_manager.get(), tab_name, conds, index_col_names);
    auto dml_plan = std::make_shared<DMLPlan>(T_Delete, scan, tab_name,
                                              std::vector<Value>(), conds,
                                              std::vector<SetClause>());

    std::vector<Rid> rids;
    if (!write_index_probe::collect_exact_write_rids(sm_manager.get(), dml_plan, context, rids)) {
        auto scan_exec = portal->convert_plan_executor(scan, context);
        for (scan_exec->beginTuple(); !scan_exec->is_end(); scan_exec->nextTuple()) {
            rids.push_back(scan_exec->rid());
        }
    }

    DeleteExecutor executor(sm_manager.get(), tab_name, conds, rids, context);
    executor.Next();
}

static bool try_parse_load_command_local(const std::string &sql, std::string &file_name,
                                         std::string &tab_name) {
    std::string raw = trim_local(sql);
    while (!raw.empty() && raw.back() == ';') {
        raw.pop_back();
        raw = trim_local(raw);
    }

    std::string lower = lower_local(raw);
    const std::string prefix = "load ";
    if (lower.rfind(prefix, 0) != 0) {
        return false;
    }

    size_t into_pos = find_keyword_local(lower, " into ", prefix.size());
    if (into_pos == std::string::npos) {
        return false;
    }

    file_name = trim_local(raw.substr(prefix.size(), into_pos - prefix.size()));
    tab_name = trim_local(raw.substr(into_pos + 6));
    if (file_name.size() >= 2 &&
        ((file_name.front() == '\'' && file_name.back() == '\'') ||
         (file_name.front() == '"' && file_name.back() == '"'))) {
        file_name = file_name.substr(1, file_name.size() - 2);
    }
    return !file_name.empty() && !tab_name.empty();
}

static bool try_handle_load_command(const std::string &sql, char *data_send, int *offset, int fd) {
    std::string file_name;
    std::string tab_name;
    if (!try_parse_load_command_local(sql, file_name, tab_name)) {
        return false;
    }

    memset(data_send, '\0', BUFFER_LENGTH);
    *offset = 0;
    try {
        sm_manager->load_csv(file_name, tab_name, nullptr);
    } catch (RMDBError &e) {
        std::cerr << e.what() << std::endl;
        set_response(data_send, offset, std::string(e.what()) + "\n");
        append_output_line("failure\n");
    } catch (std::exception &e) {
        std::cerr << "Load error: " << e.what() << std::endl;
        set_response(data_send, offset, std::string("Error: ") + e.what() + "\n");
        append_output_line("failure\n");
    } catch (...) {
        std::cerr << "Unknown load error" << std::endl;
        set_response(data_send, offset, "Error: Unknown error\n");
        append_output_line("failure\n");
    }

    write(fd, data_send, *offset + 1);
    return true;
}

static bool is_explicit_txn_context(Context *context) {
    return context != nullptr && context->txn_ != nullptr && context->txn_->get_txn_mode();
}

static void abort_failed_explicit_txn(Context *context, bool *txn_failed) {
    if (!is_explicit_txn_context(context)) {
        return;
    }
    if (txn_failed != nullptr) {
        *txn_failed = true;
    }
    Transaction *txn = context->txn_;
    if (txn->get_state() != TransactionState::COMMITTED &&
        txn->get_state() != TransactionState::ABORTED) {
        txn_manager->abort(txn, log_manager.get());
    }
    context->txn_ = nullptr;
}

static bool is_read_only_after_failed_txn(const std::string &control_cmd) {
    std::string cmd = normalize_sql_space(control_cmd);
    if (cmd.empty()) {
        return true;
    }
    if (cmd == "help" || cmd == "show tables" || cmd.rfind("show index from ", 0) == 0 ||
        cmd.rfind("desc ", 0) == 0 || cmd.rfind("select ", 0) == 0 ||
        cmd == "select" || cmd.rfind("explain analyze select ", 0) == 0 ||
        cmd == "explain analyze select") {
        return true;
    }
    return false;
}

static bool try_handle_fast_insert(const std::string &sql, txn_id_t *txn_id,
                                   char *data_send, int *offset, int fd,
                                   bool *txn_failed) {
    std::string tab_name;
    std::vector<Value> values;
    if (!try_parse_simple_insert_local(sql, tab_name, values)) {
        return false;
    }

    memset(data_send, '\0', BUFFER_LENGTH);
    *offset = 0;
    auto context = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, data_send, offset);
    SetTransaction(txn_id, context.get());

    try {
        execute_fast_insert_direct(tab_name, values, context.get());
        if (context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
            context->txn_->get_state() != TransactionState::COMMITTED &&
            context->txn_->get_state() != TransactionState::ABORTED) {
            txn_manager->commit(context->txn_, context->log_mgr_);
            context->txn_ = nullptr;
            *txn_id = INVALID_TXN_ID;
        }
    } catch (TransactionAbortException &e) {
        set_response(data_send, offset, "abort\n");
        abort_failed_explicit_txn(context.get(), txn_failed);
        if (!is_explicit_txn_context(context.get())) {
            txn_manager->abort(context->txn_, log_manager.get());
        }
        std::cout << e.GetInfo() << std::endl;

        append_output_line("abort\n");
    } catch (RMDBError &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << e.what() << std::endl;
        set_response(data_send, offset, std::string(e.what()) + "\n");

        append_output_line("failure\n");
    } catch (std::exception &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Standard exception: " << e.what() << std::endl;
        set_response(data_send, offset, std::string("Error: ") + e.what() + "\n");

        append_output_line("failure\n");
    } catch (...) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Unknown exception caught" << std::endl;
        set_response(data_send, offset, "Error: Unknown error\n");

        append_output_line("failure\n");
    }

    write(fd, data_send, *offset + 1);
    return true;
}

static bool try_handle_fast_update(const std::string &sql, txn_id_t *txn_id,
                                   char *data_send, int *offset, int fd,
                                   bool *txn_failed) {
    if (enable_output_file.load()) {
        return false;
    }

    std::string tab_name;
    std::vector<SetClause> set_clauses;
    std::vector<Condition> conds;
    try {
        if (!try_parse_simple_update_local(sm_manager.get(), sql, tab_name, set_clauses, conds)) {
            return false;
        }
    } catch (...) {
        return false;
    }

    memset(data_send, '\0', BUFFER_LENGTH);
    *offset = 0;
    auto context = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, data_send, offset);
    SetTransaction(txn_id, context.get());

    try {
        execute_fast_update_direct(tab_name, set_clauses, conds, context.get());
        if (context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
            context->txn_->get_state() != TransactionState::COMMITTED &&
            context->txn_->get_state() != TransactionState::ABORTED) {
            txn_manager->commit(context->txn_, context->log_mgr_);
            context->txn_ = nullptr;
            *txn_id = INVALID_TXN_ID;
        }
    } catch (TransactionAbortException &e) {
        set_response(data_send, offset, "abort\n");
        abort_failed_explicit_txn(context.get(), txn_failed);
        if (!is_explicit_txn_context(context.get())) {
            txn_manager->abort(context->txn_, log_manager.get());
        }
        std::cout << e.GetInfo() << std::endl;
        append_output_line("abort\n");
    } catch (RMDBError &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << e.what() << std::endl;
        set_response(data_send, offset, std::string(e.what()) + "\n");
        append_output_line("failure\n");
    } catch (std::exception &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Standard exception: " << e.what() << std::endl;
        set_response(data_send, offset, std::string("Error: ") + e.what() + "\n");
        append_output_line("failure\n");
    } catch (...) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Unknown exception caught" << std::endl;
        set_response(data_send, offset, "Error: Unknown error\n");
        append_output_line("failure\n");
    }

    write(fd, data_send, *offset + 1);
    return true;
}

static bool try_handle_fast_delete(const std::string &sql, txn_id_t *txn_id,
                                   char *data_send, int *offset, int fd,
                                   bool *txn_failed) {
    if (enable_output_file.load()) {
        return false;
    }

    std::string tab_name;
    std::vector<Condition> conds;
    try {
        if (!try_parse_simple_delete_local(sm_manager.get(), sql, tab_name, conds)) {
            return false;
        }
    } catch (...) {
        return false;
    }

    memset(data_send, '\0', BUFFER_LENGTH);
    *offset = 0;
    auto context = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, data_send, offset);
    SetTransaction(txn_id, context.get());

    try {
        execute_fast_delete_direct(tab_name, conds, context.get());
        if (context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
            context->txn_->get_state() != TransactionState::COMMITTED &&
            context->txn_->get_state() != TransactionState::ABORTED) {
            txn_manager->commit(context->txn_, context->log_mgr_);
            context->txn_ = nullptr;
            *txn_id = INVALID_TXN_ID;
        }
    } catch (TransactionAbortException &e) {
        set_response(data_send, offset, "abort\n");
        abort_failed_explicit_txn(context.get(), txn_failed);
        if (!is_explicit_txn_context(context.get())) {
            txn_manager->abort(context->txn_, log_manager.get());
        }
        std::cout << e.GetInfo() << std::endl;
        append_output_line("abort\n");
    } catch (RMDBError &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << e.what() << std::endl;
        set_response(data_send, offset, std::string(e.what()) + "\n");
        append_output_line("failure\n");
    } catch (std::exception &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Standard exception: " << e.what() << std::endl;
        set_response(data_send, offset, std::string("Error: ") + e.what() + "\n");
        append_output_line("failure\n");
    } catch (...) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Unknown exception caught" << std::endl;
        set_response(data_send, offset, "Error: Unknown error\n");
        append_output_line("failure\n");
    }

    write(fd, data_send, *offset + 1);
    return true;
}

static bool try_handle_fast_select(const std::string &sql, txn_id_t *txn_id,
                                   char *data_send, int *offset, int fd,
                                   bool *txn_failed) {
    if (enable_output_file.load()) {
        return false;
    }

    std::string tab_name;
    std::vector<TabCol> sel_cols;
    std::vector<Condition> conds;
    try {
        if (!try_parse_simple_select_local(sm_manager.get(), sql, tab_name, sel_cols, conds)) {
            return false;
        }
    } catch (...) {
        return false;
    }

    memset(data_send, '\0', BUFFER_LENGTH);
    *offset = 0;
    auto context = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, data_send, offset);
    SetTransaction(txn_id, context.get());

    try {
        std::string real_tab_name = sm_manager->resolve_table_name(tab_name);
        TabMeta &tab = sm_manager->db_.get_table(real_tab_name);
        std::vector<std::string> index_col_names;
        bool index_exist = choose_fast_select_index_local(tab, tab_name, conds, index_col_names);
        auto scan = std::make_shared<ScanPlan>(index_exist ? T_IndexScan : T_SeqScan,
                                               sm_manager.get(), tab_name, conds, index_col_names);
        auto projection = std::make_shared<ProjectionPlan>(T_Projection, scan, sel_cols);
        auto root = portal->convert_plan_executor(projection, context.get(), true);
        ql_manager->select_from(std::move(root), sel_cols, context.get());

        if (context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
            context->txn_->get_state() != TransactionState::COMMITTED &&
            context->txn_->get_state() != TransactionState::ABORTED) {
            txn_manager->commit(context->txn_, context->log_mgr_);
            context->txn_ = nullptr;
            *txn_id = INVALID_TXN_ID;
        }
    } catch (TransactionAbortException &e) {
        set_response(data_send, offset, "abort\n");
        abort_failed_explicit_txn(context.get(), txn_failed);
        if (!is_explicit_txn_context(context.get())) {
            txn_manager->abort(context->txn_, log_manager.get());
        }
        std::cout << e.GetInfo() << std::endl;
        append_output_line("abort\n");
    } catch (RMDBError &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << e.what() << std::endl;
        set_response(data_send, offset, std::string(e.what()) + "\n");
        append_output_line("failure\n");
    } catch (std::exception &e) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Standard exception: " << e.what() << std::endl;
        set_response(data_send, offset, std::string("Error: ") + e.what() + "\n");
        append_output_line("failure\n");
    } catch (...) {
        abort_failed_explicit_txn(context.get(), txn_failed);
        std::cerr << "Unknown exception caught" << std::endl;
        set_response(data_send, offset, "Error: Unknown error\n");
        append_output_line("failure\n");
    }

    write(fd, data_send, *offset + 1);
    return true;
}

void *client_handler(void *sock_fd) {
    std::unique_ptr<int> fd_holder(static_cast<int *>(sock_fd));
    int fd = *fd_holder;

    int i_recvBytes;
    // 接收客户端发送的请求
    char data_recv[BUFFER_LENGTH];
    // 需要返回给客户端的结果
    char *data_send = new char[BUFFER_LENGTH];
    // 需要返回给客户端的结果的长度
    int offset = 0;
    // 记录客户端当前正在执行的事务ID
    txn_id_t txn_id = INVALID_TXN_ID;
    bool txn_failed = false;
    std::deque<std::string> pending_requests;

    std::string output = "establish client connection, sockfd: " + std::to_string(fd) + "\n";
    std::cout << output;

    while (true) {
        memset(data_recv, 0, BUFFER_LENGTH);

        if (pending_requests.empty()) {
            i_recvBytes = read(fd, data_recv, BUFFER_LENGTH);

            if (i_recvBytes == 0) {
                std::cout << "Maybe the client has closed" << std::endl;
                break;
            }
            if (i_recvBytes == -1) {
                std::cout << "Client read error!" << std::endl;
                break;
            }

            std::string sanitized_sql = strip_sql_line_comments(std::string(data_recv));
            auto requests = split_sql_requests(sanitized_sql);
            if (requests.empty()) {
                memset(data_send, '\0', BUFFER_LENGTH);
                offset = 0;
                if (write(fd, data_send, 1) == -1) {
                    break;
                }
                continue;
            }
            for (auto &request : requests) {
                pending_requests.push_back(std::move(request));
            }
        }

        std::string current_sql = pending_requests.front();
        pending_requests.pop_front();
        memset(data_recv, 0, BUFFER_LENGTH);
        size_t sanitized_len = std::min(current_sql.size(), static_cast<size_t>(BUFFER_LENGTH - 1));
        memcpy(data_recv, current_sql.data(), sanitized_len);

        std::string control_cmd(data_recv);
        size_t ctrl_start = control_cmd.find_first_not_of(" \t\r\n");
        if (ctrl_start != std::string::npos) control_cmd = control_cmd.substr(ctrl_start);
        while (!control_cmd.empty() &&
               (control_cmd.back() == ';' || control_cmd.back() == ' ' ||
                control_cmd.back() == '\n' || control_cmd.back() == '\r' || control_cmd.back() == '\t')) {
            control_cmd.pop_back();
        }
        for (auto &c : control_cmd) c = static_cast<char>(tolower(c));

        if (control_cmd == "exit") {
            std::cout << "Client exit." << std::endl;
            break;
        }
        if (control_cmd == "crash") {
            std::cout << "Crash command received. Simulating process failure." << std::endl;
            log_manager->flush_log_to_disk();
            _exit(1);
        }
        if (control_cmd == "set output_file off") {
            enable_output_file.store(false);
            memset(data_send, '\0', BUFFER_LENGTH);
            offset = 0;
            if (write(fd, data_send, offset + 1) == -1) break;
            continue;
        }
        if (control_cmd == "set output_file on") {
            enable_output_file.store(true);
            memset(data_send, '\0', BUFFER_LENGTH);
            offset = 0;
            if (write(fd, data_send, offset + 1) == -1) break;
            continue;
        }

        if (txn_failed) {
            if (control_cmd == "begin") {
                txn_id = INVALID_TXN_ID;
                txn_failed = false;
            } else if (control_cmd == "commit" || control_cmd == "abort" || control_cmd == "rollback") {
                txn_id = INVALID_TXN_ID;
                txn_failed = false;
                memset(data_send, '\0', BUFFER_LENGTH);
                offset = 0;
                if (write(fd, data_send, offset + 1) == -1) break;
                continue;
            } else if (is_read_only_after_failed_txn(control_cmd)) {
                txn_id = INVALID_TXN_ID;
            } else {
                memset(data_send, '\0', BUFFER_LENGTH);
                offset = 0;
                set_response(data_send, &offset, "abort\n");
                append_output_line("abort\n");
                if (write(fd, data_send, offset + 1) == -1) break;
                continue;
            }
        }

        // Keep all database work for this request together.  The client
        // session/transaction remains independent, so explicit transactions
        // can still interleave at statement boundaries and MVCC decides
        // whether a conflicting writer must abort.
        std::unique_lock<std::mutex> request_lock(request_mutex);

        // Handle create static_checkpoint
        {
            std::string cmd_check(data_recv);
            // Trim whitespace
            size_t s = cmd_check.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) cmd_check = cmd_check.substr(s);
            while (!cmd_check.empty() && (cmd_check.back() == ';' || cmd_check.back() == ' ' || cmd_check.back() == '\n' || cmd_check.back() == '\r'))
                cmd_check.pop_back();
            std::string cmd_lower_ck = cmd_check;
            for (auto &c : cmd_lower_ck) c = tolower(c);
            if (cmd_lower_ck == "create static_checkpoint") {
                bool checkpoint_started = false;
                try {
                    memset(data_send, '\0', BUFFER_LENGTH);
                    offset = 0;

                    Transaction *current_txn = txn_manager->get_transaction(txn_id);
                    if (current_txn != nullptr &&
                        current_txn->get_state() != TransactionState::COMMITTED &&
                        current_txn->get_state() != TransactionState::ABORTED) {
                        txn_manager->abort(current_txn, log_manager.get());
                        // 确保事务从活跃列表中移除
                        log_manager->remove_active_txn(current_txn->get_transaction_id());
                        txn_id = INVALID_TXN_ID;
                    }

                    auto active_txns = log_manager->begin_checkpoint();
                    checkpoint_started = true;

                    // (1) Flush log buffer to disk
                    log_manager->flush_log_to_disk();
                    int checkpoint_offset = disk_manager->get_file_size(LOG_FILE_NAME);
                    if (checkpoint_offset < 0) {
                        checkpoint_offset = 0;
                    }

                    // (2) Write checkpoint record to log
                    lsn_t current_lsn = 0; // will be assigned by add_log_to_buffer
                    {
                        CheckpointLogRecord ckpt(current_lsn, active_txns);
                        current_lsn = log_manager->add_log_to_buffer(&ckpt);
                    }
                    log_manager->flush_log_to_disk();

                    // (3) Flush all dirty pages to disk
                    for (auto &entry : sm_manager->fhs_) {
                        auto fh = entry.second.get();
                        int fd_table = fh->GetFd();
                        disk_manager->write_page(fd_table, RM_FILE_HDR_PAGE,
                                                 reinterpret_cast<const char *>(&fh->get_file_hdr_ref()),
                                                 sizeof(RmFileHdr));
                        buffer_pool_manager->flush_all_pages(fd_table);
                    }
                    for (auto &entry : sm_manager->ihs_) {
                        auto ih = entry.second.get();
                        ih->flush_file_header();
                        buffer_pool_manager->flush_all_pages(ih->GetFd());
                    }

                    // (4) Write checkpoint byte offset to restart file
                    std::ofstream ofs("checkpoint.lsn", std::ios::trunc);
                    ofs << checkpoint_offset;
                    ofs.close();

                    log_manager->end_checkpoint();
                    checkpoint_started = false;
                    set_response(data_send, &offset, "OK\n");
                } catch (std::exception &e) {
                    if (checkpoint_started) {
                        log_manager->end_checkpoint();
                    }
                    memset(data_send, '\0', BUFFER_LENGTH);
                    offset = 0;
                    set_response(data_send, &offset, std::string("Error: ") + e.what() + "\n");
                }
                if (write(fd, data_send, offset + 1) == -1) break;
                continue;
            }
        }

        if (try_handle_load_command(std::string(data_recv), data_send, &offset, fd)) {
            continue;
        }

        // 检查是否是 SET TRANSACTION ISOLATION LEVEL 命令（必须在aggregate处理之前）
        {
            std::string sql_lower_check(data_recv);
            for (auto &c : sql_lower_check) c = tolower(c);
            if (sql_lower_check.find("set transaction isolation level") != std::string::npos) {
                // 直接设置会话隔离级别，不创建事务
                IsolationLevel level = IsolationLevel::SERIALIZABLE;
                if (sql_lower_check.find("snapshot isolation") != std::string::npos) {
                    level = IsolationLevel::SNAPSHOT_ISOLATION;
                }
                int session_id = static_cast<int>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
                txn_manager->set_session_isolation_level(session_id, level);
                // Topic 9 expects SET TRANSACTION ISOLATION LEVEL to be silent.
                memset(data_send, '\0', BUFFER_LENGTH);
                offset = 0;
                if (write(fd, data_send, offset + 1) == -1) break;
                continue;
            }
        }

        if (try_handle_fast_insert(std::string(data_recv), &txn_id, data_send, &offset, fd, &txn_failed)) {
            continue;
        }

        if (try_handle_fast_update(std::string(data_recv), &txn_id, data_send, &offset, fd, &txn_failed)) {
            continue;
        }

        if (try_handle_fast_delete(std::string(data_recv), &txn_id, data_send, &offset, fd, &txn_failed)) {
            continue;
        }

        if (try_handle_fast_select(std::string(data_recv), &txn_id, data_send, &offset, fd, &txn_failed)) {
            continue;
        }

        // Handle aggregate queries (GROUP BY, HAVING, LIMIT, aggregate functions, multi-col ORDER BY)
        {
            std::string sql_raw(data_recv);
            while (!sql_raw.empty() && (sql_raw.back() == ';' || sql_raw.back() == ' ' || sql_raw.back() == '\n' || sql_raw.back() == '\r'))
                sql_raw.pop_back();
            std::string sql_lower = sql_raw;
            for (auto &c : sql_lower) c = tolower(c);
            auto has_agg_func_call = [&](const std::string &fn) -> bool {
                size_t pos = 0;
                while ((pos = sql_lower.find(fn, pos)) != std::string::npos) {
                    bool left_ok = (pos == 0 || (!isalnum(static_cast<unsigned char>(sql_lower[pos - 1])) &&
                                                 sql_lower[pos - 1] != '_'));
                    size_t p = pos + fn.length();
                    while (p < sql_lower.length() && isspace(static_cast<unsigned char>(sql_lower[p]))) p++;
                    if (left_ok && p < sql_lower.length() && sql_lower[p] == '(') return true;
                    pos += fn.length();
                }
                return false;
            };
            // Check for keywords with word boundary to avoid false matches in column/table names
            auto has_keyword = [&](const std::string &kw) -> bool {
                size_t pos = sql_lower.find(kw);
                if (pos == std::string::npos) return false;
                // Check character before keyword
                if (pos > 0 && (isalnum(sql_lower[pos-1]) || sql_lower[pos-1] == '_')) return false;
                // Check character after keyword
                size_t end = pos + kw.length();
                if (end < sql_lower.length() && (isalnum(sql_lower[end]) || sql_lower[end] == '_')) return false;
                return true;
            };
            bool has_agg = has_keyword("group by") ||
                           has_keyword("having") ||
                           has_agg_func_call("count") ||
                           has_agg_func_call("max") ||
                           has_agg_func_call("min") ||
                           has_agg_func_call("sum") ||
                           has_agg_func_call("avg") ||
                           sql_lower.find(" limit ") != std::string::npos;
            bool has_multi_orderby = false;
            if (has_keyword("order by")) {
                size_t ob_pos = sql_lower.find("order by");
                std::string after_ob = sql_lower.substr(ob_pos + 8);
                if (after_ob.find(',') != std::string::npos) has_multi_orderby = true;
            }
            bool has_union = sql_lower.find(" union ") != std::string::npos;
            std::string sql_words = " " + normalize_sql_space(sql_lower) + " ";
            bool has_explain = sql_words.find(" explain analyze ") != std::string::npos;
            if (has_agg || has_multi_orderby || has_union || has_explain) {
                std::unique_ptr<Context> context_agg;
                try {
                    memset(data_send, '\0', BUFFER_LENGTH);
                    offset = 0;
                    context_agg = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, data_send, &offset);
                    SetTransaction(&txn_id, context_agg.get());
                    if (has_explain) {
                        ql_manager->handle_explain_analyze(sql_raw, context_agg.get());
                    } else if (has_union) {
                        ql_manager->handle_union(sql_raw, context_agg.get());
                    } else {
                        ql_manager->handle_aggregate(sql_raw, context_agg.get());
                    }
                    if (write(fd, data_send, offset + 1) == -1) break;
                    if(context_agg->txn_ != nullptr && context_agg->txn_->get_txn_mode() == false &&
                       context_agg->txn_->get_state() != TransactionState::COMMITTED &&
                       context_agg->txn_->get_state() != TransactionState::ABORTED) {
                        txn_manager->commit(context_agg->txn_, context_agg->log_mgr_);
                        context_agg->txn_ = nullptr;
                        txn_id = INVALID_TXN_ID;
                    }
                    continue;
                } catch (TransactionAbortException &e) {
                    set_response(data_send, &offset, "abort\n");
                    abort_failed_explicit_txn(context_agg.get(), &txn_failed);
                    if (!is_explicit_txn_context(context_agg.get())) {
                        txn_manager->abort(context_agg->txn_, log_manager.get());
                    }
                    std::cout << e.GetInfo() << std::endl;
                    append_output_line("abort\n");
                    if (write(fd, data_send, offset + 1) == -1) break;
                    continue;
                } catch (std::exception &e) {
                    abort_failed_explicit_txn(context_agg.get(), &txn_failed);
                    std::cerr << "Aggregation error: " << e.what() << std::endl;
                    set_response(data_send, &offset, std::string(e.what()) + "\n");
                    append_output_line("failure\n");
                    if (write(fd, data_send, offset + 1) == -1) break;
                    continue;
                }
            }
        }

        // Handle "show index from table_name" directly
        std::string cmd_str(data_recv);
        // Trim leading whitespace
        size_t start = cmd_str.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) cmd_str = cmd_str.substr(start);
        // Check for show index from (case insensitive)
        std::string cmd_lower = cmd_str;
        for (auto &c : cmd_lower) c = tolower(c);
        if (cmd_lower.length() > 16 &&
            cmd_lower.substr(0, 16) == "show index from ") {
            std::string tab_name = cmd_str.substr(16);
            // Remove trailing semicolon and whitespace
            while (!tab_name.empty() && (tab_name.back() == ';' || tab_name.back() == ' ' || tab_name.back() == '\n' || tab_name.back() == '\r')) {
                tab_name.pop_back();
            }
            try {
                sm_manager->show_index(tab_name, nullptr);
                set_response(data_send, &offset, "OK\n");
            } catch (std::exception &e) {
                set_response(data_send, &offset, std::string("failure\n"));
                append_output_line("failure\n");
            }
            if (write(fd, data_send, offset + 1) == -1) break;
            continue;
        }

        memset(data_send, '\0', BUFFER_LENGTH);
        offset = 0;

        // 开启事务，初始化系统所需的上下文信息（包括事务对象指针、锁管理器指针、日志管理器指针、存放结果的buffer、记录结果长度的变量）
        auto context = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, data_send, &offset);
        SetTransaction(&txn_id, context.get());

        std::string arithmetic_dummy_sql;
        std::vector<SetClause> arithmetic_set_clauses;
        if (try_parse_arithmetic_update(std::string(data_recv), arithmetic_dummy_sql, arithmetic_set_clauses)) {
            bool finish_analyze = false;
            pthread_mutex_lock(buffer_mutex);
            ast::parse_tree = nullptr;
            YY_BUFFER_STATE buf = yy_scan_string(arithmetic_dummy_sql.c_str());
            if (yyparse() == 0 && ast::parse_tree != nullptr) {
                try {
                    auto parse_tree_copy = ast::parse_tree;
                    yy_delete_buffer(buf);
                    finish_analyze = true;
                    pthread_mutex_unlock(buffer_mutex);

                    std::shared_ptr<Query> query = analyze->do_analyze(parse_tree_copy);
                    query->set_clauses = arithmetic_set_clauses;
                    std::shared_ptr<Plan> plan = optimizer->plan_query(query, context.get());
                    std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context.get());
                    portal->run(portalStmt, ql_manager.get(), &txn_id, context.get());
                    portal->drop();
                } catch (TransactionAbortException &e) {
                    set_response(data_send, &offset, "abort\n");
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    if (!is_explicit_txn_context(context.get())) {
                        txn_manager->abort(context->txn_, log_manager.get());
                    }
                    std::cout << e.GetInfo() << std::endl;

                    append_output_line("abort\n");
                } catch (RMDBError &e) {
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    std::cerr << e.what() << std::endl;
                    set_response(data_send, &offset, std::string(e.what()) + "\n");

                    append_output_line("failure\n");
                } catch (std::exception &e) {
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    std::cerr << "Standard exception: " << e.what() << std::endl;
                    set_response(data_send, &offset, std::string("Error: ") + e.what() + "\n");
                    append_output_line("failure\n");
                } catch (...) {
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    std::cerr << "Unknown exception caught" << std::endl;
                    set_response(data_send, &offset, "Error: Unknown error\n");
                    append_output_line("failure\n");
                }
            } else {
                abort_failed_explicit_txn(context.get(), &txn_failed);
                set_response(data_send, &offset, "Error: parse failed\n");
            }
            if (finish_analyze == false) {
                yy_delete_buffer(buf);
                pthread_mutex_unlock(buffer_mutex);
            }
            if (write(fd, data_send, offset + 1) == -1) {
                break;
            }
            if(context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
               context->txn_->get_state() != TransactionState::COMMITTED &&
               context->txn_->get_state() != TransactionState::ABORTED)
            {
                try {
                    txn_manager->commit(context->txn_, context->log_mgr_);
                    context->txn_ = nullptr;
                    txn_id = INVALID_TXN_ID;
                } catch (std::exception &e) {
                    std::cerr << "Auto commit failed: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Auto commit failed: unknown error" << std::endl;
                }
            }
            continue;
        }

        // SQL预处理：去别名、ON转WHERE
        auto rewrite_result = rewrite_sql_for_parser(std::string(data_recv));
        SmTableAliasGuard alias_guard(sm_manager.get(), rewrite_result.query_aliases);
        std::string processed_sql = expand_qualified_stars(
            rewrite_result.sql,
            [&](const std::string &tab_name) {
                std::vector<std::string> col_names;
                for (auto &col : sm_manager->get_query_cols(tab_name)) {
                    col_names.push_back(col.name);
                }
                return col_names;
            });
        processed_sql = strip_select_aliases_for_parser(processed_sql);

        // 用于判断是否已经调用了yy_delete_buffer来删除buf
        bool finish_analyze = false;
        pthread_mutex_lock(buffer_mutex);
        ast::parse_tree = nullptr;
        YY_BUFFER_STATE buf = yy_scan_string(processed_sql.c_str());
        if (yyparse() == 0) {
            if (ast::parse_tree != nullptr) {
                try {
                    // yyparse uses global parser state; analyze only reads metadata.
                    auto parse_tree_copy = ast::parse_tree;
                    yy_delete_buffer(buf);
                    finish_analyze = true;
                    pthread_mutex_unlock(buffer_mutex);
                    // analyze and rewrite
                    std::shared_ptr<Query> query = analyze->do_analyze(parse_tree_copy);
                    // 优化器
                    std::shared_ptr<Plan> plan = optimizer->plan_query(query, context.get());
                    // portal
                    std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context.get());
                    portal->run(portalStmt, ql_manager.get(), &txn_id, context.get());
                    portal->drop();
                } catch (TransactionAbortException &e) {
                    // 事务需要回滚，需要把abort信息返回给客户端并写入output.txt文件中
                    set_response(data_send, &offset, "abort\n");
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    if (!is_explicit_txn_context(context.get())) {
                        txn_manager->abort(context->txn_, log_manager.get());
                    }
                    std::cout << e.GetInfo() << std::endl;

                    append_output_line("abort\n");
                } catch (RMDBError &e) {
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    // 遇到异常，需要打印failure到output.txt文件中，并发异常信息返回给客户端
                    std::cerr << e.what() << std::endl;

                    set_response(data_send, &offset, std::string(e.what()) + "\n");

                    // 将报错信息写入output.txt
                    append_output_line("failure\n");
                } catch (std::exception &e) {
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    std::cerr << "Standard exception: " << e.what() << std::endl;
                    std::string err_msg = std::string("Error: ") + e.what();
                    set_response(data_send, &offset, err_msg + "\n");
                    append_output_line("failure\n");
                } catch (...) {
                    abort_failed_explicit_txn(context.get(), &txn_failed);
                    std::cerr << "Unknown exception caught" << std::endl;
                    std::string err_msg = "Error: Unknown error";
                    set_response(data_send, &offset, err_msg + "\n");
                    append_output_line("failure\n");
                }
            } else {
                abort_failed_explicit_txn(context.get(), &txn_failed);
                set_response(data_send, &offset, "Error: empty query\n");
            }
        } else {
            abort_failed_explicit_txn(context.get(), &txn_failed);
            set_response(data_send, &offset, "Error: parse failed\n");
        }
        if(finish_analyze == false) {
            yy_delete_buffer(buf);
            pthread_mutex_unlock(buffer_mutex);
        }
        // future TODO: 格式化 sql_handler.result, 传给客户端
        // send result with fixed format, use protobuf in the future
        if (write(fd, data_send, offset + 1) == -1) {
            break;
        }
        // 如果是单挑语句，需要按照一个完整的事务来执行，所以执行完当前语句后，自动提交事务
        if(context->txn_ != nullptr && context->txn_->get_txn_mode() == false &&
           context->txn_->get_state() != TransactionState::COMMITTED &&
           context->txn_->get_state() != TransactionState::ABORTED)
        {
            try {
                txn_manager->commit(context->txn_, context->log_mgr_);
                context->txn_ = nullptr;
                txn_id = INVALID_TXN_ID;
            } catch (std::exception &e) {
                std::cerr << "Auto commit failed: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Auto commit failed: unknown error" << std::endl;
            }
        }
    }

    // Clear
    {
        std::unique_lock<std::mutex> request_lock(request_mutex);
        if (txn_id != INVALID_TXN_ID) {
            Transaction *txn = txn_manager->get_transaction(txn_id);
            if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
                txn_manager->abort(txn, log_manager.get());
            }
        }
    }
    std::cout << "Terminating current client_connection..." << std::endl;
    delete[] data_send;
    close(fd);           // close a file descriptor.
    pthread_exit(NULL);  // terminate calling thread!
}

void start_server() {
    // init mutex
    buffer_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    sockfd_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(buffer_mutex, nullptr);
    pthread_mutex_init(sockfd_mutex, nullptr);

    int sockfd_server;
    int fd_temp;
    struct sockaddr_in s_addr_in {};

    // 初始化连接
    sockfd_server = socket(AF_INET, SOCK_STREAM, 0);  // ipv4,TCP
    if (sockfd_server == -1) {
        throw UnixError();
    }
    int val = 1;
    setsockopt(sockfd_server, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // before bind(), set the attr of structure sockaddr.
    memset(&s_addr_in, 0, sizeof(s_addr_in));
    s_addr_in.sin_family = AF_INET;
    s_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr_in.sin_port = htons(SOCK_PORT);
    fd_temp = bind(sockfd_server, (struct sockaddr *)(&s_addr_in), sizeof(s_addr_in));
    if (fd_temp == -1) {
        std::cout << "Bind error!" << std::endl;
        throw UnixError();
    }

    fd_temp = listen(sockfd_server, MAX_CONN_LIMIT);
    if (fd_temp == -1) {
        std::cout << "Listen error!" << std::endl;
        throw UnixError();
    }

    while (!should_exit) {
        std::cout << "Waiting for new connection..." << std::endl;
        pthread_t thread_id;
        struct sockaddr_in s_addr_client {};
        int client_length = sizeof(s_addr_client);

        if (setjmp(jmpbuf)) {
            std::cout << "Break from Server Listen Loop\n";
            break;
        }

        // Block here. Until server accepts a new connection.
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), (socklen_t *)(&client_length));
        if (sockfd == -1) {
            std::cout << "Accept error!" << std::endl;
            continue;  // ignore current socket ,continue while loop.
        }

        int tcp_no_delay = 1;
        (void)setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &tcp_no_delay, sizeof(tcp_no_delay));
        
        // 和客户端建立连接，并开启一个线程负责处理客户端请求
        auto *client_fd = new int(sockfd);
        if (pthread_create(&thread_id, nullptr, &client_handler, client_fd) != 0) {
            delete client_fd;
            close(sockfd);
            std::cout << "Create thread fail!" << std::endl;
            break;  // break while loop
        }
        pthread_detach(thread_id);

    }

    // Clear
    std::cout << " Try to close all client-connection.\n";
    int ret = shutdown(sockfd_server, SHUT_WR);  // shut down the all or part of a full-duplex connection.
    if(ret == -1) { printf("%s\n", strerror(errno)); }
//    assert(ret != -1);
    sm_manager->close_db();
    std::cout << " DB has been closed.\n";
    std::cout << "Server shuts down." << std::endl;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        // 需要指定数据库名称
        std::cerr << "Usage: " << argv[0] << " <database>" << std::endl;
        exit(1);
    }

    signal(SIGINT, sigint_handler);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    try {
        std::cout << "\n"
                     "  _____  __  __ _____  ____  \n"
                     " |  __ \\|  \\/  |  __ \\|  _ \\ \n"
                     " | |__) | \\  / | |  | | |_) |\n"
                     " |  _  /| |\\/| | |  | |  _ < \n"
                     " | | \\ \\| |  | | |__| | |_) |\n"
                     " |_|  \\_\\_|  |_|_____/|____/ \n"
                     "\n"
                     "Welcome to RMDB!\n"
                     "Type 'help;' for help.\n"
                     "\n";
        // Database name is passed by args
        std::string db_name = argv[1];
        if (!sm_manager->is_dir(db_name)) {
            // Database not found, create a new one
            sm_manager->create_db(db_name);
        }
        // Open database
        sm_manager->open_db(db_name);

        // recovery database
        recovery->analyze();
        recovery->redo();
        recovery->undo();
        
        // 开启服务端，开始接受客户端连接
        start_server();
    } catch (RMDBError &e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch (...) {
        std::cerr << "Unknown server error" << std::endl;
        exit(1);
    }
    return 0;
}
