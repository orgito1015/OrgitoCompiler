#include "typecheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"

/* ---------- symbol tables ---------- */

typedef struct {
    char name[LEX_MAX_IDENT];
    Type *type;
    int used;
    size_t line, col;
} VarBinding;

typedef struct { VarBinding *items; size_t count, capacity; } VarBindingList;

typedef struct { VarBindingList *scopes; size_t count, capacity; } ScopeStack;

typedef struct {
    char name[LEX_MAX_IDENT];
    Type *return_type;
    Type *param_types[FUNC_MAX_PARAMS];
    size_t param_count;
    int is_native;
    int is_variadic;
} FuncSig;

typedef struct { FuncSig *items; size_t count, capacity; } FuncSigList;

typedef struct {
    FuncSigList funcs;
    VarBindingList globals;
    ScopeStack scopes;
    Type *current_return_type;
    DiagList *diags;
    const SourceMap *srcmap;
} TypeEnv;

static void varlist_push(VarBindingList *l, VarBinding v) {
    if (l->count >= l->capacity) {
        size_t cap = l->capacity == 0 ? 8 : l->capacity * 2;
        l->items = (VarBinding *)realloc(l->items, cap * sizeof(VarBinding));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = cap;
    }
    l->items[l->count++] = v;
}

static VarBinding *varlist_find(VarBindingList *l, const char *name) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0) return &l->items[i];
    }
    return NULL;
}

static void funclist_sig_push(FuncSigList *l, FuncSig f) {
    if (l->count >= l->capacity) {
        size_t cap = l->capacity == 0 ? 8 : l->capacity * 2;
        l->items = (FuncSig *)realloc(l->items, cap * sizeof(FuncSig));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = cap;
    }
    l->items[l->count++] = f;
}

static FuncSig *funclist_sig_find(FuncSigList *l, const char *name) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0) return &l->items[i];
    }
    return NULL;
}

static void diagf(TypeEnv *env, DiagSeverity sev, size_t line, size_t col, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *file;
    size_t orig_line;
    sourcemap_resolve(env->srcmap, line, &file, &orig_line);
    diag_addf(env->diags, sev, file, orig_line, col, "%s", buf);
}

static void push_scope(TypeEnv *env) {
    if (env->scopes.count >= env->scopes.capacity) {
        size_t cap = env->scopes.capacity == 0 ? 8 : env->scopes.capacity * 2;
        env->scopes.scopes = (VarBindingList *)realloc(env->scopes.scopes, cap * sizeof(VarBindingList));
        if (!env->scopes.scopes) exit(EXIT_FAILURE);
        env->scopes.capacity = cap;
    }
    VarBindingList *s = &env->scopes.scopes[env->scopes.count++];
    s->items = NULL;
    s->count = 0;
    s->capacity = 0;
}

static void pop_scope(TypeEnv *env) {
    VarBindingList *s = &env->scopes.scopes[--env->scopes.count];
    for (size_t i = 0; i < s->count; i++) {
        if (!s->items[i].used) {
            diagf(env, DIAG_WARNING, s->items[i].line, s->items[i].col,
                  "unused variable '%s'", s->items[i].name);
        }
    }
    free(s->items);
}

/* Declares `name` in the innermost scope; reports (and returns 0) if it
 * already exists in that exact scope (shadowing an outer scope is fine). */
static int declare_local(TypeEnv *env, const char *name, Type *type, size_t line, size_t col) {
    VarBindingList *s = &env->scopes.scopes[env->scopes.count - 1];
    if (varlist_find(s, name)) {
        diagf(env, DIAG_ERROR, line, col, "redeclaration of '%s' in the same scope", name);
        return 0;
    }
    VarBinding v;
    memset(&v, 0, sizeof(v));
    copy_bounded(v.name, name, LEX_MAX_IDENT);
    v.type = type;
    v.used = 0;
    v.line = line;
    v.col = col;
    varlist_push(s, v);
    return 1;
}

static VarBinding *lookup_var(TypeEnv *env, const char *name) {
    for (size_t i = env->scopes.count; i > 0; i--) {
        VarBinding *v = varlist_find(&env->scopes.scopes[i - 1], name);
        if (v) return v;
    }
    return varlist_find(&env->globals, name);
}

/* ---------- lvalue check ---------- */

static int is_lvalue(const Expr *e) {
    switch (e->kind) {
        case EXPR_VAR: return 1;
        case EXPR_DEREF: return 1;
        case EXPR_INDEX: return 1;
        case EXPR_MEMBER: return e->u.member.is_arrow ? 1 : is_lvalue(e->u.member.base);
        default: return 0;
    }
}

/* ---------- numeric conversions ---------- */

static int ptr_like(const Type *t) { return type_is_pointer(t) || t->kind == TY_ARRAY; }
static Type *decay_to_ptr(Type *t) { return type_is_pointer(t) ? t : type_pointer_to(t->base); }

static Type *usual_arith(Type *a, Type *b) {
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) return type_double();
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) return type_float();
    if (a->kind == TY_LONG || b->kind == TY_LONG) return type_long();
    return type_int();
}

/* ---------- expressions ---------- */

static Type *typecheck_expr(TypeEnv *env, Expr *e);

static void check_not_void(TypeEnv *env, const Type *t, size_t line, size_t col) {
    if (t->kind == TY_VOID) {
        diagf(env, DIAG_ERROR, line, col, "expression has type 'void' and cannot be used as a value");
    }
}

static Type *typecheck_expr(TypeEnv *env, Expr *e) {
    Type *result = type_int();
    switch (e->kind) {
        case EXPR_INT_LIT:    result = type_int(); break;
        case EXPR_LONG_LIT:   result = type_long(); break;
        case EXPR_FLOAT_LIT:  result = type_float(); break;
        case EXPR_DOUBLE_LIT: result = type_double(); break;
        case EXPR_CHAR_LIT:   result = type_char(); break;
        case EXPR_BOOL_LIT:   result = type_bool(); break;
        case EXPR_STRING_LIT: result = type_pointer_to(type_char()); break;
        case EXPR_NULL_LIT:   result = type_pointer_to(type_void()); break;

        case EXPR_VAR: {
            VarBinding *v = lookup_var(env, e->u.var.name);
            if (!v) {
                diagf(env, DIAG_ERROR, e->line, e->col, "use of undeclared identifier '%s'", e->u.var.name);
                result = type_int();
            } else {
                v->used = 1;
                result = v->type;
            }
            break;
        }

        case EXPR_UNARY: {
            Type *t = typecheck_expr(env, e->u.unary.expr);
            if (e->u.unary.op == UOP_NEG) {
                if (!type_is_numeric(t)) {
                    diagf(env, DIAG_ERROR, e->line, e->col, "invalid operand of type '%s' to unary '-'", type_name_str(t));
                    result = type_int();
                } else {
                    result = (t->kind == TY_BOOL || t->kind == TY_CHAR) ? type_int() : t;
                }
            } else {
                if (!type_is_numeric(t) && !type_is_pointer(t)) {
                    diagf(env, DIAG_ERROR, e->line, e->col, "invalid operand of type '%s' to '!'", type_name_str(t));
                }
                result = type_bool();
            }
            break;
        }

        case EXPR_ADDR: {
            Type *t = typecheck_expr(env, e->u.addr.expr);
            if (!is_lvalue(e->u.addr.expr)) {
                diagf(env, DIAG_ERROR, e->line, e->col, "cannot take the address of a non-lvalue expression");
                result = type_pointer_to(type_void());
            } else if (t->kind == TY_ARRAY) {
                result = type_pointer_to(t->base);
            } else {
                result = type_pointer_to(t);
            }
            break;
        }

        case EXPR_DEREF: {
            Type *t = typecheck_expr(env, e->u.deref.expr);
            if (!type_is_pointer(t)) {
                diagf(env, DIAG_ERROR, e->line, e->col, "cannot dereference non-pointer type '%s'", type_name_str(t));
                result = type_int();
            } else if (t->base->kind == TY_VOID) {
                diagf(env, DIAG_ERROR, e->line, e->col, "cannot dereference 'void*'; cast to a concrete pointer type first");
                result = type_int();
            } else {
                result = t->base;
            }
            break;
        }

        case EXPR_BINARY: {
            Type *lt = typecheck_expr(env, e->u.binary.left);
            Type *rt = typecheck_expr(env, e->u.binary.right);
            check_not_void(env, lt, e->line, e->col);
            check_not_void(env, rt, e->line, e->col);
            BinOp op = e->u.binary.op;
            switch (op) {
                case OP_AND: case OP_OR:
                    if ((!type_is_numeric(lt) && !type_is_pointer(lt)) ||
                        (!type_is_numeric(rt) && !type_is_pointer(rt))) {
                        diagf(env, DIAG_ERROR, e->line, e->col, "invalid operands to logical operator");
                    }
                    result = type_bool();
                    break;
                case OP_EQ: case OP_NE:
                    if (type_is_numeric(lt) && type_is_numeric(rt)) { /* ok */ }
                    else if (type_is_pointer(lt) && type_is_pointer(rt)) { /* ok */ }
                    else {
                        diagf(env, DIAG_ERROR, e->line, e->col,
                              "cannot compare '%s' and '%s'", type_name_str(lt), type_name_str(rt));
                    }
                    result = type_bool();
                    break;
                case OP_LT: case OP_GT: case OP_LE: case OP_GE:
                    if (type_is_numeric(lt) && type_is_numeric(rt)) { /* ok */ }
                    else if (type_is_pointer(lt) && type_is_pointer(rt)) { /* ok */ }
                    else {
                        diagf(env, DIAG_ERROR, e->line, e->col,
                              "invalid operands of type '%s' and '%s' to relational operator",
                              type_name_str(lt), type_name_str(rt));
                    }
                    result = type_bool();
                    break;
                case OP_PLUS:
                    if (type_is_numeric(lt) && type_is_numeric(rt)) {
                        result = usual_arith(lt, rt);
                    } else if (ptr_like(lt) && type_is_integer(rt)) {
                        result = decay_to_ptr(lt);
                    } else if (type_is_integer(lt) && ptr_like(rt)) {
                        result = decay_to_ptr(rt);
                    } else {
                        diagf(env, DIAG_ERROR, e->line, e->col,
                              "invalid operands of type '%s' and '%s' to '+'", type_name_str(lt), type_name_str(rt));
                        result = type_int();
                    }
                    break;
                case OP_MINUS:
                    if (type_is_numeric(lt) && type_is_numeric(rt)) {
                        result = usual_arith(lt, rt);
                    } else if (ptr_like(lt) && type_is_integer(rt)) {
                        result = decay_to_ptr(lt);
                    } else if (ptr_like(lt) && ptr_like(rt)) {
                        result = type_long();
                    } else {
                        diagf(env, DIAG_ERROR, e->line, e->col,
                              "invalid operands of type '%s' and '%s' to '-'", type_name_str(lt), type_name_str(rt));
                        result = type_int();
                    }
                    break;
                case OP_MUL: case OP_DIV: case OP_MOD:
                    if (type_is_numeric(lt) && type_is_numeric(rt)) {
                        result = usual_arith(lt, rt);
                    } else {
                        diagf(env, DIAG_ERROR, e->line, e->col,
                              "invalid operands of type '%s' and '%s' to arithmetic operator",
                              type_name_str(lt), type_name_str(rt));
                        result = type_int();
                    }
                    break;
            }
            break;
        }

        case EXPR_CALL: {
            FuncSig *sig = funclist_sig_find(&env->funcs, e->u.call.name);
            if (!sig) {
                diagf(env, DIAG_ERROR, e->line, e->col, "call to undeclared function '%s'", e->u.call.name);
                for (size_t i = 0; i < e->u.call.args.count; i++) typecheck_expr(env, e->u.call.args.items[i]);
                result = type_int();
                break;
            }
            if (sig->is_variadic) {
                for (size_t i = 0; i < e->u.call.args.count; i++) {
                    Expr *arg = e->u.call.args.items[i];
                    Type *at = typecheck_expr(env, arg);
                    check_not_void(env, at, arg->line, arg->col);
                    if (at->kind == TY_STRUCT) {
                        diagf(env, DIAG_ERROR, arg->line, arg->col,
                              "cannot pass a value of type '%s' to '%s'", type_name_str(at), e->u.call.name);
                    }
                }
                if (e->u.call.args.count == 0) {
                    diagf(env, DIAG_ERROR, e->line, e->col, "'%s' requires at least one argument", e->u.call.name);
                }
            } else {
                if (e->u.call.args.count != sig->param_count) {
                    diagf(env, DIAG_ERROR, e->line, e->col,
                          "'%s' expects %zu argument%s, got %zu",
                          e->u.call.name, sig->param_count, sig->param_count == 1 ? "" : "s", e->u.call.args.count);
                }
                size_t n = e->u.call.args.count < sig->param_count ? e->u.call.args.count : sig->param_count;
                for (size_t i = 0; i < n; i++) {
                    Type *at = typecheck_expr(env, e->u.call.args.items[i]);
                    if (!type_assignable(sig->param_types[i], at)) {
                        diagf(env, DIAG_ERROR, e->u.call.args.items[i]->line, e->u.call.args.items[i]->col,
                              "argument %zu of '%s' has type '%s', expected '%s'",
                              i + 1, e->u.call.name, type_name_str(at), type_name_str(sig->param_types[i]));
                    }
                }
                for (size_t i = n; i < e->u.call.args.count; i++) typecheck_expr(env, e->u.call.args.items[i]);
            }
            result = sig->return_type;
            break;
        }

        case EXPR_INDEX: {
            Type *bt = typecheck_expr(env, e->u.index.base);
            Type *it = typecheck_expr(env, e->u.index.index);
            if (!type_is_integer(it)) {
                diagf(env, DIAG_ERROR, e->u.index.index->line, e->u.index.index->col,
                      "array subscript must be an integer, got '%s'", type_name_str(it));
            }
            if (bt->kind == TY_ARRAY || bt->kind == TY_POINTER) {
                result = bt->base;
            } else {
                diagf(env, DIAG_ERROR, e->line, e->col, "subscripted value of type '%s' is not an array or pointer", type_name_str(bt));
                result = type_int();
            }
            break;
        }

        case EXPR_MEMBER: {
            Type *bt = typecheck_expr(env, e->u.member.base);
            StructDef *sdef = NULL;
            if (e->u.member.is_arrow) {
                if (bt->kind == TY_POINTER && bt->base->kind == TY_STRUCT) {
                    sdef = bt->base->sdef;
                } else if (bt->kind == TY_STRUCT) {
                    diagf(env, DIAG_ERROR, e->line, e->col, "use '.' to access a member of a non-pointer struct");
                    sdef = bt->sdef;
                } else {
                    diagf(env, DIAG_ERROR, e->line, e->col, "'->' used on non-struct-pointer type '%s'", type_name_str(bt));
                }
            } else {
                if (bt->kind == TY_STRUCT) {
                    sdef = bt->sdef;
                } else if (bt->kind == TY_POINTER && bt->base->kind == TY_STRUCT) {
                    diagf(env, DIAG_ERROR, e->line, e->col, "use '->' to access a member through a pointer");
                    sdef = bt->base->sdef;
                } else {
                    diagf(env, DIAG_ERROR, e->line, e->col, "'.' used on non-struct type '%s'", type_name_str(bt));
                }
            }
            if (sdef) {
                const StructField *f = struct_find_field(sdef, e->u.member.field);
                if (!f) {
                    diagf(env, DIAG_ERROR, e->line, e->col, "struct '%s' has no member '%s'", sdef->name, e->u.member.field);
                    result = type_int();
                } else {
                    result = f->type;
                }
            } else {
                result = type_int();
            }
            break;
        }

        case EXPR_CAST: {
            Type *st = typecheck_expr(env, e->u.cast.expr);
            if (!type_castable(e->u.cast.target, st)) {
                diagf(env, DIAG_ERROR, e->line, e->col, "invalid cast from '%s' to '%s'",
                      type_name_str(st), type_name_str(e->u.cast.target));
            }
            result = e->u.cast.target;
            break;
        }

        case EXPR_SIZEOF: {
            if (!e->u.sizeof_.is_type) {
                typecheck_expr(env, e->u.sizeof_.expr);
            }
            result = type_long();
            break;
        }
    }
    e->type = result;
    return result;
}

/* ---------- statements ---------- */

static void typecheck_stmtlist(TypeEnv *env, StmtList *list);

static int stmtlist_always_returns(const StmtList *l);

/* Does entering this case body (whether via its own label or by falling
 * through from the previous case) always end in a return? An empty body,
 * or one that ends without return/break/continue, falls through to the
 * next case (real switch semantics) and inherits its outcome; a body
 * ending in `break` definitely does NOT return (it exits the switch). */
static int case_entry_always_returns(const StmtList *body, int next_case_returns) {
    if (body->count == 0) return next_case_returns;
    if (stmtlist_always_returns(body)) return 1;
    const Stmt *last = body->items[body->count - 1];
    if (last->kind == STMT_BREAK) return 0;
    return next_case_returns;
}

static int stmt_always_returns(const Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
        case STMT_RETURN: return 1;
        case STMT_BLOCK: return stmtlist_always_returns(&s->u.block.list);
        case STMT_IF:
            return s->u.if_stmt.else_branch != NULL &&
                   stmt_always_returns(s->u.if_stmt.then_branch) &&
                   stmt_always_returns(s->u.if_stmt.else_branch);
        case STMT_SWITCH: {
            size_t n = s->u.switch_stmt.cases.count;
            if (n == 0) return 0;
            int has_default = 0;
            for (size_t i = 0; i < n; i++) {
                if (s->u.switch_stmt.cases.items[i].is_default) has_default = 1;
            }
            if (!has_default) return 0;
            int next_returns = 0; /* falling off the last case exits the switch without returning */
            int all_ok = 1;
            for (size_t i = n; i-- > 0;) {
                int this_returns = case_entry_always_returns(&s->u.switch_stmt.cases.items[i].body, next_returns);
                if (!this_returns) all_ok = 0;
                next_returns = this_returns;
            }
            return all_ok;
        }
        default:
            return 0;
    }
}

static int stmtlist_always_returns(const StmtList *l) {
    for (size_t i = 0; i < l->count; i++) {
        if (stmt_always_returns(l->items[i])) return 1;
    }
    return 0;
}

static void typecheck_stmt(TypeEnv *env, Stmt *s) {
    switch (s->kind) {
        case STMT_EXPR: {
            Type *t = typecheck_expr(env, s->u.expr.expr);
            (void)t;
            break;
        }
        case STMT_VARDECL: {
            if (s->u.vardecl.type->kind == TY_VOID) {
                diagf(env, DIAG_ERROR, s->line, s->col, "variable '%s' declared with incomplete type 'void'", s->u.vardecl.name);
            }
            if (s->u.vardecl.init) {
                if (s->u.vardecl.type->kind == TY_ARRAY) {
                    diagf(env, DIAG_ERROR, s->line, s->col, "'%s' declarations cannot have an initializer", type_name_str(s->u.vardecl.type));
                }
                Type *it = typecheck_expr(env, s->u.vardecl.init);
                if (!type_assignable(s->u.vardecl.type, it)) {
                    diagf(env, DIAG_ERROR, s->line, s->col, "cannot initialize '%s' with a value of type '%s'",
                          type_name_str(s->u.vardecl.type), type_name_str(it));
                }
            }
            declare_local(env, s->u.vardecl.name, s->u.vardecl.type, s->line, s->col);
            break;
        }
        case STMT_ASSIGN: {
            Type *tt = typecheck_expr(env, s->u.assign.target);
            Type *vt = typecheck_expr(env, s->u.assign.value);
            if (!is_lvalue(s->u.assign.target)) {
                diagf(env, DIAG_ERROR, s->line, s->col, "left-hand side of assignment is not assignable");
            } else if (!type_assignable(tt, vt)) {
                diagf(env, DIAG_ERROR, s->line, s->col, "cannot assign a value of type '%s' to '%s'",
                      type_name_str(vt), type_name_str(tt));
            }
            break;
        }
        case STMT_IF: {
            Type *ct = typecheck_expr(env, s->u.if_stmt.cond);
            if (ct->kind == TY_STRUCT || ct->kind == TY_ARRAY || ct->kind == TY_VOID) {
                diagf(env, DIAG_ERROR, s->line, s->col, "value of type '%s' used where a condition is expected", type_name_str(ct));
            }
            typecheck_stmt(env, s->u.if_stmt.then_branch);
            if (s->u.if_stmt.else_branch) typecheck_stmt(env, s->u.if_stmt.else_branch);
            break;
        }
        case STMT_WHILE: {
            Type *ct = typecheck_expr(env, s->u.while_stmt.cond);
            if (ct->kind == TY_STRUCT || ct->kind == TY_ARRAY || ct->kind == TY_VOID) {
                diagf(env, DIAG_ERROR, s->line, s->col, "value of type '%s' used where a condition is expected", type_name_str(ct));
            }
            typecheck_stmt(env, s->u.while_stmt.body);
            break;
        }
        case STMT_FOR: {
            push_scope(env);
            if (s->u.for_stmt.init) typecheck_stmt(env, s->u.for_stmt.init);
            if (s->u.for_stmt.cond) {
                Type *ct = typecheck_expr(env, s->u.for_stmt.cond);
                if (ct->kind == TY_STRUCT || ct->kind == TY_ARRAY || ct->kind == TY_VOID) {
                    diagf(env, DIAG_ERROR, s->line, s->col, "value of type '%s' used where a condition is expected", type_name_str(ct));
                }
            }
            if (s->u.for_stmt.update) typecheck_stmt(env, s->u.for_stmt.update);
            typecheck_stmt(env, s->u.for_stmt.body);
            pop_scope(env);
            break;
        }
        case STMT_SWITCH: {
            Type *st = typecheck_expr(env, s->u.switch_stmt.subject);
            if (!type_is_integer(st)) {
                diagf(env, DIAG_ERROR, s->line, s->col, "switch subject must be an integer type, got '%s'", type_name_str(st));
            }
            int seen_default = 0;
            for (size_t i = 0; i < s->u.switch_stmt.cases.count; i++) {
                SwitchCase *c = &s->u.switch_stmt.cases.items[i];
                if (c->is_default) {
                    if (seen_default) {
                        diagf(env, DIAG_ERROR, c->line, c->col, "multiple 'default' labels in one switch");
                    }
                    seen_default = 1;
                } else {
                    for (size_t j = 0; j < i; j++) {
                        SwitchCase *other = &s->u.switch_stmt.cases.items[j];
                        if (!other->is_default && other->const_value == c->const_value) {
                            diagf(env, DIAG_ERROR, c->line, c->col, "duplicate case value %lld", (long long)c->const_value);
                        }
                    }
                }
                push_scope(env);
                typecheck_stmtlist(env, &c->body);
                pop_scope(env);
            }
            break;
        }
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_RETURN: {
            if (s->u.ret.value) {
                Type *vt = typecheck_expr(env, s->u.ret.value);
                if (env->current_return_type->kind == TY_VOID) {
                    diagf(env, DIAG_ERROR, s->line, s->col, "void function should not return a value");
                } else if (!type_assignable(env->current_return_type, vt)) {
                    diagf(env, DIAG_ERROR, s->line, s->col, "cannot return a value of type '%s' from a function returning '%s'",
                          type_name_str(vt), type_name_str(env->current_return_type));
                }
            } else {
                if (env->current_return_type->kind != TY_VOID) {
                    diagf(env, DIAG_ERROR, s->line, s->col, "non-void function must return a value");
                }
            }
            break;
        }
        case STMT_BLOCK:
            push_scope(env);
            typecheck_stmtlist(env, &s->u.block.list);
            pop_scope(env);
            break;
    }
}

static void typecheck_stmtlist(TypeEnv *env, StmtList *list) {
    int terminated = 0;
    int warned = 0;
    for (size_t i = 0; i < list->count; i++) {
        Stmt *s = list->items[i];
        if (terminated && !warned) {
            diagf(env, DIAG_WARNING, s->line, s->col, "unreachable code");
            warned = 1;
        }
        typecheck_stmt(env, s);
        if (s->kind == STMT_RETURN || s->kind == STMT_BREAK || s->kind == STMT_CONTINUE) {
            terminated = 1;
        }
    }
}

/* ---------- top level ---------- */

static void register_natives(TypeEnv *env) {
    for (int i = 0; i < NATIVE_COUNT; i++) {
        FuncSig sig;
        memset(&sig, 0, sizeof(sig));
        copy_bounded(sig.name, NATIVE_SIGS[i].name, LEX_MAX_IDENT);
        sig.is_native = 1;
        switch ((NativeId)i) {
            case NATIVE_PRINT:
                sig.is_variadic = 1;
                sig.return_type = type_void();
                break;
            case NATIVE_MALLOC:
                sig.param_types[0] = type_long();
                sig.param_count = 1;
                sig.return_type = type_pointer_to(type_void());
                break;
            case NATIVE_FREE:
                sig.param_types[0] = type_pointer_to(type_void());
                sig.param_count = 1;
                sig.return_type = type_void();
                break;
            case NATIVE_STRLEN:
                sig.param_types[0] = type_pointer_to(type_char());
                sig.param_count = 1;
                sig.return_type = type_int();
                break;
            case NATIVE_STRCMP:
                sig.param_types[0] = type_pointer_to(type_char());
                sig.param_types[1] = type_pointer_to(type_char());
                sig.param_count = 2;
                sig.return_type = type_int();
                break;
            case NATIVE_STRCPY:
                sig.param_types[0] = type_pointer_to(type_char());
                sig.param_types[1] = type_pointer_to(type_char());
                sig.param_count = 2;
                sig.return_type = type_pointer_to(type_char());
                break;
            case NATIVE_COUNT:
                break;
        }
        funclist_sig_push(&env->funcs, sig);
    }
}

void typecheck_program(Program *prog, DiagList *diags, const SourceMap *srcmap) {
    TypeEnv env;
    memset(&env, 0, sizeof(env));
    env.diags = diags;
    env.srcmap = srcmap;

    register_natives(&env);

    for (size_t i = 0; i < prog->funcs.count; i++) {
        FuncDecl *f = &prog->funcs.items[i];
        if (funclist_sig_find(&env.funcs, f->name)) {
            diagf(&env, DIAG_ERROR, f->line, f->col, "redefinition of function '%s'", f->name);
            continue;
        }
        FuncSig sig;
        memset(&sig, 0, sizeof(sig));
        copy_bounded(sig.name, f->name, LEX_MAX_IDENT);
        sig.return_type = f->return_type;
        sig.param_count = f->param_count;
        for (size_t j = 0; j < f->param_count; j++) sig.param_types[j] = f->params[j].type;
        funclist_sig_push(&env.funcs, sig);
    }
    FuncSig *main_sig = funclist_sig_find(&env.funcs, "main");
    if (!main_sig) {
        diagf(&env, DIAG_ERROR, 0, 0, "program has no 'int main()' entry point");
    } else if (main_sig->return_type->kind == TY_STRUCT) {
        diagf(&env, DIAG_ERROR, 0, 0, "'main' must return a scalar type, not a struct");
    }

    for (size_t i = 0; i < prog->globals.count; i++) {
        GlobalDecl *g = &prog->globals.items[i];
        if (varlist_find(&env.globals, g->name) || funclist_sig_find(&env.funcs, g->name)) {
            diagf(&env, DIAG_ERROR, g->line, g->col, "redefinition of '%s'", g->name);
            continue;
        }
        if (g->type->kind == TY_VOID) {
            diagf(&env, DIAG_ERROR, g->line, g->col, "global '%s' declared with incomplete type 'void'", g->name);
        }
        VarBinding v;
        memset(&v, 0, sizeof(v));
        copy_bounded(v.name, g->name, LEX_MAX_IDENT);
        v.type = g->type;
        v.used = 1; /* globals don't need an "unused" warning */
        v.line = g->line;
        v.col = g->col;
        varlist_push(&env.globals, v);
    }
    for (size_t i = 0; i < prog->globals.count; i++) {
        GlobalDecl *g = &prog->globals.items[i];
        if (!g->init) continue;
        if (g->type->kind == TY_ARRAY || g->type->kind == TY_STRUCT) {
            diagf(&env, DIAG_ERROR, g->line, g->col, "'%s' declarations cannot have an initializer", type_name_str(g->type));
            continue;
        }
        Type *it = typecheck_expr(&env, g->init);
        if (!type_assignable(g->type, it)) {
            diagf(&env, DIAG_ERROR, g->line, g->col, "cannot initialize '%s' with a value of type '%s'",
                  type_name_str(g->type), type_name_str(it));
        }
    }

    for (size_t i = 0; i < prog->funcs.count; i++) {
        FuncDecl *f = &prog->funcs.items[i];
        env.current_return_type = f->return_type;
        push_scope(&env);
        for (size_t j = 0; j < f->param_count; j++) {
            if (f->params[j].type->kind == TY_VOID) {
                diagf(&env, DIAG_ERROR, f->line, f->col, "parameter '%s' of '%s' has incomplete type 'void'",
                      f->params[j].name, f->name);
                continue;
            }
            declare_local(&env, f->params[j].name, f->params[j].type, f->line, f->col);
            /* parameters are considered used even if never referenced */
            VarBinding *b = varlist_find(&env.scopes.scopes[env.scopes.count - 1], f->params[j].name);
            if (b) b->used = 1;
        }
        typecheck_stmtlist(&env, &f->body->u.block.list);
        if (f->return_type->kind != TY_VOID && !stmtlist_always_returns(&f->body->u.block.list)) {
            diagf(&env, DIAG_WARNING, f->line, f->col, "control reaches end of non-void function '%s'", f->name);
        }
        pop_scope(&env);
    }

    for (size_t i = 0; i < env.scopes.count; i++) free(env.scopes.scopes[i].items);
    free(env.scopes.scopes);
    free(env.globals.items);
    free(env.funcs.items);
}
