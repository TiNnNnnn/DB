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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value

    // 检查条件是否满足
    // bool isSatisfied(const std::unique_ptr<RmRecord> &left, const std::unique_ptr<RmRecord> &right) const {
    //     const char* lhs_data = left->data + lhs_col.offset;
    //     const char* rhs_data;

    //     if (is_rhs_val) {
    //         // 右侧是常量值
    //         rhs_data = reinterpret_cast<const char*>(&rhs_val);
    //     } else {
    //         // 右侧是列值
    //         rhs_data = right->data + rhs_val.offset;
    //     }

    //     // 根据数据类型进行比较
    //     switch (rhs_val.type) {
    //         case TYPE_INT: {
    //             int lhs_value = *reinterpret_cast<const int*>(lhs_data);
    //             int rhs_value = is_rhs_val ? rhs_val.int_val : *reinterpret_cast<const int*>(rhs_data);

    //             switch (op) {
    //                 case OP_EQ:
    //                     return lhs_value == rhs_value;
    //                 case OP_NE:
    //                     return lhs_value != rhs_value;
    //                 case OP_LT:
    //                     return lhs_value < rhs_value;
    //                 case OP_LE:
    //                     return lhs_value <= rhs_value;
    //                 case OP_GT:
    //                     return lhs_value > rhs_value;
    //                 case OP_GE:
    //                     return lhs_value >= rhs_value;
    //             }
    //             break;
    //         }
    //         case TYPE_FLOAT: {
    //             float lhs_value = *reinterpret_cast<const float*>(lhs_data);
    //             float rhs_value = is_rhs_val ? rhs_val.float_val : *reinterpret_cast<const float*>(rhs_data);

    //             switch (op) {
    //                 case OP_EQ:
    //                     return lhs_value == rhs_value;
    //                 case OP_NE:
    //                     return lhs_value != rhs_value;
    //                 case OP_LT:
    //                     return lhs_value < rhs_value;
    //                 case OP_LE:
    //                     return lhs_value <= rhs_value;
    //                 case OP_GT:
    //                     return lhs_value > rhs_value;
    //                 case OP_GE:
    //                     return lhs_value >= rhs_value;
    //             }
    //             break;
    //         }
    //         case TYPE_STRING: {
    //             std::string lhs_value(lhs_data);
    //             std::string rhs_value = is_rhs_val ? rhs_val.str_val : std::string(rhs_data);

    //             switch (op) {
    //                 case OP_EQ:
    //                     return lhs_value == rhs_value;
    //                 case OP_NE:
    //                     return lhs_value != rhs_value;
    //                 case OP_LT:
    //                     return lhs_value < rhs_value;
    //                 case OP_LE:
    //                     return lhs_value <= rhs_value;
    //                 case OP_GT:
    //                     return lhs_value > rhs_value;
    //                 case OP_GE:
    //                     return lhs_value >= rhs_value;
    //             }
    //             break;
    //         }
    //     }

    //     return false; // 不支持的数据类型
    // }
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};