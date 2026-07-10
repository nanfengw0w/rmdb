/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "common/config.h"
#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"
#include "transaction/version_manager.h"
#include "transaction/transaction_manager.h"

namespace {

std::vector<std::string> index_col_names(const std::vector<ColMeta> &cols) {
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto &col : cols) {
        col_names.push_back(col.name);
    }
    return col_names;
}

std::vector<char> build_index_key(const IndexMeta &index_meta, const char *record_data) {
    std::vector<char> key(index_meta.col_tot_len);
    int offset = 0;
    for (const auto &col : index_meta.cols) {
        memcpy(key.data() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}

bool column_is_still_indexed(const TabMeta &tab, const std::string &col_name) {
    for (const auto &index : tab.indexes) {
        for (const auto &col : index.cols) {
            if (col.name == col_name) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

namespace {

void make_index_key(const IndexMeta &index, const RmRecord *rec, char *key) {
    int offset = 0;
    for (auto &col : index.cols) {
        memcpy(key + offset, rec->data + col.offset, col.len);
        offset += col.len;
    }
}

void parse_csv_line(const std::string &line, std::vector<std::string> &fields) {
    fields.clear();
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            size_t len = i - start;
            if (len > 0 && line[start + len - 1] == '\r') {
                len--;
            }
            fields.emplace_back(line.data() + start, len);
            start = i + 1;
        }
    }
}

struct LoadIndexEntry {
    const IndexMeta *index;
    IxIndexHandle *ih;
    std::vector<char> key;
};

bool is_csv_header(const std::vector<std::string> &fields, const TabMeta &tab) {
    if (fields.size() != tab.cols.size()) {
        return false;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i] != tab.cols[i].name) {
            return false;
        }
    }
    return true;
}

void write_csv_field(const std::string &field, const ColMeta &col, RmRecord &rec) {
    char *dest = rec.data + col.offset;
    if (col.type == TYPE_INT) {
        char *end = nullptr;
        errno = 0;
        long value = std::strtol(field.c_str(), &end, 10);
        if (end == field.c_str() || *end != '\0' || errno != 0) {
            throw IncompatibleTypeError(coltype2str(col.type), "STRING");
        }
        int int_value = static_cast<int>(value);
        memcpy(dest, &int_value, sizeof(int));
    } else if (col.type == TYPE_FLOAT) {
        char *end = nullptr;
        errno = 0;
        float value = std::strtof(field.c_str(), &end);
        if (end == field.c_str() || *end != '\0' || errno != 0) {
            throw IncompatibleTypeError(coltype2str(col.type), "STRING");
        }
        memcpy(dest, &value, sizeof(float));
    } else {
        if ((int)field.size() > col.len) {
            throw StringOverflowError();
        }
        memset(dest, 0, col.len);
        memcpy(dest, field.data(), field.size());
    }
}

}  // namespace

/**
 * @description: å¤æ­æ¯å¦ä¸ºä¸ä¸ªæä»¶å¤¹
 * @return {bool} è¿åæ¯å¦ä¸ºä¸ä¸ªæä»¶å¤¹
 * @param {string&} db_name æ°æ®åºæä»¶åç§°ï¼ä¸æä»¶å¤¹åå
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: åå»ºæ°æ®åºï¼ææçæ°æ®åºç¸å³æä»¶é½æ¾å¨æ°æ®åºååæä»¶å¤¹ä¸?
 * @param {string&} db_name æ°æ®åºåç§?
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //ä¸ºæ°æ®åºåå»ºä¸ä¸ªå­ç®å½
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // åå»ºä¸ä¸ªåä¸ºdb_nameçç®å½?
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // è¿å¥åä¸ºdb_nameçç®å½?
        throw UnixError();
    }
    //åå»ºç³»ç»ç®å½
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // æ³¨æï¼æ­¤å¤ofstreamä¼å¨å½åç®å½åå»º(å¦ææ²¡ææ­¤æä»¶ååå»º)åæå¼ä¸ä¸ªåä¸ºDB_META_NAMEçæä»?
    std::ofstream ofs(DB_META_NAME);

    // å°new_dbä¸­çä¿¡æ¯ï¼æç§å®ä¹å¥½çoperator<<æä½ç¬¦ï¼åå¥å°ofsæå¼çDB_META_NAMEæä»¶ä¸?
    ofs << *new_db;  // æ³¨æï¼æ­¤å¤éè½½äºæä½ç¬?<

    delete new_db;

    // åå»ºæ¥å¿æä»¶
    disk_manager_->create_file(LOG_FILE_NAME);

    // åå°æ ¹ç®å½?
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: å é¤æ°æ®åºï¼åæ¶éè¦æ¸ç©ºç¸å³æä»¶ä»¥åæ°æ®åºååæä»¶å¤?
 * @param {string&} db_name æ°æ®åºåç§°ï¼ä¸æä»¶å¤¹åå
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: æå¼æ°æ®åºï¼æ¾å°æ°æ®åºå¯¹åºçæä»¶å¤¹ï¼å¹¶å è½½æ°æ®åºåæ°æ®åç¸å³æä»¶
 * @param {string&} db_name æ°æ®åºåç§°ï¼ä¸æä»¶å¤¹åå
 */
void SmManager::open_db(const std::string& db_name) {
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        throw UnixError();
    }
    ifs >> db_;
    ifs.close();

    // Open log file for WAL
    try {
        disk_manager_->create_file(LOG_FILE_NAME);
    } catch (FileExistsError &) {
        // log file already exists, that's expected
    }
    int log_fd = disk_manager_->open_file(LOG_FILE_NAME);
    disk_manager_->SetLogFd(log_fd);

    for (auto &entry : db_.tabs_) {
        auto &tab_name = entry.first;
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
        // 设置fd2pageno，确保新页面从正确的位置开始分配
        auto &fh = fhs_.at(tab_name);
        disk_manager_->set_fd2pageno(fh->GetFd(), fh->get_file_hdr().num_pages);
        // Open existing indexes
        auto &tab = entry.second;
        for (auto &index : tab.indexes) {
            std::vector<std::string> col_names;
            for (auto &col : index.cols) {
                col_names.push_back(col.name);
            }
            std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
            if (ix_manager_->exists(tab_name, col_names)) {
                ihs_.emplace(ix_name, ix_manager_->open_index(tab_name, col_names));
            }
        }
    }
}

/**
 * @description: ææ°æ®åºç¸å³çåæ°æ®å·å¥ç£çä¸?
 */
void SmManager::flush_meta() {
    // é»è®¤æ¸ç©ºæä»¶
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: Reload database metadata from disk
 */
void SmManager::reload_meta() {
    std::ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        return;  // File doesn't exist, keep current metadata
    }

    // Read new metadata into temporary structure
    DbMeta new_db;
    ifs >> new_db;
    ifs.close();

    // Update index information for each table
    for (auto &new_entry : new_db.tabs_) {
        auto &tab_name = new_entry.first;
        auto &new_tab = new_entry.second;

        // Find existing table
        auto it = db_.tabs_.find(tab_name);
        if (it != db_.tabs_.end()) {
            auto &existing_tab = it->second;

            // Check if indexes changed
            if (existing_tab.indexes.size() != new_tab.indexes.size()) {
                // Update indexes
                existing_tab.indexes = new_tab.indexes;

                // Update column index flags
                for (auto &col : existing_tab.cols) {
                    col.index = false;
                }
                for (auto &index : existing_tab.indexes) {
                    for (auto &index_col : index.cols) {
                        for (auto &col : existing_tab.cols) {
                            if (col.name == index_col.name) {
                                col.index = true;
                                break;
                            }
                        }
                    }
                }

                // Open new indexes
                for (auto &index : existing_tab.indexes) {
                    std::vector<std::string> col_names;
                    for (auto &col : index.cols) {
                        col_names.push_back(col.name);
                    }
                    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);

                    // Check if already open
                    if (ihs_.find(ix_name) == ihs_.end() && ix_manager_->exists(tab_name, col_names)) {
                        ihs_.emplace(ix_name, ix_manager_->open_index(tab_name, col_names));
                    }
                }
            }
        }
    }
}

/**
 * @description: å³é­æ°æ®åºå¹¶ææ°æ®è½ç?
 */
void SmManager::close_db() {
    flush_meta();
    // Close all index handles first
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();
    // Close all record file handles
    for (auto &entry : fhs_) {
        VersionManager::instance().clear_fd(entry.second->GetFd());
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();
    VersionManager::instance().clear_all();
}

/**
 * @description: æ¾ç¤ºææçè¡?éè¿æµè¯éè¦å°å¶ç»æåå¥å°output.txt,è¯¦æçé¢ç®ææ¡?
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    bool write_output_file = enable_output_file.load();
    std::fstream outfile;
    if (write_output_file) {
        outfile.open("output.txt", std::ios::out | std::ios::app);
        outfile << "| Tables |\n";
    }
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        if (write_output_file) {
            outfile << "| " << tab.name << " |\n";
        }
    }
    printer.print_separator(context);
    if (write_output_file) {
        outfile.close();
    }
}

/**
 * @description: æ¾ç¤ºè¡¨çåæ°æ?
 * @param {string&} tab_name è¡¨åç§?
 * @param {Context*} context 
 */
void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    bool write_output_file = enable_output_file.load();
    std::fstream outfile;
    if (write_output_file) {
        outfile.open("output.txt", std::ios::out | std::ios::app);
    }
    for (auto &index : tab.indexes) {
        std::string col_str = "(";
        for (int i = 0; i < index.col_num; i++) {
            if (i > 0) col_str += ",";
            col_str += index.cols[i].name;
        }
        col_str += ")";
        std::string line = "| " + tab_name + " | unique | " + col_str + " |";
        if (write_output_file) {
            outfile << line << "\n";
        }
    }
    if (write_output_file) {
        outfile.close();
    }
}

void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: åå»ºè¡?
 * @param {string&} tab_name è¡¨çåç§°
 * @param {vector<ColDef>&} col_defs è¡¨çå­æ®µ
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_sizeå°±æ¯col metaæå çå¤§å°ï¼è¡¨çåæ°æ®ä¹æ¯ä»¥è®°å½çå½¢å¼è¿è¡å­å¨çï¼
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: å é¤è¡?
 * @param {string&} tab_name è¡¨çåç§°
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    // Close file handle first before destroying
    if (fhs_.count(tab_name)) {
        VersionManager::instance().clear_fd(fhs_.at(tab_name)->GetFd());
        rm_manager_->close_file(fhs_.at(tab_name).get());
        fhs_.erase(tab_name);
    }
    rm_manager_->destroy_file(tab_name);
    db_.tabs_.erase(tab_name);
    flush_meta();
}

/**
 * @description: åå»ºç´¢å¼
 * @param {string&} tab_name è¡¨çåç§°
 * @param {vector<string>&} col_names ç´¢å¼åå«çå­æ®µåç§?
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    // Check if index already exists
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    // Get column metadata for index
    std::vector<ColMeta> index_cols;
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index_cols.push_back(*col);
    }

    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_num = index_cols.size();
    index_meta.col_tot_len = 0;
    for (auto &col : index_cols) {
        index_meta.cols.push_back(col);
        index_meta.col_tot_len += col.len;
    }

    ix_manager_->create_index(tab_name, index_cols);
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    auto ih = ix_manager_->open_index(tab_name, index_cols);

    try {
        auto fh = fhs_.at(tab_name).get();
        RmScan scan(fh);
        while (!scan.is_end()) {
            auto rid = scan.rid();
            auto record = fh->get_record(rid, context);
            if (record == nullptr) {
                scan.next();
                continue;
            }
            auto key = build_index_key(index_meta, record->data);

            std::vector<Rid> existing;
            if (ih->get_value(key.data(), &existing, nullptr) && !existing.empty()) {
                throw UniqueIndexConflictError(tab_name, col_names);
            }

            ih->insert_entry(key.data(), rid, nullptr);
            scan.next();
        }
    } catch (...) {
        ix_manager_->close_index(ih.get());
        ih.reset();
        ix_manager_->destroy_index(tab_name, index_cols);
        throw;
    }

    ihs_.emplace(ix_name, std::move(ih));
    tab.indexes.push_back(index_meta);
    for (auto &col_name : col_names) {
        tab.get_col(col_name)->index = true;
    }
    flush_meta();
}

/**
 * @description: å é¤ç´¢å¼
 * @param {string&} tab_name è¡¨åç§?
 * @param {vector<string>&} col_names ç´¢å¼åå«çå­æ®µåç§?
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    // Find and remove index metadata
    auto it = tab.indexes.begin();
    while (it != tab.indexes.end()) {
        if (it->col_num == (int)col_names.size()) {
            bool match = true;
            for (int i = 0; i < (int)col_names.size(); i++) {
                if (it->cols[i].name != col_names[i]) {
                    match = false;
                    break;
                }
            }
            if (match) break;
        }
        it++;
    }
    if (it == tab.indexes.end()) {
        throw IndexNotFoundError(tab_name, col_names);
    }

    // Close and destroy index file
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    if (ihs_.count(ix_name)) {
        ix_manager_->close_index(ihs_.at(ix_name).get());
        ihs_.erase(ix_name);
    }
    ix_manager_->destroy_index(tab_name, it->cols);

    std::vector<std::string> dropped_cols = index_col_names(it->cols);
    tab.indexes.erase(it);
    for (auto &col_name : dropped_cols) {
        tab.get_col(col_name)->index = column_is_still_indexed(tab, col_name);
    }
    flush_meta();
}

/**
 * @description: å é¤ç´¢å¼
 * @param {string&} tab_name è¡¨åç§?
 * @param {vector<ColMeta>&} ç´¢å¼åå«çå­æ®µåæ°æ®
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}

void SmManager::load_csv(const std::string &file_name, const std::string &tab_name, Context *context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    std::ifstream ifs(file_name);
    if (!ifs.is_open()) {
        throw FileNotFoundError(file_name);
    }

    TabMeta &tab = db_.get_table(tab_name);
    RmFileHandle *fh = fhs_.at(tab_name).get();
    std::vector<LoadIndexEntry> load_indexes;
    load_indexes.reserve(tab.indexes.size());
    for (auto &index : tab.indexes) {
        auto ih = ihs_.at(ix_manager_->get_index_name(tab_name, index.cols)).get();
        load_indexes.push_back({&index, ih, std::vector<char>(index.col_tot_len)});
    }
    std::string line;
    std::vector<std::string> fields;
    fields.reserve(tab.cols.size());
    bool first_line = true;
    bool changed = false;
    try {
        while (std::getline(ifs, line)) {
            if (line.empty() || line == "\r") {
                continue;
            }
            parse_csv_line(line, fields);
            if (first_line) {
                first_line = false;
                if (is_csv_header(fields, tab)) {
                    continue;
                }
            }
            if (fields.size() != tab.cols.size()) {
                throw InvalidValueCountError();
            }

            RmRecord rec(fh->get_file_hdr().record_size);
            for (size_t i = 0; i < fields.size(); ++i) {
                write_csv_field(fields[i], tab.cols[i], rec);
            }

            Transaction *txn = context == nullptr ? nullptr : context->txn_;

            for (auto &load_index : load_indexes) {
                make_index_key(*load_index.index, &rec, load_index.key.data());
                std::vector<Rid> result;
                if (load_index.ih->get_value(load_index.key.data(), &result, txn)) {
                    throw IndexExistsError(tab_name, index_col_names(load_index.index->cols));
                }
            }

            Rid rid = fh->insert_record(rec.data, context);

            for (size_t i = 0; i < load_indexes.size(); ++i) {
                auto &load_index = load_indexes[i];
                if (load_index.ih->insert_entry(load_index.key.data(), rid, txn) == IX_NO_PAGE) {
                    for (size_t j = 0; j < i; ++j) {
                        load_indexes[j].ih->delete_entry(load_indexes[j].key.data(), txn);
                    }
                    fh->delete_record(rid, context);
                    throw IndexExistsError(tab_name, index_col_names(load_index.index->cols));
                }
            }

            if (txn != nullptr) {
                txn->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name, rid));
            }
            changed = true;
        }
    } catch (RMDBError &e) {
        if (changed) {
            throw std::runtime_error(e.what());
        }
        throw;
    }

    // LOAD is a system-level bulk operation and is invoked without a
    // transaction context.  Its records and indexes may still be dirty in
    // the buffer pool, while the file headers still contain the old page
    // counts.  Persist the complete table state before acknowledging LOAD;
    // otherwise a restart can reopen an apparently empty table even though
    // the load succeeded in memory.
    if (context == nullptr && changed) {
        const int table_fd = fh->GetFd();
        disk_manager_->write_page(table_fd, RM_FILE_HDR_PAGE,
                                   reinterpret_cast<const char *>(&fh->get_file_hdr_ref()),
                                   sizeof(RmFileHdr));
        buffer_pool_manager_->flush_all_pages(table_fd);

        for (const auto &load_index : load_indexes) {
            load_index.ih->flush_file_header();
            buffer_pool_manager_->flush_all_pages(load_index.ih->GetFd());
        }
    }
}
