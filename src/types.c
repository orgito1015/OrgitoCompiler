#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- primitive singletons ---------- */

static Type g_void, g_bool, g_char, g_int, g_long, g_float, g_double;
static int  g_initialized = 0;

/* ---------- interning caches ---------- */

typedef struct { Type *base; Type *ptr; } PtrCacheEntry;
typedef struct { Type *base; uint32_t len; Type *arr; } ArrCacheEntry;
typedef struct { StructDef *sdef; Type *ty; } StructTyCacheEntry;

#define CACHE_MAX 512
static PtrCacheEntry      g_ptr_cache[CACHE_MAX];
static size_t             g_ptr_cache_n = 0;
static ArrCacheEntry      g_arr_cache[CACHE_MAX];
static size_t             g_arr_cache_n = 0;
static StructTyCacheEntry g_struct_ty_cache[STRUCT_MAX_DEFS];
static size_t             g_struct_ty_cache_n = 0;

static StructDef g_structs[STRUCT_MAX_DEFS];
static size_t    g_struct_count = 0;

void types_init(void) {
    if (g_initialized) return;
    g_initialized = 1;

    g_void   = (Type){ TY_VOID,   0, 1, NULL, 0, NULL };
    g_bool   = (Type){ TY_BOOL,   1, 1, NULL, 0, NULL };
    g_char   = (Type){ TY_CHAR,   1, 1, NULL, 0, NULL };
    g_int    = (Type){ TY_INT,    4, 4, NULL, 0, NULL };
    g_long   = (Type){ TY_LONG,   8, 8, NULL, 0, NULL };
    g_float  = (Type){ TY_FLOAT,  4, 4, NULL, 0, NULL };
    g_double = (Type){ TY_DOUBLE, 8, 8, NULL, 0, NULL };

    g_ptr_cache_n = 0;
    g_arr_cache_n = 0;
    g_struct_ty_cache_n = 0;
    g_struct_count = 0;
}

Type *type_void(void)   { return &g_void; }
Type *type_bool(void)   { return &g_bool; }
Type *type_char(void)   { return &g_char; }
Type *type_int(void)    { return &g_int; }
Type *type_long(void)   { return &g_long; }
Type *type_float(void)  { return &g_float; }
Type *type_double(void) { return &g_double; }

#define PTR_SIZE 4
#define PTR_ALIGN 4

Type *type_pointer_to(Type *base) {
    for (size_t i = 0; i < g_ptr_cache_n; i++) {
        if (g_ptr_cache[i].base == base) return g_ptr_cache[i].ptr;
    }
    Type *t = (Type *)malloc(sizeof(Type));
    if (!t) { perror("malloc"); exit(EXIT_FAILURE); }
    t->kind = TY_POINTER;
    t->size = PTR_SIZE;
    t->align = PTR_ALIGN;
    t->base = base;
    t->array_len = 0;
    t->sdef = NULL;
    if (g_ptr_cache_n < CACHE_MAX) {
        g_ptr_cache[g_ptr_cache_n].base = base;
        g_ptr_cache[g_ptr_cache_n].ptr = t;
        g_ptr_cache_n++;
    }
    return t;
}

Type *type_array_of(Type *base, uint32_t len) {
    for (size_t i = 0; i < g_arr_cache_n; i++) {
        if (g_arr_cache[i].base == base && g_arr_cache[i].len == len) return g_arr_cache[i].arr;
    }
    Type *t = (Type *)malloc(sizeof(Type));
    if (!t) { perror("malloc"); exit(EXIT_FAILURE); }
    t->kind = TY_ARRAY;
    t->size = base->size * len;
    t->align = base->align;
    t->base = base;
    t->array_len = len;
    t->sdef = NULL;
    if (g_arr_cache_n < CACHE_MAX) {
        g_arr_cache[g_arr_cache_n].base = base;
        g_arr_cache[g_arr_cache_n].len = len;
        g_arr_cache[g_arr_cache_n].arr = t;
        g_arr_cache_n++;
    }
    return t;
}

Type *type_struct(StructDef *sdef) {
    for (size_t i = 0; i < g_struct_ty_cache_n; i++) {
        if (g_struct_ty_cache[i].sdef == sdef) return g_struct_ty_cache[i].ty;
    }
    Type *t = (Type *)malloc(sizeof(Type));
    if (!t) { perror("malloc"); exit(EXIT_FAILURE); }
    t->kind = TY_STRUCT;
    t->size = sdef->size;
    t->align = sdef->align;
    t->base = NULL;
    t->array_len = 0;
    t->sdef = sdef;
    if (g_struct_ty_cache_n < STRUCT_MAX_DEFS) {
        g_struct_ty_cache[g_struct_ty_cache_n].sdef = sdef;
        g_struct_ty_cache[g_struct_ty_cache_n].ty = t;
        g_struct_ty_cache_n++;
    }
    return t;
}

static uint32_t align_up(uint32_t off, uint32_t align) {
    if (align == 0) return off;
    return (off + align - 1) / align * align;
}

StructDef *struct_define(const char *name, const StructField *fields, size_t n,
                          DiagList *d, const char *file, size_t line, size_t col) {
    if (struct_lookup(name)) {
        diag_addf(d, DIAG_ERROR, file, line, col, "redefinition of struct '%s'", name);
        return NULL;
    }
    if (g_struct_count >= STRUCT_MAX_DEFS) {
        diag_addf(d, DIAG_ERROR, file, line, col, "too many struct definitions (max %d)", STRUCT_MAX_DEFS);
        return NULL;
    }
    if (n > STRUCT_MAX_FIELDS) {
        diag_addf(d, DIAG_ERROR, file, line, col, "struct '%s' has too many fields (max %d)", name, STRUCT_MAX_FIELDS);
        return NULL;
    }

    StructDef *sdef = &g_structs[g_struct_count];
    memset(sdef, 0, sizeof(*sdef));
    strncpy(sdef->name, name, TYPE_MAX_IDENT - 1);

    uint32_t offset = 0, max_align = 1;
    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        if (fields[i].type->kind == TY_VOID) {
            diag_addf(d, DIAG_ERROR, file, line, col,
                      "field '%s' of struct '%s' has incomplete type 'void'", fields[i].name, name);
            ok = 0;
            continue;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(fields[j].name, fields[i].name) == 0) {
                diag_addf(d, DIAG_ERROR, file, line, col,
                          "duplicate field '%s' in struct '%s'", fields[i].name, name);
                ok = 0;
            }
        }
        StructField f = fields[i];
        offset = align_up(offset, f.type->align);
        f.offset = offset;
        offset += f.type->size;
        if (f.type->align > max_align) max_align = f.type->align;
        sdef->fields[sdef->field_count++] = f;
    }
    if (!ok) return NULL;

    sdef->size = align_up(offset, max_align);
    sdef->align = max_align;
    if (sdef->size == 0) sdef->size = max_align; /* empty struct: at least 1 unit of storage */
    g_struct_count++;
    return sdef;
}

StructDef *struct_lookup(const char *name) {
    for (size_t i = 0; i < g_struct_count; i++) {
        if (strcmp(g_structs[i].name, name) == 0) return &g_structs[i];
    }
    return NULL;
}

const StructField *struct_find_field(const StructDef *sdef, const char *name) {
    for (size_t i = 0; i < sdef->field_count; i++) {
        if (strcmp(sdef->fields[i].name, name) == 0) return &sdef->fields[i];
    }
    return NULL;
}

int type_equal(const Type *a, const Type *b) {
    if (a == b) return 1;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case TY_POINTER: return type_equal(a->base, b->base);
        case TY_ARRAY:   return a->array_len == b->array_len && type_equal(a->base, b->base);
        case TY_STRUCT:  return a->sdef == b->sdef;
        default:         return 1; /* primitives are singletons; kind match is enough */
    }
}

int type_is_numeric(const Type *t) {
    switch (t->kind) {
        case TY_BOOL: case TY_CHAR: case TY_INT: case TY_LONG:
        case TY_FLOAT: case TY_DOUBLE:
            return 1;
        default:
            return 0;
    }
}

int type_is_integer(const Type *t) {
    switch (t->kind) {
        case TY_BOOL: case TY_CHAR: case TY_INT: case TY_LONG:
            return 1;
        default:
            return 0;
    }
}

int type_is_float_kind(const Type *t) {
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE;
}

int type_is_pointer(const Type *t) {
    return t->kind == TY_POINTER;
}

int type_is_scalar_on_stack(const Type *t) {
    return t->kind != TY_STRUCT && t->kind != TY_ARRAY && t->kind != TY_VOID;
}

int type_assignable(const Type *dst, const Type *src) {
    if (dst->kind == TY_VOID || src->kind == TY_VOID) return 0;
    if (type_equal(dst, src)) return 1;
    if (type_is_numeric(dst) && type_is_numeric(src)) return 1;
    if (type_is_pointer(dst) && type_is_pointer(src)) return 1; /* void* is the universal pointer */
    if (type_is_pointer(dst) && src->kind == TY_ARRAY) {
        /* array-to-pointer decay: T[N] used as a value is a T* */
        return type_equal(dst->base, src->base) || dst->base->kind == TY_VOID;
    }
    return 0;
}

int type_castable(const Type *dst, const Type *src) {
    if (type_assignable(dst, src)) return 1;
    if (type_is_pointer(dst) && type_is_integer(src)) return 1;
    if (type_is_integer(dst) && type_is_pointer(src)) return 1;
    return 0;
}

const char *type_name_str(const Type *t) {
    static char bufs[8][160];
    static int slot = 0;
    slot = (slot + 1) % 8;
    char *buf = bufs[slot];

    switch (t->kind) {
        case TY_VOID:    snprintf(buf, sizeof(bufs[0]), "void");   break;
        case TY_BOOL:    snprintf(buf, sizeof(bufs[0]), "bool");   break;
        case TY_CHAR:    snprintf(buf, sizeof(bufs[0]), "char");   break;
        case TY_INT:     snprintf(buf, sizeof(bufs[0]), "int");    break;
        case TY_LONG:    snprintf(buf, sizeof(bufs[0]), "long");   break;
        case TY_FLOAT:   snprintf(buf, sizeof(bufs[0]), "float");  break;
        case TY_DOUBLE:  snprintf(buf, sizeof(bufs[0]), "double"); break;
        case TY_STRUCT:  snprintf(buf, sizeof(bufs[0]), "struct %s", t->sdef->name); break;
        case TY_POINTER: snprintf(buf, sizeof(bufs[0]), "%s*", type_name_str(t->base)); break;
        case TY_ARRAY:   snprintf(buf, sizeof(bufs[0]), "%s[%u]", type_name_str(t->base), t->array_len); break;
        default:         snprintf(buf, sizeof(bufs[0]), "<?type?>"); break;
    }
    return buf;
}
