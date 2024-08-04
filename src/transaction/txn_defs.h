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

#include <atomic>

#include "common/config.h"
#include "defs.h"
#include "record/rm_defs.h"
 
/* 标识事务状态 */
enum class TransactionState { DEFAULT, GROWING, SHRINKING, COMMITTED, ABORTED };

/* 系统的隔离级别，当前赛题中为可串行化隔离级别 */
enum class IsolationLevel { READ_UNCOMMITTED, REPEATABLE_READ, READ_COMMITTED, SERIALIZABLE };

/* 事务写操作类型，包括插入、删除、更新三种操作 */
enum class WType { INSERT_TUPLE = 0, DELETE_TUPLE, UPDATE_TUPLE};

/**
 * @brief 事务的写操作记录，用于事务的回滚
 * INSERT
 * --------------------------------
 * | wtype | tab_name | tuple_rid |
 * --------------------------------
 * DELETE / UPDATE
 * ----------------------------------------------
 * | wtype | tab_name | tuple_rid | tuple_value |
 * ----------------------------------------------
 */
class WriteRecord {
   public:
    WriteRecord() = default;

    // constructor for insert operation
    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid) {}

    // constructor for delete & update operation
    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid, const RmRecord &record)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid), record_(record) {}

    ~WriteRecord() = default;

    inline RmRecord &GetRecord() { return record_; }

    inline Rid &GetRid() { return rid_; }

    inline WType &GetWriteType() { return wtype_; }

    inline std::string &GetTableName() { return tab_name_; }

    inline void SetRecord(RmRecord& record) {record_ = record;}

   private:
    WType wtype_;
    std::string tab_name_;
    Rid rid_;
    RmRecord record_;
};

/* 多粒度锁，加锁对象的类型，包括记录和表 */
enum class LockDataType { TABLE = 0, RECORD = 1};

/* 用于标识行锁的具体类型：非行锁,next_key,gap,common_reord,插入意向锁*/
enum class RecordLockType {NONE,ORDNINARY,GAP,NOT_GAP,INTENTION};

/**
 * @description: 加锁对象的唯一标识
 */
class LockDataId {
public:
    int fd_;                // 文件描述符
    Rid rid_;               // 行锁的Rid
    LockDataType type_;     // 锁的类型，表锁或行锁
    RecordLockType lock_type_; // 行锁的具体类型

    // 表级锁构造函数
    LockDataId(int fd, LockDataType type)
        : fd_(fd), type_(type), rid_({-1, -1}), lock_type_(RecordLockType::NONE) {
        assert(type == LockDataType::TABLE);
    }

    // 行级锁构造函数
    LockDataId(int fd, const Rid& rid, RecordLockType lock_type, LockDataType type)
        : fd_(fd), rid_(rid), type_(type), lock_type_(lock_type) {
        assert(type == LockDataType::RECORD);
    }

    // 获取唯一标识，对于表级锁，可能不需要rid_信息
    inline int64_t Get() const {
        if (type_ == LockDataType::TABLE) {
            // 对于表级锁，只使用文件描述符
            return static_cast<int64_t>(fd_);
        } else {
            // 对于行级锁，组合文件描述符、页面号、槽号和锁类型
            return ((static_cast<int64_t>(lock_type_) & 0x7) << 60) |
                   ((static_cast<int64_t>(fd_) & 0xFFFFFFFF) << 31) |
                   ((static_cast<int64_t>(rid_.page_no) & 0xFFFF) << 15) |
                   (static_cast<int64_t>(rid_.slot_no) & 0x7FFF);
        }
    }

    bool operator==(const LockDataId& other) const {
        return fd_ == other.fd_ && rid_ == other.rid_ && type_ == other.type_ &&
               lock_type_ == other.lock_type_;
    }
};

template <>
struct std::hash<LockDataId> {
    size_t operator()(const LockDataId &obj) const { return std::hash<int64_t>()(obj.Get()); }
};

/* 事务回滚原因 */
enum class AbortReason { LOCK_ON_SHIRINKING = 0, UPGRADE_CONFLICT, DEADLOCK_PREVENTION };

/* 事务回滚异常，在rmdb.cpp中进行处理 */
class TransactionAbortException : public std::exception {
    txn_id_t txn_id_;
    AbortReason abort_reason_;

   public:
    explicit TransactionAbortException(txn_id_t txn_id, AbortReason abort_reason)
        : txn_id_(txn_id), abort_reason_(abort_reason) {}

    txn_id_t get_transaction_id() { return txn_id_; }
    AbortReason GetAbortReason() { return abort_reason_; }
    std::string GetInfo() {
        switch (abort_reason_) {
            case AbortReason::LOCK_ON_SHIRINKING: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because it cannot request locks on SHRINKING phase\n";
            } break;

            case AbortReason::UPGRADE_CONFLICT: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because another transaction is waiting for upgrading\n";
            } break;

            case AbortReason::DEADLOCK_PREVENTION: {
                return "Transaction " + std::to_string(txn_id_) + " aborted for deadlock prevention\n";
            } break;

            default: {
                return "Transaction aborted\n";
            } break;
        }
    }
};