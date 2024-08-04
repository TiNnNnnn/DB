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
        txn->set_start_ts(txn_id);
    }
    {
        //把事务加入到全局事务表中
        std::unique_lock<std::mutex> lock(latch_);
        txn_map[txn->get_transaction_id()] = txn;
    }
    txn->set_state(TransactionState::GROWING);
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
    // if (txn->get_write_set()->size() == 0) {
    //     txn->set_state(TransactionState::COMMITTED);
    //     {
    //         std::unique_lock<std::mutex> lock(latch_);
    //         txn_map.erase(txn->get_transaction_id());
    //     }
    //     return;
    // }
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
    // {
    //     std::unique_lock<std::mutex> lock(latch_);
    //     txn_map.erase(txn->get_transaction_id());
    // }
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
    
    //auto db_name = sm_manager_->db_.get_db_name();
    auto write_set = txn->get_write_set();

    if (chdir(sm_manager_->db_.get_db_name().c_str()) < 0) {  // 进入数据库目录
            throw UnixError();
    }

    // 1. 回滚所有写操作
    while (!write_set->empty()) {
        auto w_set = write_set->back();
        auto tb_name = w_set->GetTableName();
        auto rm_file_hdr = rm_mgr->open_file(tb_name);
        if (w_set->GetWriteType() == WType::INSERT_TUPLE) {
            // 删除插入的记录
            rm_file_hdr->delete_record(w_set->GetRid(), nullptr);
            for(auto index : sm_manager_->db_.get_table(tb_name).indexes){
                auto idx_hdr = sm_manager_->get_ix_manager()->open_index(tb_name,index.cols);
                char *key = new char[index.col_tot_len];  // 为索引键分配内存
                int offset = 0;
                // 构建索引键值
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, w_set->GetRecord().data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                idx_hdr->delete_entry(key,txn);
            }
            
        } else if (w_set->GetWriteType() == WType::DELETE_TUPLE) {
            // 恢复删除的记录
            rm_file_hdr->insert_record(w_set->GetRid(), w_set->GetRecord().data);
            for(auto index : sm_manager_->db_.get_table(tb_name).indexes){
                auto idx_hdr = sm_manager_->get_ix_manager()->open_index(tb_name,index.cols);
                char *key = new char[index.col_tot_len];  // 为索引键分配内存
                int offset = 0;
                // 构建索引键值
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, w_set->GetRecord().data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                idx_hdr->insert_entry(key,w_set->GetRid(),txn);
            }
        } else if (w_set->GetWriteType() == WType::UPDATE_TUPLE) {
            // 恢复更新前的记录
            rm_file_hdr->update_record(w_set->GetRid(), w_set->GetRecord().data, nullptr);
            for(auto index : sm_manager_->db_.get_table(tb_name).indexes){
                auto idx_hdr = sm_manager_->get_ix_manager()->open_index(tb_name,index.cols);
                char *key = new char[index.col_tot_len];  // 为索引键分配内存
                int offset = 0;
                // 构建索引键值
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, w_set->GetRecord().data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                idx_hdr->delete_entry(key,txn);
                idx_hdr->insert_entry(key,w_set->GetRid(),txn);
            }
        } else {
            throw InternalError("bad wtype");
        }
        write_set->pop_back();
    }
    if (chdir("..") < 0) {
        throw UnixError();
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
    // {
    //     std::unique_lock<std::mutex> lock(latch_);
    //     txn_map.erase(txn->get_transaction_id());
    // }
}