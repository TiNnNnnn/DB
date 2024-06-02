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

#include "parser/parser.h"
#include "system/sm.h"
#include "common/common.h"

//Query： 包含查询操作需要的各个部分信息
class Query{
    public:
    //ast tree(查询语法树)
    std::shared_ptr<ast::TreeNode> parse;
    // TODO jointree

    // where条件
    std::vector<Condition> conds;
    // 投影列 (cols while select)
    std::vector<TabCol> cols;
    // 表名
    std::vector<std::string> tables;
    // update 的set 值
    std::vector<SetClause> set_clauses;
    //insert 的values值
    std::vector<Value> values;

    Query(){}

};

//Analyze: 将AST Tree转化为 Query对象，同时检查AST Tree中涉及列是否合法
class Analyze
{
private:
    SmManager *sm_manager_;
public:
    Analyze(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Analyze(){}

    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root);

private:
    //检查目标列是否存在于所有列元数据中，如果存在则返回该列的完整信息
    TabCol check_column(const std::vector<ColMeta> &all_cols, TabCol target);
    //获取给定表名列表中的所有列元数据，并将其存储在 all_cols 向量中
    void get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols);
    //从一组二元表达式（ast::BinaryExpr）中提取条件，并将其转换为 Condition 向量
    void get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds);
    //检查条件中涉及的列是否在指定的表名列表中
    void check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds);
    //将语法树中的值节点（ast::Value）转换为系统中定义的 Value 类型
    Value convert_sv_value(const std::shared_ptr<ast::Value> &sv_val);
    //将语法树中的比较操作符（ast::SvCompOp）转换为系统中定义的 CompOp 类型
    CompOp convert_sv_comp_op(ast::SvCompOp op);
};

