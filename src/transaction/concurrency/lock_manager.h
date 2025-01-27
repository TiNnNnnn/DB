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

#include <mutex>
#include <vector>
#include <algorithm>
#include <condition_variable>
#include "transaction/transaction.h"

static const std::string GroupLockModeStr[10] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

class LockManager {
    /* 加锁类型，包括共享锁、排他锁、意向共享锁、意向排他锁、SIX（意向排他锁+共享锁） */
    enum class LockMode { SHARED=0, EXLUCSIVE, INTENTION_SHARED, INTENTION_EXCLUSIVE, S_IX };

    /* 用于标识加锁队列中排他性最强的锁类型，例如加锁队列中有SHARED和EXLUSIVE两个加锁操作，则该队列的锁模式为X */
    enum class GroupLockMode { NON_LOCK=0, IS, IX, S, X, SIX};

    

    /* 事务的加锁申请 */
    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false) {}

        txn_id_t txn_id_;   // 申请加锁的事务ID
        LockMode lock_mode_;    // 事务申请加锁的类型
        bool granted_;          // 该事务是否已经被赋予锁
    };

    /* 数据项上的加锁队列 */
    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;  // 加锁队列
        std::condition_variable cv_;            // 条件变量，用于唤醒正在等待加锁的申请，在no-wait策略下无需使用
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;   // 加锁队列的锁模式
    };

public:
    LockManager() {}

    ~LockManager() {}

    bool lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd);

    //TODO：how to describe a record gap lock
    bool lock_exclusive_on_record_gap(Transaction* txn,const Rid& rid,int tab_fd);

    bool lock_shared_on_table(Transaction* txn, int tab_fd);

    bool lock_exclusive_on_table(Transaction* txn, int tab_fd);

    bool lock_IS_on_table(Transaction* txn, int tab_fd);

    bool lock_IX_on_table(Transaction* txn, int tab_fd);

    bool unlock(Transaction* txn, LockDataId lock_data_id);
private:
    bool lock_internal(Transaction* txn, LockDataId lock_data_id,LockMode lock_mode);
    bool can_grant_lock(const LockRequestQueue& queue,Transaction* txn, LockMode req_mode);
    bool should_rollback(Transaction* txn, const LockRequestQueue& queue,LockMode lock_mode);

    GroupLockMode get_group_lock_mode(LockMode mode) {
        switch (mode) {
            case LockMode::SHARED: return GroupLockMode::S;
            case LockMode::EXLUCSIVE: return GroupLockMode::X;
            case LockMode::INTENTION_SHARED: return GroupLockMode::IS;
            case LockMode::INTENTION_EXCLUSIVE: return GroupLockMode::IX;
            case LockMode::S_IX: return GroupLockMode::SIX;
            default: return GroupLockMode::NON_LOCK;
        }
    }

    std::array<std::array<bool, 6>, 6> LOCK_COMPATIBILITY_MATRIX = {
        { //   NO,   IS,   IX,   S,     X,    SIX
            { true, true, true, true, true, true },  // NO_LOCK
            { true, true, true, true, false, false }, // IS
            { true, true, true, false, false, false }, // IX
            { true, true, false, true, false, false }, // S
            { true, false, false, false, false, false }, // X
            { true, false, false, false, false, true }  // SIX
        }
    };

    // 根据队列中的锁请求确定排他性最强锁
    void update_group_lock_mode(LockRequestQueue& queue) {
        GroupLockMode current_mode = GroupLockMode::NON_LOCK;
        for (const auto& request : queue.request_queue_) {
            if (request.granted_) {
                GroupLockMode mode = get_group_lock_mode(request.lock_mode_);
                if (mode > current_mode) {
                    current_mode = mode;
                }
            }
        }
        queue.group_lock_mode_ = current_mode;
    }

private:
    std::mutex latch_;      // 用于锁表的并发
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_;   // 全局锁表
};
