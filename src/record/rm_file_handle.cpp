/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"
#include "transaction/version_manager.h"
#include "transaction/transaction_manager.h"

extern TransactionManager* g_txn_manager;

std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, false);
        return nullptr;
    }
    auto rec = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, false);

    // MVCC: 检查版本可见性
    if (context != nullptr && context->txn_ != nullptr) {
        IsolationLevel level = context->txn_->get_isolation_level();
        if (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE) {
            auto& vm = VersionManager::instance();
            bool is_deleted = false;
            RmRecord* result = nullptr;
            int vis = vm.get_visible_data(fd_, rid, context->txn_, result, is_deleted);

            if (vis == 0) {
                // 记录不存在（被未提交的INSERT或快照不可见的INSERT影响）
                return nullptr;
            } else if (vis == 1) {
                // 从old_data读取（被未提交的UPDATE/DELETE或快照不可见的UPDATE/DELETE影响）
                if (is_deleted) {
                    return nullptr;
                }
                return std::make_unique<RmRecord>(*result);
            }
            // vis == -1，从磁盘读取最新数据
        }
    }

    return rec;
}

Rid RmFileHandle::insert_record(char* buf, Context* context) {
    RmPageHandle page_handle = create_page_handle();

    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);

    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);

    Bitmap::set(page_handle.bitmap, slot_no);

    page_handle.page_hdr->num_records++;

    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    }

    BufferPoolManager::mark_dirty(page_handle.page);

    int page_no = page_handle.page->get_page_id().page_no;
    buffer_pool_manager_->unpin_page(PageId{fd_, page_no}, true);

    Rid rid{page_no, slot_no};

    // MVCC: 记录插入操作（旧数据为空，表示之前记录不存在）
    if (context != nullptr && context->txn_ != nullptr) {
        IsolationLevel level = context->txn_->get_isolation_level();
        if (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE) {
            auto& vm = VersionManager::instance();
            vm.save_old_data(fd_, rid, context->txn_, nullptr, true);  // 之前不存在
        }
    }

    return rid;
}

void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records++;

    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    }

    BufferPoolManager::mark_dirty(page_handle.page);
    buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, true);
}

void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    bool mvcc_delete = false;
    // MVCC: 检查写写冲突并保存旧数据
    if (context != nullptr && context->txn_ != nullptr) {
        IsolationLevel level = context->txn_->get_isolation_level();
        if (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE) {
            mvcc_delete = true;
            auto& vm = VersionManager::instance();

            if (!vm.check_write_conflict(fd_, rid, context->txn_)) {
                buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, false);
                throw TransactionAbortException(context->txn_->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }

            // 保存旧数据
            RmRecord old_data(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
            vm.save_old_data(fd_, rid, context->txn_, &old_data, false, true);
        }
    }

    if (mvcc_delete) {
        // MVCC 下不能清 bitmap，否则早于删除提交的快照扫描不到旧版本。
        // 记录的“写后删除”状态保存在版本链中。
        BufferPoolManager::mark_dirty(page_handle.page);
        buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, true);
        return;
    }

    Bitmap::reset(page_handle.bitmap, rid.slot_no);

    page_handle.page_hdr->num_records--;

    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page - 1) {
        release_page_handle(page_handle);
    }

    BufferPoolManager::mark_dirty(page_handle.page);
    buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, true);
}

void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // MVCC: 检查写写冲突并保存旧数据
    if (context != nullptr && context->txn_ != nullptr) {
        IsolationLevel level = context->txn_->get_isolation_level();
        if (level == IsolationLevel::SNAPSHOT_ISOLATION || level == IsolationLevel::SERIALIZABLE) {
            auto& vm = VersionManager::instance();

            if (!vm.check_write_conflict(fd_, rid, context->txn_)) {
                buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, false);
                throw TransactionAbortException(context->txn_->get_transaction_id(),
                    AbortReason::DEADLOCK_PREVENTION);
            }

            // 保存旧数据
            RmRecord old_data(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
            vm.save_old_data(fd_, rid, context->txn_, &old_data, false);
        }
    }

    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);

    BufferPoolManager::mark_dirty(page_handle.page);
    buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, true);
}

RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no < 0 || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError("table", page_no);
    }
    Page* page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    if (page == nullptr) {
        throw PageNotExistError("table", page_no);
    }
    return RmPageHandle(&file_hdr_, page);
}

RmPageHandle RmFileHandle::create_new_page_handle() {
    PageId new_page_id{fd_, -1};
    Page* page = buffer_pool_manager_->new_page(&new_page_id);
    if (page == nullptr) {
        throw InternalError("Failed to create new page");
    }

    RmPageHdr* page_hdr = reinterpret_cast<RmPageHdr*>(page->get_data() + Page::OFFSET_PAGE_HDR);
    page_hdr->next_free_page_no = RM_NO_PAGE;
    page_hdr->num_records = 0;

    char* bitmap = page->get_data() + sizeof(RmPageHdr) + Page::OFFSET_PAGE_HDR;
    Bitmap::init(bitmap, file_hdr_.bitmap_size);

    file_hdr_.num_pages++;

    // Flush file header to buffer pool and disk to ensure num_pages is persisted
    // This is critical for crash recovery - without this, recovery can't find newly allocated pages
    PageId hdr_page_id{fd_, RM_FILE_HDR_PAGE};
    Page* hdr_page = buffer_pool_manager_->fetch_page(hdr_page_id);
    if (hdr_page) {
        memcpy(hdr_page->get_data(), &file_hdr_, sizeof(file_hdr_));
        buffer_pool_manager_->unpin_page(hdr_page_id, true);
        buffer_pool_manager_->flush_page(hdr_page_id);
    }

    BufferPoolManager::mark_dirty(page);

    return RmPageHandle(&file_hdr_, page);
}

RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        return fetch_page_handle(file_hdr_.first_free_page_no);
    }
    return create_new_page_handle();
}

void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
    // Flush file header to persist first_free_page_no change
    PageId hdr_page_id{fd_, RM_FILE_HDR_PAGE};
    Page* hdr_page = buffer_pool_manager_->fetch_page(hdr_page_id);
    if (hdr_page) {
        memcpy(hdr_page->get_data(), &file_hdr_, sizeof(file_hdr_));
        buffer_pool_manager_->unpin_page(hdr_page_id, true);
        buffer_pool_manager_->flush_page(hdr_page_id);
    }
}
