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
#include "execution_defs.h"
#include "execution_manager.h"
#include "index_maintenance.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "recovery/log_manager.h"

extern TransactionManager* g_txn_manager;

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;
    bool done_;

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
        done_ = false;
    };

    std::unique_ptr<RmRecord> Next() override {
        if (done_) {
            return nullptr;
        }
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            if (col.type != val.type) {
                if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                    val.set_float(static_cast<float>(val.int_val));
                } else {
                    throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                }
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }

        std::vector<std::vector<char>> index_keys;
        index_keys.reserve(tab_.indexes.size());
        for (auto &index : tab_.indexes) {
            auto key = index_maintenance::build_key(index, rec.data);
            index_maintenance::check_unique_conflict(sm_manager_, tab_name_, index, key.data(),
                                                     std::nullopt, context_);
            index_keys.emplace_back(std::move(key));
        }
        index_maintenance::check_logical_key_write_conflict(sm_manager_, tab_, tab_name_,
                                                            rec.data, std::nullopt, context_);

        // Insert into record file
        rid_ = fh_->insert_record(rec.data, context_);

        // WAL: Write insert log record and flush (WAL: log must be on disk before data)
        if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
            InsertLogRecord insert_log(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
            lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&insert_log);
            context_->txn_->set_prev_lsn(lsn);
            context_->log_mgr_->flush_log_to_disk();
        }

        // Record write operation for transaction abort
        if (context_ != nullptr && context_->txn_ != nullptr) {
            context_->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_));
        }

        Transaction *txn = context_ == nullptr ? nullptr : context_->txn_;
        // SSI: 检查 rw 依赖
        if (txn != nullptr && g_txn_manager != nullptr) {
            g_txn_manager->record_write(txn, tab_name_, rid_, WType::INSERT_TUPLE,
                                         nullptr, &rec, true, false);
            auto deps = g_txn_manager->check_rw_on_write(txn, tab_name_, rid_, nullptr, &rec);
            for (auto& [from, to] : deps) {
                if (g_txn_manager->add_rw_dependency_and_check(from, to)) {
                    throw TransactionAbortException(txn->get_transaction_id(),
                        AbortReason::DEADLOCK_PREVENTION);
                }
            }
        }

        // Insert into index after all aborting checks that can run before index mutation.
        for(size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto& index = tab_.indexes[i];
            auto ih = index_maintenance::get_index_handle(sm_manager_, tab_name_, index);
            ih->insert_entry(index_keys[i].data(), rid_, txn);
        }

        done_ = true;
        return nullptr;
    }
    Rid &rid() override { return rid_; }

    bool is_end() const override { return done_; }
};
