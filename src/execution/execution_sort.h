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
#include <queue>
#include <functional>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   private:
    
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> project_cols_;
    std::vector<ColMeta> cols_;// 支持多个键排序
    size_t tuple_num;
    bool is_desc_;

    std::vector<std::string> temp_files; // 存储临时文件名
    std::unordered_map<std::string,int> temp_file_idxs; //存储临时文件中已访问的tuple数量

    std::ifstream current_file;
    RmRecord current_tuple; // 定义 current_tuple

    //std::priority_queue<std::pair<std::unique_ptr<RmRecord>, int>, std::vector<std::pair<std::unique_ptr<RmRecord>, int>>, Compare> pq;
    std::priority_queue<
        std::pair<std::unique_ptr<RmRecord>, int>,
        std::vector<std::pair<std::unique_ptr<RmRecord>, int>>,
        std::function<bool(const std::pair<std::unique_ptr<RmRecord>, int>&, const std::pair<std::unique_ptr<RmRecord>, int>&)>
    > pq;

    SmManager* sm_manager_;


   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol>sel_cols, bool is_desc, SmManager* sm_manager) {
        prev_ = std::move(prev);
        project_cols_ = prev_->cols();
        for(auto& col : sel_cols){
            cols_.push_back(*(get_col(prev_->cols(),col)));
        }
        is_desc_ = is_desc;

        pq = std::priority_queue<
            std::pair<std::unique_ptr<RmRecord>, int>,
            std::vector<std::pair<std::unique_ptr<RmRecord>, int>>,
            std::function<bool(const std::pair<std::unique_ptr<RmRecord>, int>&, const std::pair<std::unique_ptr<RmRecord>, int>&)>
        >(
            [this](const std::pair<std::unique_ptr<RmRecord>, int> &a,
                   const std::pair<std::unique_ptr<RmRecord>, int> &b) {
                // 根据 cols_ 中的列进行比较
                for (const auto& col : cols_) {
                    int cmp = std::memcmp(a.first->data + col.offset, b.first->data + col.offset, col.len);
                    if (cmp != 0) {
                        return is_desc_ ? cmp > 0 : cmp < 0;
                    }
                }
                return false; // 当所有列都相等时，认为 a == b
            }
        );

        sm_manager_ = sm_manager;
        
    }

    void beginTuple() override { 
        
        temp_files.clear();
        temp_file_idxs.clear();

        size_t buffer_size = 1024; // 假设内存缓冲区大小为1024条记录
        std::vector<std::unique_ptr<RmRecord>> buffer;

        prev_->beginTuple();
        while (!prev_->is_end()) {
            buffer.push_back(prev_->Next());
            if (buffer.size() >= buffer_size) {
                sort_and_store(buffer);
            }
            prev_->nextTuple();
        }

        // 处理剩余的记录
        if (!buffer.empty()) {
            sort_and_store(buffer);
        }

        // 初始化归并排序
        init_merge();

        if (!pq.empty()) {
            auto record = *(pq.top().first);
            auto file_idx = pq.top().second;
            pq.pop();

            std::string temp_file = "temp_file_" + std::to_string(file_idx);
            std::ifstream file(temp_file, std::ios::binary);
            if(!file){
                throw InternalError("temp file: "+temp_file+" not exists");
            }
            file.seekg(temp_file_idxs[temp_file]*(prev_->tupleLen()), std::ios::beg);

            std::unique_ptr<RmRecord> next_record = std::make_unique<RmRecord>(prev_->tupleLen());
            if (read_record(file, next_record)) {
                if(next_record->size && next_record->allocated_){
                    pq.push({std::move(next_record), file_idx});
                    temp_file_idxs[temp_file]++;
                }
            }
            file.close();
            current_tuple = record;
        } else {
            current_tuple.size = 0;
        }
    }

    void nextTuple() override {
        if (!pq.empty()) {
            auto record = *(pq.top().first);
            auto file_idx = pq.top().second;
            pq.pop();

            std::string temp_file = "temp_file_" + std::to_string(file_idx);
            std::ifstream file(temp_file, std::ios::binary);
            if(!file){
                throw InternalError("temp file: "+temp_file+" not exists");
            }
            file.seekg(temp_file_idxs[temp_file]*(prev_->tupleLen()), std::ios::beg);

            std::unique_ptr<RmRecord> next_record = std::make_unique<RmRecord>(prev_->tupleLen());
            if (read_record(file, next_record)) {
                if(next_record->size && next_record->allocated_){
                    pq.push({std::move(next_record), file_idx});
                    temp_file_idxs[temp_file]++;
                }
            }
            file.close();
            current_tuple = record;
        } else {
            current_tuple.size = 0;
        }
    }

    std::unique_ptr<RmRecord> Next() override {        
        return std::make_unique<RmRecord>(current_tuple);
    }

    virtual const std::vector<ColMeta> &cols() const{
        return project_cols_;
    }
    
    virtual bool is_end() const override{
        if(pq.empty() && current_tuple.size == 0){
            for(auto name : temp_files){
                // 删除文件
                // if (std::remove(name.c_str()) != 0) {
                //     throw InternalError("Failed to delete temp file: " + name);
                // }
            }
        }
        return pq.empty() && current_tuple.size == 0;
    }

    size_t tupleLen() const override {
        return prev_->tupleLen();
    }

    Rid &rid() override { return _abstract_rid; }

private:
    static int compareTuples(const std::unique_ptr<RmRecord>& a, const std::unique_ptr<RmRecord>& b, const ColMeta& col){
        ColType type =  col.type;
        if(type == TYPE_INT){
            int32_t a_val = *reinterpret_cast<const int32_t*>(a->data + col.offset);
            int32_t b_val = *reinterpret_cast<const int32_t*>(b->data + col.offset);
            return a_val - b_val;
        }else if (type == TYPE_FLOAT){
            float a_val = *reinterpret_cast<const float*>(a->data + col.offset);
            float b_val = *reinterpret_cast<const float*>(b->data + col.offset);
            return a_val - b_val;
        }else { //TYPE_STRING
            std::string a_str(a->data,col.offset);
            std::string b_str(b->data,col.offset);
            return a_str < b_str;
        }
    }

    //将块数据进行排序并写入temp_file
    void sort_and_store(std::vector<std::unique_ptr<RmRecord>>& buffer) {
        std::sort(buffer.begin(), buffer.end(), [&](std::unique_ptr<RmRecord>&a,std::unique_ptr<RmRecord>&b){
            for (auto& col : cols_) {
            int cmp = compareTuples(a, b, col);
                if (cmp != 0) {
                    return is_desc_ ? cmp > 0 : cmp < 0;
                }
            }
            return false;
        });
        std::string temp_file = "temp_file_" + std::to_string(temp_files.size());
        std::ofstream out(temp_file, std::ios::binary);
        for (auto& record : buffer) {
            write_record(out, *record);
        }
        out.close();
        temp_files.push_back(temp_file);
        temp_file_idxs[temp_file] = 0;
        buffer.clear();
    }

    //初始化归并排序
    void init_merge() {
        for (const auto& temp_file : temp_files) {
            std::ifstream file(temp_file, std::ios::binary);
            if(!file){
                throw InternalError("temp file: "+temp_file+" not exists");
            }
            std::unique_ptr<RmRecord> record = std::make_unique<RmRecord>(prev_->tupleLen());
            if (read_record(file, record)) {
                if(record->size == 0 || record->allocated_ == false)continue;
                pq.push({std::move(record), temp_files.size() - 1});
                temp_file_idxs[temp_file]++;
            }
        }
    }

    void write_record(std::ofstream& out, const RmRecord& record) {
        // 将记录写入文件
        out.write(reinterpret_cast<const char*>(record.data), record.size);
        out.flush();
        if (!out) {
            throw std::runtime_error("Failed to write record to file");
        }
    }

    bool read_record(std::ifstream& in, std::unique_ptr<RmRecord>& record) {
        // 从文件中读取一个记录，假设记录大小是固定的
        if (!in.read(reinterpret_cast<char*>(record->data), record->size)) {
            return false;
        }
        return true;
    }
};