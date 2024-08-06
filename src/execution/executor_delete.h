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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "record_printer.h"
#include "index/ix.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::vector<Rid> rids_;         // 需要删除的记录的位置
    std::string tab_name_;          // 表名称
    SmManager *sm_manager_;

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        //context_->lock_mgr_->lock_IX_on_table(context_->txn_,fh_->GetFd());
        bool ret = context_->lock_mgr_->lock_exclusive_on_table(context_->txn_,fh_->GetFd());
        
        for (auto &rid : rids_) {
            // 获取要删除的记录
            std::unique_ptr<RmRecord> rec = fh_->get_record(rid, context_);
            if(!rec) {
                throw RecordNotFoundError(rid.page_no,rid.slot_no);
            }

            //加入write_set
            WriteRecord *wr = new WriteRecord(WType::DELETE_TUPLE,tab_.name,rid,*rec,*rec);
            context_->txn_->append_write_record(wr);
            //加入log_buffer
            DeleteLogRecord *del_log_record = new  DeleteLogRecord(context_->txn_->get_transaction_id(),*rec,rid,tab_.name);
            context_->log_mgr_->add_log_to_buffer(del_log_record);

            // 删除记录
            fh_->delete_record(rid, context_);
            
            // 更新索引
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];  // 获取索引元数据
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();  // 获取索引句柄
                char *key = new char[index.col_tot_len];  // 为索引键分配内存
                int offset = 0;
                // 构建索引键值
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->delete_entry(key, context_->txn_);  // 从索引中删除项
                delete[] key;  // 释放内存
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }

    
};