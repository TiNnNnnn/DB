/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 处理表名
        query->tables = std::move(x->tabs);
        //检查表是否存在
        for(auto &tb_name : query->tables){
            if(!sm_manager_->db_.is_table(tb_name)){
                throw TableNotFoundError(tb_name);
            }
        }
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
       
        //处理col,aggregate_expr
        for (auto &sv_sel_col : x->cols) {
            if (auto col = std::dynamic_pointer_cast<ast::Col>(sv_sel_col)){
                //列名
                TabCol tab_col = {get_tb_name(query->tables,col->col_name),col->col_name};
                query->cols.push_back(tab_col);
            }else if (auto a_expr = std::dynamic_pointer_cast<ast::AggregateExpr>(sv_sel_col)){
                //聚合函数
                AggregateExpr aggregate_expr;
                auto a_name = a_expr->func_name;
                std::vector<TabCol> cols;
                if (a_name == "COUNT" or a_name == "count"){
                    //COUNT 
                    if (auto arg = std::dynamic_pointer_cast<ast::StarExpr>(a_expr->arg)){
                        //COUNT(*)
                        for (auto &col : all_cols) {
                            cols.push_back({get_tb_name(query->tables,col.name),col.name});
                        }
                        aggregate_expr = {query->tables,a_name,cols,a_expr->alias};
                    }else if (auto arg = std::dynamic_pointer_cast<ast::Col>(a_expr->arg)){
                        //COUNT(expr)
                        cols.push_back({get_tb_name(query->tables,arg->col_name),arg->col_name});
                        std::vector<std::string>tabs;
                        tabs.push_back(arg->tab_name);
                        aggregate_expr = {tabs,a_name,cols,a_expr->alias};
                    }else{
                        //TODO_06-17: support more type
                    }
                }else{
                    //SUM,AVG and so on...
                    if (auto arg = std::dynamic_pointer_cast<ast::Col>(a_expr->arg)){
                        cols.push_back({get_tb_name(query->tables,arg->col_name),arg->col_name});
                        std::vector<std::string>tabs;
                        tabs.push_back(arg->tab_name);
                        aggregate_expr = {tabs,a_name,cols,a_expr->alias};
                    }else{
                        //TODO_06-17: support more type
                    }
                }
                query->a_exprs.push_back(aggregate_expr);
            }
        }
        // for select *
        if (query->cols.empty() && query->a_exprs.empty()) {
            // select all columns
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        } else {
            // infer table name from column name
            for (auto &sel_col : query->cols) {
                sel_col = check_column(all_cols, sel_col);  // 列元数据校验
            }
        }
        //处理where条件
        get_clause(x->conds, query->conds,query->tables);
        //WHERE 子句中不能用聚集函数作为条件表达式
        for(auto &cond : query->conds){
            if(!cond.is_lhs_col){
                throw InternalError("here is aggregations in where exprs");
            }
        }
        check_clause(query->tables, query->conds);

        //处理分组条件
        if(x->group_by){
            auto group_cols = x->group_by->cols;
            auto group_having_clauses = x->group_by->havingClause;

            GroupByExpr gb_expr;
            std::set<TabCol>col_set;
            for (auto& col : group_cols){
                gb_expr.cols.push_back({get_tb_name(query->tables,col->col_name),col->col_name});
                col_set.insert({get_tb_name(query->tables,col->col_name),col->col_name});
            }

            //检查sel_cols中是否存在groupby cols中不存在的col
            for (auto &col : query->cols){
                if(col_set.find(col) == col_set.end()){
                    throw InternalError("sel_cols has cols that groupby cols not exist");
                }
            }

            //处理分组条件中的HAVING子句
            if(group_having_clauses){
                for(auto& cond : group_having_clauses->conditions){
                    if (auto expr = std::dynamic_pointer_cast<ast::Col>(cond->lhs)){
                        TabCol col;
                        if(query->tables.size()>1)col = {expr->tab_name, expr->col_name};
                        else col = {get_tb_name(query->tables,expr->col_name),expr->col_name};
                        if(col_set.find(col) == col_set.end()){
                            throw InternalError("having has cols that groupby cols not exist");
                        }
                    }
                }

                get_clause(group_having_clauses->conditions, gb_expr.havingClause,query->tables);
            }
            //check_clause(query->tables, gb_expr.havingClause);
            query->gb_expr = gb_expr;
        }
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        // 处理表名
        query->tables.push_back(x->tab_name);
        // 检查表是否存在
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        // 处理set子句
        for (auto &sv_set_clause : x->set_clauses) {
            Value sv_value = convert_sv_value(sv_set_clause->val);
            if(sm_manager_->db_.get_table(x->tab_name).get_col(
                sv_set_clause->col_name)->type == TYPE_FLOAT){
                if(sv_value.type == TYPE_INT){
                    sv_value.type = TYPE_FLOAT;
                    sv_value.float_val = (float)sv_value.int_val;
                }
            }
            SetClause set_clause = {
                {x->tab_name,sv_set_clause->col_name},sv_value
            };
            query->set_clauses.push_back(set_clause);
        }
        // 检查更新的列是否存在
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        for (auto &set_clause : query->set_clauses) {
            TabCol target_col = {x->tab_name, set_clause.lhs.col_name};
            check_column(all_cols, target_col);
        }
        // 处理where条件
        get_clause(x->conds, query->conds,query->tables);
        check_clause(query->tables, query->conds);

    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        // 处理表名
        query->tables.push_back(x->tab_name);
        // 检查表是否存在
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        //处理where条件
        get_clause(x->conds, query->conds,query->tables);
        check_clause({x->tab_name}, query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理表名
        query->tables.push_back(x->tab_name);
        // 检查表是否存在
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else if(auto x = std::dynamic_pointer_cast<ast::ShowIndex>(parse)) {
        query->tables.push_back(x->tab_name);
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
    } 
    else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        //未指定table_name，从列名推断table_name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        bool column_found = false;
        for (auto &col : all_cols) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                column_found = true;
                break;
            }
        }
        if (!column_found) {
            throw ColumnNotFoundError(target.col_name);
        }
    }
    return target;
}

//TODO: when two tables has same col name ?
 std::string Analyze::get_tb_name(const std::vector<std::string> &tab_names, std::string col_name){
    for(auto &sel_tab_name: tab_names){
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        for(auto &col : sel_tab_cols){
            if(col_name == col.name){
                return sel_tab_name;
            }
        }
    }
    return "";
 }


void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds,std::vector<std::string> tables) {
    conds.clear();
    for (auto &e : sv_conds) {
        Condition cond;
        //lhs is Col
        if (auto expr = std::dynamic_pointer_cast<ast::Col>(e->lhs)){
            cond.is_lhs_col = true;
            if(tables.size()>1)cond.lhs_col = {expr->tab_name, expr->col_name};
            else cond.lhs_col = {get_tb_name(tables,expr->col_name),expr->col_name};
            cond.op = convert_sv_comp_op(e->op);
            if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(e->rhs)) {
                cond.is_rhs_val = true;
                cond.rhs_val = convert_sv_value(rhs_val);
            } else if (auto expr = std::dynamic_pointer_cast<ast::Col>(e->rhs)) {
                cond.is_rhs_val = false;
                if(tables.size()>1)cond.rhs_col = {expr->tab_name, expr->col_name};
                else cond.rhs_col = {get_tb_name(tables,expr->col_name),expr->col_name};
            } 
        //rhs is Aggregate
        }else if(auto arg = std::dynamic_pointer_cast<ast::AggregateExpr>(e->lhs)){
            std::vector<ColMeta> all_cols;
            get_all_cols(tables, all_cols);
            cond.is_lhs_col = false;
            //聚合函数
            AggregateExpr aggregate_expr;
            auto a_name = arg->func_name;
            auto a_expr = arg->arg;
            std::vector<TabCol> cols;
            if (a_name == "COUNT" or a_name == "count"){
                    //COUNT 
                    if (auto arg = std::dynamic_pointer_cast<ast::StarExpr>(a_expr)){
                        //COUNT(*)
                        for (auto &col : all_cols) {
                            cols.push_back({get_tb_name(tables,col.name),col.name});
                        }
                        aggregate_expr = {tables,a_name,cols,""};
                    }else if (auto arg = std::dynamic_pointer_cast<ast::Col>(a_expr)){
                        //COUNT(expr)
                        cols.push_back({get_tb_name(tables,arg->col_name),arg->col_name});
                        std::vector<std::string>tabs;
                        tabs.push_back(get_tb_name(tables,arg->col_name));
                        aggregate_expr = {tabs,a_name,cols,""};
                    }else{
                        //TODO_06-17: support more type
                    }
                }else{
                    //SUM,AVG and so on...
                    if (auto arg = std::dynamic_pointer_cast<ast::Col>(a_expr)){
                        cols.push_back({get_tb_name(tables,arg->col_name),arg->col_name});
                        std::vector<std::string>tabs;
                        tabs.push_back(get_tb_name(tables,arg->col_name));
                        aggregate_expr = {tabs,a_name,cols,""};
                    }else{
                        //TODO_06-17: support more type
                    }
            }            
            cond.lhs_agg = aggregate_expr;
            cond.op = convert_sv_comp_op(e->op);
            if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(e->rhs)) {
                cond.is_rhs_val = true;
                cond.rhs_val = convert_sv_value(rhs_val);
            } else if (auto expr = std::dynamic_pointer_cast<ast::Col>(e->lhs)) {
                // cond.is_rhs_val = false;
                // cond.rhs_col = {expr->tab_name, expr->col_name};
                throw InternalError("aggregation right cond must be value");
            }
        }else{
            //ERROR
        }
      
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        if(cond.is_lhs_col){
            cond.lhs_col = check_column(all_cols, cond.lhs_col);
            if (!cond.is_rhs_val) {
                cond.rhs_col = check_column(all_cols, cond.rhs_col);
            }
            TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
            auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
            ColType lhs_type = lhs_col->type;
            ColType rhs_type;
            if (cond.is_rhs_val) {
                cond.rhs_val.init_raw(lhs_col->len);
                rhs_type = cond.rhs_val.type;
                //特殊处理
                if(lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT){
                    cond.rhs_val.float_val = (float)cond.rhs_val.int_val;
                    rhs_type = TYPE_FLOAT;
                }
            } else {
                TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
                auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
                rhs_type = rhs_col->type;
            }
            if (lhs_type != rhs_type) {
            
                throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
            }
        }else{
            //todo:check aggregating
        }
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
