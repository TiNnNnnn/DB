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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;
    
   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                //左条件在其他表，则右条件一定在当前表
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs （保证左操作对象一定是当前table）
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        build_ix_scan();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_,nullptr);
            // 检查记录是否符合条件
            if (match_conditions(record.get(), fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_,nullptr);
            // 检查记录是否符合条件
            if (match_conditions(record.get(), fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (scan_->is_end()) {
            return nullptr;
        }
        auto record = fh_->get_record(rid_,nullptr);
        return record;
        return nullptr;
    }

    void build_ix_scan() {
        std::string idx_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        auto& ix_handle = sm_manager_->ihs_[idx_name];

        char* lower_bound_key = new char[index_meta_.col_tot_len];
        char* upper_bound_key = new char[index_meta_.col_tot_len];

        memset(lower_bound_key, 0, index_meta_.col_tot_len);
        memset(upper_bound_key, 0xFF, index_meta_.col_tot_len);

        bool has_lower_bound = false;
        bool has_upper_bound = false;

        int offset = 0;
        for (const auto& idx_col : index_col_names_) {
            bool col_processed = false;
            auto col_meta = sm_manager_->db_.get_table(tab_name_).get_col(idx_col);

            for (auto& cond : fed_conds_) {
                if (!cond.is_rhs_val) {
                    continue;
                }

                if (cond.lhs_col.col_name == idx_col) {
                    col_processed = true;

                    cond.rhs_val.raw.reset();
                    cond.rhs_val.init_raw(col_meta->len);

                    if (cond.op == OP_EQ) {
                        memcpy(lower_bound_key+offset,cond.rhs_val.raw->data,col_meta->len);
                        memcpy(upper_bound_key+offset,cond.rhs_val.raw->data,col_meta->len);
                        offset += col_meta->len;
                        has_lower_bound = true;
                        has_upper_bound = true;
                    } else if (cond.op == OP_LT || cond.op == OP_LE) {
                        memcpy(upper_bound_key+offset,cond.rhs_val.raw->data,col_meta->len);
                        offset += col_meta->len;
                        has_upper_bound = true;
                    } else if (cond.op == OP_GT || cond.op == OP_GE) {
                        memcpy(lower_bound_key+offset,cond.rhs_val.raw->data,col_meta->len);
                        offset += col_meta->len;
                        has_lower_bound = true;
                    }
                    break;
                }
            }
            // if (!col_processed) {
            //     // 对于未处理的索引列，补充空白字符
            //     for(int i=0;i<col_meta->len;i++){
            //         memcpy(lower_bound_key+offset,(const char*)'\0',sizeof('\0'));
            //         memcpy(upper_bound_key+offset,(const char*)'\xFF',sizeof('xFF'));
            //         offset+=1;
            //     }
            //     break;
            // }
        }
        Iid lower_bound_iid;
        if (!has_lower_bound) {
            lower_bound_iid = {ix_handle->get_file_hdr()->first_leaf_, 0};
        } else {
            lower_bound_iid = ix_handle->lower_bound(lower_bound_key);
        }
        Iid upper_bound_iid;
        if (!has_upper_bound) {
            upper_bound_iid = {ix_handle->get_file_hdr()->last_leaf_, ix_handle->fetch_node(ix_handle->get_file_hdr()->last_leaf_)->get_size()};
        } else {
            upper_bound_iid = ix_handle->upper_bound(upper_bound_key);
        }

        scan_ = std::make_unique<IxScan>(ix_handle.get(), lower_bound_iid, upper_bound_iid);
    }

    size_t tupleLen() const override {
        return len_;
    }

    virtual bool is_end() const { 
        return scan_->is_end();
    };

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    Rid &rid() override { return rid_; }

    // 检查记录是否符合条件
    bool match_conditions(const RmRecord *record, const std::vector<Condition> &conds) {
        for (const auto &cond : conds) {
            auto lhs_col_meta = get_col(cols_, cond.lhs_col);
            char *lhs_data = record->data + lhs_col_meta->offset;

            //特殊处理IN子句
            if(cond.op == CompOp::IN){
                bool is_find = false;
                auto rhs_vals = cond.rhs_vals;
                for(auto& rhs_val : rhs_vals){
                    if (eval_condition(lhs_data, lhs_col_meta->type, CompOp::OP_EQ , rhs_val)) {
                        is_find = true;
                        break;
                    }
                }
                if(!is_find){
                    return false;
                }
                continue;
            }

            if (cond.is_rhs_val) {
                // 右边是常量值
                if (!eval_condition(lhs_data, lhs_col_meta->type, cond.op, cond.rhs_val)) {
                    return false;
                }
            } else {
                // 右边是列
                auto rhs_col_meta = get_col(cols_, cond.rhs_col);
                char *rhs_data = record->data + rhs_col_meta->offset;
                if (!eval_condition(lhs_data, lhs_col_meta->type, cond.op, rhs_data, rhs_col_meta->type)) {
                    return false;
                }
            }
        }
        return true;
    }
    // 评估条件（左值 vs 右值）
    bool eval_condition(const char *lhs_data, ColType lhs_type, CompOp op, const Value &rhs_val) {
        switch (lhs_type) {
            case TYPE_INT:
                return eval_condition(*(int *)lhs_data, op, rhs_val.int_val);
            case TYPE_FLOAT:
                return eval_condition(*(float *)lhs_data, op, rhs_val.float_val);
            case TYPE_STRING:
                return eval_condition(std::string(lhs_data, rhs_val.str_val.size()), op, rhs_val.str_val);
            default:
                return false;
        }
    }
    // 评估条件（列左值 vs 列右值）
    bool eval_condition(const char *lhs_data, ColType lhs_type, CompOp op, const char *rhs_data, ColType rhs_type) {
        //检查两列数据类型是否一致
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
        switch (lhs_type) {
            case TYPE_INT:
                return eval_condition(*(int *)lhs_data, op, *(int *)rhs_data);
            case TYPE_FLOAT:
                return eval_condition(*(float *)lhs_data, op, *(float *)rhs_data);
            case TYPE_STRING:
                return eval_condition(std::string(lhs_data), op, std::string(rhs_data));
            default:
                return false;
        }
    }
    // 评估条件（左值 vs 右值）
    template <typename T>
    bool eval_condition(T lhs, CompOp op, T rhs) {
        switch (op) {
            case OP_EQ:
                return lhs == rhs;
            case OP_NE:
                return lhs != rhs;
            case OP_LT:
                return lhs < rhs;
            case OP_LE:
                return lhs <= rhs;
            case OP_GT:
                return lhs > rhs;
            case OP_GE:
                return lhs >= rhs;
            default:
                return false;
        }
    }
};