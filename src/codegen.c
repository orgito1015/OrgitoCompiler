#include "codegen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "memlayout.h"

/* ============================================================
 * Bytecode buffer / constant pools
 * ============================================================ */

void bc_init(Bytecode *bc) {
    memset(bc, 0, sizeof(*bc));
    bc->main_func_index = -1;
}

void bc_emit(Bytecode *bc, int32_t value) {
    if (bc->count >= bc->capacity) {
        size_t cap = bc->capacity == 0 ? 1024 : bc->capacity * 2;
        bc->data = (int32_t *)realloc(bc->data, cap * sizeof(int32_t));
        if (!bc->data) exit(EXIT_FAILURE);
        bc->capacity = cap;
    }
    bc->data[bc->count++] = value;
}

int bc_add_i64(Bytecode *bc, int64_t v) {
    if (bc->i64_count >= bc->i64_capacity) {
        size_t cap = bc->i64_capacity == 0 ? 64 : bc->i64_capacity * 2;
        bc->i64s = (int64_t *)realloc(bc->i64s, cap * sizeof(int64_t));
        if (!bc->i64s) exit(EXIT_FAILURE);
        bc->i64_capacity = cap;
    }
    bc->i64s[bc->i64_count] = v;
    return (int)bc->i64_count++;
}

int bc_add_f64(Bytecode *bc, double v) {
    if (bc->f64_count >= bc->f64_capacity) {
        size_t cap = bc->f64_capacity == 0 ? 64 : bc->f64_capacity * 2;
        bc->f64s = (double *)realloc(bc->f64s, cap * sizeof(double));
        if (!bc->f64s) exit(EXIT_FAILURE);
        bc->f64_capacity = cap;
    }
    bc->f64s[bc->f64_count] = v;
    return (int)bc->f64_count++;
}

int bc_add_func(Bytecode *bc) {
    if (bc->func_count >= bc->func_capacity) {
        size_t cap = bc->func_capacity == 0 ? 16 : bc->func_capacity * 2;
        bc->funcs = (BcFunc *)realloc(bc->funcs, cap * sizeof(BcFunc));
        if (!bc->funcs) exit(EXIT_FAILURE);
        bc->func_capacity = cap;
    }
    memset(&bc->funcs[bc->func_count], 0, sizeof(BcFunc));
    bc->funcs[bc->func_count].entry = -1;
    return (int)bc->func_count++;
}

static uint32_t align_up32(uint32_t off, uint32_t align) {
    if (align == 0) return off;
    return (off + align - 1) / align * align;
}

uint32_t bc_data_alloc(Bytecode *bc, uint32_t size, uint32_t align) {
    uint32_t off = align_up32(bc->data_size, align);
    uint32_t new_size = off + size;
    if (new_size > bc->data_capacity) {
        uint32_t cap = bc->data_capacity == 0 ? 4096 : bc->data_capacity * 2;
        while (cap < new_size) cap *= 2;
        uint8_t *img = (uint8_t *)realloc(bc->data_image, cap);
        if (!img) exit(EXIT_FAILURE);
        memset(img + bc->data_capacity, 0, cap - bc->data_capacity);
        bc->data_image = img;
        bc->data_capacity = cap;
    }
    bc->data_size = new_size;
    return off;
}

void bc_data_write(Bytecode *bc, uint32_t offset, const void *bytes, uint32_t n) {
    memcpy(bc->data_image + offset, bytes, n);
}

void bc_free(Bytecode *bc) {
    free(bc->data);
    free(bc->i64s);
    free(bc->f64s);
    free(bc->funcs);
    free(bc->data_image);
    memset(bc, 0, sizeof(*bc));
}

/* ============================================================
 * Codegen context
 * ============================================================ */

typedef struct {
    char name[LEX_MAX_IDENT];
    Type *type;
    uint32_t frame_offset; /* valid if !is_global */
    uint32_t addr;         /* valid if is_global  */
    int is_global;
} VarSlot;

typedef struct { VarSlot *items; size_t count, capacity; } VarSlotList;

typedef struct {
    VarSlotList vars;
    uint32_t saved_frame_next;
} CgScope;

typedef struct { int *items; size_t count, capacity; } IntList;

typedef struct {
    IntList breaks;
    IntList continues;
    int is_switch;
} LoopCtx;

typedef struct {
    char name[LEX_MAX_IDENT];
    int func_index;
} FuncIndexEntry;

typedef struct {
    Bytecode *bc;
    DiagList *diags;
    const SourceMap *srcmap;

    VarSlotList globals;
    FuncIndexEntry *func_index_table;
    size_t func_index_count, func_index_capacity;

    CgScope *scopes;
    size_t scope_count, scope_capacity;

    uint32_t frame_next;
    uint32_t frame_high;
    int returns_struct;
    uint32_t sret_slot;
    Type *return_type;

    LoopCtx *loop_stack;
    size_t loop_count, loop_capacity;
} CodegenCtx;

static void cg_error(CodegenCtx *ctx, size_t line, size_t col, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char *file;
    size_t orig_line;
    sourcemap_resolve(ctx->srcmap, line, &file, &orig_line);
    diag_addf(ctx->diags, DIAG_ERROR, file, orig_line, col, "%s", buf);
}

static ValueTag value_tag_of(const Type *t) {
    switch (t->kind) {
        case TY_BOOL: return VAL_BOOL;
        case TY_CHAR: return VAL_CHAR;
        case TY_INT: return VAL_INT;
        case TY_LONG: return VAL_LONG;
        case TY_FLOAT: return VAL_FLOAT;
        case TY_DOUBLE: return VAL_DOUBLE;
        case TY_POINTER: return VAL_PTR;
        default: return VAL_INT;
    }
}

static Type *usual_arith(Type *a, Type *b) {
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) return type_double();
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) return type_float();
    if (a->kind == TY_LONG || b->kind == TY_LONG) return type_long();
    return type_int();
}

/* ---------- var slots ---------- */

static void varslotlist_push(VarSlotList *l, VarSlot v) {
    if (l->count >= l->capacity) {
        size_t cap = l->capacity == 0 ? 8 : l->capacity * 2;
        l->items = (VarSlot *)realloc(l->items, cap * sizeof(VarSlot));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = cap;
    }
    l->items[l->count++] = v;
}

static VarSlot *varslotlist_find(VarSlotList *l, const char *name) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].name, name) == 0) return &l->items[i];
    }
    return NULL;
}

static VarSlot *find_var_slot(CodegenCtx *ctx, const char *name) {
    for (size_t i = ctx->scope_count; i > 0; i--) {
        VarSlot *v = varslotlist_find(&ctx->scopes[i - 1].vars, name);
        if (v) return v;
    }
    return varslotlist_find(&ctx->globals, name);
}

static void push_scope(CodegenCtx *ctx) {
    if (ctx->scope_count >= ctx->scope_capacity) {
        size_t cap = ctx->scope_capacity == 0 ? 16 : ctx->scope_capacity * 2;
        ctx->scopes = (CgScope *)realloc(ctx->scopes, cap * sizeof(CgScope));
        if (!ctx->scopes) exit(EXIT_FAILURE);
        ctx->scope_capacity = cap;
    }
    CgScope *s = &ctx->scopes[ctx->scope_count++];
    memset(&s->vars, 0, sizeof(s->vars));
    s->saved_frame_next = ctx->frame_next;
}

static void pop_scope(CodegenCtx *ctx) {
    CgScope *s = &ctx->scopes[--ctx->scope_count];
    free(s->vars.items);
    ctx->frame_next = s->saved_frame_next;
}

static uint32_t alloc_local(CodegenCtx *ctx, uint32_t size, uint32_t align) {
    ctx->frame_next = align_up32(ctx->frame_next, align);
    uint32_t off = ctx->frame_next;
    ctx->frame_next += size;
    if (ctx->frame_next > ctx->frame_high) ctx->frame_high = ctx->frame_next;
    return off;
}

/* ---------- function index table ---------- */

static void register_func_index(CodegenCtx *ctx, const char *name, int index) {
    if (ctx->func_index_count >= ctx->func_index_capacity) {
        size_t cap = ctx->func_index_capacity == 0 ? 16 : ctx->func_index_capacity * 2;
        ctx->func_index_table = (FuncIndexEntry *)realloc(ctx->func_index_table, cap * sizeof(FuncIndexEntry));
        if (!ctx->func_index_table) exit(EXIT_FAILURE);
        ctx->func_index_capacity = cap;
    }
    copy_bounded(ctx->func_index_table[ctx->func_index_count].name, name, LEX_MAX_IDENT);
    ctx->func_index_table[ctx->func_index_count].func_index = index;
    ctx->func_index_count++;
}

static int find_func_index(CodegenCtx *ctx, const char *name) {
    for (size_t i = 0; i < ctx->func_index_count; i++) {
        if (strcmp(ctx->func_index_table[i].name, name) == 0) return ctx->func_index_table[i].func_index;
    }
    return -1;
}

static int find_native_id(const char *name) {
    for (int i = 0; i < NATIVE_COUNT; i++) {
        if (strcmp(NATIVE_SIGS[i].name, name) == 0) return i;
    }
    return -1;
}

/* ---------- jump patch helpers ---------- */

static int emit_jump(CodegenCtx *ctx, OpCode op) {
    bc_emit(ctx->bc, op);
    int site = (int)ctx->bc->count;
    bc_emit(ctx->bc, 0);
    return site;
}

static void patch_jump(CodegenCtx *ctx, int site, int target) {
    ctx->bc->data[site] = target;
}

static void intlist_push(IntList *l, int v) {
    if (l->count >= l->capacity) {
        size_t cap = l->capacity == 0 ? 4 : l->capacity * 2;
        l->items = (int *)realloc(l->items, cap * sizeof(int));
        if (!l->items) exit(EXIT_FAILURE);
        l->capacity = cap;
    }
    l->items[l->count++] = v;
}

static void patch_all(CodegenCtx *ctx, IntList *l, int target) {
    for (size_t i = 0; i < l->count; i++) patch_jump(ctx, l->items[i], target);
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
}

static void push_loop(CodegenCtx *ctx, int is_switch) {
    if (ctx->loop_count >= ctx->loop_capacity) {
        size_t cap = ctx->loop_capacity == 0 ? 8 : ctx->loop_capacity * 2;
        ctx->loop_stack = (LoopCtx *)realloc(ctx->loop_stack, cap * sizeof(LoopCtx));
        if (!ctx->loop_stack) exit(EXIT_FAILURE);
        ctx->loop_capacity = cap;
    }
    LoopCtx *lc = &ctx->loop_stack[ctx->loop_count++];
    memset(lc, 0, sizeof(*lc));
    lc->is_switch = is_switch;
}

static void pop_loop(CodegenCtx *ctx) { ctx->loop_count--; }

static void add_break_patch(CodegenCtx *ctx, int site) {
    intlist_push(&ctx->loop_stack[ctx->loop_count - 1].breaks, site);
}

static void add_continue_patch(CodegenCtx *ctx, int site) {
    for (size_t i = ctx->loop_count; i > 0; i--) {
        if (!ctx->loop_stack[i - 1].is_switch) {
            intlist_push(&ctx->loop_stack[i - 1].continues, site);
            return;
        }
    }
}

/* ---------- constant emission ---------- */

static void emit_push_i32(CodegenCtx *ctx, int32_t v) { bc_emit(ctx->bc, BC_PUSH_I32); bc_emit(ctx->bc, v); }
static void emit_push_char(CodegenCtx *ctx, int8_t v) { bc_emit(ctx->bc, BC_PUSH_CHAR); bc_emit(ctx->bc, (int32_t)v); }
static void emit_push_bool(CodegenCtx *ctx, int v) { bc_emit(ctx->bc, BC_PUSH_BOOL); bc_emit(ctx->bc, v ? 1 : 0); }
static void emit_push_i64(CodegenCtx *ctx, int64_t v) {
    int idx = bc_add_i64(ctx->bc, v);
    bc_emit(ctx->bc, BC_PUSH_I64);
    bc_emit(ctx->bc, idx);
}
static void emit_push_f32(CodegenCtx *ctx, float v) {
    int idx = bc_add_f64(ctx->bc, (double)v);
    bc_emit(ctx->bc, BC_PUSH_F32);
    bc_emit(ctx->bc, idx);
}
static void emit_push_f64(CodegenCtx *ctx, double v) {
    int idx = bc_add_f64(ctx->bc, v);
    bc_emit(ctx->bc, BC_PUSH_F64);
    bc_emit(ctx->bc, idx);
}
static void emit_push_ptr_const(CodegenCtx *ctx, uint32_t addr) {
    bc_emit(ctx->bc, BC_PUSH_PTR_CONST);
    bc_emit(ctx->bc, (int32_t)addr);
}

static void emit_zero_value(CodegenCtx *ctx, Type *t) {
    switch (t->kind) {
        case TY_BOOL: emit_push_bool(ctx, 0); break;
        case TY_CHAR: emit_push_char(ctx, 0); break;
        case TY_LONG: emit_push_i64(ctx, 0); break;
        case TY_FLOAT: emit_push_f32(ctx, 0.0f); break;
        case TY_DOUBLE: emit_push_f64(ctx, 0.0); break;
        case TY_POINTER: bc_emit(ctx->bc, BC_PUSH_NULL); break;
        default: emit_push_i32(ctx, 0); break;
    }
}

static void emit_push_const_matching(CodegenCtx *ctx, Type *t, int64_t v) {
    switch (t->kind) {
        case TY_LONG: emit_push_i64(ctx, v); break;
        case TY_CHAR: emit_push_char(ctx, (int8_t)v); break;
        case TY_BOOL: emit_push_bool(ctx, v != 0); break;
        default: emit_push_i32(ctx, (int32_t)v); break;
    }
}

static uint32_t codegen_intern_string(CodegenCtx *ctx, const char *text, size_t len) {
    uint32_t off = bc_data_alloc(ctx->bc, (uint32_t)len + 1, 1);
    bc_data_write(ctx->bc, off, text, (uint32_t)len);
    uint8_t zero = 0;
    bc_data_write(ctx->bc, off + (uint32_t)len, &zero, 1);
    return DATA_BASE + off;
}

/* ============================================================
 * Expressions
 * ============================================================ */

static void codegen_expr_rvalue(CodegenCtx *ctx, Expr *e);
static void codegen_addr_of(CodegenCtx *ctx, Expr *e);
static void codegen_index_addr(CodegenCtx *ctx, Expr *e);
static void codegen_member_addr(CodegenCtx *ctx, Expr *e);
static void codegen_call(CodegenCtx *ctx, Expr *e, int dest_already_pushed);
static void codegen_struct_value_addr(CodegenCtx *ctx, Expr *e);
static void codegen_stmt(CodegenCtx *ctx, Stmt *s);
static void codegen_stmtlist(CodegenCtx *ctx, StmtList *list);

static void codegen_expr_rvalue_promoted(CodegenCtx *ctx, Expr *e, Type *target) {
    codegen_expr_rvalue(ctx, e);
    if (e->type->kind != target->kind) {
        bc_emit(ctx->bc, BC_CAST);
        bc_emit(ctx->bc, value_tag_of(target));
    }
}

static void codegen_and(CodegenCtx *ctx, Expr *e) {
    codegen_expr_rvalue(ctx, e->u.binary.left);
    int jz1 = emit_jump(ctx, BC_JZ);
    codegen_expr_rvalue(ctx, e->u.binary.right);
    int jz2 = emit_jump(ctx, BC_JZ);
    emit_push_bool(ctx, 1);
    int jend = emit_jump(ctx, BC_JMP);
    int lfalse = (int)ctx->bc->count;
    patch_jump(ctx, jz1, lfalse);
    patch_jump(ctx, jz2, lfalse);
    emit_push_bool(ctx, 0);
    patch_jump(ctx, jend, (int)ctx->bc->count);
}

static void codegen_or(CodegenCtx *ctx, Expr *e) {
    codegen_expr_rvalue(ctx, e->u.binary.left);
    int jz1 = emit_jump(ctx, BC_JZ);
    emit_push_bool(ctx, 1);
    int jend1 = emit_jump(ctx, BC_JMP);
    patch_jump(ctx, jz1, (int)ctx->bc->count);
    codegen_expr_rvalue(ctx, e->u.binary.right);
    int jz2 = emit_jump(ctx, BC_JZ);
    emit_push_bool(ctx, 1);
    int jend2 = emit_jump(ctx, BC_JMP);
    int lfalse = (int)ctx->bc->count;
    patch_jump(ctx, jz2, lfalse);
    emit_push_bool(ctx, 0);
    int lend = (int)ctx->bc->count;
    patch_jump(ctx, jend1, lend);
    patch_jump(ctx, jend2, lend);
}

static void codegen_binary(CodegenCtx *ctx, Expr *e) {
    BinOp op = e->u.binary.op;
    if (op == OP_AND) { codegen_and(ctx, e); return; }
    if (op == OP_OR) { codegen_or(ctx, e); return; }

    Type *lt = e->u.binary.left->type;
    Type *rt = e->u.binary.right->type;
    int lt_ptr_like = type_is_pointer(lt) || lt->kind == TY_ARRAY;
    int rt_ptr_like = type_is_pointer(rt) || rt->kind == TY_ARRAY;

    if ((op == OP_PLUS || op == OP_MINUS) && (lt_ptr_like || rt_ptr_like)) {
        if (op == OP_MINUS && lt_ptr_like && rt_ptr_like) {
            codegen_expr_rvalue(ctx, e->u.binary.left);
            codegen_expr_rvalue(ctx, e->u.binary.right);
            uint32_t elem_size = lt->base->kind == TY_VOID ? 1 : lt->base->size;
            bc_emit(ctx->bc, BC_PTR_DIFF);
            bc_emit(ctx->bc, (int32_t)elem_size);
            return;
        }
        Expr *ptr_expr = lt_ptr_like ? e->u.binary.left : e->u.binary.right;
        Expr *int_expr = lt_ptr_like ? e->u.binary.right : e->u.binary.left;
        uint32_t elem_size = ptr_expr->type->base->kind == TY_VOID ? 1 : ptr_expr->type->base->size;
        codegen_expr_rvalue(ctx, ptr_expr);
        codegen_expr_rvalue(ctx, int_expr);
        if (int_expr->type->kind == TY_LONG) { bc_emit(ctx->bc, BC_CAST); bc_emit(ctx->bc, VAL_INT); }
        emit_push_i32(ctx, (int32_t)elem_size);
        bc_emit(ctx->bc, BC_MUL);
        bc_emit(ctx->bc, op == OP_PLUS ? BC_PTR_ADD : BC_PTR_SUB);
        return;
    }

    Type *common = (type_is_numeric(lt) && type_is_numeric(rt)) ? usual_arith(lt, rt) : lt;
    codegen_expr_rvalue_promoted(ctx, e->u.binary.left, common);
    codegen_expr_rvalue_promoted(ctx, e->u.binary.right, common);
    switch (op) {
        case OP_PLUS:  bc_emit(ctx->bc, BC_ADD); break;
        case OP_MINUS: bc_emit(ctx->bc, BC_SUB); break;
        case OP_MUL:   bc_emit(ctx->bc, BC_MUL); break;
        case OP_DIV:   bc_emit(ctx->bc, BC_DIV); break;
        case OP_MOD:   bc_emit(ctx->bc, BC_MOD); break;
        case OP_LT:    bc_emit(ctx->bc, BC_LT); break;
        case OP_GT:    bc_emit(ctx->bc, BC_GT); break;
        case OP_LE:    bc_emit(ctx->bc, BC_LE); break;
        case OP_GE:    bc_emit(ctx->bc, BC_GE); break;
        case OP_EQ:    bc_emit(ctx->bc, BC_EQ); break;
        case OP_NE:    bc_emit(ctx->bc, BC_NE); break;
        default: break;
    }
}

static void codegen_addr_of(CodegenCtx *ctx, Expr *e) {
    switch (e->kind) {
        case EXPR_VAR: {
            VarSlot *slot = find_var_slot(ctx, e->u.var.name);
            if (!slot) { emit_push_ptr_const(ctx, 0); break; }
            if (slot->is_global) {
                emit_push_ptr_const(ctx, slot->addr);
            } else {
                bc_emit(ctx->bc, BC_ADDR_LOCAL);
                bc_emit(ctx->bc, (int32_t)slot->frame_offset);
            }
            break;
        }
        case EXPR_DEREF:
            codegen_expr_rvalue(ctx, e->u.deref.expr);
            break;
        case EXPR_INDEX:
            codegen_index_addr(ctx, e);
            break;
        case EXPR_MEMBER:
            codegen_member_addr(ctx, e);
            break;
        default:
            break; /* unreachable for well-typed input (typecheck restricts lvalues to the above) */
    }
}

static void codegen_index_addr(CodegenCtx *ctx, Expr *e) {
    Type *base_type = e->u.index.base->type;
    if (base_type->kind == TY_ARRAY) {
        codegen_addr_of(ctx, e->u.index.base);
        codegen_expr_rvalue(ctx, e->u.index.index);
        if (e->u.index.index->type->kind == TY_LONG) { bc_emit(ctx->bc, BC_CAST); bc_emit(ctx->bc, VAL_INT); }
        bc_emit(ctx->bc, BC_ARR_ADDR);
        bc_emit(ctx->bc, (int32_t)base_type->base->size);
        bc_emit(ctx->bc, (int32_t)base_type->array_len);
    } else {
        codegen_expr_rvalue(ctx, e->u.index.base);
        codegen_expr_rvalue(ctx, e->u.index.index);
        if (e->u.index.index->type->kind == TY_LONG) { bc_emit(ctx->bc, BC_CAST); bc_emit(ctx->bc, VAL_INT); }
        uint32_t elem_size = base_type->base->kind == TY_VOID ? 1 : base_type->base->size;
        emit_push_i32(ctx, (int32_t)elem_size);
        bc_emit(ctx->bc, BC_MUL);
        bc_emit(ctx->bc, BC_PTR_ADD);
    }
}

static void codegen_member_addr(CodegenCtx *ctx, Expr *e) {
    const StructField *f = NULL;
    if (e->u.member.is_arrow) {
        codegen_expr_rvalue(ctx, e->u.member.base);
        Type *pt = e->u.member.base->type;
        if (pt->kind == TY_POINTER && pt->base->kind == TY_STRUCT) f = struct_find_field(pt->base->sdef, e->u.member.field);
    } else {
        codegen_addr_of(ctx, e->u.member.base);
        Type *st = e->u.member.base->type;
        if (st->kind == TY_STRUCT) f = struct_find_field(st->sdef, e->u.member.field);
    }
    bc_emit(ctx->bc, BC_MEMBER_ADDR);
    bc_emit(ctx->bc, f ? (int32_t)f->offset : 0);
}

static void codegen_call(CodegenCtx *ctx, Expr *e, int dest_already_pushed) {
    int native_id = find_native_id(e->u.call.name);
    if (native_id >= 0) {
        for (size_t a = 0; a < e->u.call.args.count; a++) codegen_expr_rvalue(ctx, e->u.call.args.items[a]);
        bc_emit(ctx->bc, BC_CALL_NATIVE);
        bc_emit(ctx->bc, native_id);
        bc_emit(ctx->bc, (int32_t)e->u.call.args.count);
        return;
    }

    int func_index = find_func_index(ctx, e->u.call.name);
    if (func_index < 0) return; /* unreachable for well-typed input */
    BcFunc *fn = &ctx->bc->funcs[func_index];

    if (fn->returns_struct) {
        if (!dest_already_pushed) {
            uint32_t temp_off = alloc_local(ctx, fn->return_size, fn->return_align);
            bc_emit(ctx->bc, BC_ADDR_LOCAL);
            bc_emit(ctx->bc, (int32_t)temp_off);
        }
        bc_emit(ctx->bc, BC_DUP);
    }

    for (size_t a = 0; a < e->u.call.args.count; a++) {
        BcParam *bp = &fn->params[a];
        if (bp->is_struct) {
            codegen_struct_value_addr(ctx, e->u.call.args.items[a]);
        } else {
            codegen_expr_rvalue(ctx, e->u.call.args.items[a]);
            if (value_tag_of(e->u.call.args.items[a]->type) != bp->tag) {
                bc_emit(ctx->bc, BC_CAST);
                bc_emit(ctx->bc, bp->tag);
            }
        }
    }
    bc_emit(ctx->bc, BC_CALL);
    bc_emit(ctx->bc, func_index);
}

static void codegen_struct_value_addr(CodegenCtx *ctx, Expr *e) {
    if (e->kind == EXPR_CALL) {
        codegen_call(ctx, e, 0);
    } else {
        codegen_addr_of(ctx, e);
    }
}

static void codegen_expr_rvalue(CodegenCtx *ctx, Expr *e) {
    switch (e->kind) {
        case EXPR_INT_LIT: emit_push_i32(ctx, e->u.int_lit.value); break;
        case EXPR_LONG_LIT: emit_push_i64(ctx, e->u.long_lit.value); break;
        case EXPR_FLOAT_LIT: emit_push_f32(ctx, e->u.float_lit.value); break;
        case EXPR_DOUBLE_LIT: emit_push_f64(ctx, e->u.double_lit.value); break;
        case EXPR_CHAR_LIT: emit_push_char(ctx, e->u.char_lit.value); break;
        case EXPR_BOOL_LIT: emit_push_bool(ctx, e->u.bool_lit.value); break;
        case EXPR_NULL_LIT: bc_emit(ctx->bc, BC_PUSH_NULL); break;
        case EXPR_STRING_LIT: {
            uint32_t addr = codegen_intern_string(ctx, e->u.string_lit.text, e->u.string_lit.len);
            emit_push_ptr_const(ctx, addr);
            break;
        }
        case EXPR_VAR:
            codegen_addr_of(ctx, e);
            if (e->type->kind != TY_ARRAY) {
                bc_emit(ctx->bc, BC_LOAD);
                bc_emit(ctx->bc, value_tag_of(e->type));
            }
            /* else: array-to-pointer decay - its address IS its value */
            break;
        case EXPR_UNARY:
            codegen_expr_rvalue(ctx, e->u.unary.expr);
            if (e->u.unary.op == UOP_NEG) bc_emit(ctx->bc, BC_NEG);
            else bc_emit(ctx->bc, BC_NOT);
            break;
        case EXPR_ADDR:
            codegen_addr_of(ctx, e->u.addr.expr);
            break;
        case EXPR_DEREF:
            codegen_expr_rvalue(ctx, e->u.deref.expr);
            bc_emit(ctx->bc, BC_LOAD);
            bc_emit(ctx->bc, value_tag_of(e->type));
            break;
        case EXPR_BINARY:
            codegen_binary(ctx, e);
            break;
        case EXPR_CALL:
            codegen_call(ctx, e, 0);
            break;
        case EXPR_INDEX:
            codegen_index_addr(ctx, e);
            if (e->type->kind != TY_ARRAY) {
                bc_emit(ctx->bc, BC_LOAD);
                bc_emit(ctx->bc, value_tag_of(e->type));
            }
            break;
        case EXPR_MEMBER:
            codegen_member_addr(ctx, e);
            if (e->type->kind != TY_ARRAY) {
                bc_emit(ctx->bc, BC_LOAD);
                bc_emit(ctx->bc, value_tag_of(e->type));
            }
            break;
        case EXPR_CAST:
            codegen_expr_rvalue(ctx, e->u.cast.expr);
            bc_emit(ctx->bc, BC_CAST);
            bc_emit(ctx->bc, value_tag_of(e->type));
            break;
        case EXPR_SIZEOF: {
            uint32_t sz = e->u.sizeof_.is_type ? e->u.sizeof_.type->size : e->u.sizeof_.expr->type->size;
            emit_push_i64(ctx, (int64_t)sz);
            break;
        }
    }
}

/* ============================================================
 * Statements
 * ============================================================ */

static void codegen_stmt(CodegenCtx *ctx, Stmt *s) {
    switch (s->kind) {
        case STMT_EXPR: {
            Expr *ex = s->u.expr.expr;
            if (ex->type->kind == TY_STRUCT) {
                codegen_struct_value_addr(ctx, ex);
                bc_emit(ctx->bc, BC_POP);
            } else if (ex->type->kind != TY_VOID) {
                codegen_expr_rvalue(ctx, ex);
                bc_emit(ctx->bc, BC_POP);
            } else {
                codegen_expr_rvalue(ctx, ex);
            }
            break;
        }
        case STMT_VARDECL: {
            Type *t = s->u.vardecl.type;
            uint32_t off = alloc_local(ctx, t->size, t->align);
            VarSlot slot;
            memset(&slot, 0, sizeof(slot));
            copy_bounded(slot.name, s->u.vardecl.name, LEX_MAX_IDENT);
            slot.type = t;
            slot.frame_offset = off;
            varslotlist_push(&ctx->scopes[ctx->scope_count - 1].vars, slot);
            if (s->u.vardecl.init && t->kind == TY_STRUCT) {
                if (s->u.vardecl.init->kind == EXPR_CALL) {
                    bc_emit(ctx->bc, BC_ADDR_LOCAL);
                    bc_emit(ctx->bc, (int32_t)off);
                    codegen_call(ctx, s->u.vardecl.init, 1);
                    bc_emit(ctx->bc, BC_POP);
                } else {
                    bc_emit(ctx->bc, BC_ADDR_LOCAL);
                    bc_emit(ctx->bc, (int32_t)off);
                    codegen_addr_of(ctx, s->u.vardecl.init);
                    bc_emit(ctx->bc, BC_COPY);
                    bc_emit(ctx->bc, (int32_t)t->size);
                }
            } else if (s->u.vardecl.init) {
                bc_emit(ctx->bc, BC_ADDR_LOCAL);
                bc_emit(ctx->bc, (int32_t)off);
                codegen_expr_rvalue_promoted(ctx, s->u.vardecl.init, t);
                bc_emit(ctx->bc, BC_STORE);
                bc_emit(ctx->bc, value_tag_of(t));
            } else {
                bc_emit(ctx->bc, BC_ADDR_LOCAL);
                bc_emit(ctx->bc, (int32_t)off);
                bc_emit(ctx->bc, BC_ZERO_MEM);
                bc_emit(ctx->bc, (int32_t)t->size);
            }
            break;
        }
        case STMT_ASSIGN: {
            Type *target_type = s->u.assign.target->type;
            if (target_type->kind == TY_STRUCT) {
                if (s->u.assign.value->kind == EXPR_CALL) {
                    codegen_addr_of(ctx, s->u.assign.target);
                    codegen_call(ctx, s->u.assign.value, 1);
                    bc_emit(ctx->bc, BC_POP);
                } else {
                    codegen_addr_of(ctx, s->u.assign.target);
                    codegen_addr_of(ctx, s->u.assign.value);
                    bc_emit(ctx->bc, BC_COPY);
                    bc_emit(ctx->bc, (int32_t)target_type->size);
                }
            } else {
                codegen_addr_of(ctx, s->u.assign.target);
                codegen_expr_rvalue_promoted(ctx, s->u.assign.value, target_type);
                bc_emit(ctx->bc, BC_STORE);
                bc_emit(ctx->bc, value_tag_of(target_type));
            }
            break;
        }
        case STMT_IF: {
            codegen_expr_rvalue(ctx, s->u.if_stmt.cond);
            int jz = emit_jump(ctx, BC_JZ);
            codegen_stmt(ctx, s->u.if_stmt.then_branch);
            if (s->u.if_stmt.else_branch) {
                int jend = emit_jump(ctx, BC_JMP);
                patch_jump(ctx, jz, (int)ctx->bc->count);
                codegen_stmt(ctx, s->u.if_stmt.else_branch);
                patch_jump(ctx, jend, (int)ctx->bc->count);
            } else {
                patch_jump(ctx, jz, (int)ctx->bc->count);
            }
            break;
        }
        case STMT_WHILE: {
            int loop_start = (int)ctx->bc->count;
            codegen_expr_rvalue(ctx, s->u.while_stmt.cond);
            int jz = emit_jump(ctx, BC_JZ);
            push_loop(ctx, 0);
            codegen_stmt(ctx, s->u.while_stmt.body);
            patch_all(ctx, &ctx->loop_stack[ctx->loop_count - 1].continues, loop_start);
            bc_emit(ctx->bc, BC_JMP);
            bc_emit(ctx->bc, loop_start);
            int loop_end = (int)ctx->bc->count;
            patch_jump(ctx, jz, loop_end);
            patch_all(ctx, &ctx->loop_stack[ctx->loop_count - 1].breaks, loop_end);
            pop_loop(ctx);
            break;
        }
        case STMT_FOR: {
            push_scope(ctx);
            if (s->u.for_stmt.init) codegen_stmt(ctx, s->u.for_stmt.init);
            int loop_start = (int)ctx->bc->count;
            int jz = -1;
            if (s->u.for_stmt.cond) {
                codegen_expr_rvalue(ctx, s->u.for_stmt.cond);
                jz = emit_jump(ctx, BC_JZ);
            }
            push_loop(ctx, 0);
            codegen_stmt(ctx, s->u.for_stmt.body);
            int update_start = (int)ctx->bc->count;
            patch_all(ctx, &ctx->loop_stack[ctx->loop_count - 1].continues, update_start);
            if (s->u.for_stmt.update) codegen_stmt(ctx, s->u.for_stmt.update);
            bc_emit(ctx->bc, BC_JMP);
            bc_emit(ctx->bc, loop_start);
            int loop_end = (int)ctx->bc->count;
            if (jz >= 0) patch_jump(ctx, jz, loop_end);
            patch_all(ctx, &ctx->loop_stack[ctx->loop_count - 1].breaks, loop_end);
            pop_loop(ctx);
            pop_scope(ctx);
            break;
        }
        case STMT_SWITCH: {
            Type *subj_type = s->u.switch_stmt.subject->type;
            uint32_t temp_off = alloc_local(ctx, subj_type->size, subj_type->align);
            bc_emit(ctx->bc, BC_ADDR_LOCAL);
            bc_emit(ctx->bc, (int32_t)temp_off);
            codegen_expr_rvalue(ctx, s->u.switch_stmt.subject);
            bc_emit(ctx->bc, BC_STORE);
            bc_emit(ctx->bc, value_tag_of(subj_type));

            size_t ncases = s->u.switch_stmt.cases.count;
            int *body_target_patch = (int *)malloc(ncases * sizeof(int));
            int default_index = -1;
            int no_match_jump = -1;

            for (size_t i = 0; i < ncases; i++) {
                SwitchCase *c = &s->u.switch_stmt.cases.items[i];
                if (c->is_default) { default_index = (int)i; continue; }
                bc_emit(ctx->bc, BC_ADDR_LOCAL);
                bc_emit(ctx->bc, (int32_t)temp_off);
                bc_emit(ctx->bc, BC_LOAD);
                bc_emit(ctx->bc, value_tag_of(subj_type));
                emit_push_const_matching(ctx, subj_type, c->const_value);
                bc_emit(ctx->bc, BC_EQ);
                int jz = emit_jump(ctx, BC_JZ);
                body_target_patch[i] = emit_jump(ctx, BC_JMP);
                patch_jump(ctx, jz, (int)ctx->bc->count);
            }
            if (default_index >= 0) {
                body_target_patch[default_index] = emit_jump(ctx, BC_JMP);
            } else {
                no_match_jump = emit_jump(ctx, BC_JMP);
            }

            push_loop(ctx, 1);
            for (size_t i = 0; i < ncases; i++) {
                SwitchCase *c = &s->u.switch_stmt.cases.items[i];
                patch_jump(ctx, body_target_patch[i], (int)ctx->bc->count);
                codegen_stmtlist(ctx, &c->body);
            }
            int switch_end = (int)ctx->bc->count;
            if (no_match_jump >= 0) patch_jump(ctx, no_match_jump, switch_end);
            patch_all(ctx, &ctx->loop_stack[ctx->loop_count - 1].breaks, switch_end);
            pop_loop(ctx);
            free(body_target_patch);
            break;
        }
        case STMT_BREAK: {
            int site = emit_jump(ctx, BC_JMP);
            add_break_patch(ctx, site);
            break;
        }
        case STMT_CONTINUE: {
            int site = emit_jump(ctx, BC_JMP);
            add_continue_patch(ctx, site);
            break;
        }
        case STMT_RETURN: {
            if (ctx->returns_struct) {
                if (s->u.ret.value) {
                    if (s->u.ret.value->kind == EXPR_CALL) {
                        bc_emit(ctx->bc, BC_ADDR_LOCAL);
                        bc_emit(ctx->bc, (int32_t)ctx->sret_slot);
                        bc_emit(ctx->bc, BC_LOAD);
                        bc_emit(ctx->bc, VAL_PTR);
                        codegen_call(ctx, s->u.ret.value, 1);
                        bc_emit(ctx->bc, BC_POP);
                    } else {
                        bc_emit(ctx->bc, BC_ADDR_LOCAL);
                        bc_emit(ctx->bc, (int32_t)ctx->sret_slot);
                        bc_emit(ctx->bc, BC_LOAD);
                        bc_emit(ctx->bc, VAL_PTR);
                        codegen_addr_of(ctx, s->u.ret.value);
                        bc_emit(ctx->bc, BC_COPY);
                        bc_emit(ctx->bc, (int32_t)ctx->return_type->size);
                    }
                }
                bc_emit(ctx->bc, BC_RET);
            } else if (ctx->return_type->kind == TY_VOID) {
                bc_emit(ctx->bc, BC_RET);
            } else {
                if (s->u.ret.value) codegen_expr_rvalue_promoted(ctx, s->u.ret.value, ctx->return_type);
                else emit_zero_value(ctx, ctx->return_type);
                bc_emit(ctx->bc, BC_RET);
            }
            break;
        }
        case STMT_BLOCK:
            codegen_stmtlist(ctx, &s->u.block.list);
            break;
    }
}

static void codegen_stmtlist(CodegenCtx *ctx, StmtList *list) {
    push_scope(ctx);
    for (size_t i = 0; i < list->count; i++) codegen_stmt(ctx, list->items[i]);
    pop_scope(ctx);
}

/* ============================================================
 * Global initializer constant folding
 * ============================================================ */

typedef struct { int is_float; int64_t i; double f; } ConstVal;

static int codegen_eval_const(const Expr *e, ConstVal *out) {
    switch (e->kind) {
        case EXPR_INT_LIT: out->is_float = 0; out->i = e->u.int_lit.value; return 1;
        case EXPR_LONG_LIT: out->is_float = 0; out->i = e->u.long_lit.value; return 1;
        case EXPR_CHAR_LIT: out->is_float = 0; out->i = e->u.char_lit.value; return 1;
        case EXPR_BOOL_LIT: out->is_float = 0; out->i = e->u.bool_lit.value; return 1;
        case EXPR_FLOAT_LIT: out->is_float = 1; out->f = e->u.float_lit.value; return 1;
        case EXPR_DOUBLE_LIT: out->is_float = 1; out->f = e->u.double_lit.value; return 1;
        case EXPR_NULL_LIT: out->is_float = 0; out->i = 0; return 1;
        case EXPR_UNARY: {
            ConstVal v;
            if (!codegen_eval_const(e->u.unary.expr, &v)) return 0;
            if (e->u.unary.op == UOP_NEG) { if (v.is_float) v.f = -v.f; else v.i = -v.i; }
            else { int truthy = v.is_float ? (v.f != 0.0) : (v.i != 0); v.is_float = 0; v.i = !truthy; }
            *out = v;
            return 1;
        }
        case EXPR_BINARY: {
            ConstVal a, b;
            if (!codegen_eval_const(e->u.binary.left, &a)) return 0;
            if (!codegen_eval_const(e->u.binary.right, &b)) return 0;
            int is_float = a.is_float || b.is_float;
            double af = a.is_float ? a.f : (double)a.i, bf = b.is_float ? b.f : (double)b.i;
            switch (e->u.binary.op) {
                case OP_PLUS:  if (is_float) { out->is_float = 1; out->f = af + bf; } else { out->is_float = 0; out->i = a.i + b.i; } return 1;
                case OP_MINUS: if (is_float) { out->is_float = 1; out->f = af - bf; } else { out->is_float = 0; out->i = a.i - b.i; } return 1;
                case OP_MUL:   if (is_float) { out->is_float = 1; out->f = af * bf; } else { out->is_float = 0; out->i = a.i * b.i; } return 1;
                case OP_DIV:
                    if (is_float) { out->is_float = 1; out->f = af / bf; return 1; }
                    if (b.i == 0) return 0;
                    out->is_float = 0; out->i = a.i / b.i; return 1;
                case OP_MOD:
                    if (is_float || b.i == 0) return 0;
                    out->is_float = 0; out->i = a.i % b.i; return 1;
                default: return 0;
            }
        }
        case EXPR_SIZEOF:
            out->is_float = 0;
            out->i = e->u.sizeof_.is_type ? e->u.sizeof_.type->size : e->u.sizeof_.expr->type->size;
            return 1;
        default:
            return 0;
    }
}

static void codegen_write_const_bytes(uint8_t *dst, const Type *t, ConstVal v) {
    switch (t->kind) {
        case TY_BOOL: { int8_t x = (int8_t)(v.is_float ? (v.f != 0.0) : (v.i != 0)); memcpy(dst, &x, 1); break; }
        case TY_CHAR: { int8_t x = (int8_t)(v.is_float ? (int64_t)v.f : v.i); memcpy(dst, &x, 1); break; }
        case TY_INT: { int32_t x = (int32_t)(v.is_float ? (int64_t)v.f : v.i); memcpy(dst, &x, 4); break; }
        case TY_LONG: { int64_t x = v.is_float ? (int64_t)v.f : v.i; memcpy(dst, &x, 8); break; }
        case TY_FLOAT: { float x = (float)(v.is_float ? v.f : (double)v.i); memcpy(dst, &x, 4); break; }
        case TY_DOUBLE: { double x = v.is_float ? v.f : (double)v.i; memcpy(dst, &x, 8); break; }
        case TY_POINTER: { int32_t x = (int32_t)(v.is_float ? (int64_t)v.f : v.i); memcpy(dst, &x, 4); break; }
        default: break;
    }
}

/* ============================================================
 * Program driver
 * ============================================================ */

static void codegen_layout_globals(CodegenCtx *ctx, Program *prog) {
    for (size_t i = 0; i < prog->globals.count; i++) {
        GlobalDecl *g = &prog->globals.items[i];
        uint32_t off = bc_data_alloc(ctx->bc, g->type->size, g->type->align);
        VarSlot slot;
        memset(&slot, 0, sizeof(slot));
        copy_bounded(slot.name, g->name, LEX_MAX_IDENT);
        slot.type = g->type;
        slot.addr = DATA_BASE + off;
        slot.is_global = 1;
        varslotlist_push(&ctx->globals, slot);
    }
    for (size_t i = 0; i < prog->globals.count; i++) {
        GlobalDecl *g = &prog->globals.items[i];
        if (!g->init) continue;
        ConstVal v;
        if (!codegen_eval_const(g->init, &v)) {
            cg_error(ctx, g->line, g->col, "initializer for global '%s' is not a compile-time constant", g->name);
            continue;
        }
        VarSlot *slot = varslotlist_find(&ctx->globals, g->name);
        codegen_write_const_bytes(ctx->bc->data_image + (slot->addr - DATA_BASE), g->type, v);
    }
}

static void codegen_layout_func_params(BcFunc *fn, FuncDecl *f) {
    fn->nparams = (int)f->param_count;
    fn->returns_struct = (f->return_type->kind == TY_STRUCT);
    fn->return_size = f->return_type->size;
    fn->return_align = f->return_type->align ? f->return_type->align : 1;
    fn->return_tag = fn->returns_struct ? VAL_VOID : value_tag_of(f->return_type);

    uint32_t next = 0;
    if (fn->returns_struct) {
        fn->sret_slot = 0;
        next = 4;
    }
    for (size_t i = 0; i < f->param_count; i++) {
        Type *pt = f->params[i].type;
        next = align_up32(next, pt->align);
        fn->params[i].frame_offset = next;
        fn->params[i].size = pt->size;
        fn->params[i].is_struct = (pt->kind == TY_STRUCT);
        fn->params[i].tag = fn->params[i].is_struct ? VAL_VOID : value_tag_of(pt);
        next += pt->size;
    }
    fn->param_area_size = next;
}

static void codegen_function(CodegenCtx *ctx, FuncDecl *f, int func_index) {
    BcFunc *fn = &ctx->bc->funcs[func_index];
    fn->entry = (int)ctx->bc->count;

    ctx->frame_next = fn->param_area_size;
    ctx->frame_high = ctx->frame_next;
    ctx->returns_struct = fn->returns_struct;
    ctx->sret_slot = fn->sret_slot;
    ctx->return_type = f->return_type;
    ctx->scope_count = 0;

    push_scope(ctx);
    for (size_t i = 0; i < f->param_count; i++) {
        VarSlot slot;
        memset(&slot, 0, sizeof(slot));
        copy_bounded(slot.name, f->params[i].name, LEX_MAX_IDENT);
        slot.type = f->params[i].type;
        slot.frame_offset = fn->params[i].frame_offset;
        varslotlist_push(&ctx->scopes[ctx->scope_count - 1].vars, slot);
    }
    codegen_stmt(ctx, f->body);
    pop_scope(ctx);

    if (fn->returns_struct) {
        bc_emit(ctx->bc, BC_ADDR_LOCAL);
        bc_emit(ctx->bc, (int32_t)fn->sret_slot);
        bc_emit(ctx->bc, BC_LOAD);
        bc_emit(ctx->bc, VAL_PTR);
        bc_emit(ctx->bc, BC_ZERO_MEM);
        bc_emit(ctx->bc, (int32_t)fn->return_size);
        bc_emit(ctx->bc, BC_RET);
    } else if (fn->return_tag == VAL_VOID) {
        bc_emit(ctx->bc, BC_RET);
    } else {
        emit_zero_value(ctx, f->return_type);
        bc_emit(ctx->bc, BC_RET);
    }

    fn->frame_size = ctx->frame_high;
}

void codegen_program(Bytecode *bc, Program *prog, DiagList *diags, const SourceMap *srcmap) {
    CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.bc = bc;
    ctx.diags = diags;
    ctx.srcmap = srcmap;

    codegen_layout_globals(&ctx, prog);

    for (size_t i = 0; i < prog->funcs.count; i++) {
        int idx = bc_add_func(bc);
        register_func_index(&ctx, prog->funcs.items[i].name, idx);
        codegen_layout_func_params(&bc->funcs[idx], &prog->funcs.items[i]);
        if (strcmp(prog->funcs.items[i].name, "main") == 0) bc->main_func_index = idx;
    }

    /* prologue: call main, discard its result if it has one, then halt;
     * function bodies follow. */
    bc_emit(bc, BC_CALL);
    bc_emit(bc, bc->main_func_index);
    if (bc->main_func_index >= 0) {
        BcFunc *main_fn = &bc->funcs[bc->main_func_index];
        if (main_fn->returns_struct) {
            bc_emit(bc, BC_POP); /* discard the sret address left on the stack */
        } else if (main_fn->return_tag != VAL_VOID) {
            bc_emit(bc, BC_POP);
        }
    }
    bc_emit(bc, BC_HALT);

    for (size_t i = 0; i < prog->funcs.count; i++) {
        int idx = find_func_index(&ctx, prog->funcs.items[i].name);
        codegen_function(&ctx, &prog->funcs.items[i], idx);
    }

    free(ctx.globals.items);
    free(ctx.func_index_table);
    free(ctx.scopes);
    free(ctx.loop_stack);
}

void codegen_finish(Bytecode *bc, DiagList *diags) {
    bc_emit(bc, BC_HALT);
    if (bc->data_size > DATA_SIZE) {
        diag_addf(diags, DIAG_ERROR, NULL, 0, 0,
                  "global/string data (%u bytes) exceeds the compiler's static data segment budget (%u bytes)",
                  bc->data_size, (unsigned)DATA_SIZE);
    }
}

/* ============================================================
 * Disassembler (--emit-bytecode)
 * ============================================================ */

static const char *opcode_name(OpCode op) {
    switch (op) {
        case BC_PUSH_I32: return "PUSH_I32";
        case BC_PUSH_I64: return "PUSH_I64";
        case BC_PUSH_F32: return "PUSH_F32";
        case BC_PUSH_F64: return "PUSH_F64";
        case BC_PUSH_CHAR: return "PUSH_CHAR";
        case BC_PUSH_BOOL: return "PUSH_BOOL";
        case BC_PUSH_NULL: return "PUSH_NULL";
        case BC_PUSH_PTR_CONST: return "PUSH_PTR_CONST";
        case BC_POP: return "POP";
        case BC_DUP: return "DUP";
        case BC_ADDR_LOCAL: return "ADDR_LOCAL";
        case BC_LOAD: return "LOAD";
        case BC_STORE: return "STORE";
        case BC_COPY: return "COPY";
        case BC_ZERO_MEM: return "ZERO_MEM";
        case BC_ADD: return "ADD";
        case BC_SUB: return "SUB";
        case BC_MUL: return "MUL";
        case BC_DIV: return "DIV";
        case BC_MOD: return "MOD";
        case BC_NEG: return "NEG";
        case BC_PTR_ADD: return "PTR_ADD";
        case BC_PTR_SUB: return "PTR_SUB";
        case BC_PTR_DIFF: return "PTR_DIFF";
        case BC_LT: return "LT";
        case BC_GT: return "GT";
        case BC_LE: return "LE";
        case BC_GE: return "GE";
        case BC_EQ: return "EQ";
        case BC_NE: return "NE";
        case BC_NOT: return "NOT";
        case BC_CAST: return "CAST";
        case BC_ARR_ADDR: return "ARR_ADDR";
        case BC_MEMBER_ADDR: return "MEMBER_ADDR";
        case BC_JMP: return "JMP";
        case BC_JZ: return "JZ";
        case BC_CALL: return "CALL";
        case BC_CALL_NATIVE: return "CALL_NATIVE";
        case BC_RET: return "RET";
        case BC_HALT: return "HALT";
    }
    return "?";
}

int bc_opcode_operand_count(OpCode op) {
    switch (op) {
        case BC_PUSH_I32: case BC_PUSH_I64: case BC_PUSH_F32: case BC_PUSH_F64:
        case BC_PUSH_CHAR: case BC_PUSH_BOOL: case BC_PUSH_PTR_CONST:
        case BC_ADDR_LOCAL: case BC_LOAD: case BC_STORE: case BC_COPY: case BC_ZERO_MEM:
        case BC_PTR_DIFF: case BC_CAST: case BC_MEMBER_ADDR: case BC_JMP: case BC_JZ: case BC_CALL:
            return 1;
        case BC_ARR_ADDR: case BC_CALL_NATIVE:
            return 2;
        default:
            return 0;
    }
}

void codegen_disassemble(const Bytecode *bc) {
    printf("; %zu instructions cells, %zu functions, data segment %u bytes\n", bc->count, bc->func_count, bc->data_size);
    for (size_t i = 0; i < bc->func_count; i++) {
        printf("; func[%zu] entry=%d nparams=%d frame_size=%u returns_struct=%d\n",
               i, bc->funcs[i].entry, bc->funcs[i].nparams, bc->funcs[i].frame_size, bc->funcs[i].returns_struct);
    }
    size_t ip = 0;
    while (ip < bc->count) {
        OpCode op = (OpCode)bc->data[ip];
        printf("%6zu: %-14s", ip, opcode_name(op));
        int n = bc_opcode_operand_count(op);
        for (int k = 0; k < n && ip + 1 + (size_t)k < bc->count; k++) {
            printf(" %d", bc->data[ip + 1 + (size_t)k]);
        }
        printf("\n");
        ip += 1 + (size_t)n;
    }
}
