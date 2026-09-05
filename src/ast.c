#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Expr *new_expr(ExprKind kind, size_t line, size_t col) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) { exit(EXIT_FAILURE); }
    e->kind = kind;
    e->type = NULL;
    e->line = line;
    e->col = col;
    return e;
}

Expr *ast_int(int32_t value, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_INT_LIT, line, col);
    e->u.int_lit.value = value;
    return e;
}
Expr *ast_long(int64_t value, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_LONG_LIT, line, col);
    e->u.long_lit.value = value;
    return e;
}
Expr *ast_float(float value, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_FLOAT_LIT, line, col);
    e->u.float_lit.value = value;
    return e;
}
Expr *ast_double(double value, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_DOUBLE_LIT, line, col);
    e->u.double_lit.value = value;
    return e;
}
Expr *ast_char(int8_t value, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_CHAR_LIT, line, col);
    e->u.char_lit.value = value;
    return e;
}
Expr *ast_bool(int value, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_BOOL_LIT, line, col);
    e->u.bool_lit.value = value ? 1 : 0;
    return e;
}
Expr *ast_string(const char *text, size_t len, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_STRING_LIT, line, col);
    char *copy = (char *)malloc(len + 1);
    if (!copy) exit(EXIT_FAILURE);
    memcpy(copy, text, len);
    copy[len] = '\0';
    e->u.string_lit.text = copy;
    e->u.string_lit.len = len;
    return e;
}
Expr *ast_null(size_t line, size_t col) {
    return new_expr(EXPR_NULL_LIT, line, col);
}
Expr *ast_var(const char *name, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_VAR, line, col);
    copy_bounded(e->u.var.name, name, LEX_MAX_IDENT);
    return e;
}
Expr *ast_unary(UnOp op, Expr *expr, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_UNARY, line, col);
    e->u.unary.op = op;
    e->u.unary.expr = expr;
    return e;
}
Expr *ast_addr(Expr *expr, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_ADDR, line, col);
    e->u.addr.expr = expr;
    return e;
}
Expr *ast_deref(Expr *expr, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_DEREF, line, col);
    e->u.deref.expr = expr;
    return e;
}
Expr *ast_binary(BinOp op, Expr *left, Expr *right, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_BINARY, line, col);
    e->u.binary.op = op;
    e->u.binary.left = left;
    e->u.binary.right = right;
    return e;
}
Expr *ast_call(const char *name, ExprList args, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_CALL, line, col);
    copy_bounded(e->u.call.name, name, LEX_MAX_IDENT);
    e->u.call.args = args;
    return e;
}
Expr *ast_index(Expr *base, Expr *index, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_INDEX, line, col);
    e->u.index.base = base;
    e->u.index.index = index;
    return e;
}
Expr *ast_member(Expr *base, const char *field, int is_arrow, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_MEMBER, line, col);
    e->u.member.base = base;
    copy_bounded(e->u.member.field, field, LEX_MAX_IDENT);
    e->u.member.is_arrow = is_arrow;
    return e;
}
Expr *ast_cast(Type *target, Expr *expr, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_CAST, line, col);
    e->u.cast.target = target;
    e->u.cast.expr = expr;
    return e;
}
Expr *ast_sizeof_type(Type *type, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_SIZEOF, line, col);
    e->u.sizeof_.is_type = 1;
    e->u.sizeof_.type = type;
    e->u.sizeof_.expr = NULL;
    return e;
}
Expr *ast_sizeof_expr(Expr *expr, size_t line, size_t col) {
    Expr *e = new_expr(EXPR_SIZEOF, line, col);
    e->u.sizeof_.is_type = 0;
    e->u.sizeof_.type = NULL;
    e->u.sizeof_.expr = expr;
    return e;
}

void ast_free(Expr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case EXPR_STRING_LIT:
            free(expr->u.string_lit.text);
            break;
        case EXPR_UNARY:
            ast_free(expr->u.unary.expr);
            break;
        case EXPR_ADDR:
            ast_free(expr->u.addr.expr);
            break;
        case EXPR_DEREF:
            ast_free(expr->u.deref.expr);
            break;
        case EXPR_BINARY:
            ast_free(expr->u.binary.left);
            ast_free(expr->u.binary.right);
            break;
        case EXPR_CALL:
            exprlist_free(&expr->u.call.args);
            break;
        case EXPR_INDEX:
            ast_free(expr->u.index.base);
            ast_free(expr->u.index.index);
            break;
        case EXPR_MEMBER:
            ast_free(expr->u.member.base);
            break;
        case EXPR_CAST:
            ast_free(expr->u.cast.expr);
            break;
        case EXPR_SIZEOF:
            if (!expr->u.sizeof_.is_type) ast_free(expr->u.sizeof_.expr);
            break;
        default:
            break;
    }
    free(expr);
}

Expr *ast_expr_clone(const Expr *expr) {
    if (!expr) return NULL;
    Expr *e = new_expr(expr->kind, expr->line, expr->col);
    e->type = expr->type;
    switch (expr->kind) {
        case EXPR_INT_LIT: e->u.int_lit = expr->u.int_lit; break;
        case EXPR_LONG_LIT: e->u.long_lit = expr->u.long_lit; break;
        case EXPR_FLOAT_LIT: e->u.float_lit = expr->u.float_lit; break;
        case EXPR_DOUBLE_LIT: e->u.double_lit = expr->u.double_lit; break;
        case EXPR_CHAR_LIT: e->u.char_lit = expr->u.char_lit; break;
        case EXPR_BOOL_LIT: e->u.bool_lit = expr->u.bool_lit; break;
        case EXPR_STRING_LIT: {
            char *copy = (char *)malloc(expr->u.string_lit.len + 1);
            if (!copy) exit(EXIT_FAILURE);
            memcpy(copy, expr->u.string_lit.text, expr->u.string_lit.len + 1);
            e->u.string_lit.text = copy;
            e->u.string_lit.len = expr->u.string_lit.len;
            break;
        }
        case EXPR_NULL_LIT: break;
        case EXPR_VAR: e->u.var = expr->u.var; break;
        case EXPR_UNARY:
            e->u.unary.op = expr->u.unary.op;
            e->u.unary.expr = ast_expr_clone(expr->u.unary.expr);
            break;
        case EXPR_ADDR: e->u.addr.expr = ast_expr_clone(expr->u.addr.expr); break;
        case EXPR_DEREF: e->u.deref.expr = ast_expr_clone(expr->u.deref.expr); break;
        case EXPR_BINARY:
            e->u.binary.op = expr->u.binary.op;
            e->u.binary.left = ast_expr_clone(expr->u.binary.left);
            e->u.binary.right = ast_expr_clone(expr->u.binary.right);
            break;
        case EXPR_CALL:
            strcpy(e->u.call.name, expr->u.call.name);
            exprlist_init(&e->u.call.args);
            for (size_t i = 0; i < expr->u.call.args.count; i++) {
                exprlist_push(&e->u.call.args, ast_expr_clone(expr->u.call.args.items[i]));
            }
            break;
        case EXPR_INDEX:
            e->u.index.base = ast_expr_clone(expr->u.index.base);
            e->u.index.index = ast_expr_clone(expr->u.index.index);
            break;
        case EXPR_MEMBER:
            e->u.member.base = ast_expr_clone(expr->u.member.base);
            strcpy(e->u.member.field, expr->u.member.field);
            e->u.member.is_arrow = expr->u.member.is_arrow;
            break;
        case EXPR_CAST:
            e->u.cast.target = expr->u.cast.target;
            e->u.cast.expr = ast_expr_clone(expr->u.cast.expr);
            break;
        case EXPR_SIZEOF:
            e->u.sizeof_.is_type = expr->u.sizeof_.is_type;
            e->u.sizeof_.type = expr->u.sizeof_.type;
            e->u.sizeof_.expr = ast_expr_clone(expr->u.sizeof_.expr);
            break;
    }
    return e;
}

void exprlist_init(ExprList *l) { l->items = NULL; l->count = 0; l->capacity = 0; }

void exprlist_push(ExprList *l, Expr *e) {
    if (l->count >= l->capacity) {
        size_t new_cap = l->capacity == 0 ? 4 : l->capacity * 2;
        l->items = (Expr **)realloc(l->items, new_cap * sizeof(Expr *));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = new_cap;
    }
    l->items[l->count++] = e;
}

void exprlist_free(ExprList *l) {
    for (size_t i = 0; i < l->count; i++) ast_free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
}

/* ---------- statements ---------- */

static Stmt *new_stmt(StmtKind kind, size_t line, size_t col) {
    Stmt *s = (Stmt *)calloc(1, sizeof(Stmt));
    if (!s) exit(EXIT_FAILURE);
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

Stmt *stmt_expr(Expr *e, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_EXPR, line, col);
    s->u.expr.expr = e;
    return s;
}
Stmt *stmt_vardecl(Type *type, const char *name, Expr *init, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_VARDECL, line, col);
    s->u.vardecl.type = type;
    copy_bounded(s->u.vardecl.name, name, LEX_MAX_IDENT);
    s->u.vardecl.init = init;
    return s;
}
Stmt *stmt_assign(Expr *target, Expr *value, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_ASSIGN, line, col);
    s->u.assign.target = target;
    s->u.assign.value = value;
    return s;
}
Stmt *stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_IF, line, col);
    s->u.if_stmt.cond = cond;
    s->u.if_stmt.then_branch = then_branch;
    s->u.if_stmt.else_branch = else_branch;
    return s;
}
Stmt *stmt_while(Expr *cond, Stmt *body, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_WHILE, line, col);
    s->u.while_stmt.cond = cond;
    s->u.while_stmt.body = body;
    return s;
}
Stmt *stmt_for(Stmt *init, Expr *cond, Stmt *update, Stmt *body, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_FOR, line, col);
    s->u.for_stmt.init = init;
    s->u.for_stmt.cond = cond;
    s->u.for_stmt.update = update;
    s->u.for_stmt.body = body;
    return s;
}
Stmt *stmt_switch(Expr *subject, SwitchCaseList cases, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_SWITCH, line, col);
    s->u.switch_stmt.subject = subject;
    s->u.switch_stmt.cases = cases;
    return s;
}
Stmt *stmt_break(size_t line, size_t col) { return new_stmt(STMT_BREAK, line, col); }
Stmt *stmt_continue(size_t line, size_t col) { return new_stmt(STMT_CONTINUE, line, col); }
Stmt *stmt_return(Expr *value, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_RETURN, line, col);
    s->u.ret.value = value;
    return s;
}
Stmt *stmt_block(StmtList list, size_t line, size_t col) {
    Stmt *s = new_stmt(STMT_BLOCK, line, col);
    s->u.block.list = list;
    return s;
}

void stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_EXPR: ast_free(s->u.expr.expr); break;
        case STMT_VARDECL: ast_free(s->u.vardecl.init); break;
        case STMT_ASSIGN:
            ast_free(s->u.assign.target);
            ast_free(s->u.assign.value);
            break;
        case STMT_IF:
            ast_free(s->u.if_stmt.cond);
            stmt_free(s->u.if_stmt.then_branch);
            stmt_free(s->u.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            ast_free(s->u.while_stmt.cond);
            stmt_free(s->u.while_stmt.body);
            break;
        case STMT_FOR:
            stmt_free(s->u.for_stmt.init);
            ast_free(s->u.for_stmt.cond);
            stmt_free(s->u.for_stmt.update);
            stmt_free(s->u.for_stmt.body);
            break;
        case STMT_SWITCH:
            ast_free(s->u.switch_stmt.subject);
            switchcaselist_free(&s->u.switch_stmt.cases);
            break;
        case STMT_RETURN:
            ast_free(s->u.ret.value);
            break;
        case STMT_BLOCK:
            stmtlist_free(&s->u.block.list);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
    }
    free(s);
}

void stmtlist_init(StmtList *l) { l->items = NULL; l->count = 0; l->capacity = 0; }

void stmtlist_push(StmtList *l, Stmt *s) {
    if (!s) return;
    if (l->count >= l->capacity) {
        size_t new_cap = l->capacity == 0 ? 4 : l->capacity * 2;
        l->items = (Stmt **)realloc(l->items, new_cap * sizeof(Stmt *));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = new_cap;
    }
    l->items[l->count++] = s;
}

void stmtlist_free(StmtList *l) {
    for (size_t i = 0; i < l->count; i++) stmt_free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
}

void switchcaselist_init(SwitchCaseList *l) { l->items = NULL; l->count = 0; l->capacity = 0; }

void switchcaselist_push(SwitchCaseList *l, SwitchCase c) {
    if (l->count >= l->capacity) {
        size_t new_cap = l->capacity == 0 ? 4 : l->capacity * 2;
        l->items = (SwitchCase *)realloc(l->items, new_cap * sizeof(SwitchCase));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = new_cap;
    }
    l->items[l->count++] = c;
}

void switchcaselist_free(SwitchCaseList *l) {
    for (size_t i = 0; i < l->count; i++) stmtlist_free(&l->items[i].body);
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
}

/* ---------- top level ---------- */

void funclist_init(FuncList *l) { l->items = NULL; l->count = 0; l->capacity = 0; }

void funclist_push(FuncList *l, const FuncDecl *f) {
    if (l->count >= l->capacity) {
        size_t new_cap = l->capacity == 0 ? 4 : l->capacity * 2;
        l->items = (FuncDecl *)realloc(l->items, new_cap * sizeof(FuncDecl));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = new_cap;
    }
    l->items[l->count++] = *f;
}

void funclist_free(FuncList *l) {
    for (size_t i = 0; i < l->count; i++) stmt_free(l->items[i].body);
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
}

void globallist_init(GlobalList *l) { l->items = NULL; l->count = 0; l->capacity = 0; }

void globallist_push(GlobalList *l, const GlobalDecl *g) {
    if (l->count >= l->capacity) {
        size_t new_cap = l->capacity == 0 ? 4 : l->capacity * 2;
        l->items = (GlobalDecl *)realloc(l->items, new_cap * sizeof(GlobalDecl));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = new_cap;
    }
    l->items[l->count++] = *g;
}

void globallist_free(GlobalList *l) {
    for (size_t i = 0; i < l->count; i++) ast_free(l->items[i].init);
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
}

void program_init(Program *p) {
    globallist_init(&p->globals);
    funclist_init(&p->funcs);
}

void program_free(Program *p) {
    globallist_free(&p->globals);
    funclist_free(&p->funcs);
}

/* ---------- textual dump (--emit-ast) ---------- */

static void indent(int n) { for (int i = 0; i < n; i++) printf("  "); }

static const char *binop_str(BinOp op) {
    switch (op) {
        case OP_PLUS: return "+"; case OP_MINUS: return "-"; case OP_MUL: return "*";
        case OP_DIV: return "/"; case OP_MOD: return "%";
        case OP_LT: return "<"; case OP_GT: return ">"; case OP_LE: return "<="; case OP_GE: return ">=";
        case OP_EQ: return "=="; case OP_NE: return "!=";
        case OP_AND: return "&&"; case OP_OR: return "||";
    }
    return "?";
}

static void dump_expr(const Expr *e, int d) {
    if (!e) { indent(d); printf("<null>\n"); return; }
    indent(d);
    switch (e->kind) {
        case EXPR_INT_LIT: printf("Int %d\n", e->u.int_lit.value); break;
        case EXPR_LONG_LIT: printf("Long %lld\n", (long long)e->u.long_lit.value); break;
        case EXPR_FLOAT_LIT: printf("Float %g\n", (double)e->u.float_lit.value); break;
        case EXPR_DOUBLE_LIT: printf("Double %g\n", e->u.double_lit.value); break;
        case EXPR_CHAR_LIT: printf("Char %d\n", e->u.char_lit.value); break;
        case EXPR_BOOL_LIT: printf("Bool %s\n", e->u.bool_lit.value ? "true" : "false"); break;
        case EXPR_STRING_LIT: printf("String \"%s\"\n", e->u.string_lit.text); break;
        case EXPR_NULL_LIT: printf("Null\n"); break;
        case EXPR_VAR: printf("Var %s\n", e->u.var.name); break;
        case EXPR_UNARY:
            printf("Unary %s\n", e->u.unary.op == UOP_NEG ? "-" : "!");
            dump_expr(e->u.unary.expr, d + 1);
            break;
        case EXPR_ADDR:
            printf("Addr\n");
            dump_expr(e->u.addr.expr, d + 1);
            break;
        case EXPR_DEREF:
            printf("Deref\n");
            dump_expr(e->u.deref.expr, d + 1);
            break;
        case EXPR_BINARY:
            printf("Binary %s\n", binop_str(e->u.binary.op));
            dump_expr(e->u.binary.left, d + 1);
            dump_expr(e->u.binary.right, d + 1);
            break;
        case EXPR_CALL:
            printf("Call %s\n", e->u.call.name);
            for (size_t i = 0; i < e->u.call.args.count; i++) dump_expr(e->u.call.args.items[i], d + 1);
            break;
        case EXPR_INDEX:
            printf("Index\n");
            dump_expr(e->u.index.base, d + 1);
            dump_expr(e->u.index.index, d + 1);
            break;
        case EXPR_MEMBER:
            printf("Member %s%s\n", e->u.member.is_arrow ? "->" : ".", e->u.member.field);
            dump_expr(e->u.member.base, d + 1);
            break;
        case EXPR_CAST:
            printf("Cast %s\n", type_name_str(e->u.cast.target));
            dump_expr(e->u.cast.expr, d + 1);
            break;
        case EXPR_SIZEOF:
            if (e->u.sizeof_.is_type) printf("Sizeof(type %s)\n", type_name_str(e->u.sizeof_.type));
            else { printf("Sizeof(expr)\n"); dump_expr(e->u.sizeof_.expr, d + 1); }
            break;
    }
}

static void dump_stmtlist(const StmtList *l, int d);

static void dump_stmt(const Stmt *s, int d) {
    if (!s) { indent(d); printf("<null>\n"); return; }
    indent(d);
    switch (s->kind) {
        case STMT_EXPR:
            printf("ExprStmt\n");
            dump_expr(s->u.expr.expr, d + 1);
            break;
        case STMT_VARDECL:
            printf("VarDecl %s : %s\n", s->u.vardecl.name, type_name_str(s->u.vardecl.type));
            if (s->u.vardecl.init) dump_expr(s->u.vardecl.init, d + 1);
            break;
        case STMT_ASSIGN:
            printf("Assign\n");
            dump_expr(s->u.assign.target, d + 1);
            dump_expr(s->u.assign.value, d + 1);
            break;
        case STMT_IF:
            printf("If\n");
            dump_expr(s->u.if_stmt.cond, d + 1);
            dump_stmt(s->u.if_stmt.then_branch, d + 1);
            if (s->u.if_stmt.else_branch) dump_stmt(s->u.if_stmt.else_branch, d + 1);
            break;
        case STMT_WHILE:
            printf("While\n");
            dump_expr(s->u.while_stmt.cond, d + 1);
            dump_stmt(s->u.while_stmt.body, d + 1);
            break;
        case STMT_FOR:
            printf("For\n");
            if (s->u.for_stmt.init) dump_stmt(s->u.for_stmt.init, d + 1);
            if (s->u.for_stmt.cond) dump_expr(s->u.for_stmt.cond, d + 1);
            if (s->u.for_stmt.update) dump_stmt(s->u.for_stmt.update, d + 1);
            dump_stmt(s->u.for_stmt.body, d + 1);
            break;
        case STMT_SWITCH:
            printf("Switch\n");
            dump_expr(s->u.switch_stmt.subject, d + 1);
            for (size_t i = 0; i < s->u.switch_stmt.cases.count; i++) {
                const SwitchCase *c = &s->u.switch_stmt.cases.items[i];
                indent(d + 1);
                if (c->is_default) printf("Default:\n"); else printf("Case %lld:\n", (long long)c->const_value);
                dump_stmtlist(&c->body, d + 2);
            }
            break;
        case STMT_BREAK: printf("Break\n"); break;
        case STMT_CONTINUE: printf("Continue\n"); break;
        case STMT_RETURN:
            printf("Return\n");
            if (s->u.ret.value) dump_expr(s->u.ret.value, d + 1);
            break;
        case STMT_BLOCK:
            printf("Block\n");
            dump_stmtlist(&s->u.block.list, d + 1);
            break;
    }
}

static void dump_stmtlist(const StmtList *l, int d) {
    for (size_t i = 0; i < l->count; i++) dump_stmt(l->items[i], d);
}

void ast_dump_program(const Program *p) {
    for (size_t i = 0; i < p->globals.count; i++) {
        const GlobalDecl *g = &p->globals.items[i];
        printf("Global %s : %s\n", g->name, type_name_str(g->type));
        if (g->init) dump_expr(g->init, 1);
    }
    for (size_t i = 0; i < p->funcs.count; i++) {
        const FuncDecl *f = &p->funcs.items[i];
        printf("Func %s(", f->name);
        for (size_t j = 0; j < f->param_count; j++) {
            printf("%s%s : %s", j ? ", " : "", f->params[j].name, type_name_str(f->params[j].type));
        }
        printf(") : %s\n", type_name_str(f->return_type));
        dump_stmt(f->body, 1);
    }
}
