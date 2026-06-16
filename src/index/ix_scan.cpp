/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

void IxScan::next() {
    assert(!is_end());
    assert(current_node_ != nullptr);
    assert(current_node_->is_leaf_page());
    assert(iid_.slot_no < current_node_->get_size());
    // increment slot no
    iid_.slot_no++;
    if (iid_.page_no != ih_->file_hdr_->last_leaf_ && iid_.slot_no == current_node_->get_size()) {
        // go to next leaf - save next_leaf before releasing current node
        page_id_t next_leaf = current_node_->get_next_leaf();
        bpm_->unpin_page(current_node_->get_page_id(), false);
        delete current_node_;
        iid_.slot_no = 0;
        iid_.page_no = next_leaf;
        current_node_ = ih_->fetch_node(iid_.page_no);
    }
}

Rid IxScan::rid() const {
    assert(current_node_ != nullptr);
    if (iid_.slot_no >= current_node_->get_size()) {
        throw IndexEntryNotFoundError();
    }
    return *current_node_->get_rid(iid_.slot_no);
}
