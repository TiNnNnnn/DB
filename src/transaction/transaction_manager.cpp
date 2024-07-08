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
#include "record/rm_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针

    if (txn == nullptr) {
        txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id);
        
    }
    {
        //把开始事务加入到全局事务表中
        std::unique_lock<std::mutex> lock(latch_);
        txn_map[txn->get_transaction_id()] = txn;
    }
    //将begin_log写入log_buffer
    BeginLogRecord begin_log(txn->get_transaction_id());
    log_manager->add_log_to_buffer(&begin_log);

    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    RmManager* rm_mgr =  sm_manager_->get_rm_manager();
    //提交所有未提交的写操作
    if (txn->get_write_set()->size() > 0) {
        auto write_set = txn->get_write_set();
        while(!write_set->empty()){
            auto w_set = write_set->front();
            auto rm_file_hdr = rm_mgr->open_file(w_set->GetTableName());
            if(w_set->GetWriteType() == WType::INSERT_TUPLE){
                rm_file_hdr->insert_record(w_set->GetRid(),w_set->GetRecord().data);
            }else if(w_set->GetWriteType() == WType::DELETE_TUPLE){
                rm_file_hdr->delete_record(w_set->GetRid(),nullptr);
            }else if(w_set->GetWriteType() == WType::UPDATE_TUPLE){
                auto old_value = rm_file_hdr->get_record(w_set->GetRid(),nullptr);
                if(!old_value){
                    throw InternalError("no value in page: "+std::to_string(w_set->GetRid().page_no)+",slot_no:"+std::to_string(w_set->GetRid().slot_no));
                }
                rm_file_hdr->update_record(w_set->GetRid(),w_set->GetRecord().data,nullptr);
                w_set->SetRecord(*old_value);
            }else{
                throw InternalError("bad wtype");
            }
        }
    }
    //释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto& lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    //释放所有资源
    txn->get_write_set()->clear();
    txn->get_lock_set()->clear();

    // 将事务日志刷入磁盘
    CommitLogRecord commit_log(txn->get_transaction_id());
    log_manager->add_log_to_buffer(&commit_log);
    log_manager->flush_log_to_disk();

    txn->set_state(TransactionState::COMMITTED);
    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn->get_transaction_id());
    }
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
      RmManager* rm_mgr = sm_manager_->get_rm_manager();
    auto write_set = txn->get_write_set();

    // 1. 回滚所有写操作
    while (!write_set->empty()) {
        auto w_set = write_set->back();
        auto rm_file_hdr = rm_mgr->open_file(w_set->GetTableName());
        if (w_set->GetWriteType() == WType::INSERT_TUPLE) {
            // 删除插入的记录
            rm_file_hdr->delete_record(w_set->GetRid(), nullptr);
        } else if (w_set->GetWriteType() == WType::DELETE_TUPLE) {
            // 恢复删除的记录
            rm_file_hdr->insert_record(w_set->GetRid(), w_set->GetRecord().data);
        } else if (w_set->GetWriteType() == WType::UPDATE_TUPLE) {
            // 恢复更新前的记录
            rm_file_hdr->update_record(w_set->GetRid(), w_set->GetRecord().data, nullptr);
        } else {
            throw InternalError("bad wtype");
        }
        write_set->pop_back();
    }
    // 2. 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (const auto& lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    // 3. 清空事务相关资源
    txn->get_write_set()->clear();
    txn->get_lock_set()->clear();
    // 4. 把事务日志刷入磁盘中
    AbortLogRecord abort_log(txn->get_transaction_id());
    log_manager->add_log_to_buffer(&abort_log);
    log_manager->flush_log_to_disk();
    // 5. 更新事务状态
    txn->set_state(TransactionState::ABORTED);
    // 从全局事务表中移除该事务
    {
        std::unique_lock<std::mutex> lock(latch_);
        txn_map.erase(txn->get_transaction_id());
    }
}