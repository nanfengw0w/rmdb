#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <utility>

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

static std::string normalize_not_equal_operator(const std::string &sql) {
    std::string result;
    result.reserve(sql.size());
    bool in_string = false;
    for (size_t i = 0; i < sql.size(); i++) {
        char c = sql[i];
        if (c == '\'') {
            in_string = !in_string;
            result.push_back(c);
            continue;
        }
        if (!in_string && c == '!' && i + 1 < sql.size() && sql[i + 1] == '=') {
            result += "<>";
            i++;
            continue;
        }
        result.push_back(c);
    }
    return result;
}

static bool is_sql_comp_token(const std::string &token) {
    return token == "=" || token == "<>" || token == "!=" || token == "<" ||
           token == ">" || token == "<=" || token == ">=";
}

static std::string swap_sql_comp_token(const std::string &token) {
    if (token == "<") return ">";
    if (token == ">") return "<";
    if (token == "<=") return ">=";
    if (token == ">=") return "<=";
    return token;
}

static bool is_sql_number_token(const std::string &token) {
    if (token.empty()) {
        return false;
    }
    size_t pos = 0;
    if (token[pos] == '+' || token[pos] == '-') {
        pos++;
    }
    bool has_digit = false;
    bool has_dot = false;
    for (; pos < token.size(); pos++) {
        unsigned char ch = static_cast<unsigned char>(token[pos]);
        if (std::isdigit(ch)) {
            has_digit = true;
            continue;
        }
        if (token[pos] == '.' && !has_dot) {
            has_dot = true;
            continue;
        }
        return false;
    }
    return has_digit;
}

static bool is_sql_value_token(const std::string &token) {
    if (token.size() >= 2 && token.front() == '\'' && token.back() == '\'') {
        return true;
    }
    std::string lower = to_lower(token);
    return is_sql_number_token(token) || lower == "true" || lower == "false";
}

static std::vector<std::string> normalize_condition_tokens(std::vector<std::string> tokens) {
    std::vector<std::string> normalized;
    std::vector<std::string> term;

    auto flush_term = [&]() {
        if (term.size() == 3 && is_sql_value_token(term[0]) && is_sql_comp_token(term[1]) &&
            !is_sql_value_token(term[2])) {
            normalized.push_back(term[2]);
            normalized.push_back(swap_sql_comp_token(term[1]));
            normalized.push_back(term[0]);
        } else {
            normalized.insert(normalized.end(), term.begin(), term.end());
        }
        term.clear();
    };

    for (auto &token : tokens) {
        if (to_lower(token) == "and") {
            flush_term();
            normalized.push_back(token);
        } else {
            term.push_back(token);
        }
    }
    flush_term();
    return normalized;
}

static SqlRewriteResult rewrite_sql_for_parser(const std::string &original_sql) {
    SqlRewriteResult result;
    result.is_select_star = false;
    std::string parser_sql = normalize_not_equal_operator(original_sql);

    // 检测 SELECT *
    std::string ol = to_lower(parser_sql);
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
    bool has_condition_parens = false;
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
        size_t where_pos_check = ol_normalized.find(" where ");
        if (where_pos_check != std::string::npos) {
            bool in_string = false;
            for (size_t i = where_pos_check + 7; i < parser_sql.size(); i++) {
                if (parser_sql[i] == '\'') {
                    in_string = !in_string;
                    continue;
                }
                if (!in_string && (parser_sql[i] == '(' || parser_sql[i] == ')')) {
                    has_condition_parens = true;
                    break;
                }
            }
        }
    }
    // 按token分割，保留双字符比较操作符
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (size_t ci = 0; ci < parser_sql.size(); ci++) {
            char c = parser_sql[ci];
            if (c == '\'') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                std::string lit;
                lit.push_back(c);
                ci++;
                while (ci < parser_sql.size()) {
                    lit.push_back(parser_sql[ci]);
                    if (parser_sql[ci] == '\'') {
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
                    if (ci + 1 < parser_sql.size()) {
                        char next = parser_sql[ci + 1];
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

    bool has_value_left_condition = false;
    for (size_t ti = 0; ti + 2 < tokens.size(); ti++) {
        if (is_sql_value_token(tokens[ti]) && is_sql_comp_token(tokens[ti + 1]) &&
            !is_sql_value_token(tokens[ti + 2])) {
            has_value_left_condition = true;
            break;
        }
    }

    if (!has_join && !has_implicit_join && !has_single_table_alias &&
        !has_condition_parens && !has_value_left_condition) {
        result.sql = parser_sql;
        return result;
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
               tl == "inner" || tl == "and" || tl == "," || tl == ";" ||
               tl == "(" || tl == ")";
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
    auto append_condition_token = [&](const std::string &token) {
        // The base parser only accepts a flat AND list of binary predicates.
        // Parentheses around those predicates do not change semantics, so
        // remove them during JOIN/WHERE rewriting.
        if (token == "(" || token == ")") {
            return;
        }
        cond_tokens.push_back(token);
    };

    auto process_table_alias = [&](size_t &idx) {
        while (idx < tokens.size() && tokens[idx] == "(") {
            idx++;
        }
        if (idx >= tokens.size()) {
            return;
        }
        if (tokens[idx] == ")") {
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
        if (tl == "(" || tl == ")") {
            i++;
            continue;
        }
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
                if (cur_lower == "join" || cur_lower == "inner" || cur_lower == "where" || cur_lower == "group" ||
                    cur_lower == "order" || cur_lower == "having" || cur_lower == "limit" ||
                    cur_lower == ";") {
                    break;
                }
                append_condition_token(tokens[i++]);
            }
            continue;
        }
        if (tl == "inner") {
            i++;
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
                append_condition_token(tokens[i++]);
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
        cond_tokens = normalize_condition_tokens(std::move(cond_tokens));
        result.sql += " WHERE " + join_tokens(cond_tokens);
    }
    if (!suffix_tokens.empty()) {
        result.sql += " " + join_tokens(suffix_tokens);
    }
    if (parser_sql.find(';') != std::string::npos && result.sql.find(';') == std::string::npos) {
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
