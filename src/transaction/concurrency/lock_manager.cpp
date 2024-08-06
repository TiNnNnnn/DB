/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"
#include "transaction/txn_defs.h"
#include <algorithm>


/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    LockDataId* id = new LockDataId(tab_fd,rid,RecordLockType::NOT_GAP,LockDataType::RECORD);
    return lock_internal(txn,*id , LockMode::SHARED);
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    LockDataId* id = new LockDataId(tab_fd,rid,RecordLockType::NOT_GAP,LockDataType::RECORD);
    return lock_internal(txn,*id , LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    LockDataId* id = new LockDataId(tab_fd,LockDataType::TABLE);
    return lock_internal(txn,*id , LockMode::SHARED);
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    LockDataId* id = new LockDataId(tab_fd,LockDataType::TABLE);
    return lock_internal(txn,*id , LockMode::EXLUCSIVE);
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    LockDataId* id = new LockDataId(tab_fd,LockDataType::TABLE);
    return lock_internal(txn,*id , LockMode::INTENTION_SHARED);
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    LockDataId* id = new LockDataId(tab_fd,LockDataType::TABLE);
    return lock_internal(txn,*id , LockMode::INTENTION_EXCLUSIVE);
}




/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
   std::lock_guard<std::mutex> lock(latch_);
   auto it = lock_table_.find(lock_data_id);
   if (it != lock_table_.end()) {
        auto& queue = it->second;
        auto& request_queue = queue.request_queue_;
        // 移除队列中该事务的所有锁请求
        request_queue.remove_if([&txn](const LockRequest& request) {
            return request.txn_id_ == txn->get_transaction_id();
        });
        update_group_lock_mode(queue);
        queue.cv_.notify_all();
        return true;
    }
    return false;
}

bool LockManager::lock_internal(Transaction* txn, LockDataId lock_data_id,LockMode lock_mode){
    std::unique_lock<std::mutex>unique_lock(latch_);
    auto& q = lock_table_[lock_data_id];

    // 获取事务当前持有的锁集合
    auto& ls = txn->get_lock_set();
    int x_count = 0;
    // 检查事务是否已经持有相同类型的锁
    if (ls->find(lock_data_id) != ls->end()) {
        // 事务已经持有锁，检查是否与请求的锁类型相同
        auto& existing_locks = lock_table_[lock_data_id].request_queue_;
        for (auto& request : existing_locks) {
            if(request.lock_mode_ == LockMode::EXLUCSIVE){
                x_count++;
            }

            if (request.txn_id_ == txn->get_transaction_id() && request.lock_mode_ == lock_mode) {
                // 事务已经持有相同类型的锁，不需要再次授予
                if(lock_mode == LockMode::EXLUCSIVE){
                    q.cv_.notify_all(); 
                    return true;
                }else if(lock_mode == LockMode::SHARED && x_count == 0){
                    q.cv_.notify_all(); 
                    return true;
                }
                break;
                
            }else if(request.txn_id_ == txn->get_transaction_id() && request.lock_mode_ == LockMode::EXLUCSIVE && lock_mode == LockMode::SHARED){
                //事务已经持有X锁，不需要再申请S锁
                q.cv_.notify_all(); 
                return true;
            }else if (request.txn_id_ == txn->get_transaction_id() && request.lock_mode_ == LockMode::SHARED && lock_mode == LockMode::EXLUCSIVE && x_count == 0){
                // //锁升级
                // request.lock_mode_ = LockMode::EXLUCSIVE;
                // update_group_lock_mode(q);
                // q.cv_.notify_all();
                // return true; 
            }
        }
    }

    //判定是否可以授予锁
    if(can_grant_lock(q,txn,lock_mode)){
        q.request_queue_.emplace_back(txn->get_transaction_id(),lock_mode);
        q.request_queue_.back().granted_ = true;
        update_group_lock_mode(q);
        txn->append_lock_set(lock_data_id);
        q.cv_.notify_all();
        return true;
    }else{
        if(should_rollback(txn,q,lock_mode))
            throw TransactionAbortException(txn->get_transaction_id(),AbortReason::DEADLOCK_PREVENTION);
        q.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
        //等待锁被授予或者需要回滚的信号
        q.cv_.wait(unique_lock,[this,&q,txn,lock_mode]{
            return q.request_queue_.empty() || can_grant_lock(q,txn,lock_mode);
        });
        // 唤醒后再次检查是否可以授予锁
        if (can_grant_lock(q, txn, lock_mode)) {
            q.request_queue_.back().granted_ = true;
            update_group_lock_mode(q);
            txn->append_lock_set(lock_data_id);
            q.cv_.notify_all();
            return true;
        }
        return false;
    }
}

bool LockManager::can_grant_lock(const LockRequestQueue& queue,Transaction* txn, LockMode req_mode){
    //std::lock_guard<std::mutex> lock(latch_);
    GroupLockMode requested_group_mode = get_group_lock_mode(req_mode);
    //INFO:group_lock_mode_记录该加锁队列中的排他性最强的锁类型
    if (LOCK_COMPATIBILITY_MATRIX[static_cast<size_t>(queue.group_lock_mode_)][static_cast<size_t>(requested_group_mode)]) {
            // 检查是否存在优先级更高的等待事务
            for (auto& req : queue.request_queue_) {
                if (req.granted_ && !LOCK_COMPATIBILITY_MATRIX[static_cast<size_t>(req.lock_mode_)][static_cast<size_t>(requested_group_mode)]) {
                    return false; 
                }
            }
            return true;
    }
    return false;
}

bool LockManager::should_rollback(Transaction* txn, const LockRequestQueue& queue,LockMode lock_mode){
    // 检查队列中的所有锁请求，如果存在优先级更高的事务，则当前事务应该回滚
    for(const auto& req: queue.request_queue_){
        if(req.txn_id_ != txn->get_transaction_id() && txn->get_transaction_id() > req.txn_id_ && req.lock_mode_ == lock_mode){
            return true;
        }
    }
    return false;
}