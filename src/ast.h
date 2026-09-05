#ifndef AST_H
#define AST_H

#include <stddef.h>
#include <stdint.h>
#include "lexer.h"   /* LEX_MAX_IDENT */
#include "types.h"   /* Type, FUNC_MAX_PARAMS */

/* ---------- Expressions ---------- */

typedef enum {
    EXPR_INT_LIT,
    EXPR_LONG_LIT,
    EXPR_FLOAT_LIT,
    EXPR_DOUBLE_LIT,
    EXPR_CHAR_LIT,
    EXPR_BOOL_LIT,
    EXPR_STRING_LIT,
    EXPR_NULL_LIT,
    EXPR_VAR,       /* identifier reference                     */
    EXPR_UNARY,     /* NEG / NOT                                */
    EXPR_ADDR,      /* '&' expr  (expr must be an lvalue)        */
    EXPR_DEREF,     /* '*' expr  (expr must have pointer type;
                        DEREF is itself an lvalue)               */
    EXPR_BINARY,
    EXPR_CALL,      /* name '(' args? ')'                        */
    EXPR_INDEX,     /* base '[' index ']'  (base is any expr)    */
    EXPR_MEMBER,    /* base '.' field | base '->' field          */
    EXPR_CAST,      /* '(' type ')' expr                         */
    EXPR_SIZEOF     /* sizeof '(' type ')'  |  sizeof unary       */
} ExprKind;

typedef enum {
    OP_PLUS, OP_MINUS, OP_MUL, OP_DIV, OP_MOD,
    OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE,
    OP_AND, OP_OR
} BinOp;

typedef enum { UOP_NEG, UOP_NOT } UnOp;

typedef struct Expr Expr;

typedef struct { Expr **items; size_t count, capacity; } ExprList;

struct Expr {
    ExprKind kind;
    Type    *type;    /* NULL until typecheck.c annotates it */
    size_t   line, col; /* merged-buffer coordinates (see preprocess.h) */
    union {
        struct { int32_t value; } int_lit;
        struct { int64_t value; } long_lit;
        struct { float value; } float_lit;
        struct { double value; } double_lit;
        struct { int8_t value; } char_lit;
        struct { int value; } bool_lit;               /* 0/1 */
        struct { char *text; size_t len; } string_lit; /* heap copy, owned */
        struct { char name[LEX_MAX_IDENT]; } var;
        struct { UnOp op; Expr *expr; } unary;
        struct { Expr *expr; } addr;
        struct { Expr *expr; } deref;
        struct { BinOp op; Expr *left, *right; } binary;
        struct { char name[LEX_MAX_IDENT]; ExprList args; } call;
        struct { Expr *base; Expr *index; } index;
        struct { Expr *base; char field[LEX_MAX_IDENT]; int is_arrow; } member;
        struct { Type *target; Expr *expr; } cast;
        struct { int is_type; Type *type; Expr *expr; } sizeof_;
    } u;
};

Expr *ast_int(int32_t value, size_t line, size_t col);
Expr *ast_long(int64_t value, size_t line, size_t col);
Expr *ast_float(float value, size_t line, size_t col);
Expr *ast_double(double value, size_t line, size_t col);
Expr *ast_char(int8_t value, size_t line, size_t col);
Expr *ast_bool(int value, size_t line, size_t col);
Expr *ast_string(const char *text, size_t len, size_t line, size_t col);
Expr *ast_null(size_t line, size_t col);
Expr *ast_var(const char *name, size_t line, size_t col);
Expr *ast_unary(UnOp op, Expr *expr, size_t line, size_t col);
Expr *ast_addr(Expr *expr, size_t line, size_t col);
Expr *ast_deref(Expr *expr, size_t line, size_t col);
Expr *ast_binary(BinOp op, Expr *left, Expr *right, size_t line, size_t col);
Expr *ast_call(const char *name, ExprList args, size_t line, size_t col); /* takes ownership of args */
Expr *ast_index(Expr *base, Expr *index, size_t line, size_t col);
Expr *ast_member(Expr *base, const char *field, int is_arrow, size_t line, size_t col);
Expr *ast_cast(Type *target, Expr *expr, size_t line, size_t col);
Expr *ast_sizeof_type(Type *type, size_t line, size_t col);
Expr *ast_sizeof_expr(Expr *expr, size_t line, size_t col);
void  ast_free(Expr *expr);
Expr *ast_expr_clone(const Expr *expr); /* deep copy; used to desugar `x op= e` / `x++` on non-trivial lvalues */

void exprlist_init(ExprList *l);
void exprlist_push(ExprList *l, Expr *e);
void exprlist_free(ExprList *l);

/* ---------- Statements ---------- */

typedef enum {
    STMT_EXPR,      /* expr ';'  (includes bare calls like print(x);)   */
    STMT_VARDECL,   /* type name ('[' N ']')? ('=' expr)? ';'           */
    STMT_ASSIGN,    /* lvalue '=' expr ';'                              */
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_SWITCH,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_RETURN,
    STMT_BLOCK
} StmtKind;

typedef struct Stmt Stmt;

typedef struct { Stmt **items; size_t count, capacity; } StmtList;

typedef struct {
    int     is_default;
    int64_t const_value;   /* meaningful iff !is_default */
    size_t  line, col;
    StmtList body;
} SwitchCase;

typedef struct { SwitchCase *items; size_t count, capacity; } SwitchCaseList;

struct Stmt {
    StmtKind kind;
    size_t   line, col;
    union {
        struct { Expr *expr; } expr;
        struct { Type *type; char name[LEX_MAX_IDENT]; Expr *init; } vardecl;
        struct { Expr *target; Expr *value; } assign;
        struct { Expr *cond; Stmt *then_branch; Stmt *else_branch; } if_stmt;
        struct { Expr *cond; Stmt *body; } while_stmt;
        struct { Stmt *init; Expr *cond; Stmt *update; Stmt *body; } for_stmt;
        struct { Expr *subject; SwitchCaseList cases; } switch_stmt;
        struct { Expr *value; } ret; /* NULL = bare `return;` */
        struct { StmtList list; } block;
    } u;
};

Stmt *stmt_expr(Expr *e, size_t line, size_t col);
Stmt *stmt_vardecl(Type *type, const char *name, Expr *init, size_t line, size_t col);
Stmt *stmt_assign(Expr *target, Expr *value, size_t line, size_t col);
Stmt *stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch, size_t line, size_t col);
Stmt *stmt_while(Expr *cond, Stmt *body, size_t line, size_t col);
Stmt *stmt_for(Stmt *init, Expr *cond, Stmt *update, Stmt *body, size_t line, size_t col);
Stmt *stmt_switch(Expr *subject, SwitchCaseList cases, size_t line, size_t col);
Stmt *stmt_break(size_t line, size_t col);
Stmt *stmt_continue(size_t line, size_t col);
Stmt *stmt_return(Expr *value, size_t line, size_t col);
Stmt *stmt_block(StmtList list, size_t line, size_t col);
void  stmt_free(Stmt *s);

void stmtlist_init(StmtList *l);
void stmtlist_push(StmtList *l, Stmt *s);
void stmtlist_free(StmtList *l);

void switchcaselist_init(SwitchCaseList *l);
void switchcaselist_push(SwitchCaseList *l, SwitchCase c);
void switchcaselist_free(SwitchCaseList *l);

/* ---------- Top level ---------- */

typedef struct {
    Type   *type;
    char    name[LEX_MAX_IDENT];
} Param;

typedef struct {
    Type   *return_type;
    char    name[LEX_MAX_IDENT];
    Param   params[FUNC_MAX_PARAMS];
    size_t  param_count;
    Stmt   *body; /* always a STMT_BLOCK */
    size_t  line, col;
} FuncDecl;

typedef struct { FuncDecl *items; size_t count, capacity; } FuncList;

typedef struct {
    Type   *type;
    char    name[LEX_MAX_IDENT];
    Expr   *init; /* NULL = zero-initialized; must fold to a compile-time constant */
    size_t  line, col;
} GlobalDecl;

typedef struct { GlobalDecl *items; size_t count, capacity; } GlobalList;

typedef struct {
    GlobalList globals;
    FuncList   funcs;
} Program;

void funclist_init(FuncList *l);
void funclist_push(FuncList *l, const FuncDecl *f); /* copies *f */
void funclist_free(FuncList *l);

void globallist_init(GlobalList *l);
void globallist_push(GlobalList *l, const GlobalDecl *g);
void globallist_free(GlobalList *l);

void program_init(Program *p);
void program_free(Program *p);

/* Textual dump of the whole program to stdout, used by --emit-ast. */
void ast_dump_program(const Program *p);

#endif /* AST_H */
