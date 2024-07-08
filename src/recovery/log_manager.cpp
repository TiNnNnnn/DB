/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include "log_manager.h"

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::unique_lock<std::mutex> lock(latch_);
    // 为日志记录分配一个全局唯一的LSN
    lsn_t lsn = global_lsn_++;
    log_record->lsn_ = lsn;
    // 序列化日志记录
    int log_size = log_record->log_tot_len_;
    char* log_data = new char[log_size];
    log_record->serialize(log_data);
    // 检查是否有足够的空间
    if (log_buffer_.is_full(log_size)) {
        flush_log_to_disk();
    }
    // 将日志记录添加到缓冲区
    memcpy(log_buffer_.buffer_ + log_buffer_.offset_, log_data, log_size);
    log_buffer_.offset_ += log_size;

    delete[] log_data;
    return lsn;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::unique_lock<std::mutex> lock(latch_);
    if (log_buffer_.offset_ > 0) {
        // 将缓冲区内容写入磁盘
        disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
        // 更新已持久化的LSN
        persist_lsn_ = global_lsn_ - 1;
        // 重置缓冲区
        log_buffer_.offset_ = 0;
        memset(log_buffer_.buffer_, 0, sizeof(log_buffer_.buffer_));
    }
}
