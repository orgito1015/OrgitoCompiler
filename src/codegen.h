#ifndef CODEGEN_H
#define CODEGEN_H

#include <stddef.h>
#include <stdint.h>
#include "ast.h"
#include "diag.h"
#include "preprocess.h"

/* ---------- runtime value representation ---------- */

typedef enum {
    VAL_VOID = 0,
    VAL_BOOL,
    VAL_CHAR,
    VAL_INT,
    VAL_LONG,
    VAL_FLOAT,
    VAL_DOUBLE,
    VAL_PTR
} ValueTag;

typedef struct {
    ValueTag tag;
    union {
        int8_t  b;
        int8_t  c;
        int32_t i;
        int64_t l;
        float   f;
        double  d;
        int32_t ptr; /* offset into the VM's memory[]; 0 = NULL */
    } as;
} Value;

/* ---------- opcodes ----------
 * Bytecode.data is a flat array of int32 cells: one opcode cell followed
 * by however many int32 operand cells the row below lists. Jump operands
 * are absolute indices into Bytecode.data (same idiom as the original
 * OrgitoCompiler bytecode). */
typedef enum {
    BC_PUSH_I32,       /* i32 value                          -> push INT               */
    BC_PUSH_I64,       /* i64_pool_idx                        -> push LONG              */
    BC_PUSH_F32,       /* f64_pool_idx (value narrowed to float) -> push FLOAT          */
    BC_PUSH_F64,       /* f64_pool_idx                        -> push DOUBLE            */
    BC_PUSH_CHAR,      /* i32 value (0..255)                  -> push CHAR              */
    BC_PUSH_BOOL,      /* i32 0/1                              -> push BOOL             */
    BC_PUSH_NULL,      /*                                      -> push PTR(0)           */
    BC_PUSH_PTR_CONST, /* i32 absolute address                 -> push PTR(address)     */

    BC_POP,            /* v ->                                                          */
    BC_DUP,            /* v -> v v                                                      */

    BC_ADDR_LOCAL,     /* i32 frame_offset -> push PTR(frame_base + offset)             */

    BC_LOAD,           /* ValueTag tag; addr:PTR -> value (reads sizeof(tag) bytes)     */
    BC_STORE,          /* ValueTag tag; addr:PTR, value -> (writes sizeof(tag) bytes)   */
    BC_COPY,           /* i32 size; dst:PTR, src:PTR ->  (memcpy, src popped first)     */
    BC_ZERO_MEM,       /* i32 size; addr:PTR ->  (memset 0)                             */

    BC_ADD, BC_SUB, BC_MUL, BC_DIV, BC_MOD, BC_NEG, /* operands share one runtime tag   */
    BC_PTR_ADD,        /* ptr:PTR, byte_off:INT -> PTR                                  */
    BC_PTR_SUB,        /* ptr:PTR, byte_off:INT -> PTR                                  */
    BC_PTR_DIFF,       /* i32 elem_size; a:PTR, b:PTR -> LONG ((a-b)/elem_size)         */

    BC_LT, BC_GT, BC_LE, BC_GE, BC_EQ, BC_NE,       /* -> BOOL                          */
    BC_NOT,            /* v:any scalar -> BOOL                                          */

    BC_CAST,           /* ValueTag to_tag; v -> v'                                      */

    BC_ARR_ADDR,       /* i32 elem_size, i32 len; base:PTR, index:INT -> PTR (bounds-checked) */
    BC_MEMBER_ADDR,    /* i32 byte_off; base:PTR -> PTR                                 */

    BC_JMP,            /* i32 target_ip ->                                              */
    BC_JZ,             /* i32 target_ip; v -> (pop, jump if "falsy")                    */

    BC_CALL,           /* i32 func_index; args... -> retval?                            */
    BC_CALL_NATIVE,    /* i32 native_id, i32 argc; args... -> retval?                   */
    BC_RET,            /* v? ->                                                         */

    BC_HALT
} OpCode;

/* ---------- function metadata ---------- */

typedef struct {
    ValueTag tag;          /* VAL_VOID when is_struct                                   */
    uint32_t frame_offset;
    uint32_t size;
    int      is_struct;
} BcParam;

typedef struct {
    int      entry;             /* absolute bytecode offset of the function body        */
    int      nparams;
    BcParam  params[FUNC_MAX_PARAMS];
    uint32_t frame_size;        /* total bytes reserved for one call's frame            */
    uint32_t param_area_size;   /* frame bytes used by the sret slot (if any) + params  */
    int      returns_struct;
    uint32_t return_size;
    uint32_t return_align;
    uint32_t sret_slot;         /* frame offset of the hidden destination pointer (0)   */
    ValueTag return_tag;        /* meaningful iff !returns_struct                       */
} BcFunc;

typedef struct {
    int32_t *data;
    size_t count, capacity;

    int64_t *i64s;
    size_t i64_count, i64_capacity;
    double *f64s; /* shared pool for both float and double constants */
    size_t f64_count, f64_capacity;

    BcFunc *funcs;
    size_t func_count, func_capacity;

    uint8_t *data_image; /* static/global data segment image, copied to VM memory[DATA_BASE..] at load time */
    uint32_t data_size, data_capacity;

    int main_func_index;
} Bytecode;

void bc_init(Bytecode *bc);
void bc_emit(Bytecode *bc, int32_t value);
int  bc_add_i64(Bytecode *bc, int64_t v);
int  bc_add_f64(Bytecode *bc, double v);
int  bc_add_func(Bytecode *bc); /* returns index; caller fills in the BcFunc fields */
uint32_t bc_data_alloc(Bytecode *bc, uint32_t size, uint32_t align); /* returns a DATA_BASE-relative offset */
void bc_data_write(Bytecode *bc, uint32_t offset, const void *bytes, uint32_t n);
void bc_free(Bytecode *bc);

/* Compiles an already-typechecked Program (typecheck_program must have
 * reported zero errors) into bytecode. Resource-limit errors (e.g. the
 * static data segment budget) are still possible here and are reported to
 * `diags`/`srcmap` the same way earlier passes do. */
void codegen_program(Bytecode *bc, Program *prog, DiagList *diags, const SourceMap *srcmap);
void codegen_finish(Bytecode *bc, DiagList *diags);

/* Disassembles the whole program to stdout (used by --emit-bytecode). */
void codegen_disassemble(const Bytecode *bc);

/* Number of int32 operand cells following this opcode's cell. Shared with
 * optimize.c's bytecode peephole pass so both walk the instruction stream
 * identically. */
int bc_opcode_operand_count(OpCode op);

#endif /* CODEGEN_H */
