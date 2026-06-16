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

#include <cassert>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include "common/context.h"
#include "common/config.h"
#include "errors.h"

#define RECORD_COUNT_LENGTH 40

class RecordPrinter {
    static constexpr size_t COL_WIDTH = 16;
    size_t num_cols;
public:
    RecordPrinter(size_t num_cols_) : num_cols(num_cols_) {
        if (num_cols_ == 0) {
            throw InternalError("No columns to print");
        }
    }

    static void append_to_buffer(Context *context, const std::string &str, bool reserve_count = true) {
        if (context == nullptr || context->data_send_ == nullptr || context->offset_ == nullptr) {
            return;
        }
        int reserve = reserve_count ? RECORD_COUNT_LENGTH : 1;
        int available = BUFFER_LENGTH - *(context->offset_) - reserve;
        if ((context->ellipsis_ && reserve_count) || available <= 0) {
            context->ellipsis_ = true;
            return;
        }
        size_t write_len = std::min(str.size(), static_cast<size_t>(available));
        if (write_len > 0) {
            memcpy(context->data_send_ + *(context->offset_), str.data(), write_len);
            *(context->offset_) += static_cast<int>(write_len);
        }
        if (write_len < str.size()) {
            context->ellipsis_ = true;
        }
    }

    void print_separator(Context *context) const {
        for (size_t i = 0; i < num_cols; i++) {
            // std::cout << '+' << std::string(COL_WIDTH + 2, '-');
            std::string str = "+" + std::string(COL_WIDTH + 2, '-');
            append_to_buffer(context, str);
        }
        std::string str = "+\n";
        append_to_buffer(context, str);
    }

    void print_record(const std::vector<std::string> &rec_str, Context *context) const {
        if (rec_str.size() != num_cols) {
            throw InternalError("Printed record column count mismatch");
        }
        for (auto col: rec_str) {
            if (col.size() > COL_WIDTH) {
                col = col.substr(0, COL_WIDTH - 3) + "...";
            }
            // std::cout << "| " << std::setw(COL_WIDTH) << col << ' ';
            std::stringstream ss;
            ss << "| " << std::setw(COL_WIDTH) << col << " ";
            append_to_buffer(context, ss.str());
        }
        // std::cout << "|\n";
        std::string str = "|\n";
        append_to_buffer(context, str);
    }

    static void print_record_count(size_t num_rec, Context *context) {
        // std::cout << "Total record(s): " << num_rec << '\n';
        std::string str = "";
        if(context->ellipsis_ == true) {
            str = "... ...\n";
        }
        str += "Total record(s): " + std::to_string(num_rec) + '\n';
        append_to_buffer(context, str, false);
    }
};
