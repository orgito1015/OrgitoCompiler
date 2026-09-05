#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>
#include <stdint.h>
#include "diag.h"

#define TYPE_MAX_IDENT     64
#define STRUCT_MAX_FIELDS  64
#define STRUCT_MAX_DEFS    128
#define STRUCT_MAX_PARAMS  16 /* re-exported for ast.h/codegen.h via FUNC_MAX_PARAMS below */
#define FUNC_MAX_PARAMS    16

typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,
    TY_INT,
    TY_LONG,
    TY_FLOAT,
    TY_DOUBLE,
    TY_POINTER,
    TY_ARRAY,
    TY_STRUCT
} TypeKind;

typedef struct StructDef StructDef;

typedef struct Type {
    TypeKind kind;
    uint32_t size;
    uint32_t align;
    struct Type *base;   /* TY_POINTER: pointee. TY_ARRAY: element type. */
    uint32_t array_len;  /* TY_ARRAY only */
    StructDef *sdef;     /* TY_STRUCT only */
} Type;

typedef struct {
    char name[TYPE_MAX_IDENT];
    Type *type;
    uint32_t offset;
} StructField;

struct StructDef {
    char name[TYPE_MAX_IDENT];
    StructField fields[STRUCT_MAX_FIELDS];
    size_t field_count;
    uint32_t size;
    uint32_t align;
};

/* Must be called exactly once before any other function in this header
 * is used. Not thread-safe (this project is single-threaded). */
void types_init(void);

Type *type_void(void);
Type *type_bool(void);
Type *type_char(void);
Type *type_int(void);
Type *type_long(void);
Type *type_float(void);
Type *type_double(void);

/* All of the following intern their result: two calls with equal arguments
 * return the same Type* pointer, so type_equal() can be a pointer compare
 * for everything except structs (compared by StructDef* identity, which is
 * itself unique per name via struct_define/struct_lookup). */
Type *type_pointer_to(Type *base);
Type *type_array_of(Type *base, uint32_t len);
Type *type_struct(StructDef *sdef);

/* Defines a new struct type from a field list in declaration order. Field
 * offsets/size/align are computed with ordinary C layout rules (each field
 * padded to its own alignment; struct size rounded up to the max member
 * alignment). Reports to `d` (via diag_addf with the given location) and
 * returns NULL on: redefinition of `name`, an unknown/void field type, a
 * duplicate field name, or too many fields. */
StructDef *struct_define(const char *name, const StructField *fields, size_t n,
                          DiagList *d, const char *file, size_t line, size_t col);
StructDef *struct_lookup(const char *name);
const StructField *struct_find_field(const StructDef *sdef, const char *name);

int type_equal(const Type *a, const Type *b);
int type_is_numeric(const Type *t);   /* bool/char/int/long/float/double     */
int type_is_integer(const Type *t);   /* bool/char/int/long                  */
int type_is_float_kind(const Type *t);/* float/double                       */
int type_is_pointer(const Type *t);
int type_is_scalar_on_stack(const Type *t); /* fits in a Value cell (not struct/array/void) */

/* True if a value of type `src` may be assigned/passed/returned where `dst`
 * is expected: identical types; numeric-to-numeric (any direction, C-like
 * implicit conversion); any pointer <-> any pointer (void* is the wildcard,
 * NULL literal's type is a void* per the parser); exact match required for
 * struct/array. */
int type_assignable(const Type *dst, const Type *src);
/* True if an explicit `(dst)src` cast is legal: any assignable pair, plus
 * numeric<->pointer reinterpretation. Structs/arrays/void are never
 * castable. */
int type_castable(const Type *dst, const Type *src);

/* Returns a pointer into a small internal ring of static buffers - safe to
 * use several times in one printf() call, not safe to retain long-term. */
const char *type_name_str(const Type *t);

#endif /* TYPES_H */
