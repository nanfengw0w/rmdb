/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }

    // 懒加载：只在需要淘汰时才将候选页面加入LRU
    seed_replacer_candidates();
    
    if (replacer_->victim(frame_id)) {
        in_replacer_[*frame_id] = 0;
        return true;
    }

    return false;
}

void BufferPoolManager::seed_replacer_candidates() {
    for (frame_id_t frame_id = 0; frame_id < static_cast<frame_id_t>(pool_size_); ++frame_id) {
        Page *page = &pages_[frame_id];
        if (page->id_.page_no != INVALID_PAGE_ID && page->pin_count_ == 0 && !in_replacer_[frame_id]) {
            replacer_->unpin(frame_id);
            in_replacer_[frame_id] = 1;
        }
    }
}

void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }

    if (page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->id_);
    }

    page->reset_memory();
    page->id_ = new_page_id;
    page_table_[new_page_id] = new_frame_id;
}

Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Page *page = &pages_[frame_id];
        // 懒加载：如果页面在LRU中，移除它
        if (page->pin_count_ == 0 && in_replacer_[frame_id]) {
            replacer_->pin(frame_id);
            in_replacer_[frame_id] = 0;
        }
        page->pin_count_++;
        return page;
    }

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    Page *page = &pages_[frame_id];
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }

    if (page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->id_);
    }

    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->id_ = page_id;
    page->pin_count_ = 1;
    page_table_[page_id] = frame_id;

    return page;
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    if (page->pin_count_ <= 0) {
        return false;
    }

    page->pin_count_--;
    // 懒加载：只在free_list为空时才将页面加入LRU候选
    if (page->pin_count_ == 0 && free_list_.empty()) {
        replacer_->unpin(frame_id);
        in_replacer_[frame_id] = 1;
    }

    if (is_dirty) {
        page->is_dirty_ = true;
    }

    return true;
}

bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;

    return true;
}

Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    page_id_t new_page_no = disk_manager_->allocate_page(page_id->fd);
    PageId new_page_id{page_id->fd, new_page_no};

    Page *page = &pages_[frame_id];
    update_page(page, new_page_id, frame_id);
    page->pin_count_ = 1;
    page->is_dirty_ = false;

    return page;
}

bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }

    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    if (page->pin_count_ != 0) {
        return false;
    }

    if (page->is_dirty_) {
        disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }

    // 懒加载：从LRU中移除
    if (in_replacer_[frame_id]) {
        replacer_->pin(frame_id);
        in_replacer_[frame_id] = 0;
    }

    page_table_.erase(it);

    page->reset_memory();
    page->id_ = PageId{-1, INVALID_PAGE_ID};
    page->pin_count_ = 0;

    free_list_.push_back(frame_id);

    return true;
}

void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};

    for (auto it = page_table_.begin(); it != page_table_.end(); ++it) {
        if (it->first.fd == fd) {
            frame_id_t frame_id = it->second;
            Page *page = &pages_[frame_id];
            disk_manager_->write_page(fd, it->first.page_no, page->data_, PAGE_SIZE);
            page->is_dirty_ = false;
        }
    }
}
