#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- small helpers ---------- */

static const Token *cur(Parser *p) { return &p->lex->current_token; }
static int check(Parser *p, TokenKind k) { return cur(p)->kind == k; }
static void advance_tok(Parser *p) { lexer_next(p->lex); }

static int match(Parser *p, TokenKind k) {
    if (check(p, k)) { advance_tok(p); return 1; }
    return 0;
}

static void perror_at(Parser *p, size_t line, size_t col, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *file;
    size_t orig_line;
    sourcemap_resolve(p->srcmap, line, &file, &orig_line);
    diag_addf(p->diags, DIAG_ERROR, file, orig_line, col, "%s", buf);
}

static int expect_tok(Parser *p, TokenKind k) {
    if (check(p, k)) { advance_tok(p); return 1; }
    perror_at(p, cur(p)->line, cur(p)->col, "expected %s, got %s",
              token_kind_name(k), token_kind_name(cur(p)->kind));
    return 0;
}

static int expect_ident(Parser *p, char *out, size_t out_size) {
    if (check(p, TOK_IDENT)) {
        strncpy(out, cur(p)->text, out_size - 1);
        out[out_size - 1] = '\0';
        advance_tok(p);
        return 1;
    }
    perror_at(p, cur(p)->line, cur(p)->col, "expected an identifier, got %s", token_kind_name(cur(p)->kind));
    out[0] = '\0';
    return 0;
}

static int starts_type(TokenKind k) {
    switch (k) {
        case TOK_KW_VOID: case TOK_KW_BOOL: case TOK_KW_CHAR: case TOK_KW_INT:
        case TOK_KW_LONG: case TOK_KW_FLOAT: case TOK_KW_DOUBLE: case TOK_KW_STRUCT:
            return 1;
        default:
            return 0;
    }
}

/* Skips tokens until a ';' (consumed) or '}'/EOF (not consumed) - used to
 * recover after a malformed statement so the rest of the file still gets
 * checked in the same run. */
static void sync_to_stmt_boundary(Parser *p) {
    while (!check(p, TOK_SEMI) && !check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        advance_tok(p);
    }
    if (check(p, TOK_SEMI)) advance_tok(p);
}

/* Skips tokens until one that could plausibly start a new top-level
 * declaration (every top_decl begins with a type keyword or 'struct'). */
static void sync_to_toplevel(Parser *p) {
    advance_tok(p);
    while (!check(p, TOK_EOF) && !starts_type(cur(p)->kind)) advance_tok(p);
}

/* Minimal constant-expression evaluator for array sizes and case labels
 * (which need a concrete value immediately, to build a Type* or compare
 * against at parse time - unlike ordinary runtime expressions, which are
 * left as AST for typecheck/codegen/optimize). Supports literals, the
 * arithmetic operators, unary negation/not, and sizeof(type). */
static int eval_const_expr(const Expr *e, int64_t *out) {
    switch (e->kind) {
        case EXPR_INT_LIT:  *out = e->u.int_lit.value; return 1;
        case EXPR_LONG_LIT: *out = e->u.long_lit.value; return 1;
        case EXPR_CHAR_LIT: *out = e->u.char_lit.value; return 1;
        case EXPR_BOOL_LIT: *out = e->u.bool_lit.value; return 1;
        case EXPR_UNARY: {
            int64_t v;
            if (!eval_const_expr(e->u.unary.expr, &v)) return 0;
            *out = (e->u.unary.op == UOP_NEG) ? -v : (v == 0);
            return 1;
        }
        case EXPR_BINARY: {
            int64_t a, b;
            if (!eval_const_expr(e->u.binary.left, &a)) return 0;
            if (!eval_const_expr(e->u.binary.right, &b)) return 0;
            switch (e->u.binary.op) {
                case OP_PLUS:  *out = a + b; return 1;
                case OP_MINUS: *out = a - b; return 1;
                case OP_MUL:   *out = a * b; return 1;
                case OP_DIV:   if (b == 0) return 0; *out = a / b; return 1;
                case OP_MOD:   if (b == 0) return 0; *out = a % b; return 1;
                default: return 0;
            }
        }
        case EXPR_SIZEOF:
            if (e->u.sizeof_.is_type) { *out = e->u.sizeof_.type->size; return 1; }
            return 0;
        default:
            return 0;
    }
}

static Expr *parse_expr(Parser *p);

static int parse_const_int(Parser *p, int64_t *out, const char *what) {
    size_t line = cur(p)->line, col = cur(p)->col;
    Expr *e = parse_expr(p);
    if (!e) return 0;
    int ok = eval_const_expr(e, out);
    ast_free(e);
    if (!ok) {
        perror_at(p, line, col, "%s must be a compile-time constant expression", what);
        return 0;
    }
    return 1;
}

static int parse_const_array_len(Parser *p, uint32_t *out) {
    size_t line = cur(p)->line, col = cur(p)->col;
    int64_t v;
    if (!parse_const_int(p, &v, "array size")) return 0;
    if (v <= 0) {
        perror_at(p, line, col, "array size must be greater than 0 (got %lld)", (long long)v);
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* ---------- forward declarations ---------- */

static Expr *parse_logic_or(Parser *p);
static Expr *parse_logic_and(Parser *p);
static Expr *parse_equality(Parser *p);
static Expr *parse_relational(Parser *p);
static Expr *parse_additive(Parser *p);
static Expr *parse_term(Parser *p);
static Expr *parse_cast_expr(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_postfix(Parser *p);
static Expr *parse_primary(Parser *p);
static Type *parse_type_name(Parser *p);
static Stmt *parse_stmt(Parser *p);
static Stmt *parse_block(Parser *p);
static Stmt *parse_simple_stmt(Parser *p, size_t line, size_t col);
static void  parse_top_decl(Parser *p, Program *prog);

void parser_init(Parser *p, Lexer *lex, DiagList *diags, const SourceMap *srcmap) {
    p->lex = lex;
    p->diags = diags;
    p->srcmap = srcmap;
    p->depth = 0;
    p->stmt_depth = 0;
    p->loop_depth = 0;
    p->switch_depth = 0;
}

/* ---------- types ---------- */

static Type *parse_type_name(Parser *p) {
    Type *base = NULL;
    switch (cur(p)->kind) {
        case TOK_KW_VOID:   base = type_void();   advance_tok(p); break;
        case TOK_KW_BOOL:   base = type_bool();   advance_tok(p); break;
        case TOK_KW_CHAR:   base = type_char();   advance_tok(p); break;
        case TOK_KW_INT:    base = type_int();    advance_tok(p); break;
        case TOK_KW_LONG:   base = type_long();   advance_tok(p); break;
        case TOK_KW_FLOAT:  base = type_float();  advance_tok(p); break;
        case TOK_KW_DOUBLE: base = type_double(); advance_tok(p); break;
        case TOK_KW_STRUCT: {
            size_t line = cur(p)->line, col = cur(p)->col;
            advance_tok(p);
            char name[LEX_MAX_IDENT];
            if (!expect_ident(p, name, sizeof(name))) return NULL;
            StructDef *sdef = struct_lookup(name);
            if (!sdef) {
                perror_at(p, line, col, "unknown struct '%s' (structs must be fully defined before use)", name);
                return NULL;
            }
            base = type_struct(sdef);
            break;
        }
        default:
            perror_at(p, cur(p)->line, cur(p)->col, "expected a type name, got %s", token_kind_name(cur(p)->kind));
            return NULL;
    }
    while (check(p, TOK_STAR)) {
        advance_tok(p);
        base = type_pointer_to(base);
    }
    return base;
}

/* ---------- expressions ---------- */

static Expr *parse_expr(Parser *p) {
    if (++p->depth > PARSER_MAX_DEPTH) {
        perror_at(p, cur(p)->line, cur(p)->col, "expression nested too deeply");
        p->depth--;
        Expr *placeholder = ast_int(0, cur(p)->line, cur(p)->col);
        return placeholder;
    }
    Expr *e = parse_logic_or(p);
    p->depth--;
    return e;
}

static Expr *parse_logic_or(Parser *p) {
    Expr *left = parse_logic_and(p);
    while (check(p, TOK_OR)) {
        size_t line = cur(p)->line, col = cur(p)->col;
        advance_tok(p);
        Expr *right = parse_logic_and(p);
        left = ast_binary(OP_OR, left, right, line, col);
    }
    return left;
}

static Expr *parse_logic_and(Parser *p) {
    Expr *left = parse_equality(p);
    while (check(p, TOK_AND)) {
        size_t line = cur(p)->line, col = cur(p)->col;
        advance_tok(p);
        Expr *right = parse_equality(p);
        left = ast_binary(OP_AND, left, right, line, col);
    }
    return left;
}

static Expr *parse_equality(Parser *p) {
    Expr *left = parse_relational(p);
    for (;;) {
        BinOp op;
        if (check(p, TOK_EQ)) op = OP_EQ;
        else if (check(p, TOK_NE)) op = OP_NE;
        else break;
        size_t line = cur(p)->line, col = cur(p)->col;
        advance_tok(p);
        Expr *right = parse_relational(p);
        left = ast_binary(op, left, right, line, col);
    }
    return left;
}

static Expr *parse_relational(Parser *p) {
    Expr *left = parse_additive(p);
    for (;;) {
        BinOp op;
        if (check(p, TOK_LT)) op = OP_LT;
        else if (check(p, TOK_GT)) op = OP_GT;
        else if (check(p, TOK_LE)) op = OP_LE;
        else if (check(p, TOK_GE)) op = OP_GE;
        else break;
        size_t line = cur(p)->line, col = cur(p)->col;
        advance_tok(p);
        Expr *right = parse_additive(p);
        left = ast_binary(op, left, right, line, col);
    }
    return left;
}

static Expr *parse_additive(Parser *p) {
    Expr *left = parse_term(p);
    for (;;) {
        BinOp op;
        if (check(p, TOK_PLUS)) op = OP_PLUS;
        else if (check(p, TOK_MINUS)) op = OP_MINUS;
        else break;
        size_t line = cur(p)->line, col = cur(p)->col;
        advance_tok(p);
        Expr *right = parse_term(p);
        left = ast_binary(op, left, right, line, col);
    }
    return left;
}

static Expr *parse_term(Parser *p) {
    Expr *left = parse_cast_expr(p);
    for (;;) {
        BinOp op;
        if (check(p, TOK_STAR)) op = OP_MUL;
        else if (check(p, TOK_SLASH)) op = OP_DIV;
        else if (check(p, TOK_PERCENT)) op = OP_MOD;
        else break;
        size_t line = cur(p)->line, col = cur(p)->col;
        advance_tok(p);
        Expr *right = parse_cast_expr(p);
        left = ast_binary(op, left, right, line, col);
    }
    return left;
}

static Expr *parse_cast_expr(Parser *p) {
    if (check(p, TOK_LPAREN) && starts_type(lexer_peek(p->lex)->kind)) {
        size_t line = cur(p)->line, col = cur(p)->col;
        advance_tok(p); /* '(' */
        Type *t = parse_type_name(p);
        expect_tok(p, TOK_RPAREN);
        Expr *inner = parse_cast_expr(p);
        if (!t) return inner;
        return ast_cast(t, inner, line, col);
    }
    return parse_unary(p);
}

static Expr *parse_unary(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    if (check(p, TOK_MINUS)) { advance_tok(p); return ast_unary(UOP_NEG, parse_unary(p), line, col); }
    if (check(p, TOK_NOT))   { advance_tok(p); return ast_unary(UOP_NOT, parse_unary(p), line, col); }
    if (check(p, TOK_STAR))  { advance_tok(p); return ast_deref(parse_unary(p), line, col); }
    if (check(p, TOK_AMP))   { advance_tok(p); return ast_addr(parse_unary(p), line, col); }
    if (check(p, TOK_KW_SIZEOF)) {
        advance_tok(p);
        if (check(p, TOK_LPAREN) && starts_type(lexer_peek(p->lex)->kind)) {
            advance_tok(p);
            Type *t = parse_type_name(p);
            expect_tok(p, TOK_RPAREN);
            return ast_sizeof_type(t, line, col);
        }
        return ast_sizeof_expr(parse_unary(p), line, col);
    }
    return parse_postfix(p);
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    if (!e) return NULL;
    for (;;) {
        size_t line = cur(p)->line, col = cur(p)->col;
        if (match(p, TOK_LBRACKET)) {
            Expr *idx = parse_expr(p);
            expect_tok(p, TOK_RBRACKET);
            e = ast_index(e, idx, line, col);
        } else if (match(p, TOK_DOT)) {
            char name[LEX_MAX_IDENT];
            if (!expect_ident(p, name, sizeof(name))) break;
            e = ast_member(e, name, 0, line, col);
        } else if (match(p, TOK_ARROW)) {
            char name[LEX_MAX_IDENT];
            if (!expect_ident(p, name, sizeof(name))) break;
            e = ast_member(e, name, 1, line, col);
        } else if (check(p, TOK_LPAREN)) {
            if (e->kind != EXPR_VAR) {
                perror_at(p, line, col, "expression is not callable");
                advance_tok(p);
                break;
            }
            char fname[LEX_MAX_IDENT];
            copy_bounded(fname, e->u.var.name, LEX_MAX_IDENT);
            fname[LEX_MAX_IDENT - 1] = '\0';
            advance_tok(p); /* '(' */
            ExprList args;
            exprlist_init(&args);
            if (!check(p, TOK_RPAREN)) {
                for (;;) {
                    Expr *a = parse_expr(p);
                    if (a) exprlist_push(&args, a);
                    if (!match(p, TOK_COMMA)) break;
                }
            }
            expect_tok(p, TOK_RPAREN);
            Expr *call = ast_call(fname, args, line, col);
            ast_free(e);
            e = call;
        } else {
            break;
        }
    }
    return e;
}

static Expr *parse_primary(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    switch (cur(p)->kind) {
        case TOK_INT_LIT: {
            int32_t v = cur(p)->int_value;
            advance_tok(p);
            return ast_int(v, line, col);
        }
        case TOK_LONG_LIT: {
            int64_t v = cur(p)->long_value;
            advance_tok(p);
            return ast_long(v, line, col);
        }
        case TOK_FLOAT_LIT: {
            float v = cur(p)->float_value;
            advance_tok(p);
            return ast_float(v, line, col);
        }
        case TOK_DOUBLE_LIT: {
            double v = cur(p)->double_value;
            advance_tok(p);
            return ast_double(v, line, col);
        }
        case TOK_CHAR_LIT: {
            int8_t v = cur(p)->char_value;
            advance_tok(p);
            return ast_char(v, line, col);
        }
        case TOK_KW_TRUE:  advance_tok(p); return ast_bool(1, line, col);
        case TOK_KW_FALSE: advance_tok(p); return ast_bool(0, line, col);
        case TOK_KW_NULL:  advance_tok(p); return ast_null(line, col);
        case TOK_STRING: {
            Expr *e = ast_string(cur(p)->str_text, cur(p)->str_len, line, col);
            advance_tok(p);
            return e;
        }
        case TOK_IDENT: {
            char name[LEX_MAX_IDENT];
            copy_bounded(name, cur(p)->text, LEX_MAX_IDENT);
            name[LEX_MAX_IDENT - 1] = '\0';
            advance_tok(p);
            return ast_var(name, line, col);
        }
        case TOK_LPAREN: {
            advance_tok(p);
            Expr *e = parse_expr(p);
            expect_tok(p, TOK_RPAREN);
            return e;
        }
        default:
            perror_at(p, line, col, "unexpected token %s in expression", token_kind_name(cur(p)->kind));
            advance_tok(p);
            return NULL;
    }
}

/* ---------- statements ---------- */

static Stmt *parse_if(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    advance_tok(p);
    expect_tok(p, TOK_LPAREN);
    Expr *cond = parse_expr(p);
    expect_tok(p, TOK_RPAREN);
    Stmt *then_b = parse_stmt(p);
    Stmt *else_b = NULL;
    if (match(p, TOK_KW_ELSE)) else_b = parse_stmt(p);
    return stmt_if(cond, then_b, else_b, line, col);
}

static Stmt *parse_while(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    advance_tok(p);
    expect_tok(p, TOK_LPAREN);
    Expr *cond = parse_expr(p);
    expect_tok(p, TOK_RPAREN);
    p->loop_depth++;
    Stmt *body = parse_stmt(p);
    p->loop_depth--;
    return stmt_while(cond, body, line, col);
}

static Stmt *parse_for(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    advance_tok(p);
    expect_tok(p, TOK_LPAREN);

    Stmt *init = NULL;
    if (!check(p, TOK_SEMI)) {
        size_t iline = cur(p)->line, icol = cur(p)->col;
        if (starts_type(cur(p)->kind)) {
            Type *t = parse_type_name(p);
            char name[LEX_MAX_IDENT];
            expect_ident(p, name, sizeof(name));
            expect_tok(p, TOK_ASSIGN);
            Expr *e = parse_expr(p);
            init = stmt_vardecl(t, name, e, iline, icol);
        } else {
            init = parse_simple_stmt(p, iline, icol);
        }
    }
    expect_tok(p, TOK_SEMI);

    Expr *cond = NULL;
    if (!check(p, TOK_SEMI)) cond = parse_expr(p);
    expect_tok(p, TOK_SEMI);

    Stmt *update = NULL;
    if (!check(p, TOK_RPAREN)) {
        size_t uline = cur(p)->line, ucol = cur(p)->col;
        update = parse_simple_stmt(p, uline, ucol);
    }
    expect_tok(p, TOK_RPAREN);

    p->loop_depth++;
    Stmt *body = parse_stmt(p);
    p->loop_depth--;
    return stmt_for(init, cond, update, body, line, col);
}

static Stmt *parse_switch(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    advance_tok(p);
    expect_tok(p, TOK_LPAREN);
    Expr *subject = parse_expr(p);
    expect_tok(p, TOK_RPAREN);
    expect_tok(p, TOK_LBRACE);

    SwitchCaseList cases;
    switchcaselist_init(&cases);
    p->switch_depth++;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        size_t cline = cur(p)->line, ccol = cur(p)->col;
        SwitchCase c;
        memset(&c, 0, sizeof(c));
        c.line = cline;
        c.col = ccol;
        stmtlist_init(&c.body);

        if (match(p, TOK_KW_DEFAULT)) {
            c.is_default = 1;
            expect_tok(p, TOK_COLON);
        } else if (match(p, TOK_KW_CASE)) {
            int64_t v;
            if (!parse_const_int(p, &v, "case label")) { sync_to_stmt_boundary(p); continue; }
            c.const_value = v;
            expect_tok(p, TOK_COLON);
        } else {
            perror_at(p, cline, ccol, "expected 'case' or 'default' inside switch body, got %s", token_kind_name(cur(p)->kind));
            sync_to_stmt_boundary(p);
            continue;
        }

        while (!check(p, TOK_KW_CASE) && !check(p, TOK_KW_DEFAULT) &&
               !check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            Stmt *s = parse_stmt(p);
            if (s) stmtlist_push(&c.body, s);
        }
        switchcaselist_push(&cases, c);
    }
    p->switch_depth--;
    expect_tok(p, TOK_RBRACE);
    return stmt_switch(subject, cases, line, col);
}

static Stmt *parse_simple_stmt(Parser *p, size_t line, size_t col) {
    if (check(p, TOK_INCR) || check(p, TOK_DECR)) {
        int is_incr = check(p, TOK_INCR);
        advance_tok(p);
        Expr *target = parse_unary(p);
        Expr *value = ast_binary(is_incr ? OP_PLUS : OP_MINUS,
                                  ast_expr_clone(target), ast_int(1, line, col), line, col);
        return stmt_assign(target, value, line, col);
    }

    Expr *lhs = parse_expr(p);
    if (!lhs) return NULL;

    if (check(p, TOK_INCR) || check(p, TOK_DECR)) {
        int is_incr = check(p, TOK_INCR);
        advance_tok(p);
        Expr *value = ast_binary(is_incr ? OP_PLUS : OP_MINUS,
                                  ast_expr_clone(lhs), ast_int(1, line, col), line, col);
        return stmt_assign(lhs, value, line, col);
    }
    if (match(p, TOK_ASSIGN)) {
        Expr *rhs = parse_expr(p);
        return stmt_assign(lhs, rhs, line, col);
    }
    static const struct { TokenKind tok; BinOp op; } compound[] = {
        { TOK_PLUS_ASSIGN, OP_PLUS }, { TOK_MINUS_ASSIGN, OP_MINUS },
        { TOK_STAR_ASSIGN, OP_MUL }, { TOK_SLASH_ASSIGN, OP_DIV },
        { TOK_PERCENT_ASSIGN, OP_MOD },
    };
    for (size_t i = 0; i < sizeof(compound) / sizeof(compound[0]); i++) {
        if (check(p, compound[i].tok)) {
            advance_tok(p);
            Expr *rhs = parse_expr(p);
            Expr *value = ast_binary(compound[i].op, ast_expr_clone(lhs), rhs, line, col);
            return stmt_assign(lhs, value, line, col);
        }
    }
    return stmt_expr(lhs, line, col);
}

static Stmt *parse_stmt_inner(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;

    if (check(p, TOK_LBRACE)) return parse_block(p);

    if (starts_type(cur(p)->kind)) {
        Type *t = parse_type_name(p);
        if (!t) { sync_to_stmt_boundary(p); return NULL; }
        char name[LEX_MAX_IDENT];
        if (!expect_ident(p, name, sizeof(name))) { sync_to_stmt_boundary(p); return NULL; }
        if (match(p, TOK_LBRACKET)) {
            uint32_t len;
            if (!parse_const_array_len(p, &len)) { sync_to_stmt_boundary(p); return NULL; }
            if (!expect_tok(p, TOK_RBRACKET)) { sync_to_stmt_boundary(p); return NULL; }
            t = type_array_of(t, len);
        }
        Expr *init = NULL;
        if (match(p, TOK_ASSIGN)) init = parse_expr(p);
        expect_tok(p, TOK_SEMI);
        return stmt_vardecl(t, name, init, line, col);
    }

    if (check(p, TOK_KW_IF)) return parse_if(p);
    if (check(p, TOK_KW_WHILE)) return parse_while(p);
    if (check(p, TOK_KW_FOR)) return parse_for(p);
    if (check(p, TOK_KW_SWITCH)) return parse_switch(p);

    if (check(p, TOK_KW_BREAK)) {
        advance_tok(p);
        if (p->loop_depth == 0 && p->switch_depth == 0) {
            perror_at(p, line, col, "'break' outside any loop or switch");
        }
        expect_tok(p, TOK_SEMI);
        return stmt_break(line, col);
    }
    if (check(p, TOK_KW_CONTINUE)) {
        advance_tok(p);
        if (p->loop_depth == 0) {
            perror_at(p, line, col, "'continue' outside any loop");
        }
        expect_tok(p, TOK_SEMI);
        return stmt_continue(line, col);
    }
    if (check(p, TOK_KW_RETURN)) {
        advance_tok(p);
        Expr *value = NULL;
        if (!check(p, TOK_SEMI)) value = parse_expr(p);
        expect_tok(p, TOK_SEMI);
        return stmt_return(value, line, col);
    }

    Stmt *s = parse_simple_stmt(p, line, col);
    expect_tok(p, TOK_SEMI);
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    if (++p->stmt_depth > PARSER_MAX_DEPTH) {
        perror_at(p, cur(p)->line, cur(p)->col, "statement nested too deeply");
        p->stmt_depth--;
        sync_to_stmt_boundary(p);
        return NULL;
    }
    Stmt *result = parse_stmt_inner(p);
    p->stmt_depth--;
    return result;
}

static Stmt *parse_block(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    expect_tok(p, TOK_LBRACE);
    StmtList list;
    stmtlist_init(&list);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Stmt *s = parse_stmt(p);
        if (s) stmtlist_push(&list, s);
    }
    expect_tok(p, TOK_RBRACE);
    return stmt_block(list, line, col);
}

/* ---------- top level ---------- */

static void resolve_and_diag(Parser *p, size_t line, size_t col, DiagSeverity sev, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *file;
    size_t orig_line;
    sourcemap_resolve(p->srcmap, line, &file, &orig_line);
    diag_addf(p->diags, sev, file, orig_line, col, "%s", buf);
}

static void parse_struct_decl(Parser *p) {
    size_t line = cur(p)->line, col = cur(p)->col;
    advance_tok(p); /* 'struct' */
    char name[LEX_MAX_IDENT];
    if (!expect_ident(p, name, sizeof(name))) { sync_to_toplevel(p); return; }
    if (!expect_tok(p, TOK_LBRACE)) { sync_to_toplevel(p); return; }

    StructField fields[STRUCT_MAX_FIELDS];
    size_t n = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Type *ftype = parse_type_name(p);
        if (!ftype) { sync_to_stmt_boundary(p); continue; }
        char fname[LEX_MAX_IDENT];
        if (!expect_ident(p, fname, sizeof(fname))) { sync_to_stmt_boundary(p); continue; }
        if (match(p, TOK_LBRACKET)) {
            uint32_t len;
            if (!parse_const_array_len(p, &len)) { sync_to_stmt_boundary(p); continue; }
            if (!expect_tok(p, TOK_RBRACKET)) { sync_to_stmt_boundary(p); continue; }
            ftype = type_array_of(ftype, len);
        }
        if (!expect_tok(p, TOK_SEMI)) { sync_to_stmt_boundary(p); continue; }
        if (n < STRUCT_MAX_FIELDS) {
            memset(&fields[n], 0, sizeof(fields[n]));
            copy_bounded(fields[n].name, fname, TYPE_MAX_IDENT);
            fields[n].type = ftype;
            n++;
        } else {
            resolve_and_diag(p, line, col, DIAG_ERROR, "struct '%s' has too many fields (max %d)", name, STRUCT_MAX_FIELDS);
        }
    }
    expect_tok(p, TOK_RBRACE);
    match(p, TOK_SEMI);

    const char *file;
    size_t orig_line;
    sourcemap_resolve(p->srcmap, line, &file, &orig_line);
    struct_define(name, fields, n, p->diags, file, orig_line, col);
}

static void parse_func_decl(Parser *p, Program *prog, Type *ret_type, const char *name, size_t line, size_t col) {
    advance_tok(p); /* '(' */
    FuncDecl f;
    memset(&f, 0, sizeof(f));
    f.return_type = ret_type;
    copy_bounded(f.name, name, LEX_MAX_IDENT);
    f.line = line;
    f.col = col;

    if (check(p, TOK_KW_VOID) && lexer_peek(p->lex)->kind == TOK_RPAREN) {
        advance_tok(p);
    } else if (!check(p, TOK_RPAREN)) {
        for (;;) {
            Type *pt = parse_type_name(p);
            if (!pt) { sync_to_stmt_boundary(p); break; }
            char pname[LEX_MAX_IDENT];
            if (!expect_ident(p, pname, sizeof(pname))) break;
            if (match(p, TOK_LBRACKET)) {
                if (check(p, TOK_RBRACKET)) {
                    advance_tok(p);
                } else {
                    uint32_t len;
                    parse_const_array_len(p, &len);
                    expect_tok(p, TOK_RBRACKET);
                }
                pt = type_pointer_to(pt); /* array params decay to pointers */
            }
            if (f.param_count < FUNC_MAX_PARAMS) {
                f.params[f.param_count].type = pt;
                copy_bounded(f.params[f.param_count].name, pname, LEX_MAX_IDENT);
                f.param_count++;
            } else {
                resolve_and_diag(p, line, col, DIAG_ERROR, "function '%s' has too many parameters (max %d)", name, FUNC_MAX_PARAMS);
            }
            if (!match(p, TOK_COMMA)) break;
        }
    }
    expect_tok(p, TOK_RPAREN);
    f.body = parse_block(p);
    funclist_push(&prog->funcs, &f);
}

static void parse_top_decl(Parser *p, Program *prog) {
    if (check(p, TOK_KW_STRUCT) && lexer_peek(p->lex)->kind == TOK_IDENT &&
        lexer_peek2(p->lex)->kind == TOK_LBRACE) {
        parse_struct_decl(p);
        return;
    }

    size_t line = cur(p)->line, col = cur(p)->col;
    Type *t = parse_type_name(p);
    if (!t) { sync_to_toplevel(p); return; }
    char name[LEX_MAX_IDENT];
    if (!expect_ident(p, name, sizeof(name))) { sync_to_toplevel(p); return; }

    if (check(p, TOK_LPAREN)) {
        parse_func_decl(p, prog, t, name, line, col);
        return;
    }

    if (match(p, TOK_LBRACKET)) {
        uint32_t len;
        if (parse_const_array_len(p, &len)) t = type_array_of(t, len);
        expect_tok(p, TOK_RBRACKET);
    }
    Expr *init = NULL;
    if (match(p, TOK_ASSIGN)) init = parse_expr(p);
    if (!expect_tok(p, TOK_SEMI)) { sync_to_toplevel(p); }

    GlobalDecl g;
    g.type = t;
    copy_bounded(g.name, name, LEX_MAX_IDENT);
    g.name[LEX_MAX_IDENT - 1] = '\0';
    g.init = init;
    g.line = line;
    g.col = col;
    globallist_push(&prog->globals, &g);
}

Program parse_program(Parser *p) {
    Program prog;
    program_init(&prog);
    while (!check(p, TOK_EOF)) {
        parse_top_decl(p, &prog);
    }
    return prog;
}
