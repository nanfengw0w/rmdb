/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"
#include "record/bitmap.h"
#include "record/rm_scan.h"
#include "index/ix.h"
#include <fstream>
#include <cstring>

/**
 * @description: 计算bitmap中设置的位数（即记录数）
 */
static int get_record_count_from_bitmap(const char* bitmap, int max_records) {
    int count = 0;
    for (int i = 0; i < max_records; i++) {
        if (Bitmap::is_set(bitmap, i)) {
            count++;
        }
    }
    return count;
}

/**
 * @description: 从磁盘中读取所有日志记录
 */
static std::vector<LogRecord*> read_all_logs(DiskManager* disk_manager, int start_offset = 0) {
    std::vector<LogRecord*> records;
    int file_size = disk_manager->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0 || start_offset >= file_size) return records;

    int offset = start_offset;
    while (offset < file_size) {
        // Read header first to get log type and total length
        char header_buf[LOG_HEADER_SIZE];
        int bytes_read = disk_manager->read_log(header_buf, LOG_HEADER_SIZE, offset);
        if (bytes_read < LOG_HEADER_SIZE) break;

        LogType log_type = *reinterpret_cast<const LogType*>(header_buf);
        uint32_t log_tot_len = *reinterpret_cast<const uint32_t*>(header_buf + OFFSET_LOG_TOT_LEN);

        if (log_tot_len == 0 || log_tot_len > LOG_BUFFER_SIZE) break;

        // Read the full log record
        char* log_data = new char[log_tot_len];
        bytes_read = disk_manager->read_log(log_data, log_tot_len, offset);
        if (bytes_read < (int)log_tot_len) {
            delete[] log_data;
            break;
        }

        LogRecord* record = nullptr;
        switch (log_type) {
            case LogType::begin: {
                record = new BeginLogRecord();
                record->deserialize(log_data);
                break;
            }
            case LogType::commit: {
                record = new CommitLogRecord();
                record->deserialize(log_data);
                break;
            }
            case LogType::ABORT: {
                record = new AbortLogRecord();
                record->deserialize(log_data);
                break;
            }
            case LogType::INSERT: {
                record = new InsertLogRecord();
                record->deserialize(log_data);
                break;
            }
            case LogType::DELETE: {
                record = new DeleteLogRecord();
                record->deserialize(log_data);
                break;
            }
            case LogType::UPDATE: {
                record = new UpdateLogRecord();
                record->deserialize(log_data);
                break;
            }
            case LogType::CHECKPOINT: {
                record = new CheckpointLogRecord();
                record->deserialize(log_data);
                break;
            }
            default:
                delete[] log_data;
                offset += log_tot_len;
                continue;
        }

        delete[] log_data;
        if (record) {
            records.push_back(record);
        }
        offset += log_tot_len;
    }

    return records;
}

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    // Clean up any previous state
    for (auto* rec : log_records_) {
        delete rec;
    }
    log_records_.clear();
    undo_txns_.clear();
    redo_txns_.clear();
    txn_ops_.clear();
    min_rec_lsn_ = INVALID_LSN;
    checkpoint_lsn_ = INVALID_LSN;

    // Read checkpoint LSN from restart file
    int start_offset = 0;
    std::ifstream ifs("checkpoint.lsn");
    if (ifs.is_open()) {
        ifs >> checkpoint_lsn_;
        ifs.close();

        // Find the byte offset of the checkpoint record in the log file
        int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
        if (file_size > 0) {
            int offset = 0;
            while (offset < file_size) {
                char header_buf[LOG_HEADER_SIZE];
                int bytes_read = disk_manager_->read_log(header_buf, LOG_HEADER_SIZE, offset);
                if (bytes_read < LOG_HEADER_SIZE) break;

                lsn_t lsn = *reinterpret_cast<const lsn_t*>(header_buf + OFFSET_LSN);
                uint32_t log_tot_len = *reinterpret_cast<const uint32_t*>(header_buf + OFFSET_LOG_TOT_LEN);

                if (lsn == checkpoint_lsn_) {
                    start_offset = offset;
                    break;
                }
                if (log_tot_len == 0 || log_tot_len > LOG_BUFFER_SIZE) break;
                offset += log_tot_len;
            }
        }
    }

    // Read all log records from start_offset
    log_records_ = read_all_logs(disk_manager_, start_offset);

    // Scan log records to build ATT and track operations
    for (int i = 0; i < (int)log_records_.size(); i++) {
        LogType type = log_records_[i]->log_type_;
        txn_id_t txn_id = log_records_[i]->log_tid_;

        switch (type) {
            case LogType::begin:
                undo_txns_.insert(txn_id);
                txn_ops_[txn_id].push_back(i);
                break;
            case LogType::commit:
                undo_txns_.erase(txn_id);
                redo_txns_.insert(txn_id);
                break;
            case LogType::ABORT:
                undo_txns_.erase(txn_id);
                break;
            case LogType::INSERT:
            case LogType::UPDATE:
            case LogType::DELETE:
                txn_ops_[txn_id].push_back(i);
                // Track the minimum LSN for redo
                if (min_rec_lsn_ == INVALID_LSN || log_records_[i]->lsn_ < min_rec_lsn_) {
                    min_rec_lsn_ = log_records_[i]->lsn_;
                }
                break;
            case LogType::CHECKPOINT: {
                // Add checkpoint's active transactions to undo list
                CheckpointLogRecord* ckpt = static_cast<CheckpointLogRecord*>(log_records_[i]);
                if (ckpt) {
                    for (auto active_txn : ckpt->active_txns_) {
                        undo_txns_.insert(active_txn);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    if (log_records_.empty()) return;

    for (auto* record : log_records_) {
        if (record->log_type_ != LogType::INSERT &&
            record->log_type_ != LogType::UPDATE &&
            record->log_type_ != LogType::DELETE) {
            continue;
        }

        lsn_t lsn = record->lsn_;
        txn_id_t txn_id = record->log_tid_;

        // Only redo if the transaction was committed or still in undo list
        // (for committed txns, redo is needed; for uncommitted, redo+undo)
        if (redo_txns_.find(txn_id) == redo_txns_.end() &&
            undo_txns_.find(txn_id) == undo_txns_.end()) {
            continue;
        }

        switch (record->log_type_) {
            case LogType::INSERT: {
                InsertLogRecord* insert_rec = static_cast<InsertLogRecord*>(record);
                if (!insert_rec) break;

                std::string table_name(insert_rec->table_name_, insert_rec->table_name_size_);
                auto it = sm_manager_->fhs_.find(table_name);
                if (it == sm_manager_->fhs_.end()) break;

                RmFileHandle* fh = it->second.get();
                int fd = fh->GetFd();
                RmFileHdr file_hdr = fh->get_file_hdr();
                Rid rid = insert_rec->rid_;

                // Extend file if needed (num_pages on disk may be stale)
                if (rid.page_no >= file_hdr.num_pages) {
                    fh->get_file_hdr_ref().num_pages = rid.page_no + 1;
                }

                PageId page_id{fd, rid.page_no};
                Page* page = buffer_pool_manager_->fetch_page(page_id);
                if (!page) break;

                // Check if already applied
                if (page->get_page_lsn() >= lsn) {
                    buffer_pool_manager_->unpin_page(page_id, false);
                    break;
                }

                // Redo: set bitmap and copy record data
                char* bitmap = page->get_data() + Page::OFFSET_PAGE_HDR + sizeof(RmPageHdr);
                char* slots = bitmap + file_hdr.bitmap_size;

                Bitmap::set(bitmap, rid.slot_no);
                memcpy(slots + rid.slot_no * file_hdr.record_size,
                       insert_rec->insert_value_.data, file_hdr.record_size);

                // Update page header record count
                RmPageHdr* page_hdr = reinterpret_cast<RmPageHdr*>(page->get_data() + Page::OFFSET_PAGE_HDR);
                page_hdr->num_records = get_record_count_from_bitmap(bitmap, file_hdr.num_records_per_page);

                page->set_page_lsn(lsn);
                buffer_pool_manager_->unpin_page(page_id, true);
                break;
            }
            case LogType::UPDATE: {
                UpdateLogRecord* update_rec = static_cast<UpdateLogRecord*>(record);
                if (!update_rec) break;

                std::string table_name(update_rec->table_name_, update_rec->table_name_size_);
                auto it = sm_manager_->fhs_.find(table_name);
                if (it == sm_manager_->fhs_.end()) break;

                RmFileHandle* fh = it->second.get();
                int fd = fh->GetFd();
                RmFileHdr file_hdr = fh->get_file_hdr();
                Rid rid = update_rec->rid_;

                if (rid.page_no >= file_hdr.num_pages) {
                    fh->get_file_hdr_ref().num_pages = rid.page_no + 1;
                }

                PageId page_id{fd, rid.page_no};
                Page* page = buffer_pool_manager_->fetch_page(page_id);
                if (!page) break;

                if (page->get_page_lsn() >= lsn) {
                    buffer_pool_manager_->unpin_page(page_id, false);
                    break;
                }

                // Redo: copy new record data
                char* bitmap = page->get_data() + Page::OFFSET_PAGE_HDR + sizeof(RmPageHdr);
                char* slots = bitmap + file_hdr.bitmap_size;
                memcpy(slots + rid.slot_no * file_hdr.record_size,
                       update_rec->new_value_.data, file_hdr.record_size);

                page->set_page_lsn(lsn);
                buffer_pool_manager_->unpin_page(page_id, true);
                break;
            }
            case LogType::DELETE: {
                DeleteLogRecord* delete_rec = static_cast<DeleteLogRecord*>(record);
                if (!delete_rec) break;

                std::string table_name(delete_rec->table_name_, delete_rec->table_name_size_);
                auto it = sm_manager_->fhs_.find(table_name);
                if (it == sm_manager_->fhs_.end()) break;

                RmFileHandle* fh = it->second.get();
                int fd = fh->GetFd();
                RmFileHdr file_hdr = fh->get_file_hdr();
                Rid rid = delete_rec->rid_;

                if (rid.page_no >= file_hdr.num_pages) {
                    fh->get_file_hdr_ref().num_pages = rid.page_no + 1;
                }

                PageId page_id{fd, rid.page_no};
                Page* page = buffer_pool_manager_->fetch_page(page_id);
                if (!page) break;

                if (page->get_page_lsn() >= lsn) {
                    buffer_pool_manager_->unpin_page(page_id, false);
                    break;
                }

                // Redo: clear bitmap
                char* bitmap = page->get_data() + Page::OFFSET_PAGE_HDR + sizeof(RmPageHdr);
                Bitmap::reset(bitmap, rid.slot_no);

                RmPageHdr* page_hdr = reinterpret_cast<RmPageHdr*>(page->get_data() + Page::OFFSET_PAGE_HDR);
                page_hdr->num_records = get_record_count_from_bitmap(bitmap, file_hdr.num_records_per_page);

                page->set_page_lsn(lsn);
                buffer_pool_manager_->unpin_page(page_id, true);
                break;
            }
            default:
                break;
        }
    }

    // Flush updated file headers to disk (num_pages may have been extended)
    for (auto& entry : sm_manager_->fhs_) {
        RmFileHandle* fh = entry.second.get();
        disk_manager_->write_page(fh->GetFd(), RM_FILE_HDR_PAGE,
                                  (const char *)&fh->get_file_hdr_ref(), sizeof(RmFileHdr));
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    // For each uncommitted transaction, undo operations in reverse order
    for (auto txn_id : undo_txns_) {
        auto it = txn_ops_.find(txn_id);
        if (it == txn_ops_.end()) continue;

        auto& ops = it->second;
        // Undo in reverse order
        for (int i = ops.size() - 1; i >= 0; i--) {
            int idx = ops[i];
            if (idx < 0 || idx >= (int)log_records_.size()) continue;

            LogRecord* record = log_records_[idx];
            // Skip non-data operations (begin, commit, abort, checkpoint)
            if (record->log_type_ != LogType::INSERT &&
                record->log_type_ != LogType::UPDATE &&
                record->log_type_ != LogType::DELETE) {
                continue;
            }

            switch (record->log_type_) {
                case LogType::INSERT: {
                    InsertLogRecord* insert_rec = static_cast<InsertLogRecord*>(record);
                    if (!insert_rec) break;

                    std::string table_name(insert_rec->table_name_, insert_rec->table_name_size_);
                    auto it_fh = sm_manager_->fhs_.find(table_name);
                    if (it_fh == sm_manager_->fhs_.end()) break;

                    RmFileHandle* fh = it_fh->second.get();
                    int fd = fh->GetFd();
                    RmFileHdr file_hdr = fh->get_file_hdr();
                    Rid rid = insert_rec->rid_;

                    if (rid.page_no >= file_hdr.num_pages) break;

                    PageId page_id{fd, rid.page_no};
                    Page* page = buffer_pool_manager_->fetch_page(page_id);
                    if (!page) break;

                    // Undo insert: clear bitmap bit
                    char* bitmap = page->get_data() + Page::OFFSET_PAGE_HDR + sizeof(RmPageHdr);
                    Bitmap::reset(bitmap, rid.slot_no);

                    RmPageHdr* page_hdr = reinterpret_cast<RmPageHdr*>(page->get_data() + Page::OFFSET_PAGE_HDR);
                    page_hdr->num_records = get_record_count_from_bitmap(bitmap, file_hdr.num_records_per_page);

                    page->set_page_lsn(record->lsn_);
                    buffer_pool_manager_->unpin_page(page_id, true);
                    break;
                }
                case LogType::UPDATE: {
                    UpdateLogRecord* update_rec = static_cast<UpdateLogRecord*>(record);
                    if (!update_rec) break;

                    std::string table_name(update_rec->table_name_, update_rec->table_name_size_);
                    auto it_fh = sm_manager_->fhs_.find(table_name);
                    if (it_fh == sm_manager_->fhs_.end()) break;

                    RmFileHandle* fh = it_fh->second.get();
                    int fd = fh->GetFd();
                    RmFileHdr file_hdr = fh->get_file_hdr();
                    Rid rid = update_rec->rid_;

                    if (rid.page_no >= file_hdr.num_pages) break;

                    PageId page_id{fd, rid.page_no};
                    Page* page = buffer_pool_manager_->fetch_page(page_id);
                    if (!page) break;

                    // Undo update: restore old record data
                    char* bitmap = page->get_data() + Page::OFFSET_PAGE_HDR + sizeof(RmPageHdr);
                    char* slots = bitmap + file_hdr.bitmap_size;
                    memcpy(slots + rid.slot_no * file_hdr.record_size,
                           update_rec->old_value_.data, file_hdr.record_size);

                    page->set_page_lsn(record->lsn_);
                    buffer_pool_manager_->unpin_page(page_id, true);
                    break;
                }
                case LogType::DELETE: {
                    DeleteLogRecord* delete_rec = static_cast<DeleteLogRecord*>(record);
                    if (!delete_rec) break;

                    std::string table_name(delete_rec->table_name_, delete_rec->table_name_size_);
                    auto it_fh = sm_manager_->fhs_.find(table_name);
                    if (it_fh == sm_manager_->fhs_.end()) break;

                    RmFileHandle* fh = it_fh->second.get();
                    int fd = fh->GetFd();
                    RmFileHdr file_hdr = fh->get_file_hdr();
                    Rid rid = delete_rec->rid_;

                    if (rid.page_no >= file_hdr.num_pages) break;

                    PageId page_id{fd, rid.page_no};
                    Page* page = buffer_pool_manager_->fetch_page(page_id);
                    if (!page) break;

                    // Undo delete: set bitmap and restore record data
                    char* bitmap = page->get_data() + Page::OFFSET_PAGE_HDR + sizeof(RmPageHdr);
                    char* slots = bitmap + file_hdr.bitmap_size;

                    Bitmap::set(bitmap, rid.slot_no);
                    memcpy(slots + rid.slot_no * file_hdr.record_size,
                           delete_rec->delete_value_.data, file_hdr.record_size);

                    RmPageHdr* page_hdr = reinterpret_cast<RmPageHdr*>(page->get_data() + Page::OFFSET_PAGE_HDR);
                    page_hdr->num_records = get_record_count_from_bitmap(bitmap, file_hdr.num_records_per_page);

                    page->set_page_lsn(record->lsn_);
                    buffer_pool_manager_->unpin_page(page_id, true);
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Flush all dirty pages to ensure recovered data is on disk
    for (auto& entry : sm_manager_->fhs_) {
        int fd = entry.second->GetFd();
        buffer_pool_manager_->flush_all_pages(fd);
    }

    // Rebuild indexes to be consistent with recovered record data
    for (auto& fh_entry : sm_manager_->fhs_) {
        auto& tab_name = fh_entry.first;
        if (!sm_manager_->db_.is_table(tab_name)) continue;
        TabMeta& tab = sm_manager_->db_.get_table(tab_name);
        if (tab.indexes.empty()) continue;

        // Collect index info before destroying
        std::vector<IndexMeta> indexes = tab.indexes;
        std::vector<std::vector<std::string>> index_col_names;
        for (auto& index : indexes) {
            std::vector<std::string> cols;
            for (auto& col : index.cols) {
                cols.push_back(col.name);
            }
            index_col_names.push_back(cols);
        }

        // Destroy and recreate each index
        for (size_t i = 0; i < indexes.size(); i++) {
            try {
                // Destroy old index
                std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index_col_names[i]);
                if (sm_manager_->ihs_.count(ix_name)) {
                    sm_manager_->get_ix_manager()->close_index(sm_manager_->ihs_.at(ix_name).get());
                    sm_manager_->ihs_.erase(ix_name);
                }
                sm_manager_->get_ix_manager()->destroy_index(tab_name, indexes[i].cols);

                // Recreate index
                sm_manager_->get_ix_manager()->create_index(tab_name, indexes[i].cols);
                auto ih = sm_manager_->get_ix_manager()->open_index(tab_name, indexes[i].cols);

                // Scan all records and insert into index
                auto fh = sm_manager_->fhs_.at(tab_name).get();
                RmScan scan(fh);
                while (!scan.is_end()) {
                    auto rid = scan.rid();
                    auto record = fh->get_record(rid, nullptr);
                    if (record != nullptr) {
                        std::vector<char> key(indexes[i].col_tot_len);
                        int offset = 0;
                        for (auto& col : indexes[i].cols) {
                            memcpy(key.data() + offset, record->data + col.offset, col.len);
                            offset += col.len;
                        }
                        ih->insert_entry(key.data(), rid, nullptr);
                    }
                    scan.next();
                }
                sm_manager_->ihs_.emplace(ix_name, std::move(ih));
            } catch (std::exception& e) {
                // Index rebuild failed, continue with other indexes
                std::cerr << "[Recovery] Index rebuild failed for " << tab_name << ": " << e.what() << std::endl;
            }
        }
    }

    // Clean up log records
    for (auto* rec : log_records_) {
        delete rec;
    }
    log_records_.clear();
}
