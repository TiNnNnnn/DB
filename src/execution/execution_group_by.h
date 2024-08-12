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
#include "system/sm.h"

struct AggregateResult {
    int count = 0;
    int count_for_star = 0;
    int count_for_avg = 0;
    float sum = 0;
    float sum_for_avg = 0;
    float max = std::numeric_limits<float>::min();
    float min = std::numeric_limits<float>::max();
};

class GroupByExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_; //输入节点 seq_scan
    std::vector<TabCol> group_by_cols_; //待分组的列
    std::vector<Condition> having_clauses_; //groupby的having子句
    std::vector<AggregateExpr> agg_exprs_;  //聚合函数列表
    std::vector<TabCol> sel_cols_;  //需要投影的字段

    size_t len_;                        //tuple len
    std::vector<ColMeta> cols_;         //table中所有列
    std::unordered_map<std::string, std::vector<RmRecord>> groups_; // 用于存储分组后的记录
    std::unordered_map<std::string, AggregateResult> aggregated_results_; // 用于存储聚合结果
    std::unordered_map<std::string, AggregateResult>::iterator current_group_; 

public:
    GroupByExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &group_by_cols,
        const std::vector<Condition>having_clauses,std::vector<AggregateExpr>agg_exprs,const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);
        group_by_cols_ = std::move(group_by_cols);
        having_clauses_ = std::move(having_clauses);
        sel_cols_ = std::move(sel_cols);
        agg_exprs_ = std::move(agg_exprs);

        cols_ = prev_->cols();

        len_ = 0;
        for(auto &col : sel_cols_){
            auto pos = get_col(cols_, col);
            len_+= pos->len;
        }
        for(auto &agg : agg_exprs_){
            len_+= 4;
        }
    }

    void beginTuple() override {
        groups_.clear();
        aggregated_results_.clear();
        //获取table中所有的record
        for(prev_->beginTuple();!prev_->is_end();prev_->nextTuple()){
            auto record = prev_->Next();
            if (record) {
                std::string key = generate_group_key(*record);
                groups_[key].push_back(*record);
            }
        }

        if(groups_.empty()){
            AggregateResult aggregate_result;
            aggregated_results_["empty_group"] = aggregate_result;
        }

        //依次处理分组
        for(auto &group: groups_){
            AggregateResult aggregate_result;
            //根据聚合函数处理所有分组
            for (const auto &record : group.second) {
                update_aggregate_record(aggregate_result, record);
            }
            //检测分组是否符合having子句
            if (satisfies_having_clauses(group)) {
                aggregated_results_[group.first] = aggregate_result;
            }
        }
        current_group_ = aggregated_results_.begin();
    }

    void nextTuple() override {
        if (current_group_ != aggregated_results_.end()) {
            ++current_group_;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_group_ != aggregated_results_.end()) {
            // 构造聚合结果记录
            auto &result = current_group_->second;
            auto tuples = groups_[current_group_->first];
            auto record = std::make_unique<RmRecord>(len_);
            // 填充记录数据
            size_t offset = 0;
            for (const auto &col : sel_cols_) {
                auto col_meta = get_col(cols_, col);
                auto src_data = tuples[0].data + col_meta->offset;
                memcpy(record->data + offset, src_data, col_meta->len);
                offset += col_meta->len;
            }
            for (const auto &agg_expr : agg_exprs_) {
                if (agg_expr.func_name == "COUNT" ) {
                    if(agg_expr.cols.size()==1){
                        int count_value = result.count;
                        memcpy(record->data + offset, &count_value, sizeof(int));
                        offset += sizeof(int);
                    }else{
                        int count_value = result.count_for_star;
                        memcpy(record->data + offset, &count_value, sizeof(int));
                        offset += sizeof(int);
                    }
                    
                } else if (agg_expr.func_name == "SUM") {
                    ColType c_type;
                    for(auto e : cols_){
                        if(agg_expr.cols[0].col_name == e.name){
                            c_type = e.type;
                            break;
                        }
                    }
                    if(c_type == TYPE_FLOAT){
                        float sum_value = result.sum;
                        memcpy(record->data + offset, &sum_value, sizeof(float));
                        offset += sizeof(float);
                    }else if(c_type == TYPE_INT){
                        int sum_value = int(result.sum);
                        memcpy(record->data + offset, &sum_value, sizeof(int));
                        offset += sizeof(int);
                    }
                    
                } else if (agg_expr.func_name == "AVG") {
                    float avg_value = result.sum_for_avg / result.count_for_avg;
                    memcpy(record->data + offset, &avg_value, sizeof(float));
                    offset += sizeof(float);
                } else if (agg_expr.func_name == "MAX") {
                    ColType c_type;
                    for(auto e : cols_){
                        if(agg_expr.cols[0].col_name == e.name){
                            c_type = e.type;
                            break;
                        }
                    }
                    if(c_type == TYPE_FLOAT){
                        float max_value = result.max;
                        memcpy(record->data + offset, &max_value, sizeof(float));
                        offset += sizeof(float);
                    }else if(c_type == TYPE_INT){
                        int max_value = int(result.max);
                        memcpy(record->data + offset, &max_value, sizeof(int));
                        offset += sizeof(int);
                    }
                    
                } else if (agg_expr.func_name == "MIN") {
                    ColType c_type;
                    for(auto e : cols_){
                        if(agg_expr.cols[0].col_name == e.name){
                            c_type = e.type;
                            break;
                        }
                    }
                    if(c_type == TYPE_FLOAT){
                        float min_value = result.min;
                        memcpy(record->data + offset, &min_value, sizeof(float));
                        offset += sizeof(float);
                    }else if(c_type == TYPE_INT){
                        int min_value = int(result.min);
                        memcpy(record->data + offset, &min_value, sizeof(int));
                        offset += sizeof(int);
                    }
                    
                }
            }
            return record;
        }
        return nullptr;
    }

    bool is_end() const override{
        return current_group_ == aggregated_results_.end();
    }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    std::string getType() override{
        return "GroupByExecutor";
    }

    Rid &rid() override {
    }
private:
    std::string generate_group_key(const RmRecord &record) {
        // 根据分组列生成分组键
        std::string key;

        //for aggregation without groupby,than we merge all rows into one group
        if(group_by_cols_.size() == 0){
            key = "aggregations";
            return key;
        }

        for (const auto &col : group_by_cols_) {
            auto col_meta = get_col(cols_, col);
            key.append(record.data + col_meta->offset, col_meta->len);
        }
        return key;
    }

    void update_aggregate_record(AggregateResult &aggregate_result, const RmRecord &record) {
        for (const auto &agg_expr : agg_exprs_) {
            if (agg_expr.func_name == "COUNT") {
                if(agg_expr.cols.size() == 1){
                    //COUNT(expr)
                    ++aggregate_result.count;
                }else{
                    //COUNT(*)
                    ++aggregate_result.count_for_star;
                }
            } else {
                assert(agg_expr.cols.size() == 1); 
                auto col_meta = get_col(cols_, agg_expr.cols[0]);

                Value value;
                char *rec_buf = record.data + col_meta->offset;
                if (col_meta->type == TYPE_INT) {
                    value.set_int(*(int *)rec_buf);
                }else if(col_meta->type == TYPE_FLOAT){
                    value.set_float(*(float *)rec_buf);
                }else if (col_meta->type == TYPE_STRING){
                    //error
                }
                //float value = *reinterpret_cast<float *>(record.data + col_meta->offset);
                if (agg_expr.func_name == "SUM") {
                    aggregate_result.sum += value.type == TYPE_INT?value.int_val:value.float_val;
                } else if (agg_expr.func_name == "MAX") {
                    aggregate_result.max = std::max(aggregate_result.max, value.type == TYPE_INT?value.int_val:value.float_val);
                } else if (agg_expr.func_name == "MIN") {
                    aggregate_result.min = std::min(aggregate_result.min,value.type == TYPE_INT?value.int_val:value.float_val);
                } else if (agg_expr.func_name == "AVG") {
                    aggregate_result.sum_for_avg += (value.type == TYPE_INT?value.int_val:value.float_val);
                    ++aggregate_result.count_for_avg;
                }
            }
        }
    }

    bool satisfies_having_clauses(const std::pair<const std::string, std::vector<RmRecord>>& group) {

        for (const auto &cond : having_clauses_) {
            bool condition_met = false;

            if(cond.op == CompOp::IN){
                for(auto r: group.second){
                    if(!match_condition(&r,cond)){
                        return false;
                    }
                }
                continue;
            }
            if(cond.is_lhs_col){
                for(auto r : group.second){
                    if(!match_condition(&r,cond)){
                        return false;
                    }
                }
            }else{ 
                AggregateResult aggregate_result;
                for (const auto &record : group.second) {   
                    update_aggregate_record_for_having(aggregate_result, record, cond);
                }
                auto agg_expr = cond.lhs_agg;
                if (agg_expr.func_name == "COUNT") {
                    if(agg_expr.cols.size()==1){
                        condition_met = evaluate_condition(cond, aggregate_result.count);
                    }else{
                        condition_met = evaluate_condition(cond, aggregate_result.count_for_star);
                    }
                } else if (agg_expr.func_name == "SUM") {
                    condition_met = evaluate_condition(cond, aggregate_result.sum);
                } else if (agg_expr.func_name == "AVG") {
                    condition_met = evaluate_condition(cond, aggregate_result.sum_for_avg / aggregate_result.count_for_avg);
                } else if (agg_expr.func_name == "MAX") {
                    condition_met = evaluate_condition(cond, aggregate_result.max);
                } else if (agg_expr.func_name == "MIN") {
                    condition_met = evaluate_condition(cond, aggregate_result.min);
                }
            }
            if (!condition_met) {
                return false;
            }
        }
        return true;
    }

    //comapare for aggreation
    template <typename T>
    bool evaluate_condition(const Condition &cond, T value) {
        if (cond.rhs_val.type == TYPE_INT){
                switch (cond.op) {
                case OP_EQ:
                    return value == cond.rhs_val.int_val;
                case OP_NE:
                    return value != cond.rhs_val.int_val;
                case OP_LT:
                    return value < cond.rhs_val.int_val;
                case OP_LE:
                    return value <= cond.rhs_val.int_val;
                case OP_GT:
                    return value > cond.rhs_val.int_val;
                case OP_GE:
                    return value >= cond.rhs_val.int_val;
                default:
                    return false;
            }
        }else if(cond.rhs_val.type == TYPE_FLOAT){
            switch (cond.op) {
                case OP_EQ:
                    return value == cond.rhs_val.float_val;
                case OP_NE:
                    return value != cond.rhs_val.float_val;
                case OP_LT:
                    return value < cond.rhs_val.float_val;
                case OP_LE:
                    return value <= cond.rhs_val.float_val;
                case OP_GT:
                    return value > cond.rhs_val.float_val;
                case OP_GE:
                    return value >= cond.rhs_val.float_val;
                default:
                    return false;
            }
        }
        return false;
    }

    void update_aggregate_record_for_having(AggregateResult &aggregate_result, const RmRecord &record,const Condition& cond) {
            assert(!cond.is_lhs_col);
            auto agg_expr = cond.lhs_agg;
            if (agg_expr.func_name == "COUNT") {
                if(agg_expr.cols.size() == 1){
                    //COUNT(expr)
                    ++aggregate_result.count;
                }else{
                    //COUNT(*)
                    ++aggregate_result.count_for_star;
                }
            } else {
                assert(agg_expr.cols.size() == 1); 
                auto col_meta = get_col(cols_, agg_expr.cols[0]);

                Value value;
                char *rec_buf = record.data + col_meta->offset;
                if (col_meta->type == TYPE_INT) {
                    value.set_int(*(int *)rec_buf);
                }else if(col_meta->type == TYPE_FLOAT){
                    value.set_float(*(float *)rec_buf);
                }else if (col_meta->type == TYPE_STRING){
                    //error
                }
                //float value = *reinterpret_cast<float *>(record.data + col_meta->offset);
                if (agg_expr.func_name == "SUM") {
                    aggregate_result.sum += value.type == TYPE_INT?value.int_val:value.float_val;
                } else if (agg_expr.func_name == "MAX") {
                    aggregate_result.max = std::max(aggregate_result.max, value.type == TYPE_INT?value.int_val:value.float_val);
                } else if (agg_expr.func_name == "MIN") {
                    aggregate_result.min = std::min(aggregate_result.min,value.type == TYPE_INT?value.int_val:value.float_val);
                } else if (agg_expr.func_name == "AVG") {
                    aggregate_result.sum_for_avg += (value.type == TYPE_INT?value.int_val:value.float_val);
                    ++aggregate_result.count_for_avg;
                }
            }
    }

    // 检查记录是否符合条件
    bool match_condition(const RmRecord *record, const Condition &cond) {
        auto lhs_col_meta = get_col(cols_, cond.lhs_col);
        char *lhs_data = record->data + lhs_col_meta->offset;

        if(cond.op == CompOp::IN){
            bool is_find = false;
            auto rhs_vals = cond.rhs_vals;
            for(auto& rhs_val : rhs_vals){
                if (eval_condition(lhs_data, lhs_col_meta->type, lhs_col_meta->len,CompOp::OP_EQ , rhs_val)) {
                    is_find = true;
                    break;
                }
            }
            if(!is_find){
                return false;
            }
            return true; 
        }


        if (cond.is_rhs_val) {
            // 右边是常量值
            if (!eval_condition(lhs_data, lhs_col_meta->type, lhs_col_meta->len,cond.op, cond.rhs_val)) {
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
        return true;
    }
    // 评估条件（左值 vs 右值）
    bool eval_condition(const char *lhs_data, ColType lhs_type, int lhs_len,CompOp op, const Value &rhs_val) {
        switch (lhs_type) {
            case TYPE_INT:
                return eval_condition(*(int *)lhs_data, op, rhs_val.int_val);
            case TYPE_FLOAT:
                return eval_condition(*(float *)lhs_data, op, rhs_val.float_val);
            case TYPE_STRING:
                return eval_condition(std::string(lhs_data, lhs_len), op, rhs_val.str_val);
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