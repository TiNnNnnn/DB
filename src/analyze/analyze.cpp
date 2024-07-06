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
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "portal.h"
#include "analyze/analyze.h"

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
                        if(query->tables.size()>1){
                            if(expr->tab_name != "")col = {expr->tab_name, expr->col_name};
                            else col = {get_tb_name(query->tables,expr->col_name),expr->col_name};
                        }
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

        //处理order by
        if(x->order){
            auto sel_cols = x->order->cols;
            auto dir = x->order->orderby_dir;

            OrderBy_Dir op_dir;

            if(dir == ast::OrderBy_DEFAULT){
                op_dir = OrderBy_Dir::OP_ASC;
            }else if(dir == ast::OrderBy_ASC){
                op_dir = OrderBy_Dir::OP_ASC;
            }else if(dir == ast::OrderBy_DESC){
                op_dir = OrderBy_Dir::OP_DESC;
            }

            for (auto col : sel_cols) {
                TabCol tab_col = {get_tb_name(query->tables,col->col_name),col->col_name};
                check_column(all_cols, tab_col);  // 列元数据校验

                query->order_expr.cols.push_back(tab_col);
            }
            query->order_expr.dir = op_dir;
            
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
    } else if (auto x = std::dynamic_pointer_cast<ast::SetStmt>(parse)){
        if(x->set_knob_type_ == ast::EnableNestLoop){
            g_enable_nestloop = x->bool_val_;
        }else if (x->set_knob_type_ == ast::EnableSortMerge){
            g_enable_sortmerge = x->bool_val_;
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

        //检查cond左右expr,并进行规范化处理
        if(auto l_expr = std::dynamic_pointer_cast<ast::Value>(e->lhs)){
            if(auto r_expr = std::dynamic_pointer_cast<ast::Col>(e->rhs)){
                std::swap(e->lhs,e->rhs);
            }else if(auto r_expr = std::dynamic_pointer_cast<ast::AggregateExpr>(e->rhs)){
                std::swap(e->lhs,e->rhs);
            }
        }else if(auto l_expr = std::dynamic_pointer_cast<ast::Subquery>(e->lhs)){
            if(auto r_expr = std::dynamic_pointer_cast<ast::Col>(e->rhs)){
                std::swap(e->lhs,e->rhs);
            }
        }

        
        if (auto expr = std::dynamic_pointer_cast<ast::Col>(e->lhs)){
            cond.is_lhs_col = true;
            if(tables.size()>1){
                if(expr->tab_name == ""){
                    cond.lhs_col = {get_tb_name(tables,expr->col_name),expr->col_name};
                }else cond.lhs_col = {expr->tab_name, expr->col_name};
            }
            else cond.lhs_col = {get_tb_name(tables,expr->col_name),expr->col_name};

           //特殊处理IN子句
            if(e->op == ast::SvCompOp::SV_OP_IN){
                std::vector<ColMeta> all_cols;
                get_all_cols(tables, all_cols);

                TabCol l_tab_col;
                ColMeta l_col_meta;
                if(auto l_expr = std::dynamic_pointer_cast<ast::Col>(e->lhs)){
                    if(tables.size()>1){
                            if(l_expr->tab_name == "")l_tab_col = {get_tb_name(tables,l_expr->col_name),l_expr->col_name};
                            else l_tab_col = {l_expr->tab_name,l_expr->col_name};
                    }else{
                            l_tab_col = {get_tb_name(tables,l_expr->col_name),l_expr->col_name};
                    }
                    check_column(all_cols,l_tab_col);
                    l_col_meta = *sm_manager_->db_.get_table(l_tab_col.tab_name).get_col(l_tab_col.col_name);
                }else{
                    throw InternalError("inPredicate's left expr must be col");
                }

                //run right sub query
                auto disk_manager = std::make_unique<DiskManager>();
                auto analyze = std::make_unique<Analyze>(sm_manager_);
                auto planner = std::make_unique<Planner>(sm_manager_);
                auto optimizer = std::make_unique<Optimizer>(sm_manager_, planner.get());
                auto lock_manager = std::make_unique<LockManager>();
                auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager_);
                auto log_manager = std::make_unique<LogManager>(disk_manager.get());
                auto ql_manager = std::make_unique<QlManager>(sm_manager_, txn_manager.get(),planner.get());
                auto portal = std::make_unique<Portal>(sm_manager_);
                txn_id_t txn_id = INVALID_TXN_ID;
                Context *context = new Context(lock_manager.get(), log_manager.get(), nullptr, nullptr, 0);

                // 解析器
                auto in_query = std::dynamic_pointer_cast<ast::Subquery>(e->rhs);
                std::shared_ptr<Query> query = analyze->do_analyze(in_query->select_stmt);
                //确保子查询字段数量为1
                if(query->cols.size()>1 || query->a_exprs.size()>1 || (query->cols.size()+query->a_exprs.size()>1)){
                        throw InternalError("Scalar Subquery's return value must be a tuple with one col");
                }
                //检查类型是否匹配
                if(query->cols.size()){
                    assert(query->cols.size() == 1);
                    auto tab_col = query->cols[0];
                    auto col_meta = sm_manager_->db_.get_table(tab_col.tab_name).get_col(tab_col.col_name);
                    if(col_meta->type != l_col_meta.type){
                        throw IncompatibleTypeError(std::to_string(l_col_meta.type),std::to_string(col_meta->type));
                    }
                }
                if(query->a_exprs.size()){
                    assert(query->a_exprs.size() == 1);
                    auto a_expr = query->a_exprs[0];
                    if(a_expr.func_name == "COUNT"){
                        if(l_col_meta.type != TYPE_INT){
                            throw InternalError("type compatible error");
                        }
                    }else{
                        if(l_col_meta.type != TYPE_FLOAT){
                            throw InternalError("type compatible error");
                        }
                    }
                }
                // 优化器
                std::shared_ptr<Plan> plan = optimizer->plan_query(query, context);
                // 执行器
                std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context);
                auto ret = portal->run(portalStmt, ql_manager.get(), &txn_id, context,true);
                portal->drop();

                cond.is_lhs_col = true;
                cond.is_rhs_val = false; //no_used
                cond.op = CompOp::IN;
                            
                for(int i=0;i<ret.size();i++){
                    std::string r_str = ret[i][0];
                    Value r_val;
                    if(l_col_meta.type == TYPE_INT){
                        r_val.set_int(std::atoi(r_str.c_str()));
                    }else if(l_col_meta.type == TYPE_FLOAT){
                        r_val.set_float(std::atof(r_str.c_str()));
                    }else{
                        r_val.set_str(r_str);
                    }
                    cond.rhs_vals.push_back(r_val);
                }
                continue;
            }

            cond.op = convert_sv_comp_op(e->op);
            if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(e->rhs)) {
                //右边是定值
                cond.is_rhs_val = true;
                cond.rhs_val = convert_sv_value(rhs_val);
            } else if (auto expr = std::dynamic_pointer_cast<ast::Col>(e->rhs)) {
                //右边也是列值
                cond.is_rhs_val = false;

                if(tables.size()>1){
                    if(expr->tab_name != ""){
                        cond.rhs_col = {expr->tab_name, expr->col_name};
                    }else cond.rhs_col = {get_tb_name(tables,expr->col_name),expr->col_name};
                }else cond.rhs_col = {get_tb_name(tables,expr->col_name),expr->col_name};

                if(tables.size()==2){
                    if(tables[0] == cond.rhs_col.tab_name){
                        auto temp_col = cond.lhs_col;
                        cond.lhs_col = cond.rhs_col;
                        cond.rhs_col = temp_col;
                    }
                }
            } else if (auto sub_query = std::dynamic_pointer_cast<ast::Subquery>(e->rhs)){
                //右边是标量子查询 （TODO: 子查询可能在左边）
                auto disk_manager = std::make_unique<DiskManager>();
                auto analyze = std::make_unique<Analyze>(sm_manager_);
                auto planner = std::make_unique<Planner>(sm_manager_);
                auto optimizer = std::make_unique<Optimizer>(sm_manager_, planner.get());
                auto lock_manager = std::make_unique<LockManager>();
                auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager_);
                auto log_manager = std::make_unique<LogManager>(disk_manager.get());
                auto ql_manager = std::make_unique<QlManager>(sm_manager_, txn_manager.get(),planner.get());
                auto portal = std::make_unique<Portal>(sm_manager_);
                txn_id_t txn_id = INVALID_TXN_ID;
                Context *context = new Context(lock_manager.get(), log_manager.get(), nullptr, nullptr, 0);

                // 解析器
                std::shared_ptr<Query> query = analyze->do_analyze(sub_query->select_stmt);
                std::vector<ColMeta> all_cols;
                get_all_cols(query->tables, all_cols);

                //取出左值
                TabCol l_tab_col;
                ColMeta l_col_meta;
                if(auto l_expr = std::dynamic_pointer_cast<ast::Col>(e->lhs)){
                    //检查左右类型是否匹配
                    if(query->tables.size()>1){
                        if(l_expr->tab_name != ""){
                             l_tab_col = {l_expr->tab_name,l_expr->col_name};
                        }else  l_tab_col = {get_tb_name(query->tables,l_expr->col_name),l_expr->col_name};
                    }else{
                        l_tab_col = {get_tb_name(query->tables,l_expr->col_name),l_expr->col_name};
                    }
                    check_column(all_cols,l_tab_col);
                    l_col_meta = *sm_manager_->db_.get_table(l_tab_col.tab_name).get_col(l_tab_col.col_name);
                }else{
                    throw InternalError("Scalar Subquery left must be col");
                }

                //保证返回结果为单列
                if(query->cols.size()>1 || query->a_exprs.size()>1 || (query->cols.size()+query->a_exprs.size()>1)){
                    throw InternalError("Scalar Subquery's return value must be a tuple with one col");
                }

                //检验类型是否匹配
                if(query->cols.size()){
                    assert(query->cols.size() == 1);
                    auto tab_col = query->cols[0];
                    auto col_meta = sm_manager_->db_.get_table(tab_col.tab_name).get_col(tab_col.col_name);
                    if(col_meta->type != l_col_meta.type){
                        throw IncompatibleTypeError(std::to_string(l_col_meta.type),std::to_string(col_meta->type));
                    }
                }

                if(query->a_exprs.size()){
                    assert(query->a_exprs.size() == 1);
                    auto a_expr = query->a_exprs[0];
                    if(a_expr.func_name == "COUNT"){
                        if(l_col_meta.type != TYPE_INT){
                            throw InternalError("type compatible error");
                        }
                    }else{
                        if(l_col_meta.type != TYPE_FLOAT){
                            throw InternalError("type compatible error");
                        }
                    }
                }
                // 优化器
                std::shared_ptr<Plan> plan = optimizer->plan_query(query, context);
                // 执行器
                std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context);
                auto ret = portal->run(portalStmt, ql_manager.get(), &txn_id, context,true);
                portal->drop();
                if( ret.size() != 1){
                    throw InternalError("Scalar Subquery's return value must be single value");
                }
                if( ret[0].size() != 1){
                    throw InternalError("Scalar Subquery's return value must be a tuple with one col");
                }
                std::string r_str = ret[0][0];

                Value r_val;
                if(l_col_meta.type == TYPE_INT){
                    r_val.set_int(std::atoi(r_str.c_str()));
                }else if(l_col_meta.type == TYPE_FLOAT){
                    r_val.set_float(std::atof(r_str.c_str()));
                }else{
                    r_val.set_str(r_str);
                }
                cond.is_lhs_col = true;
                cond.is_rhs_val = true;
                cond.lhs_col = l_tab_col;
                cond.rhs_val = r_val;
            }
        //lhs is Aggregate
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
            } else if (auto expr = std::dynamic_pointer_cast<ast::Col>(e->rhs)) {
                // cond.is_rhs_val = false;
                // cond.rhs_col = {expr->tab_name, expr->col_name};
                throw InternalError("aggregation right cond must be value");
            }
        }else{
            throw InternalError("failed to parse cond");
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
