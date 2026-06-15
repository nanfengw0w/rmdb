/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    rid_ = {RM_FIRST_RECORD_PAGE, -1};
    next();
}

void RmScan::next() {
    if (is_end()) return;

    // Find the next valid record starting from the current position
    rid_.slot_no = Bitmap::next_bit(true, file_handle_->fetch_page_handle(rid_.page_no).bitmap,
                                    file_handle_->file_hdr_.num_records_per_page, rid_.slot_no);

    // If we didn't find a valid slot on the current page, move to the next page
    while (rid_.slot_no >= file_handle_->file_hdr_.num_records_per_page) {
        rid_.page_no++;
        if (rid_.page_no >= file_handle_->file_hdr_.num_pages) {
            return;
        }
        rid_.slot_no = Bitmap::first_bit(true, file_handle_->fetch_page_handle(rid_.page_no).bitmap,
                                         file_handle_->file_hdr_.num_records_per_page);
    }
}

bool RmScan::is_end() const {
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}

Rid RmScan::rid() const {
    return rid_;
}
