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

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 字段总长度
    std::vector<size_t> sel_idxs_;                  

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override {
        prev_->beginTuple();
    }

    void nextTuple() override {
        prev_->nextTuple();
    }

    std::unique_ptr<RmRecord> Next() override {
        // 从上一个节点获取下一个记录
        auto record = prev_->Next();
        if (!record) {
            return nullptr; // 如果上一个节点没有下一个记录，则返回空指针
        }

        // 创建一个新的记录
        auto projected_record = std::make_unique<RmRecord>(len_);

        // 从上一个记录中复制需要投影的字段到新记录中
        for (size_t i = 0; i < sel_idxs_.size(); ++i) {
            auto idx = sel_idxs_[i]; // 获取需要投影的字段的索引
            auto &col = cols_[i];    // 获取投影字段的元数据
            auto src_data = record->data + prev_->cols()[idx].offset; // 获取上一个记录中需要投影字段的数据
            auto dest_data = projected_record->data + col.offset;     // 获取新记录中投影字段的位置
            std::memcpy(dest_data, src_data, col.len); // 将数据复制到新记录中
        }

        return projected_record;
    }

    Rid &rid() override { return _abstract_rid; }
};