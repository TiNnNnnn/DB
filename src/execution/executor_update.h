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
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    
    std::unique_ptr<RmRecord> Next() override {
        context_->lock_mgr_->lock_exclusive_on_table(context_->txn_,fh_->GetFd());
        for (auto &rid : rids_) {
            // 获取要更新的记录
            std::unique_ptr<RmRecord> rec = fh_->get_record(rid, context_);
            auto old_rec = *(rec.get());

            // 记录原始数据用于索引更新
            std::vector<char*> old_keys;
            for (const auto &index : tab_.indexes) {
                char *old_key = new char[index.col_tot_len];
                int offset = 0;
                for (const auto &col : index.cols) {
                    memcpy(old_key + offset, rec->data + col.offset, col.len);
                    offset += col.len;
                }
                old_keys.push_back(old_key);
            }

            // 更新记录内容
            for (auto &set_clause : set_clauses_) {
                auto col_iter = std::find_if(tab_.cols.begin(), tab_.cols.end(),
                                             [&set_clause](const ColMeta &col) { return col.name == set_clause.lhs.col_name; });
                if (col_iter == tab_.cols.end()) {
                    throw ColumnNotFoundError(set_clause.lhs.col_name);
                }
                auto &col = *col_iter;
                if (col.type != set_clause.rhs.type) {
                    throw IncompatibleTypeError(coltype2str(col.type), coltype2str(set_clause.rhs.type));
                }
                (set_clause.rhs).init_raw(col.len);
                memcpy(rec->data + col.offset, set_clause.rhs.raw->data, col.len);
                set_clause.rhs.raw.reset();
            }

            // 删除原索引项
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                ih->delete_entry(old_keys[i], context_->txn_);
                delete[] old_keys[i];
            }

            // 插入新索引项
            for (const auto &index : tab_.indexes) {
                char *new_key = new char[index.col_tot_len];
                int offset = 0;
                for (const auto &col : index.cols) {
                    memcpy(new_key + offset, rec->data + col.offset, col.len);
                    offset += col.len;
                }
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                ih->insert_entry(new_key, rid, context_->txn_);
                delete[] new_key;
            }
            //加入write_set
            WriteRecord *wr = new WriteRecord (WType::UPDATE_TUPLE,tab_.name,rid,old_rec,*rec);
            context_->txn_->append_write_record(wr);
            //加入log_buffer
            UpdateLogRecord *update_log_record = new UpdateLogRecord(context_->txn_->get_transaction_id(),old_rec,*rec,rid,tab_.name);
            context_->log_mgr_->add_log_to_buffer(update_log_record);

            // 更新记录到文件
            fh_->update_record(rid, rec->data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};