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

#include <map>
#include <unordered_set>
#include <unordered_map>
#include "storage/disk_manager.h"
#include "storage/buffer_pool_manager.h"
#include "log_manager.h"
//#include "system/sm_manager.h"


class RedoLogsInPage {
public:
    RedoLogsInPage() {} //table_file_hdr_ = nullptr; }
    //RmFileHandle* table_file_hdr_;
    std::vector<std::pair<lsn_t,txn_id_t>> redo_logs_;   // 在该page上需要redo的操作的lsn
};

class UndoLogsInPage {
public:
    UndoLogsInPage() {}//table_file_hdr_ = nullptr; }
    //RmFileHandle* table_file_hdr_;
    std::vector<std::pair<lsn_t,txn_id_t>> undo_logs_;  
};


class RecoveryManager {
public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        //sm_manager_ = sm_manager;
    }
    void analyze();
    void redo();
    void undo();

    std::set<int>& get_tb_set(){return tb_set_;}
private:
    LogBuffer buffer_;                                              // 读入日志
    DiskManager* disk_manager_;                                     // 用来读写文件
    BufferPoolManager* buffer_pool_manager_;                        // 对页面进行读写
    //SmManager* sm_manager_;                                         // 访问数据库元数据

    std::map<PageId,RedoLogsInPage>redo_list_;
    std::map<PageId,UndoLogsInPage>undo_list_;
    std::unordered_set<txn_id_t>att_;
    std::set<int>tb_set_;
};