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
#include <memory>
#include <sstream>

#include "errors.h"
#include "common/sql_rewrite.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"

#define SOCK_PORT 8765
#define MAX_CONN_LIMIT 8

static bool should_exit = false;

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
        std::string lhs = trim_local(assignment.substr(0, eq_pos));
        std::string rhs = trim_local(assignment.substr(eq_pos + 1));
        if (lhs.empty() || rhs.empty()) {
            return false;
        }

        SetClause clause;
        clause.lhs = {.tab_name = tab_name, .col_name = lhs};
        clause.op = ArithOp::NO_OP;

        std::string rhs_lower = lower_local(rhs);
        std::string lhs_lower = lower_local(lhs);
        size_t pos = 0;
        while (pos < rhs_lower.size() && std::isspace(static_cast<unsigned char>(rhs_lower[pos]))) {
            pos++;
        }
        bool parsed_arith = false;
        if (rhs_lower.compare(pos, lhs_lower.size(), lhs_lower) == 0) {
            size_t op_pos = pos + lhs_lower.size();
            while (op_pos < rhs_lower.size() && std::isspace(static_cast<unsigned char>(rhs_lower[op_pos]))) {
                op_pos++;
            }
            if (op_pos < rhs_lower.size()) {
                char op = rhs_lower[op_pos];
                if (op == '+' || op == '-' || op == '*' || op == '/') {
                    std::string val_text = trim_local(rhs.substr(op_pos + 1));
                    if (parse_literal_local(val_text, clause.rhs)) {
                        if (clause.rhs.type != TYPE_INT && clause.rhs.type != TYPE_FLOAT) {
                            return false;
                        }
                        clause.op = op == '+' ? ArithOp::ADD :
                                    op == '-' ? ArithOp::SUB :
                                    op == '*' ? ArithOp::MUL : ArithOp::DIV;
                        has_arith = true;
                        parsed_arith = true;
                    }
                }
            }
        }

        if (!parsed_arith) {
            if (!parse_literal_local(rhs, clause.rhs)) {
                return false;
            }
        }
        parsed.push_back(clause);
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
}

void *client_handler(void *sock_fd) {
    int fd = *((int *)sock_fd);
    pthread_mutex_unlock(sockfd_mutex);

    int i_recvBytes;
    // 接收客户端发送的请求
    char data_recv[BUFFER_LENGTH];
    // 需要返回给客户端的结果
    char *data_send = new char[BUFFER_LENGTH];
    // 需要返回给客户端的结果的长度
    int offset = 0;
    // 记录客户端当前正在执行的事务ID
    txn_id_t txn_id = INVALID_TXN_ID;

    std::string output = "establish client connection, sockfd: " + std::to_string(fd) + "\n";
    std::cout << output;

    while (true) {
        memset(data_recv, 0, BUFFER_LENGTH);

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
        if (is_blank_sql(sanitized_sql)) {
            memset(data_send, '\0', BUFFER_LENGTH);
            offset = 0;
            if (write(fd, data_send, 1) == -1) {
                break;
            }
            continue;
        }
        memset(data_recv, 0, BUFFER_LENGTH);
        size_t sanitized_len = std::min(sanitized_sql.size(), static_cast<size_t>(BUFFER_LENGTH - 1));
        memcpy(data_recv, sanitized_sql.data(), sanitized_len);

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
            _exit(1);
        }

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
                try {
                    // (1) Flush log buffer to disk
                    log_manager->flush_log_to_disk();
                    int checkpoint_offset = disk_manager->get_file_size(LOG_FILE_NAME);
                    if (checkpoint_offset < 0) {
                        checkpoint_offset = 0;
                    }

                    // (2) Write checkpoint record to log
                    auto active_txns = log_manager->get_active_txns();
                    lsn_t current_lsn = 0; // will be assigned by add_log_to_buffer
                    {
                        CheckpointLogRecord ckpt(current_lsn, active_txns);
                        current_lsn = log_manager->add_log_to_buffer(&ckpt);
                    }
                    log_manager->flush_log_to_disk();

                    // (3) Flush all dirty pages to disk
                    for (auto &entry : sm_manager->fhs_) {
                        int fd_table = entry.second->GetFd();
                        buffer_pool_manager->flush_all_pages(fd_table);
                    }

                    // (4) Write checkpoint byte offset to restart file
                    std::ofstream ofs("checkpoint.lsn", std::ios::trunc);
                    ofs << checkpoint_offset;
                    ofs.close();

                    set_response(data_send, &offset, "OK\n");
                } catch (std::exception &e) {
                    set_response(data_send, &offset, std::string("Error: ") + e.what() + "\n");
                }
                if (write(fd, data_send, offset + 1) == -1) break;
                continue;
            }
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

        // Handle aggregate queries (GROUP BY, HAVING, LIMIT, aggregate functions, multi-col ORDER BY)
        {
            std::string sql_raw(data_recv);
            while (!sql_raw.empty() && (sql_raw.back() == ';' || sql_raw.back() == ' ' || sql_raw.back() == '\n' || sql_raw.back() == '\r'))
                sql_raw.pop_back();
            std::string sql_lower = sql_raw;
            for (auto &c : sql_lower) c = tolower(c);
            // Check for aggregate functions - use word boundary to avoid false matches
            auto has_word = [&](const std::string &word) -> bool {
                size_t pos = 0;
                while ((pos = sql_lower.find(word, pos)) != std::string::npos) {
                    // Check character before: should not be alphanumeric or underscore
                    if (pos > 0 && (isalnum(sql_lower[pos-1]) || sql_lower[pos-1] == '_')) {
                        pos += word.length();
                        continue;
                    }
                    return true;
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
                           has_word("count(") ||
                           has_word("max(") ||
                           has_word("min(") ||
                           has_word("sum(") ||
                           has_word("avg(") ||
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
                try {
                    memset(data_send, '\0', BUFFER_LENGTH);
                    offset = 0;
                    auto context_agg = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, data_send, &offset);
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
                    }
                    continue;
                } catch (std::exception &e) {
                    std::cerr << "Aggregation error: " << e.what() << std::endl;
                    set_response(data_send, &offset, std::string(e.what()) + "\n");
                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "failure\n";
                    outfile.close();
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
                std::fstream outfile;
                outfile.open("output.txt", std::ios::out | std::ios::app);
                outfile << "failure\n";
                outfile.close();
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
                    std::shared_ptr<Query> query = analyze->do_analyze(ast::parse_tree);
                    query->set_clauses = arithmetic_set_clauses;
                    yy_delete_buffer(buf);
                    finish_analyze = true;
                    pthread_mutex_unlock(buffer_mutex);

                    std::shared_ptr<Plan> plan = optimizer->plan_query(query, context.get());
                    std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context.get());
                    portal->run(portalStmt, ql_manager.get(), &txn_id, context.get());
                    portal->drop();
                } catch (TransactionAbortException &e) {
                    set_response(data_send, &offset, "abort\n");
                    txn_manager->abort(context->txn_, log_manager.get());
                    std::cout << e.GetInfo() << std::endl;

                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "abort\n";
                    outfile.close();
                } catch (RMDBError &e) {
                    std::cerr << e.what() << std::endl;
                    set_response(data_send, &offset, std::string(e.what()) + "\n");

                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "failure\n";
                    outfile.close();
                } catch (std::exception &e) {
                    std::cerr << "Standard exception: " << e.what() << std::endl;
                    set_response(data_send, &offset, std::string("Error: ") + e.what() + "\n");
                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "failure\n";
                    outfile.close();
                } catch (...) {
                    std::cerr << "Unknown exception caught" << std::endl;
                    set_response(data_send, &offset, "Error: Unknown error\n");
                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "failure\n";
                    outfile.close();
                }
            } else {
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
                    // analyze and rewrite
                    std::shared_ptr<Query> query = analyze->do_analyze(ast::parse_tree);
                    yy_delete_buffer(buf);
                    finish_analyze = true;
                    pthread_mutex_unlock(buffer_mutex);
                    // 优化器
                    std::shared_ptr<Plan> plan = optimizer->plan_query(query, context.get());
                    // portal
                    std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context.get());
                    portal->run(portalStmt, ql_manager.get(), &txn_id, context.get());
                    portal->drop();
                } catch (TransactionAbortException &e) {
                    // 事务需要回滚，需要把abort信息返回给客户端并写入output.txt文件中
                    set_response(data_send, &offset, "abort\n");

                    // 回滚事务
                    txn_manager->abort(context->txn_, log_manager.get());
                    std::cout << e.GetInfo() << std::endl;

                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "abort\n";
                    outfile.close();
                } catch (RMDBError &e) {
                    // 遇到异常，需要打印failure到output.txt文件中，并发异常信息返回给客户端
                    std::cerr << e.what() << std::endl;

                    set_response(data_send, &offset, std::string(e.what()) + "\n");

                    // 将报错信息写入output.txt
                    std::fstream outfile;
                    outfile.open("output.txt",std::ios::out | std::ios::app);
                    outfile << "failure\n";
                    outfile.close();
                } catch (std::exception &e) {
                    std::cerr << "Standard exception: " << e.what() << std::endl;
                    std::string err_msg = std::string("Error: ") + e.what();
                    set_response(data_send, &offset, err_msg + "\n");
                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "failure\n";
                    outfile.close();
                } catch (...) {
                    std::cerr << "Unknown exception caught" << std::endl;
                    std::string err_msg = "Error: Unknown error";
                    set_response(data_send, &offset, err_msg + "\n");
                    std::fstream outfile;
                    outfile.open("output.txt", std::ios::out | std::ios::app);
                    outfile << "failure\n";
                    outfile.close();
                }
            } else {
                set_response(data_send, &offset, "Error: empty query\n");
            }
        } else {
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
            } catch (std::exception &e) {
                std::cerr << "Auto commit failed: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Auto commit failed: unknown error" << std::endl;
            }
        }
    }

    // Clear
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
        pthread_mutex_lock(sockfd_mutex);
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), (socklen_t *)(&client_length));
        if (sockfd == -1) {
            std::cout << "Accept error!" << std::endl;
            continue;  // ignore current socket ,continue while loop.
        }
        
        // 和客户端建立连接，并开启一个线程负责处理客户端请求
        if (pthread_create(&thread_id, nullptr, &client_handler, (void *)(&sockfd)) != 0) {
            std::cout << "Create thread fail!" << std::endl;
            break;  // break while loop
        }

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
