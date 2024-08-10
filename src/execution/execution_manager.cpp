/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */
#include "execution_manager.h"

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    }
}

/**
 * @description:
 *  创建静态检查点的具体步骤如下：
    （1）停止接收新事务和正在运行事务
    （2）将仍保留在日志缓冲区中的内容写到日志文件中;
    （3）在日志文件中写入一个“检查点记录”;
    （4）将当前数据库缓冲区中的内容写到数据库中;
    （5）把日志文件中检查点记录的地址写到“重新启动文件”中
 */
void create_static_checkpoint(TransactionManager* txn_mgr_,Context* context){
    //设置标志位，停止接受新事务
    txn_mgr_->set_is_checkpointing(true);
    //停止正在运行的事务
    for(auto t: txn_mgr_->att){
        if(t->get_transaction_id() != context->txn_->get_transaction_id()){
            txn_mgr_->abort(t,context->log_mgr_);
        } 
    }
    
    
    //在日志文件中写入一个“检查点记录”
    CheckPointRecord* rec = new CheckPointRecord();
    auto c_lsn =  context->log_mgr_->add_log_to_buffer(rec);

    //将仍保留在日志缓冲区中的内容写到日志文件中
    context->log_mgr_->flush_log_to_disk();

    //将当前数据库缓冲区中的内容写到数据库中
    context->log_mgr_->get_bp()->flush_all_pages();
    
    //把日志文件中检查点记录的地址写到“重新启动文件”中
    char data[sizeof(lsn_t)];
    memcpy(data, &c_lsn, sizeof(c_lsn));
    context->log_mgr_->get_dm()->write_start_file(data,sizeof(lsn_t));

    //手动提交，不记录日志
    //context->txn_->set_txn_mode(true);


    //重置标志位，恢复接受新事务
    txn_mgr_->set_is_checkpointing(false);
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_indexs(x->tab_name_,context);
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                context->txn_->set_txn_mode(true);
                break;
            }  
            case T_Transaction_commit:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->commit(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_rollback:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }    
            case T_Transaction_abort:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                break;
            }
            case T_Create_static_checkpoint:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                create_static_checkpoint(txn_mgr_,context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;                        
        }

    } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
        switch (x->set_knob_type_)
        {
        case ast::SetKnobType::EnableNestLoop: {
            planner_->set_enable_nestedloop_join(x->bool_value_);
            break;
        }
        case ast::SetKnobType::EnableSortMerge: {
            planner_->set_enable_sortmerge_join(x->bool_value_);
            break;
        }
        default: {
            throw RMDBError("Not implemented!\n");
            break;
        }
        }
    }
}



// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
std::vector<std::vector<std::string>> QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols, 
                            std::vector<AggregateExpr> sel_aggs,Context *context,bool is_son) {
    std::string tb_name;
    if(sel_cols.size())tb_name = sel_cols[0].tab_name;
    else if(sel_aggs.size())tb_name = sel_aggs[0].cols[0].tab_name;
    else throw InternalError("tb_name empty");
    int tb_fd = sm_manager_->fhs_[tb_name]->GetFd();

    bool ret = context->lock_mgr_->lock_shared_on_table(context->txn_,tb_fd);                    
    
    //将待查询列名存储在captions
    std::vector<std::string> captions;
    captions.reserve(sel_cols.size());
    for (auto &sel_col : sel_cols) {
        captions.push_back(sel_col.col_name);
    }
    for(auto &sel_agg : sel_aggs){
        if(sel_agg.asia != "")
            captions.push_back(sel_agg.asia);
        else{
            std::string agg_str =sel_agg.func_name+"(";
            if(sel_agg.func_name == "COUNT" && sel_agg.cols.size()>1){
                agg_str += "*)";
            }else{
                agg_str += sel_agg.cols[0].col_name;
                agg_str += ")";
            }
            captions.push_back(agg_str);
        } 
    }


    // Print header into buffer(打印表头到缓冲区)
    RecordPrinter rec_printer(sel_cols.size() + sel_aggs.size());

    if(!is_son){
        rec_printer.print_separator(context);
        rec_printer.print_record(captions, context);
        rec_printer.print_separator(context);
    }
    
    // print header into file (将表头信息写入output.txt)
    std::fstream outfile;
    if(!is_son){
        std::string out_file_name = sm_manager_->get_db_name() + "/output.txt";
        outfile.open(out_file_name, std::ios::out | std::ios::app);
        outfile << "|";
        for(int i = 0; i < captions.size(); ++i) {
            outfile << " " << captions[i] << " |";
        }
        outfile << "\n";
    }
   
    std::vector<std::vector<std::string>>rets;

    // Print records
    size_t num_rec = 0;
    // 执行query_plan
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto Tuple = executorTreeRoot->Next();
        std::vector<std::string> columns;

        if(!sel_aggs.size()){
            int cols_offset = 0;
            for (auto &col : executorTreeRoot->cols()) {
                std::string col_str;
                char *rec_buf = Tuple->data + col.offset;
                if (col.type == TYPE_INT) {
                    col_str = std::to_string(*(int *)rec_buf);
                } else if (col.type == TYPE_FLOAT) {
                    col_str = std::to_string(*(float *)rec_buf);
                } else if (col.type == TYPE_STRING) {
                    col_str = std::string((char *)rec_buf, col.len);
                    col_str.resize(strlen(col_str.c_str()));
                }
                cols_offset += col.offset;
                columns.push_back(col_str);
            }
        }else{
            int cols_offset = 0;
            for (auto &col : executorTreeRoot->cols()) {
                std::string col_str;
                char *rec_buf = Tuple->data + cols_offset;
                if (col.type == TYPE_INT) {
                    col_str = std::to_string(*(int *)rec_buf);
                } else if (col.type == TYPE_FLOAT) {
                    col_str = std::to_string(*(float *)rec_buf);
                } else if (col.type == TYPE_STRING) {
                    col_str = std::string((char *)rec_buf, col.len);
                    col_str.resize(strlen(col_str.c_str()));
                }
                cols_offset += col.len;
                columns.push_back(col_str);
            }
            int temp_offset = cols_offset;
            for (auto &agg : sel_aggs){
                std::string agg_str;
                char *rec_buf = Tuple->data + temp_offset;
                if(agg.func_name == "COUNT"){
                    agg_str = std::to_string(*(int *)rec_buf);
                }else{
                    auto col_meta = sm_manager_->db_.get_table(tb_name).get_col(agg.cols[0].col_name);
                    if(col_meta->type == TYPE_FLOAT)
                        agg_str = std::to_string(*(float*)rec_buf);
                    else if(col_meta->type == TYPE_INT)
                        agg_str = std::to_string(*(int*)rec_buf);
                }
                columns.push_back(agg_str);
                temp_offset+=4;
            }
        }

        if(!is_son){
            // print record into buffer
            rec_printer.print_record(columns, context);
            // print record into file
            outfile << "|";
            for(int i = 0; i < columns.size(); ++i) {
                outfile << " " << columns[i] << " |";
            }
            outfile << "\n";
        }
       
        num_rec++;
        rets.push_back(columns);
    }
    outfile.close();
    if(!is_son){
        // Print footer into buffer
        rec_printer.print_separator(context);
        // Print record count into buffer
        RecordPrinter::print_record_count(num_rec, context);
    }
    return rets;
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}