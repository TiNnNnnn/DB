%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc);

void yyerror(YYLTYPE *locp, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY WHERE UPDATE SET SELECT INT CHAR FLOAT INDEX AND JOIN EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ENABLE_NESTLOOP ENABLE_SORTMERGE
// non-keywords
%token IN 
%token AS
%token LEQ NEQ GEQ T_EOF
%token COUNT SUM AVG MIN MAX 
%token GROUP
%token HAVING

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING OP_IN  
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr selector_item
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName
%type <sv_strs> tableList colNameList
%type <sv_col> col col_with_alias
%type <sv_cols> colList 
%type <sv_exprs> selector
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause
%type <sv_orderby> order_clause opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_group_by> group_clause opt_group_clause
%type <sv_having> opt_having_clause
%type <sv_aggregate_expr> aggregate_expr
//%type <sv_aggregate_exprs> aggregate_exprs
%type <sv_setKnobType> set_knob_type
%type <sv_str> opt_as_alias
%type <sv_subquery> subquery

%%
start:
        stmt ';'
    {
        parse_tree = $1;
        YYACCEPT;
    }
    | HELP
    {
        parse_tree = std::make_shared<Help>();
        YYACCEPT;
    }
    | EXIT
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    | T_EOF
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    | ddl
    | dml
    | txnStmt
    | setStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    | TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    | TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    | SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    | DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    | DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    | CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    | DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    | DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    | UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    | SELECT selector FROM tableList optWhereClause opt_group_clause opt_order_clause
    {
        $$ = std::make_shared<SelectStmt>($2, $4, $5, $6, $7);
    }
    ;

subquery:
    '(' SELECT selector FROM tableList optWhereClause opt_group_clause opt_order_clause ')'
    {
        $$ = std::make_shared<Subquery>(
            std::make_shared<SelectStmt>($3, $5, $6, $7, $8)
        );
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    | fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    | CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    | FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    | valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    | VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    | VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    | VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    ;

condition:
      expr op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    | aggregate_expr op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    | expr op aggregate_expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    | expr op subquery
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    | subquery op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    | expr IN subquery
    {
        $$ = std::make_shared<BinaryExpr>($1,SvCompOp::SV_OP_IN,$3);
    }
    ;

optWhereClause:
        /* epsilon */ { /* ignore*/ }
    | WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition 
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    | whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    | colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    ;

colList:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    | colList ',' col
    {
        $$.push_back($3);
    }
    ;
col_with_alias:
        col AS IDENTIFIER
    {
        $$ = std::make_shared<Col>($1->tab_name, $1->col_name, $3);
    }
    | col
    {
        $$ = $1;
    }
    ;
/*    
colList_with_alias:
        col_with_alias
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    | colList_with_alias ',' col_with_alias
    {
        $$.push_back($3);
    }
    ;
*/


op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    | '<'
    {
        $$ = SV_OP_LT;
    }
    | '>'
    {
        $$ = SV_OP_GT;
    }
    | NEQ
    {
        $$ = SV_OP_NE;
    }
    | LEQ
    {
        $$ = SV_OP_LE;
    }
    | GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
    value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    | col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

opt_as_alias:
    AS IDENTIFIER       
    { 
        $$ = $2; 
    }
    | /* epsilon */ { /* ignore*/ }
    ;

aggregate_expr:
       COUNT '(' '*' ')' opt_as_alias
    {
        $$ = std::make_shared<AggregateExpr>("COUNT", std::make_shared<StarExpr>(), $5);
    }
    | COUNT '(' expr ')' opt_as_alias
    {
        $$ = std::make_shared<AggregateExpr>("COUNT", $3, $5);
    }
    | SUM '(' expr ')' opt_as_alias
    {
        $$ = std::make_shared<AggregateExpr>("SUM", $3, $5);
    }
    | AVG '(' expr ')' opt_as_alias
    {
        $$ = std::make_shared<AggregateExpr>("AVG", $3, $5);
    }
    | MIN '(' expr ')' opt_as_alias
    {
        $$ = std::make_shared<AggregateExpr>("MIN", $3, $5);
    }
    | MAX '(' expr ')' opt_as_alias
    {
        $$ = std::make_shared<AggregateExpr>("MAX", $3, $5);
    }
    ;
 /*   
aggregate_exprs:
        aggregate_expr
    {
        $$ = std::vector<std::shared_ptr<AggregateExpr>>{$1};
    }
    | aggregate_exprs ',' aggregate_expr
    {
        $$.push_back($3);
    }
    ;
*/
setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    | setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    ;

selector:
        '*'
    {
        $$ = std::vector<std::shared_ptr<ast::Expr>>{};
    }
    | selector_item
    {
        $$ = std::vector<std::shared_ptr<ast::Expr>>{$1};
    }
    | selector ',' selector_item
    {
        $$ = $1;
        $$.push_back($3);
    }
    ;

selector_item:
    col_with_alias
    {
        $$ = $1;
    }
    | aggregate_expr
    {
        $$ = $1;
    }
    ;

tableList:
        tbName
    {
        $$ = std::vector<std::string>{$1};
    }
    | tableList ',' tbName
    {
        $$.push_back($3);
    }
    | tableList JOIN tbName
    {
        $$.push_back($3);
    }
    ;

/* order by */
opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = $3; 
    }
    | /* epsilon */ { /* ignore*/ }
    ;

order_clause:
    colList opt_asc_desc 
    { 
        $$ = std::make_shared<OrderBy>($1, $2);
    }
    ;   

opt_asc_desc:
    ASC { $$ = OrderBy_ASC; }
    | DESC { $$ = OrderBy_DESC; }
    | { $$ = OrderBy_DEFAULT; }
    ;    


/*----------- group by ------------*/
opt_group_clause:
    GROUP BY group_clause
    {
        $$ = $3;
    }
    | /* epsilon */ { /* ignore*/ }
    ;

group_clause:
    colList opt_having_clause
    {
        $$ = std::make_shared<GroupBy>($1, $2);
    }
    ;

opt_having_clause:
    HAVING whereClause 
    {
        $$ = std::make_shared<Having>($2);
    }
    | /* epsilon */ 
    {
        $$ = nullptr;
    }
    ;

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    | ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
