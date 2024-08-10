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

#include "transaction/transaction.h"
#include "transaction/concurrency/lock_manager.h"
#include "recovery/log_manager.h"
#include "recovery/log_recovery.h"

// class TransactionManager;

// used for data_send
static int const_offset = -1;

class Context {
public:
    Context (LockManager *lock_mgr, LogManager *log_mgr, 
            Transaction *txn, RecoveryManager *rev_mgr,char *data_send = nullptr, int *offset = &const_offset)
        : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_(txn),rev_mgr_(rev_mgr),
          data_send_(data_send), offset_(offset) {
            ellipsis_ = false;
          }

    // TransactionManager *txn_mgr_;
    
    LockManager *lock_mgr_;
    LogManager *log_mgr_;
    Transaction *txn_;
    RecoveryManager *rev_mgr_;

    char *data_send_;
    int *offset_;
    bool ellipsis_;
};