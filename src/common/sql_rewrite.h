#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>

// SQL预处理：去别名、ON转WHERE、合并WHERE
// 返回预处理后的SQL和别名映射
struct SqlRewriteResult {
    std::string sql;
    std::map<std::string, std::string> alias_to_table;  // alias(lowercase) -> table(lowercase)
    std::map<std::string, std::string> query_aliases;   // aliases kept as logical table instances
    bool is_select_star;  // 原始SQL是否是 SELECT *
};

static std::string to_lower(const std::string &s) {
    std::string r = s;
    for (auto &c : r) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return r;
}

static bool sql_is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

static std::string normalize_sql_space(const std::string &s) {
    std::string result;
    bool in_space = true;
    for (char c : s) {
        if (sql_is_space(c)) {
            if (!in_space) {
                result.push_back(' ');
                in_space = true;
            }
        } else {
            result.push_back(c);
            in_space = false;
        }
    }
    if (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

static SqlRewriteResult rewrite_sql_for_parser(const std::string &original_sql) {
    SqlRewriteResult result;
    result.is_select_star = false;

    // 检测 SELECT *
    std::string ol = to_lower(original_sql);
    {
        size_t sel_pos = ol.find("select");
        if (sel_pos != std::string::npos) {
            size_t after_sel = sel_pos + 6;
            while (after_sel < ol.size() && sql_is_space(ol[after_sel])) after_sel++;
            if (after_sel < ol.size() && ol[after_sel] == '*') {
                result.is_select_star = true;
            }
        }
    }

    // 检查是否有显式 JOIN 或逗号连接。逗号连接也可能带别名，需要同一套预处理。
    std::string ol_normalized = " " + normalize_sql_space(ol) + " ";
    bool has_join = ol_normalized.find(" join ") != std::string::npos;
    bool has_implicit_join = false;
    bool has_single_table_alias = false;
    {
        size_t from_pos_check = ol_normalized.find(" from ");
        if (from_pos_check != std::string::npos) {
            size_t clause_end = ol_normalized.size();
            for (const auto &kw : {" where ", " group ", " order ", " having ", " limit ", " union ", " ;"}) {
                size_t kw_pos = ol_normalized.find(kw, from_pos_check + 6);
                if (kw_pos != std::string::npos) {
                    clause_end = std::min(clause_end, kw_pos);
                }
            }
            std::string from_clause = ol_normalized.substr(from_pos_check + 6, clause_end - from_pos_check - 6);
            has_implicit_join = from_clause.find(',') != std::string::npos;
            if (!has_join && !has_implicit_join) {
                std::string normalized_from = normalize_sql_space(from_clause);
                int token_count = 0;
                bool in_token = false;
                for (char c : normalized_from) {
                    if (sql_is_space(c)) {
                        if (in_token) {
                            token_count++;
                            in_token = false;
                        }
                    } else {
                        in_token = true;
                    }
                }
                if (in_token) {
                    token_count++;
                }
                has_single_table_alias = token_count > 1;
            }
        }
    }
    if (!has_join && !has_implicit_join && !has_single_table_alias) {
        result.sql = original_sql;
        return result;
    }

    // 按token分割，保留双字符比较操作符
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (size_t ci = 0; ci < original_sql.size(); ci++) {
            char c = original_sql[ci];
            if (c == '\'') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                std::string lit;
                lit.push_back(c);
                ci++;
                while (ci < original_sql.size()) {
                    lit.push_back(original_sql[ci]);
                    if (original_sql[ci] == '\'') {
                        break;
                    }
                    ci++;
                }
                tokens.push_back(lit);
                continue;
            }
            if (sql_is_space(c) || c == '(' || c == ')' ||
                c == ',' || c == ';' || c == '=' || c == '<' || c == '>' || c == '!') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                if (!sql_is_space(c)) {
                    if (ci + 1 < original_sql.size()) {
                        char next = original_sql[ci + 1];
                        if ((c == '<' && (next == '>' || next == '=')) ||
                            (c == '>' && next == '=') ||
                            (c == '!' && next == '=')) {
                            tokens.push_back(std::string(1, c) + std::string(1, next));
                            ci++;
                            continue;
                        }
                    }
                    tokens.push_back(std::string(1, c));
                }
            } else cur += c;
        }
        if (!cur.empty()) tokens.push_back(cur);
    }

    auto join_tokens = [](const std::vector<std::string> &ts) {
        std::string s;
        for (size_t i = 0; i < ts.size(); i++) {
            if (i > 0) s += " ";
            s += ts[i];
        }
        return s;
    };

    auto is_relation_end = [](const std::string &tl) {
        return tl == "join" || tl == "where" || tl == "on" || tl == "group" ||
               tl == "order" || tl == "having" || tl == "limit" || tl == "union" ||
               tl == "and" || tl == "," || tl == ";";
    };

    std::map<std::string, std::string> alias_map;
    size_t from_pos = std::string::npos;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (to_lower(tokens[i]) == "from") {
            from_pos = i;
            break;
        }
    }
    if (from_pos == std::string::npos) {
        result.sql = original_sql;
        return result;
    }

    std::vector<std::string> select_part(tokens.begin(), tokens.begin() + from_pos);
    struct TableRef {
        std::string table;
        std::string alias;
    };
    std::vector<TableRef> tables;
    std::vector<std::string> cond_tokens;
    std::vector<std::string> suffix_tokens;

    auto append_and = [&]() {
        if (!cond_tokens.empty()) {
            cond_tokens.push_back("AND");
        }
    };

    auto process_table_alias = [&](size_t &idx) {
        if (idx >= tokens.size()) {
            return;
        }
        std::string table = tokens[idx++];
        std::string table_lower = to_lower(table);
        std::string alias;
        if (idx < tokens.size() && to_lower(tokens[idx]) == "as" && idx + 1 < tokens.size()) {
            alias = tokens[idx + 1];
            alias_map[to_lower(alias)] = table_lower;
            idx += 2;
        } else if (idx < tokens.size()) {
            std::string next_lower = to_lower(tokens[idx]);
            if (!is_relation_end(next_lower)) {
                alias = tokens[idx];
                alias_map[next_lower] = table_lower;
                idx++;
            }
        }
        tables.push_back({table, alias});
    };

    size_t i = from_pos;
    while (i < tokens.size()) {
        std::string tl = to_lower(tokens[i]);
        if (tl == "from" || tl == "join") {
            i++;
            if (i >= tokens.size()) break;
            process_table_alias(i);
            continue;
        }
        if (tl == ",") {
            i++;
            if (i >= tokens.size()) break;
            process_table_alias(i);
            continue;
        }
        if (tl == "on") {
            append_and();
            i++;
            while (i < tokens.size()) {
                std::string cur_lower = to_lower(tokens[i]);
                if (cur_lower == "join" || cur_lower == "where" || cur_lower == "group" ||
                    cur_lower == "order" || cur_lower == "having" || cur_lower == "limit" ||
                    cur_lower == ";") {
                    break;
                }
                cond_tokens.push_back(tokens[i++]);
            }
            continue;
        }
        if (tl == "where") {
            append_and();
            i++;
            while (i < tokens.size()) {
                std::string cur_lower = to_lower(tokens[i]);
                if (cur_lower == "group" || cur_lower == "order" || cur_lower == "having" ||
                    cur_lower == "limit" || cur_lower == ";") {
                    break;
                }
                cond_tokens.push_back(tokens[i++]);
            }
            continue;
        }
        if (tl == "group" || tl == "order" || tl == "having" || tl == "limit") {
            suffix_tokens.assign(tokens.begin() + i, tokens.end());
            break;
        }
        i++;
    }

    std::set<std::string> preserved_aliases;
    for (auto &table_ref : tables) {
        if (!table_ref.alias.empty()) {
            std::string alias_lower = to_lower(table_ref.alias);
            preserved_aliases.insert(alias_lower);
            result.query_aliases[alias_lower] = to_lower(table_ref.table);
        }
    }

    result.sql = join_tokens(select_part) + " FROM ";
    for (size_t ti = 0; ti < tables.size(); ti++) {
        if (ti > 0) result.sql += " , ";
        std::string alias_lower = to_lower(tables[ti].alias);
        if (!tables[ti].alias.empty() && preserved_aliases.count(alias_lower)) {
            result.sql += tables[ti].alias;
        } else {
            result.sql += tables[ti].table;
        }
    }
    if (!cond_tokens.empty()) {
        result.sql += " WHERE " + join_tokens(cond_tokens);
    }
    if (!suffix_tokens.empty()) {
        result.sql += " " + join_tokens(suffix_tokens);
    }
    if (original_sql.find(';') != std::string::npos && result.sql.find(';') == std::string::npos) {
        result.sql += " ;";
    }
    result.alias_to_table = alias_map;

    for (const auto &entry : alias_map) {
        if (preserved_aliases.count(entry.first)) {
            continue;
        }
        const std::string from = entry.first + ".";
        const std::string to = entry.second + ".";
        size_t pos = 0;
        std::string sql_lower = to_lower(result.sql);
        while ((pos = sql_lower.find(from, pos)) != std::string::npos) {
            // 检查前一个字符不是字母/数字/下划线（避免 sc. 中的 c. 被错误替换）
            if (pos > 0 && (isalnum(static_cast<unsigned char>(sql_lower[pos - 1])) || sql_lower[pos - 1] == '_')) {
                pos += from.length();
                continue;
            }
            result.sql.replace(pos, from.length(), to);
            sql_lower = to_lower(result.sql);
            pos += to.length();
        }
    }
    return result;
}
