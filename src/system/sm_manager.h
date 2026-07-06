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

#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "sm_defs.h"
#include "sm_meta.h"
#include "common/context.h"
#include <cctype>
#include <map>
#include <unordered_map>
#include <utility>

class Context;

struct ColDef {
    std::string name;  // Column name
    ColType type;      // Type of column
    int len;           // Length of column
};

/* 系统管理器，负责元数据管理和DDL语句的执行 */
class SmManager {
   public:
    DbMeta db_;             // 当前打开的数据库的元数据
    std::unordered_map<std::string, std::unique_ptr<RmFileHandle>> fhs_;    // file name -> record file handle, 当前数据库中每张表的数据文件
    std::unordered_map<std::string, std::unique_ptr<IxIndexHandle>> ihs_;   // file name -> index file handle, 当前数据库中每个索引的文件
    std::unordered_map<std::string, std::string> table_aliases_;             // query table alias -> real table name
   private:
    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    RmManager* rm_manager_;
    IxManager* ix_manager_;

   public:
    SmManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, RmManager* rm_manager,
              IxManager* ix_manager)
        : disk_manager_(disk_manager),
          buffer_pool_manager_(buffer_pool_manager),
          rm_manager_(rm_manager),
          ix_manager_(ix_manager) {}

    ~SmManager() {}

    BufferPoolManager* get_bpm() { return buffer_pool_manager_; }

    RmManager* get_rm_manager() { return rm_manager_; }  

    IxManager* get_ix_manager() { return ix_manager_; }  

    static std::string normalize_alias_key(const std::string& name) {
        std::string key = name;
        for (auto &ch : key) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return key;
    }

    void set_table_aliases(const std::map<std::string, std::string>& aliases) {
        table_aliases_.clear();
        for (const auto &entry : aliases) {
            table_aliases_[entry.first] = entry.second;
            table_aliases_[normalize_alias_key(entry.first)] = entry.second;
        }
    }

    void clear_table_aliases() { table_aliases_.clear(); }

    std::string resolve_table_name(const std::string& tab_name) const {
        auto it = table_aliases_.find(tab_name);
        if (it != table_aliases_.end()) {
            return it->second;
        }
        it = table_aliases_.find(normalize_alias_key(tab_name));
        return it == table_aliases_.end() ? tab_name : it->second;
    }

    std::vector<ColMeta> get_query_cols(const std::string& tab_name) {
        auto real_tab = resolve_table_name(tab_name);
        auto cols = db_.get_table(real_tab).cols;
        if (real_tab != tab_name) {
            for (auto &col : cols) {
                col.tab_name = tab_name;
            }
        }
        return cols;
    }

    RmFileHandle* get_table_fh(const std::string& tab_name) {
        return fhs_.at(resolve_table_name(tab_name)).get();
    }

    bool is_dir(const std::string& db_name);

    void create_db(const std::string& db_name);

    void drop_db(const std::string& db_name);

    void open_db(const std::string& db_name);

    void close_db();

    void flush_meta();

    void reload_meta();

    void show_tables(Context* context);

    void desc_table(const std::string& tab_name, Context* context);

    void show_index(const std::string& tab_name, Context* context);

    void create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context);

    void drop_table(const std::string& tab_name, Context* context);

    void create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context);

    void drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context);
    
    void drop_index(const std::string& tab_name, const std::vector<ColMeta>& col_names, Context* context);
};

class SmTableAliasGuard {
   public:
    SmTableAliasGuard(SmManager* sm_manager, const std::map<std::string, std::string>& aliases)
        : sm_manager_(sm_manager), old_aliases_(sm_manager->table_aliases_) {
        sm_manager_->set_table_aliases(aliases);
    }

    ~SmTableAliasGuard() {
        sm_manager_->table_aliases_ = std::move(old_aliases_);
    }

   private:
    SmManager* sm_manager_;
    std::unordered_map<std::string, std::string> old_aliases_;
};
