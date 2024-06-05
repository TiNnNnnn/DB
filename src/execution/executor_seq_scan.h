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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;

        scan_ = std::make_unique<RmScan>(fh_);
    }

    //找到第一条符合条件的记录
    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
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

    //找到下一个符合条件的记录
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

    //获取当前记录
    std::unique_ptr<RmRecord> Next() override {
        if (scan_->is_end()) {
            return nullptr;
        }
        auto record = fh_->get_record(rid_,nullptr);
        return record;
    }

    Rid &rid() override { return rid_; }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    size_t tupleLen() const override {
        return len_;
    }

    std::string getType() override{
        return "SeqScanExecutor";
    }

    virtual bool is_end() const { 
        return scan_->is_end();
    };

     // 检查记录是否符合条件
    bool match_conditions(const RmRecord *record, const std::vector<Condition> &conds) {
        for (const auto &cond : conds) {
            auto lhs_col_meta = get_col(cols_, cond.lhs_col);
            char *lhs_data = record->data + lhs_col_meta->offset;
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