#pragma once
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// SQL预处理：去别名、ON转WHERE、合并WHERE
// 返回预处理后的SQL和别名映射
struct SqlRewriteResult {
    std::string sql;
    std::map<std::string, std::string> alias_to_table;  // alias(lowercase) -> table(lowercase)
    bool is_select_star;  // 原始SQL是否是 SELECT *
};

static std::string to_lower(const std::string &s) {
    std::string r = s;
    for (auto &c : r) c = tolower(c);
    return r;
}

static SqlRewriteResult rewrite_sql_for_parser(const std::string &original_sql) {
    SqlRewriteResult result;
    result.is_select_star = false;

    // 检测 SELECT *
    std::string ol = to_lower(original_sql);
    {
        size_t sel_pos = ol.find("select ");
        if (sel_pos != std::string::npos) {
            size_t after_sel = sel_pos + 7;
            while (after_sel < ol.size() && ol[after_sel] == ' ') after_sel++;
            if (after_sel < ol.size() && ol[after_sel] == '*') {
                result.is_select_star = true;
            }
        }
    }

    // 检查是否需要重写（有JOIN）
    if (ol.find(" join ") == std::string::npos) {
        result.sql = original_sql;
        return result;
    }

    // 按token分割
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : original_sql) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' ||
                c == ',' || c == ';' || c == '=' || c == '<' || c == '>' || c == '!') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') tokens.push_back(std::string(1, c));
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
    std::vector<std::string> tables;
    std::vector<std::string> cond_tokens;
    std::vector<std::string> suffix_tokens;

    auto append_and = [&]() {
        if (!cond_tokens.empty()) {
            cond_tokens.push_back("AND");
        }
    };

    size_t i = from_pos;
    while (i < tokens.size()) {
        std::string tl = to_lower(tokens[i]);
        if (tl == "from" || tl == "join") {
            i++;
            if (i >= tokens.size()) break;
            std::string table = tokens[i++];
            tables.push_back(table);
            std::string table_lower = to_lower(table);
            if (i < tokens.size() && to_lower(tokens[i]) == "as" && i + 1 < tokens.size()) {
                alias_map[to_lower(tokens[i + 1])] = table_lower;
                i += 2;
            } else if (i < tokens.size()) {
                std::string next_lower = to_lower(tokens[i]);
                if (!is_relation_end(next_lower)) {
                    alias_map[next_lower] = table_lower;
                    i++;
                }
            }
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

    result.sql = join_tokens(select_part) + " FROM ";
    for (size_t ti = 0; ti < tables.size(); ti++) {
        if (ti > 0) result.sql += " , ";
        result.sql += tables[ti];
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
        const std::string from = entry.first + ".";
        const std::string to = entry.second + ".";
        size_t pos = 0;
        while ((pos = to_lower(result.sql).find(from, pos)) != std::string::npos) {
            result.sql.replace(pos, from.length(), to);
            pos += to.length();
        }
    }
    return result;
}
