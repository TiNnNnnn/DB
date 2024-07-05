#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

//归并连接算法 (left 和 right 已经按照cols进行了排序)

//TODO：如何处理非等值连接条件下的归并连接，可能退化为nsetloop_join
class MergeJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

    std::unique_ptr<RmRecord> left_tuple_;      // 左子执行器当前的元组
    std::unique_ptr<RmRecord> right_tuple_;     // 右子执行器当前的元组


    std::vector<std::unique_ptr<RmRecord>> left_buffer_;  // 缓存当前匹配的左表记录
    std::vector<std::unique_ptr<RmRecord>> right_buffer_; // 缓存当前匹配的右表记录
    int left_index_;  // 当前左表缓存的索引
    int right_index_; // 当前右表缓存的索引

    RmRecord* temp_left_tuple_;
    RmRecord* temp_right_tuple_;

public:
    MergeJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds){
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

        left_buffer_.clear();
        right_buffer_.clear();
        left_index_ = 0;
        right_index_ = 0;

        left_->beginTuple(); 
        left_tuple_ = left_->Next();
        right_->beginTuple();
        right_tuple_ = right_->Next();

        while(true){
            if ((left_->is_end() || right_->is_end()) && (left_index_ == int(left_buffer_.size()))) {
                isend = true;
                return;
            }
            int cmp = 0;
            if(left_tuple_ && right_tuple_)
                cmp = compareJoinKeys(left_tuple_, right_tuple_);
            if(cmp == 0 || left_index_ < int(left_buffer_.size())){
                if(right_buffer_.empty() || left_buffer_.empty()){

                    left_index_ = 0,right_index_ = 0;

                    temp_left_tuple_ = left_tuple_.get();
                    temp_right_tuple_ = right_tuple_.get(); 

                    bufferMatchingLeftTuples();
                    bufferMatchingRightTuples();
                }
                if (right_index_ < int(right_buffer_.size())) {
                    right_tuple_ = std::make_unique<RmRecord>(*right_buffer_[right_index_]);
                    ++right_index_;
                }
                if (left_index_ < int(left_buffer_.size())) {
                    left_tuple_ = std::make_unique<RmRecord>(*left_buffer_[left_index_]);
                    if(right_index_ == int(right_buffer_.size())){
                        ++left_index_;
                        right_index_ = 0;
                    }
                }
                if(left_index_ == int(left_buffer_.size())){
                    //over
                    left_index_ = 0,right_index_ = 0;
                    left_buffer_.clear();
                    right_buffer_.clear();
                    temp_left_tuple_= nullptr;
                    temp_right_tuple_=nullptr;
                }
                // if(matchConditions(left_tuple_,right_tuple_)){
                //     break;
                // }
                break;
            }else if (cmp < 0) {
                left_->nextTuple();
                left_tuple_ = left_->Next();
            } else {
                right_->nextTuple();
                right_tuple_ = right_->Next();
            }

        }

    }

    void nextTuple() override {
        left_tuple_ = left_->Next();
        right_tuple_ = right_->Next();
        while(true){
            if ((left_->is_end() || right_->is_end()) && (left_index_ == int(left_buffer_.size()))) {
                isend = true;
                return;
            }
            int cmp = 0;
            if(left_tuple_ && right_tuple_)
                cmp = compareJoinKeys(left_tuple_, right_tuple_);

            if(cmp == 0 || left_index_ < int(left_buffer_.size())){
                if(right_buffer_.empty() || left_buffer_.empty()){
                    left_index_ = 0,right_index_ = 0;
                    temp_left_tuple_ = left_tuple_.get();
                    temp_right_tuple_ = right_tuple_.get(); 
                    bufferMatchingLeftTuples();
                    bufferMatchingRightTuples();
                }
                if (right_index_ < int(right_buffer_.size())) {
                    right_tuple_ = std::make_unique<RmRecord>(*right_buffer_[right_index_]);
                    ++right_index_;
                }
                if (left_index_ < int(left_buffer_.size())) {
                    left_tuple_ = std::make_unique<RmRecord>(*left_buffer_[left_index_]);
                    if(right_index_ == int(right_buffer_.size())){
                        ++left_index_;
                        right_index_ = 0;
                    }
                }
                if(left_index_ == int(left_buffer_.size())){
                    //over
                    left_index_ = 0,right_index_ = 0;
                    left_buffer_.clear();
                    right_buffer_.clear();
                    temp_left_tuple_= nullptr;
                    temp_right_tuple_=nullptr;
                }
                break;
            }else if (cmp < 0) { // 左表的键较小
                left_->nextTuple();
                left_tuple_ = left_->Next();
            } else { // 右表的键较小
                right_->nextTuple();
                right_tuple_ = right_->Next();
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if(isend)return nullptr;
        return joinTuples(left_tuple_, right_tuple_);        
    }

    bool is_end() const override { return isend && left_index_ >= int(left_buffer_.size()); }

    size_t tupleLen() const override {
        return len_;
    }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }
    
    Rid &rid() override { return _abstract_rid; }

private:

    void bufferMatchingLeftTuples() {
        left_buffer_.clear();
        left_index_ = 0;

        while (!left_->is_end() && compareJoinKeys(left_tuple_, std::make_unique<RmRecord>(*temp_right_tuple_)) == 0) {
            left_buffer_.push_back(std::move(left_tuple_));
            left_->nextTuple();
            left_tuple_ = left_->Next();
        }
    }

    void bufferMatchingRightTuples() {
        right_buffer_.clear();
        right_index_ = 0;

        while (!right_->is_end() && compareJoinKeys(std::make_unique<RmRecord>(*temp_left_tuple_), right_tuple_) == 0) {
            right_buffer_.push_back(std::move(right_tuple_));
            right_->nextTuple();
            right_tuple_ = right_->Next();
        }
    }


    int compareJoinKeys(const std::unique_ptr<RmRecord> &left, const std::unique_ptr<RmRecord> &right) {
        for (const auto &cond : fed_conds_) {
            if(cond.is_rhs_val == true || cond.is_lhs_col == false)continue;
            auto lhs_col_meta = get_col(cols_, cond.lhs_col);
            auto rhs_col_meta = get_col(cols_, cond.rhs_col);

            assert(lhs_col_meta->type == rhs_col_meta->type);
            ColType type = lhs_col_meta->type;
            if(type == TYPE_INT){
                int32_t a_val = *reinterpret_cast<const int32_t*>(left->data + lhs_col_meta->offset);
                int32_t b_val = *reinterpret_cast<const int32_t*>(right->data + rhs_col_meta->offset - left_->tupleLen());
                return a_val - b_val;
            }else if (type == TYPE_FLOAT){
                float a_val = *reinterpret_cast<const float*>(left->data + lhs_col_meta->offset);
                float b_val = *reinterpret_cast<const float*>(right->data + rhs_col_meta->offset - left_->tupleLen());
                return a_val - b_val;
            }else { //TYPE_STRING
                std::string a_str(left->data,lhs_col_meta->offset);
                std::string b_str(right->data,rhs_col_meta->offset - left_->tupleLen());
                return a_str < b_str;
            }
        }
        return 0;
    }
    

    bool matchConditions(const std::unique_ptr<RmRecord> &left, const std::unique_ptr<RmRecord> &right) {
        for (const auto &cond : fed_conds_) {
            auto lhs_col_meta = get_col(cols_, cond.lhs_col);
            char *lhs_data = left->data + lhs_col_meta->offset;

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

};