/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;  

bool LRUReplacer::victim(frame_id_t* frame_id) {
    std::scoped_lock lock{latch_};

    if (LRUlist_.empty()) {
        return false;
    }

    // Evict from the back (least recently used)
    *frame_id = LRUlist_.back();
    LRUhash_.erase(*frame_id);
    LRUlist_.pop_back();
    return true;
}

void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};

    auto it = LRUhash_.find(frame_id);
    if (it != LRUhash_.end()) {
        LRUlist_.erase(it->second);
        LRUhash_.erase(it);
    }
}

void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};

    // If already in the replacer, do not change position
    auto it = LRUhash_.find(frame_id);
    if (it != LRUhash_.end()) {
        return;
    }

    // Add to the front (most recently used)
    LRUlist_.push_front(frame_id);
    LRUhash_[frame_id] = LRUlist_.begin();
}

size_t LRUReplacer::Size() { return LRUlist_.size(); }
