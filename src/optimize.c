#include "optimize.h"

#include <stdlib.h>

/* ============================================================
 * AST constant folding + dead-code elimination
 * ============================================================ */

static int is_const_literal(const Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT: case EXPR_LONG_LIT: case EXPR_FLOAT_LIT:
        case EXPR_DOUBLE_LIT: case EXPR_CHAR_LIT: case EXPR_BOOL_LIT:
            return 1;
        default:
            return 0;
    }
}

typedef struct { int is_float; double f; int64_t i; } NumVal;

static NumVal literal_to_numval(const Expr *e) {
    NumVal v; v.is_float = 0; v.f = 0.0; v.i = 0;
    switch (e->kind) {
        case EXPR_INT_LIT: v.i = e->u.int_lit.value; break;
        case EXPR_LONG_LIT: v.i = e->u.long_lit.value; break;
        case EXPR_CHAR_LIT: v.i = e->u.char_lit.value; break;
        case EXPR_BOOL_LIT: v.i = e->u.bool_lit.value; break;
        case EXPR_FLOAT_LIT: v.is_float = 1; v.f = e->u.float_lit.value; break;
        case EXPR_DOUBLE_LIT: v.is_float = 1; v.f = e->u.double_lit.value; break;
        default: break;
    }
    return v;
}

static Expr *numval_to_literal(NumVal v, ExprKind preferred_kind, size_t line, size_t col) {
    switch (preferred_kind) {
        case EXPR_LONG_LIT: return ast_long(v.is_float ? (int64_t)v.f : v.i, line, col);
        case EXPR_FLOAT_LIT: return ast_float((float)(v.is_float ? v.f : (double)v.i), line, col);
        case EXPR_DOUBLE_LIT: return ast_double(v.is_float ? v.f : (double)v.i, line, col);
        case EXPR_CHAR_LIT: return ast_char((int8_t)(v.is_float ? (int64_t)v.f : v.i), line, col);
        case EXPR_BOOL_LIT: return ast_bool(v.is_float ? (v.f != 0.0) : (v.i != 0), line, col);
        default: return ast_int((int32_t)(v.is_float ? (int64_t)v.f : v.i), line, col);
    }
}

static ExprKind wider_literal_kind(ExprKind a, ExprKind b) {
    if (a == EXPR_DOUBLE_LIT || b == EXPR_DOUBLE_LIT) return EXPR_DOUBLE_LIT;
    if (a == EXPR_FLOAT_LIT || b == EXPR_FLOAT_LIT) return EXPR_FLOAT_LIT;
    if (a == EXPR_LONG_LIT || b == EXPR_LONG_LIT) return EXPR_LONG_LIT;
    return EXPR_INT_LIT;
}

static Expr *optimize_expr(Expr *e);

static Expr *fold_unary(Expr *e) {
    e->u.unary.expr = optimize_expr(e->u.unary.expr);
    Expr *inner = e->u.unary.expr;
    if (!is_const_literal(inner)) return e;
    NumVal v = literal_to_numval(inner);
    Type *saved_type = e->type;
    Expr *folded;
    if (e->u.unary.op == UOP_NEG) {
        if (v.is_float) v.f = -v.f; else v.i = -v.i;
        folded = numval_to_literal(v, inner->kind, e->line, e->col);
    } else {
        int truthy = v.is_float ? (v.f != 0.0) : (v.i != 0);
        folded = ast_bool(!truthy, e->line, e->col);
    }
    folded->type = saved_type;
    ast_free(e);
    return folded;
}

static Expr *fold_binary(Expr *e) {
    e->u.binary.left = optimize_expr(e->u.binary.left);
    e->u.binary.right = optimize_expr(e->u.binary.right);
    Expr *l = e->u.binary.left, *r = e->u.binary.right;
    if (!is_const_literal(l) || !is_const_literal(r)) return e;

    NumVal a = literal_to_numval(l), b = literal_to_numval(r);
    int is_float = a.is_float || b.is_float;
    double af = a.is_float ? a.f : (double)a.i;
    double bf = b.is_float ? b.f : (double)b.i;
    Type *saved_type = e->type;
    Expr *folded = NULL;

    switch (e->u.binary.op) {
        case OP_PLUS: case OP_MINUS: case OP_MUL: case OP_DIV: case OP_MOD: {
            if ((e->u.binary.op == OP_DIV || e->u.binary.op == OP_MOD) && !is_float && b.i == 0) {
                return e; /* leave division/modulo by zero for the VM to report */
            }
            NumVal v; v.is_float = is_float; v.f = 0.0; v.i = 0;
            switch (e->u.binary.op) {
                case OP_PLUS:  if (is_float) v.f = af + bf; else v.i = a.i + b.i; break;
                case OP_MINUS: if (is_float) v.f = af - bf; else v.i = a.i - b.i; break;
                case OP_MUL:   if (is_float) v.f = af * bf; else v.i = a.i * b.i; break;
                case OP_DIV:   if (is_float) v.f = af / bf; else v.i = a.i / b.i; break;
                case OP_MOD:   v.i = a.i % b.i; break;
                default: break;
            }
            folded = numval_to_literal(v, wider_literal_kind(l->kind, r->kind), e->line, e->col);
            break;
        }
        case OP_LT: folded = ast_bool(is_float ? af < bf  : a.i < b.i,  e->line, e->col); break;
        case OP_GT: folded = ast_bool(is_float ? af > bf  : a.i > b.i,  e->line, e->col); break;
        case OP_LE: folded = ast_bool(is_float ? af <= bf : a.i <= b.i, e->line, e->col); break;
        case OP_GE: folded = ast_bool(is_float ? af >= bf : a.i >= b.i, e->line, e->col); break;
        case OP_EQ: folded = ast_bool(is_float ? af == bf : a.i == b.i, e->line, e->col); break;
        case OP_NE: folded = ast_bool(is_float ? af != bf : a.i != b.i, e->line, e->col); break;
        case OP_AND: folded = ast_bool((is_float ? af != 0.0 : a.i != 0) && (is_float ? bf != 0.0 : b.i != 0), e->line, e->col); break;
        case OP_OR:  folded = ast_bool((is_float ? af != 0.0 : a.i != 0) || (is_float ? bf != 0.0 : b.i != 0), e->line, e->col); break;
    }
    if (!folded) return e;
    folded->type = saved_type;
    ast_free(e);
    return folded;
}

static Expr *optimize_expr(Expr *e) {
    if (!e) return e;
    switch (e->kind) {
        case EXPR_UNARY: return fold_unary(e);
        case EXPR_BINARY: return fold_binary(e);
        case EXPR_ADDR: e->u.addr.expr = optimize_expr(e->u.addr.expr); return e;
        case EXPR_DEREF: e->u.deref.expr = optimize_expr(e->u.deref.expr); return e;
        case EXPR_CALL:
            for (size_t i = 0; i < e->u.call.args.count; i++) {
                e->u.call.args.items[i] = optimize_expr(e->u.call.args.items[i]);
            }
            return e;
        case EXPR_INDEX:
            e->u.index.base = optimize_expr(e->u.index.base);
            e->u.index.index = optimize_expr(e->u.index.index);
            return e;
        case EXPR_MEMBER:
            e->u.member.base = optimize_expr(e->u.member.base);
            return e;
        case EXPR_CAST:
            e->u.cast.expr = optimize_expr(e->u.cast.expr);
            return e;
        case EXPR_SIZEOF:
            if (!e->u.sizeof_.is_type) e->u.sizeof_.expr = optimize_expr(e->u.sizeof_.expr);
            return e;
        default:
            return e;
    }
}

static void optimize_stmtlist(StmtList *list);

static void optimize_stmt(Stmt *s) {
    switch (s->kind) {
        case STMT_EXPR:
            s->u.expr.expr = optimize_expr(s->u.expr.expr);
            break;
        case STMT_VARDECL:
            if (s->u.vardecl.init) s->u.vardecl.init = optimize_expr(s->u.vardecl.init);
            break;
        case STMT_ASSIGN:
            s->u.assign.target = optimize_expr(s->u.assign.target);
            s->u.assign.value = optimize_expr(s->u.assign.value);
            break;
        case STMT_IF:
            s->u.if_stmt.cond = optimize_expr(s->u.if_stmt.cond);
            optimize_stmt(s->u.if_stmt.then_branch);
            if (s->u.if_stmt.else_branch) optimize_stmt(s->u.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            s->u.while_stmt.cond = optimize_expr(s->u.while_stmt.cond);
            optimize_stmt(s->u.while_stmt.body);
            break;
        case STMT_FOR:
            if (s->u.for_stmt.init) optimize_stmt(s->u.for_stmt.init);
            if (s->u.for_stmt.cond) s->u.for_stmt.cond = optimize_expr(s->u.for_stmt.cond);
            if (s->u.for_stmt.update) optimize_stmt(s->u.for_stmt.update);
            optimize_stmt(s->u.for_stmt.body);
            break;
        case STMT_SWITCH:
            s->u.switch_stmt.subject = optimize_expr(s->u.switch_stmt.subject);
            for (size_t i = 0; i < s->u.switch_stmt.cases.count; i++) {
                optimize_stmtlist(&s->u.switch_stmt.cases.items[i].body);
            }
            break;
        case STMT_RETURN:
            if (s->u.ret.value) s->u.ret.value = optimize_expr(s->u.ret.value);
            break;
        case STMT_BLOCK:
            optimize_stmtlist(&s->u.block.list);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
    }
}

static void optimize_stmtlist(StmtList *list) {
    size_t new_count = 0;
    int terminated = 0;
    for (size_t i = 0; i < list->count; i++) {
        Stmt *s = list->items[i];
        if (terminated) {
            stmt_free(s);
            continue;
        }
        optimize_stmt(s);
        list->items[new_count++] = s;
        if (s->kind == STMT_RETURN || s->kind == STMT_BREAK || s->kind == STMT_CONTINUE) terminated = 1;
    }
    list->count = new_count;
}

void optimize_ast(Program *prog) {
    for (size_t i = 0; i < prog->globals.count; i++) {
        if (prog->globals.items[i].init) prog->globals.items[i].init = optimize_expr(prog->globals.items[i].init);
    }
    for (size_t i = 0; i < prog->funcs.count; i++) {
        optimize_stmt(prog->funcs.items[i].body);
    }
}

/* ============================================================
 * Bytecode peephole: jump-to-jump collapsing
 * ============================================================ */

void optimize_bytecode(Bytecode *bc) {
    size_t ip = 0;
    while (ip < bc->count) {
        OpCode op = (OpCode)bc->data[ip];
        if (op == BC_JMP || op == BC_JZ) {
            int32_t target = bc->data[ip + 1];
            int guard = 0;
            while (target >= 0 && (size_t)target < bc->count &&
                   (OpCode)bc->data[target] == BC_JMP && guard < 64) {
                int32_t next_target = bc->data[target + 1];
                if (next_target == target) break;
                target = next_target;
                guard++;
            }
            bc->data[ip + 1] = target;
        }
        ip += 1 + (size_t)bc_opcode_operand_count(op);
    }
}
