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

    // 重建SQL，去别名，ON转WHERE
    std::vector<std::string> out;
    std::map<std::string, std::string> alias_map;

    for (size_t i = 0; i < tokens.size(); i++) {
        std::string tl = to_lower(tokens[i]);

        // AS: 记录映射，跳过
        if (tl == "as" && i + 1 < tokens.size() && !out.empty()) {
            std::string a = to_lower(tokens[i+1]);
            alias_map[a] = to_lower(out.back());
            i++; continue;
        }

        // FROM/JOIN后面的表名和可能的别名
        if ((tl == "from" || tl == "join") && i + 1 < tokens.size()) {
            out.push_back(tokens[i]); i++;
            out.push_back(tokens[i]);
            std::string tname = to_lower(tokens[i]);
            if (i + 1 < tokens.size()) {
                std::string nl = to_lower(tokens[i+1]);
                bool is_kw = (nl=="where"||nl=="on"||nl=="join"||nl=="inner"||nl=="left"||
                    nl=="right"||nl=="group"||nl=="order"||nl=="having"||nl=="limit"||
                    nl=="union"||nl=="select"||nl=="and"||nl=="or"||nl=="as"||nl==",");
                if (!is_kw) { alias_map[nl] = tname; i++; }
            }
            continue;
        }

        // ON -> WHERE
        if (tl == "on") { out.push_back("WHERE"); continue; }

        // 替换别名前缀: alias.col -> table.col
        size_t dot = tokens[i].find('.');
        if (dot != std::string::npos && dot > 0) {
            std::string prefix = to_lower(tokens[i].substr(0, dot));
            auto it = alias_map.find(prefix);
            if (it != alias_map.end()) {
                tokens[i] = it->second + tokens[i].substr(dot);
            }
        }

        out.push_back(tokens[i]);
    }

    // 合并多个WHERE
    std::vector<std::string> merged;
    bool seen_where = false;
    for (auto &t : out) {
        std::string tl = to_lower(t);
        if (tl == "where") {
            if (seen_where) merged.push_back("AND");
            else { merged.push_back(t); seen_where = true; }
        } else merged.push_back(t);
    }

    result.sql = "";
    for (size_t i = 0; i < merged.size(); i++) {
        if (i > 0) result.sql += " ";
        result.sql += merged[i];
    }
    result.alias_to_table = alias_map;
    return result;
}
