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

#include "ix_defs.h"
#include "ix_index_handle.h"

// class IxIndexHandle;

// 用于遍历叶子结点
// 用于直接遍历叶子结点，而不用findleafpage来得到叶子结点
// TODO：对page遍历时，要加上读锁
class IxScan : public RecScan {
    const IxIndexHandle *ih_;
    Iid iid_;  // 初始为lower（用于遍历的指针）
    Iid end_;  // 初始为upper
    BufferPoolManager *bpm_;
    IxNodeHandle *current_node_;  // 缓存当前叶子节点

   public:
    IxScan(const IxIndexHandle *ih, const Iid &lower, const Iid &upper, BufferPoolManager *bpm)
        : ih_(ih), iid_(lower), end_(upper), bpm_(bpm), current_node_(nullptr) {
        // 预加载当前叶子节点
        if (!is_end()) {
            current_node_ = ih_->fetch_node(iid_.page_no);
        }
    }

    ~IxScan() {
        if (current_node_) {
            bpm_->unpin_page(current_node_->get_page_id(), false);
            delete current_node_;
            current_node_ = nullptr;
        }
    }

    void next() override;

    bool is_end() const override { return iid_ == end_; }

    Rid rid() const override;

    const char *key() const {
        assert(current_node_ != nullptr);
        if (iid_.slot_no >= current_node_->get_size()) {
            throw IndexEntryNotFoundError();
        }
        return current_node_->get_key(iid_.slot_no);
    }

    const Iid &iid() const { return iid_; }
};
