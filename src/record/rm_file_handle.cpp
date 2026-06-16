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

std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    auto rec = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    buffer_pool_manager_->unpin_page(PageId{fd_, rid.page_no}, false);
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

    return Rid{page_no, slot_no};
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
}
