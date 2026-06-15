/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: äºå¡çå¼å§æ¹æ³
 * @return {Transaction*} å¼å§äºå¡çæé
 * @param {Transaction*} txn äºå¡æéï¼ç©ºæéä»£è¡¨éè¦åå»ºæ°äºå¡ï¼å¦åå¼å§å·²æäºå¡
 * @param {LogManager*} log_manager æ¥å¿ç®¡çå¨æé
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_state(TransactionState::GROWING);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

/**
 * @description: äºå¡çæäº¤æ¹æ³
 * @param {Transaction*} txn éè¦æäº¤çäºå¡
 * @param {LogManager*} log_manager æ¥å¿ç®¡çå¨æé
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) return;
    txn->set_state(TransactionState::COMMITTED);
    // Release all locks held by this transaction
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
}

/**
 * @description: äºå¡çç»æ­¢ï¼åæ»ï¼æ¹æ³
 * @param {Transaction *} txn éè¦åæ»çäºå¡
 * @param {LogManager} *log_manager æ¥å¿ç®¡çå¨æé
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) return;

    // Undo all write operations in reverse order
    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        WriteRecord *wr = *it;
        auto &tab_name = wr->GetTableName();
        auto &rid = wr->GetRid();
        auto fh = sm_manager_->fhs_.at(tab_name).get();

        switch (wr->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                // Undo insert -> delete the record
                fh->delete_record(rid, nullptr);
                break;
            }
            case WType::DELETE_TUPLE: {
                // Undo delete -> re-insert the record
                fh->insert_record(rid, wr->GetRecord().data);
                break;
            }
            case WType::UPDATE_TUPLE: {
                // Undo update -> restore old record
                fh->update_record(rid, wr->GetRecord().data, nullptr);
                break;
            }
        }
    }

    // Release all locks
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }

    txn->set_state(TransactionState::ABORTED);
}
