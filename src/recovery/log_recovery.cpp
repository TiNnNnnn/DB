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
#include "record/rm_file_handle.h"

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() { 
    //读取最新检查点位置
    char buf[sizeof(lsn_t)];
    disk_manager_->read_start_file(buf,sizeof(lsn_t),0);
    lsn_t c_lsn;
   
    memcpy(&c_lsn,buf,sizeof(lsn_t));
    if(c_lsn == -1){//checkpoint不存在，从头开始读取logfile
        c_lsn = 0;
    }else{
        //检查当前位置是否为checkpoint_record.
        int sz  = disk_manager_->read_log(buffer_.buffer_,LOG_HEADER_SIZE,c_lsn);
        if(sz == -1)
            throw InternalError("read log error");
        LogRecord cp_record;
        cp_record.deserialize(buffer_.buffer_);
        if(cp_record.log_type_ != LogType::CHECKPOINT){
            throw InternalError("checkpoint record type error");
        }
        buffer_.clear();
        c_lsn += LOG_HEADER_SIZE;
    }
    
    char l_buf[sizeof(uint32_t)];
    if(-1 ==disk_manager_->read_log(l_buf,sizeof(uint32_t),c_lsn+OFFSET_LOG_TOT_LEN)){
        //当前日志为空
        return;
    }
    
    uint32_t rec_tot_len;
    memcpy(&rec_tot_len,&l_buf,sizeof(uint32_t));

    //从checkpoint之后开始读取log文件
    while(-1 != disk_manager_->read_log(buffer_.buffer_,rec_tot_len,c_lsn)){
        LogRecord rec;
        rec.deserialize(buffer_.buffer_);

        UpdateLogRecord ur;
        InsertLogRecord ir;
        DeleteLogRecord dr;

        switch(rec.log_type_){
                case LogType::BEGIN:
                        att_.insert(rec.log_tid_);
                        break;
                case LogType::COMMIT:
                case LogType::ABORT:
                        att_.erase(rec.log_tid_);
                        if(rec.log_type_ == LogType::COMMIT){
                            //事务已经提交，将涉及的页面从undo_list中删除
                            for(auto &dp :undo_list_){
                                auto& logs = dp.second.undo_logs_;
                                for (auto it = logs.begin(); it != logs.end(); ) {
                                    if (it->second == rec.log_tid_) {
                                        it = logs.erase(it);
                                    } else {
                                        ++it;
                                    }
                                }
                            }
                        }
                        break;
                case LogType::UPDATE:
                        ur.deserialize(buffer_.buffer_);
                        //将数据修改操作记录
                        if(att_.count(rec.log_tid_)){
                                std::string name = std::string(ur.table_name_,ur.table_name_size_);
                                int fd = disk_manager_->get_file_fd(name);
                                PageId pid(fd,ur.rid_.page_no);
                                redo_list_[pid].redo_logs_.push_back(std::make_pair(ur.lsn_,ur.log_tid_));
                                undo_list_[pid].undo_logs_.push_back(std::make_pair(ur.lsn_,ur.log_tid_));  
                                tb_set_.insert(fd);
                        }
                        break;
                case LogType::INSERT:
                        ir.deserialize(buffer_.buffer_);
                        if(att_.count(rec.log_tid_)){
                                std::string name = std::string(ir.table_name_,ir.table_name_size_);
                                int fd = disk_manager_->get_file_fd(name);
                                PageId pid(fd,ir.rid_.page_no);
                                redo_list_[pid].redo_logs_.push_back(std::make_pair(ir.lsn_,ir.log_tid_));
                                undo_list_[pid].undo_logs_.push_back(std::make_pair(ir.lsn_,ir.log_tid_));
                                tb_set_.insert(fd);
                        }
                        break;
                case LogType::DELETE:
                        dr.deserialize(buffer_.buffer_);
                        if(att_.count(rec.log_tid_)){
                                std::string name = std::string(dr.table_name_,dr.table_name_size_);
                                int fd = disk_manager_->get_file_fd(name);
                                PageId pid(fd,dr.rid_.page_no);
                                redo_list_[pid].redo_logs_.push_back(std::make_pair(dr.lsn_,dr.log_tid_));
                                undo_list_[pid].undo_logs_.push_back(std::make_pair(dr.lsn_,dr.log_tid_));
                                tb_set_.insert(fd);
                        }
                        break;
                case LogType::HEADER:
                        break;
                default:
                        throw InternalError("error log_record type");
        }
        buffer_.clear();
        c_lsn+=rec.log_tot_len_;

        //读取下一条log_record的长度
        if(-1 == disk_manager_->read_log(l_buf,sizeof(uint32_t),c_lsn+OFFSET_LOG_TOT_LEN)){
            break;
        };
        memcpy(&rec_tot_len,&l_buf,sizeof(uint32_t));
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    for(const auto& dp : redo_list_){
        auto& redo_lsns = dp.second.redo_logs_;
        
        auto rm_file_hdr = new RmFileHandle(disk_manager_,buffer_pool_manager_,dp.first.fd);
        //顺序执行日志修改每个数据页
        for(auto& redo_lsn : redo_lsns){
            lsn_t lsn = redo_lsn.first;
            char l_buf[LOG_HEADER_SIZE];
            disk_manager_->read_log(l_buf,LOG_HEADER_SIZE,lsn);
            LogRecord rec;
            rec.deserialize(l_buf);
            
            char record_buf[rec.log_tot_len_];
            disk_manager_->read_log(record_buf,rec.log_tot_len_,lsn);

            if(rec.log_type_ == LogType::INSERT){
                InsertLogRecord ir;
                ir.deserialize(record_buf);
                std::cout<<"redo insert ["<<ir.rid_.page_no<<","<<ir.rid_.slot_no<<"],"<<std::string(ir.insert_value_.data,ir.insert_value_.size)<<std::endl;
                rm_file_hdr->insert_record_for_recovery(ir.rid_,ir.insert_value_.data);

            }else if(rec.log_type_ == LogType::UPDATE){
                UpdateLogRecord ur;
                ur.deserialize(record_buf);
                std::cout<<"redo update ["<<ur.rid_.page_no<<","<<ur.rid_.slot_no<<"],"<<std::string(ur.new_value_.data,ur.new_value_.size)<<std::endl;
                rm_file_hdr->update_record_for_recovery(ur.rid_,ur.new_value_.data,nullptr);

            }else if(rec.log_type_ == LogType::DELETE){
                DeleteLogRecord dr;
                dr.deserialize(record_buf);
                std::cout<<"redo delete ["<<dr.rid_.page_no<<","<<dr.rid_.slot_no<<"],"<<std::string(dr.delete_value_.data,dr.delete_value_.size)<<std::endl;
                rm_file_hdr->delete_record_for_recovery(dr.rid_,nullptr);
            }
        }
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    for(const auto& up : undo_list_ ){
        auto& undo_lsns = up.second.undo_logs_;
        auto rm_file_hdr = new RmFileHandle(disk_manager_,buffer_pool_manager_,up.first.fd);
        //逆序执行日志回滚每个数据页
        for(auto undo_lsn = undo_lsns.rbegin(); undo_lsn != undo_lsns.rend(); ++undo_lsn){
            lsn_t lsn = undo_lsn->first;
            char l_buf[LOG_HEADER_SIZE];
            disk_manager_->read_log(l_buf,LOG_HEADER_SIZE,lsn);
            LogRecord rec;
            rec.deserialize(l_buf);
            
            char record_buf[rec.log_tot_len_];
            disk_manager_->read_log(record_buf,rec.log_tot_len_,lsn);

            if(rec.log_type_ == LogType::INSERT){
                InsertLogRecord ir;
                ir.deserialize(record_buf);
                std::cout<<"undo insert rollback ["<<ir.rid_.page_no<<","<<ir.rid_.slot_no<<"],"<<std::string(ir.insert_value_.data,ir.insert_value_.size)<<std::endl;
                rm_file_hdr->delete_record_for_recovery(ir.rid_,nullptr);

            }else if(rec.log_type_ == LogType::UPDATE){
                UpdateLogRecord ur;
                ur.deserialize(record_buf);
                std::cout<<"undo update rollback ["<<ur.rid_.page_no<<","<<ur.rid_.slot_no<<"],"<<std::string(ur.old_value_.data,ur.old_value_.size)<<std::endl;
                rm_file_hdr->update_record_for_recovery(ur.rid_,ur.old_value_.data,nullptr);

            }else if(rec.log_type_ == LogType::DELETE){
                DeleteLogRecord dr;
                dr.deserialize(record_buf);
                std::cout<<"undo delete rollback ["<<dr.rid_.page_no<<","<<dr.rid_.slot_no<<"],"<<std::string(dr.delete_value_.data,dr.delete_value_.size)<<std::endl;
                rm_file_hdr->insert_record_for_recovery(dr.rid_,dr.delete_value_.data);
            }
        }
    }
}
