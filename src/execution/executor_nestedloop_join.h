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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

    std::unique_ptr<RmRecord> left_tuple_;      // 左子执行器当前的元组
    std::unique_ptr<RmRecord> right_tuple_;     // 右子执行器当前的元组

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        //join之后每条记录的长度
        len_ = left_->tupleLen() + right_->tupleLen();
        //获取left table的字段偏移量
        cols_ = left_->cols();
        //计算join之后的right table的字段偏移量
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
    }

    void beginTuple() override {
        left_->beginTuple(); 
        left_tuple_ = left_->Next();
        right_->beginTuple();
        right_tuple_ = right_->Next();
        isend = left_->is_end() || right_->is_end();
        if(matchConditions(left_tuple_,right_tuple_))return;
        while(!isend){
            if(!right_->is_end()){
                right_tuple_ = right_->Next();
            }else{
                right_->beginTuple();
                right_tuple_ = right_->Next();

                left_->nextTuple();
                if(left_->is_end()){
                    isend = true;
                    return;
                }
                left_tuple_ = left_->Next();
            }
            if(matchConditions(left_tuple_,right_tuple_))return;
            right_->nextTuple();
        }
    }

    void nextTuple() override {
        //尝试获取右表下一个元素的位置
        right_->nextTuple();
        while(!isend){
            if (!right_->is_end()) {
                //获取右表下一个位置的tuple
                right_tuple_ = right_->Next();
            }else{
                //重新遍历右表
                right_->beginTuple();
                //获取右表第一个tuple
                right_tuple_ = right_->Next();

                //获取左表的下一个tuple
                left_->nextTuple();
                if(left_->is_end()){
                    isend = true;
                    return;
                }
                left_tuple_ = left_->Next();
            }
            //判断当前左右表tuple是否满足condition,满足直接返回
            if(matchConditions(left_tuple_,right_tuple_)){
                return;
            }
            right_->nextTuple();
        }
    }

    //获取当前位置的tuple(已经保证满足cond)
    std::unique_ptr<RmRecord> Next() override {
        if(isend)return nullptr;
        return joinTuples(left_tuple_, right_tuple_);           
    }
    
    bool matchConditions(const std::unique_ptr<RmRecord> &left, const std::unique_ptr<RmRecord> &right) {
        for (const auto &cond : fed_conds_) {
            auto lhs_col_meta = get_col(cols_, cond.lhs_col);
            char *lhs_data = left->data + lhs_col_meta->offset;
            if (cond.is_rhs_val) {
                // 右边是常量值
                if (!eval_condition(lhs_data, lhs_col_meta->type, cond.op, cond.rhs_val)) {
                    return false;
                }
            } else {
                // 右边是列
                auto rhs_col_meta = get_col(cols_, cond.rhs_col);
                char *rhs_data = right->data + (rhs_col_meta->offset - left_->tupleLen());
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

    std::unique_ptr<RmRecord> joinTuples(const std::unique_ptr<RmRecord> &left, const std::unique_ptr<RmRecord> &right) {
        auto joined_data = std::make_unique<char[]>(len_);
        memcpy(joined_data.get(), left->data, left_->tupleLen());
        memcpy(joined_data.get() + left_->tupleLen(), right->data, right_->tupleLen());
        return std::make_unique<RmRecord>(len_, joined_data.release());
    }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override {
        return len_;
    }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }
    
    Rid &rid() override { return _abstract_rid; }
};