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
#include <utility>

#include "execution_defs.h"
#include "execution_manager.h"
#include "index_maintenance.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    size_t cur_idx_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
        cur_idx_ = 0;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (cur_idx_ >= rids_.size()) {
            return nullptr;
        }

        struct PendingUpdate {
            Rid rid;
            std::unique_ptr<RmRecord> old_record;
            std::unique_ptr<RmRecord> new_record;
            std::vector<std::vector<char>> old_keys;
            std::vector<std::vector<char>> new_keys;
        };

        std::vector<PendingUpdate> pending_updates;
        pending_updates.reserve(rids_.size());

        for (auto &rid : rids_) {
            PendingUpdate pending;
            pending.rid = rid;
            pending.old_record = fh_->get_record(rid, context_);
            pending.new_record = std::make_unique<RmRecord>(*pending.old_record);

            for (auto set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                Value rhs = set_clause.rhs;
                if (col->type != rhs.type) {
                    if (col->type == TYPE_FLOAT && rhs.type == TYPE_INT) {
                        rhs.set_float(static_cast<float>(rhs.int_val));
                    } else {
                        throw IncompatibleTypeError(coltype2str(col->type), coltype2str(rhs.type));
                    }
                }
                rhs.init_raw(col->len);
                memcpy(pending.new_record->data + col->offset, rhs.raw->data, col->len);
            }

            pending.old_keys.reserve(tab_.indexes.size());
            pending.new_keys.reserve(tab_.indexes.size());
            for (auto &index : tab_.indexes) {
                pending.old_keys.emplace_back(index_maintenance::build_key(index, pending.old_record->data));
                pending.new_keys.emplace_back(index_maintenance::build_key(index, pending.new_record->data));
            }
            pending_updates.emplace_back(std::move(pending));
        }

        for (auto &pending : pending_updates) {
            for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
                index_maintenance::check_unique_conflict(sm_manager_, tab_name_, tab_.indexes[index_no],
                                                         pending.new_keys[index_no].data(), pending.rid);
            }
        }

        for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
            for (size_t i = 0; i < pending_updates.size(); ++i) {
                for (size_t j = i + 1; j < pending_updates.size(); ++j) {
                    if (pending_updates[i].new_keys[index_no] == pending_updates[j].new_keys[index_no]) {
                        throw UniqueIndexConflictError(tab_name_,
                                                       index_maintenance::index_col_names(tab_.indexes[index_no]));
                    }
                }
            }
        }

        Transaction *txn = context_ == nullptr ? nullptr : context_->txn_;
        for (auto &pending : pending_updates) {
            if (txn != nullptr) {
                txn->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, pending.rid, *pending.old_record));
            }

            for (size_t index_no = 0; index_no < tab_.indexes.size(); ++index_no) {
                if (pending.old_keys[index_no] == pending.new_keys[index_no]) {
                    continue;
                }
                auto ih = index_maintenance::get_index_handle(sm_manager_, tab_name_, tab_.indexes[index_no]);
                ih->delete_entry(pending.old_keys[index_no].data(), txn);
                ih->insert_entry(pending.new_keys[index_no].data(), pending.rid, txn);
            }
            fh_->update_record(pending.rid, pending.new_record->data, context_);
        }

        cur_idx_ = rids_.size();
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return cur_idx_ >= rids_.size(); }
};
