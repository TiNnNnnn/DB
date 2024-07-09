/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    // 清空缓存并从磁盘读取日志
    disk_manager_->read_log(buffer_.buffer_,buffer_.offset_,0);

    // 重建脏页表和活动事务表
    std::unordered_map<txn_id_t, TransactionState> txn_table;
    std::unordered_map<page_id_t, lsn_t> dirty_page_table;

    int offset = 0;
    while(offset < buffer_.offset_){
        LogRecord log;
        log.deserialize(buffer_.buffer_ + offset);
        offset += log.log_tot_len_;

        if(log.log_type_ == LogType::begin){
                txn_table[log.log_tid_] = TransactionState::GROWING;
        }else if(log.log_type_ == LogType::commit){
                txn_table[log.log_tid_] = TransactionState::COMMITTED;
        }else if(log.log_type_ == LogType::ABORT){
                txn_table[log.log_tid_] = TransactionState::ABORTED;
        } else if (log.log_type_ == LogType::UPDATE || log.log_type_ == LogType::INSERT || log.log_type_ == LogType::DELETE){

        }

    }


}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {

}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {

}