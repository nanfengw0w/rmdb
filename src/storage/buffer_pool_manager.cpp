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

    if (replacer_->victim(frame_id)) {
        return true;
    }

    return false;
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
    // 快速路径：页面已在缓冲池中（只用分片锁）
    {
        size_t shard = std::hash<PageId>{}(page_id) % NUM_LATCHES;
        std::scoped_lock lock{page_latches_[shard]};
        auto it = page_table_.find(page_id);
        if (it != page_table_.end()) {
            frame_id_t frame_id = it->second;
            Page *page = &pages_[frame_id];
            page->pin_count_++;
            replacer_->pin(frame_id);
            return page;
        }
    }

    // 慢速路径：需要磁盘 I/O
    frame_id_t frame_id;
    Page *page = nullptr;
    PageId old_page_id;
    bool need_flush = false;

    {
        std::scoped_lock lock{latch_};
        // 双重检查：可能其他线程已经加载了页面
        auto it = page_table_.find(page_id);
        if (it != page_table_.end()) {
            frame_id_t frame_id = it->second;
            Page *page = &pages_[frame_id];
            page->pin_count_++;
            replacer_->pin(frame_id);
            return page;
        }

        if (!find_victim_page(&frame_id)) {
            return nullptr;
        }
        page = &pages_[frame_id];
        if (page->is_dirty_) {
            old_page_id = page->id_;
            need_flush = true;
            page->is_dirty_ = false;
        }
        if (page->id_.page_no != INVALID_PAGE_ID) {
            page_table_.erase(page->id_);
        }
        page->id_ = PageId{-1, INVALID_PAGE_ID};
        page->pin_count_ = 1;
    }

    // 在锁外做磁盘 I/O
    if (need_flush) {
        disk_manager_->write_page(old_page_id.fd, old_page_id.page_no, page->data_, PAGE_SIZE);
    }
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);

    {
        std::scoped_lock lock{latch_};
        page->id_ = page_id;
        page_table_[page_id] = frame_id;
        replacer_->pin(frame_id);
    }

    return page;
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    size_t shard = std::hash<PageId>{}(page_id) % NUM_LATCHES;
    std::scoped_lock lock{page_latches_[shard]};

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
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
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

    // Allocate a new page number using the caller's fd
    page_id_t new_page_no = disk_manager_->allocate_page(page_id->fd);
    PageId new_page_id{page_id->fd, new_page_no};

    Page *page = &pages_[frame_id];
    
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }

    if (page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->id_);
    }

    page->reset_memory();
    page->id_ = new_page_id;
    page->pin_count_ = 1;
    page_table_[new_page_id] = frame_id;

    replacer_->pin(frame_id);

    *page_id = new_page_id;

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
