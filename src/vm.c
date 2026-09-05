#include "vm.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtins.h"
#include "memlayout.h"

#define VALUE_STACK_MAX 65536
#define CALL_STACK_MAX  4096
#define NATIVE_MAX_ARGS 64
#define HDR_SIZE 12u /* heap block header: uint32 size, uint32 next_free, uint32 in_use */

typedef struct {
    int ret_ip;
    uint32_t this_frame_base;
    uint32_t saved_frame_base;
    int saved_func_index;
} CallFrame;

typedef struct {
    uint8_t *memory;

    Value value_stack[VALUE_STACK_MAX];
    int value_top;

    CallFrame call_stack[CALL_STACK_MAX];
    int call_top;

    uint32_t frame_base;
    uint32_t stack_alloc_top;
    int current_func_index;

    uint32_t heap_free_head;
} VM;

static void fatal(VM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "VM error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    free(vm->memory);
    exit(2);
}

static void push(VM *vm, Value v) {
    if (vm->value_top >= VALUE_STACK_MAX) fatal(vm, "operand stack overflow");
    vm->value_stack[vm->value_top++] = v;
}

static Value pop(VM *vm) {
    if (vm->value_top <= 0) fatal(vm, "operand stack underflow");
    return vm->value_stack[--vm->value_top];
}

static uint32_t value_tag_size(ValueTag t) {
    switch (t) {
        case VAL_BOOL: case VAL_CHAR: return 1;
        case VAL_INT: return 4;
        case VAL_LONG: return 8;
        case VAL_FLOAT: return 4;
        case VAL_DOUBLE: return 8;
        case VAL_PTR: return 4;
        default: return 0;
    }
}

static void check_range(VM *vm, uint32_t addr, uint32_t size, const char *what) {
    if (addr == 0) fatal(vm, "null pointer dereference (%s)", what);
    if (addr < DATA_BASE || (uint64_t)addr + size > (uint64_t)MEM_SIZE) {
        fatal(vm, "segmentation fault: out-of-bounds memory access (%s, address %u, size %u)", what, addr, size);
    }
}

static Value read_value(VM *vm, uint32_t addr, ValueTag tag) {
    check_range(vm, addr, value_tag_size(tag), "read");
    Value v;
    memset(&v, 0, sizeof(v));
    v.tag = tag;
    switch (tag) {
        case VAL_BOOL: { int8_t x; memcpy(&x, vm->memory + addr, 1); v.as.b = x; break; }
        case VAL_CHAR: { int8_t x; memcpy(&x, vm->memory + addr, 1); v.as.c = x; break; }
        case VAL_INT: { int32_t x; memcpy(&x, vm->memory + addr, 4); v.as.i = x; break; }
        case VAL_LONG: { int64_t x; memcpy(&x, vm->memory + addr, 8); v.as.l = x; break; }
        case VAL_FLOAT: { float x; memcpy(&x, vm->memory + addr, 4); v.as.f = x; break; }
        case VAL_DOUBLE: { double x; memcpy(&x, vm->memory + addr, 8); v.as.d = x; break; }
        case VAL_PTR: { int32_t x; memcpy(&x, vm->memory + addr, 4); v.as.ptr = x; break; }
        default: break;
    }
    return v;
}

static void write_value(VM *vm, uint32_t addr, ValueTag tag, Value v) {
    check_range(vm, addr, value_tag_size(tag), "write");
    switch (tag) {
        case VAL_BOOL: memcpy(vm->memory + addr, &v.as.b, 1); break;
        case VAL_CHAR: memcpy(vm->memory + addr, &v.as.c, 1); break;
        case VAL_INT: memcpy(vm->memory + addr, &v.as.i, 4); break;
        case VAL_LONG: memcpy(vm->memory + addr, &v.as.l, 8); break;
        case VAL_FLOAT: memcpy(vm->memory + addr, &v.as.f, 4); break;
        case VAL_DOUBLE: memcpy(vm->memory + addr, &v.as.d, 8); break;
        case VAL_PTR: memcpy(vm->memory + addr, &v.as.ptr, 4); break;
        default: break;
    }
}

/* ---------- numeric helpers ---------- */

static double as_double(Value v) {
    switch (v.tag) {
        case VAL_BOOL: return v.as.b;
        case VAL_CHAR: return v.as.c;
        case VAL_INT: return v.as.i;
        case VAL_LONG: return (double)v.as.l;
        case VAL_FLOAT: return v.as.f;
        case VAL_DOUBLE: return v.as.d;
        case VAL_PTR: return (double)(uint32_t)v.as.ptr;
        default: return 0.0;
    }
}

static int64_t as_int64(Value v) {
    switch (v.tag) {
        case VAL_BOOL: return v.as.b;
        case VAL_CHAR: return v.as.c;
        case VAL_INT: return v.as.i;
        case VAL_LONG: return v.as.l;
        case VAL_FLOAT: return (int64_t)v.as.f;
        case VAL_DOUBLE: return (int64_t)v.as.d;
        case VAL_PTR: return (int64_t)(uint32_t)v.as.ptr;
        default: return 0;
    }
}

static int is_truthy(Value v) {
    switch (v.tag) {
        case VAL_FLOAT: return v.as.f != 0.0f;
        case VAL_DOUBLE: return v.as.d != 0.0;
        case VAL_PTR: return v.as.ptr != 0;
        default: return as_int64(v) != 0;
    }
}

static Value arith(VM *vm, OpCode op, Value a, Value b) {
    Value r;
    r.tag = a.tag;
    if (a.tag == VAL_FLOAT) {
        float x = a.as.f, y = b.as.f, z = 0;
        switch (op) {
            case BC_ADD: z = x + y; break;
            case BC_SUB: z = x - y; break;
            case BC_MUL: z = x * y; break;
            case BC_DIV: z = x / y; break;
            default: break;
        }
        r.as.f = z;
        return r;
    }
    if (a.tag == VAL_DOUBLE) {
        double x = a.as.d, y = b.as.d, z = 0;
        switch (op) {
            case BC_ADD: z = x + y; break;
            case BC_SUB: z = x - y; break;
            case BC_MUL: z = x * y; break;
            case BC_DIV: z = x / y; break;
            default: break;
        }
        r.as.d = z;
        return r;
    }
    int64_t x = as_int64(a), y = as_int64(b), z = 0;
    switch (op) {
        case BC_ADD: z = x + y; break;
        case BC_SUB: z = x - y; break;
        case BC_MUL: z = x * y; break;
        case BC_DIV:
            if (y == 0) fatal(vm, "division by zero");
            z = x / y;
            break;
        case BC_MOD:
            if (y == 0) fatal(vm, "division by zero (modulo)");
            z = x % y;
            break;
        default: break;
    }
    switch (a.tag) {
        case VAL_BOOL: r.as.b = (int8_t)(z != 0); break;
        case VAL_CHAR: r.as.c = (int8_t)z; break;
        case VAL_INT: r.as.i = (int32_t)z; break;
        case VAL_LONG: r.as.l = z; break;
        default: r.as.i = (int32_t)z; break;
    }
    return r;
}

static Value negate(Value a) {
    Value r;
    r.tag = a.tag;
    if (a.tag == VAL_FLOAT) { r.as.f = -a.as.f; return r; }
    if (a.tag == VAL_DOUBLE) { r.as.d = -a.as.d; return r; }
    int64_t x = -as_int64(a);
    switch (a.tag) {
        case VAL_BOOL: r.as.b = (int8_t)x; break;
        case VAL_CHAR: r.as.c = (int8_t)x; break;
        case VAL_INT: r.as.i = (int32_t)x; break;
        case VAL_LONG: r.as.l = x; break;
        default: r.as.i = (int32_t)x; break;
    }
    return r;
}

static int compare(Value a, Value b, OpCode op) {
    if (a.tag == VAL_FLOAT || a.tag == VAL_DOUBLE) {
        double x = as_double(a), y = as_double(b);
        switch (op) {
            case BC_LT: return x < y;
            case BC_GT: return x > y;
            case BC_LE: return x <= y;
            case BC_GE: return x >= y;
            case BC_EQ: return x == y;
            case BC_NE: return x != y;
            default: return 0;
        }
    }
    if (a.tag == VAL_PTR) {
        uint32_t x = (uint32_t)a.as.ptr, y = (uint32_t)b.as.ptr;
        switch (op) {
            case BC_LT: return x < y;
            case BC_GT: return x > y;
            case BC_LE: return x <= y;
            case BC_GE: return x >= y;
            case BC_EQ: return x == y;
            case BC_NE: return x != y;
            default: return 0;
        }
    }
    int64_t x = as_int64(a), y = as_int64(b);
    switch (op) {
        case BC_LT: return x < y;
        case BC_GT: return x > y;
        case BC_LE: return x <= y;
        case BC_GE: return x >= y;
        case BC_EQ: return x == y;
        case BC_NE: return x != y;
        default: return 0;
    }
}

static Value cast_value(Value v, ValueTag to) {
    Value r;
    r.tag = to;
    if (to == VAL_FLOAT) { r.as.f = (float)as_double(v); return r; }
    if (to == VAL_DOUBLE) { r.as.d = as_double(v); return r; }
    if (to == VAL_PTR) { r.as.ptr = (int32_t)as_int64(v); return r; }
    int64_t x = as_int64(v);
    switch (to) {
        case VAL_BOOL: r.as.b = (int8_t)(x != 0); break;
        case VAL_CHAR: r.as.c = (int8_t)x; break;
        case VAL_INT: r.as.i = (int32_t)x; break;
        case VAL_LONG: r.as.l = x; break;
        default: break;
    }
    return r;
}

/* ---------- heap allocator: first-fit free list ---------- */

static uint32_t hdr_get_u32(VM *vm, uint32_t addr, uint32_t field_off) {
    uint32_t v;
    memcpy(&v, vm->memory + addr + field_off, 4);
    return v;
}
static void hdr_set_u32(VM *vm, uint32_t addr, uint32_t field_off, uint32_t v) {
    memcpy(vm->memory + addr + field_off, &v, 4);
}
#define HDR_SIZE_OFF  0u
#define HDR_NEXT_OFF  4u
#define HDR_INUSE_OFF 8u

static void heap_init(VM *vm) {
    hdr_set_u32(vm, HEAP_BASE, HDR_SIZE_OFF, HEAP_SIZE - HDR_SIZE);
    hdr_set_u32(vm, HEAP_BASE, HDR_NEXT_OFF, 0);
    hdr_set_u32(vm, HEAP_BASE, HDR_INUSE_OFF, 0);
    vm->heap_free_head = HEAP_BASE;
}

static uint32_t vm_malloc(VM *vm, uint32_t size) {
    if (size == 0) size = 1;
    uint32_t prev = 0;
    uint32_t cur = vm->heap_free_head;
    while (cur != 0) {
        uint32_t block_size = hdr_get_u32(vm, cur, HDR_SIZE_OFF);
        if (block_size >= size) {
            uint32_t next = hdr_get_u32(vm, cur, HDR_NEXT_OFF);
            if (prev == 0) vm->heap_free_head = next;
            else hdr_set_u32(vm, prev, HDR_NEXT_OFF, next);

            if (block_size >= size + HDR_SIZE + 8) {
                uint32_t remainder_addr = cur + HDR_SIZE + size;
                uint32_t remainder_size = block_size - size - HDR_SIZE;
                hdr_set_u32(vm, remainder_addr, HDR_SIZE_OFF, remainder_size);
                hdr_set_u32(vm, remainder_addr, HDR_INUSE_OFF, 0);
                hdr_set_u32(vm, remainder_addr, HDR_NEXT_OFF, vm->heap_free_head);
                vm->heap_free_head = remainder_addr;
                hdr_set_u32(vm, cur, HDR_SIZE_OFF, size);
            }
            hdr_set_u32(vm, cur, HDR_INUSE_OFF, 1);
            return cur + HDR_SIZE;
        }
        prev = cur;
        cur = hdr_get_u32(vm, cur, HDR_NEXT_OFF);
    }
    return 0;
}

static void vm_free(VM *vm, uint32_t ptr) {
    if (ptr == 0) return;
    if (ptr < HEAP_BASE + HDR_SIZE || ptr >= HEAP_BASE + HEAP_SIZE) {
        fatal(vm, "free(): address is not a live heap allocation");
    }
    uint32_t hdr = ptr - HDR_SIZE;
    if (hdr_get_u32(vm, hdr, HDR_INUSE_OFF) == 0) {
        fatal(vm, "double free or invalid free");
    }
    hdr_set_u32(vm, hdr, HDR_INUSE_OFF, 0);
    hdr_set_u32(vm, hdr, HDR_NEXT_OFF, vm->heap_free_head);
    vm->heap_free_head = hdr;
}

/* ---------- native functions ---------- */

static void native_print_one(VM *vm, Value v) {
    switch (v.tag) {
        case VAL_BOOL: printf("%s", v.as.b ? "true" : "false"); break;
        case VAL_CHAR: putchar((int)(unsigned char)v.as.c); break;
        case VAL_INT: printf("%d", v.as.i); break;
        case VAL_LONG: printf("%lld", (long long)v.as.l); break;
        case VAL_FLOAT: printf("%g", (double)v.as.f); break;
        case VAL_DOUBLE: printf("%g", v.as.d); break;
        case VAL_PTR: {
            uint32_t addr = (uint32_t)v.as.ptr;
            if (addr == 0) { printf("(null)"); break; }
            /* Every char* our language produces is meant to be printed as
             * text; there is no separate runtime "this is a string" tag,
             * so any in-bounds pointer is printed as a NUL-terminated C
             * string (the realistic use case), and anything else falls
             * back to a raw address. */
            if (addr >= DATA_BASE && addr < MEM_SIZE) {
                uint32_t i = 0;
                while (addr + i < MEM_SIZE && vm->memory[addr + i] != 0 && i < (1u << 20)) i++;
                fwrite(vm->memory + addr, 1, i, stdout);
            } else {
                printf("0x%x", addr);
            }
            break;
        }
        default: break;
    }
}

/* ---------- main dispatch loop ---------- */

void vm_execute(const Bytecode *bc) {
    VM vm_storage;
    VM *vm = &vm_storage;
    memset(vm, 0, sizeof(*vm));

    vm->memory = (uint8_t *)calloc(1, MEM_SIZE);
    if (!vm->memory) { fprintf(stderr, "fatal: out of memory allocating VM address space\n"); exit(2); }
    if (bc->data_size > 0) memcpy(vm->memory + DATA_BASE, bc->data_image, bc->data_size);

    heap_init(vm);
    vm->current_func_index = -1;
    vm->frame_base = STACK_BASE;
    vm->stack_alloc_top = STACK_BASE;

    size_t ip = 0;
#define FETCH() (bc->data[ip++])

    for (;;) {
        if (ip >= bc->count) fatal(vm, "instruction pointer ran off the end of the program");
        OpCode op = (OpCode)FETCH();
        switch (op) {
            case BC_PUSH_I32: { int32_t v = FETCH(); Value r; r.tag = VAL_INT; r.as.i = v; push(vm, r); break; }
            case BC_PUSH_I64: { int32_t idx = FETCH(); Value r; r.tag = VAL_LONG; r.as.l = bc->i64s[idx]; push(vm, r); break; }
            case BC_PUSH_F32: { int32_t idx = FETCH(); Value r; r.tag = VAL_FLOAT; r.as.f = (float)bc->f64s[idx]; push(vm, r); break; }
            case BC_PUSH_F64: { int32_t idx = FETCH(); Value r; r.tag = VAL_DOUBLE; r.as.d = bc->f64s[idx]; push(vm, r); break; }
            case BC_PUSH_CHAR: { int32_t v = FETCH(); Value r; r.tag = VAL_CHAR; r.as.c = (int8_t)v; push(vm, r); break; }
            case BC_PUSH_BOOL: { int32_t v = FETCH(); Value r; r.tag = VAL_BOOL; r.as.b = (int8_t)(v != 0); push(vm, r); break; }
            case BC_PUSH_NULL: { Value r; r.tag = VAL_PTR; r.as.ptr = 0; push(vm, r); break; }
            case BC_PUSH_PTR_CONST: { int32_t v = FETCH(); Value r; r.tag = VAL_PTR; r.as.ptr = v; push(vm, r); break; }

            case BC_POP: pop(vm); break;
            case BC_DUP: { Value v = pop(vm); push(vm, v); push(vm, v); break; }

            case BC_ADDR_LOCAL: {
                int32_t off = FETCH();
                Value r; r.tag = VAL_PTR; r.as.ptr = (int32_t)(vm->frame_base + (uint32_t)off);
                push(vm, r);
                break;
            }

            case BC_LOAD: { ValueTag tag = (ValueTag)FETCH(); Value addr = pop(vm); push(vm, read_value(vm, (uint32_t)addr.as.ptr, tag)); break; }
            case BC_STORE: { ValueTag tag = (ValueTag)FETCH(); Value val = pop(vm); Value addr = pop(vm); write_value(vm, (uint32_t)addr.as.ptr, tag, val); break; }
            case BC_COPY: {
                int32_t size = FETCH();
                Value src = pop(vm), dst = pop(vm);
                check_range(vm, (uint32_t)dst.as.ptr, (uint32_t)size, "struct copy (dst)");
                check_range(vm, (uint32_t)src.as.ptr, (uint32_t)size, "struct copy (src)");
                memmove(vm->memory + (uint32_t)dst.as.ptr, vm->memory + (uint32_t)src.as.ptr, (size_t)size);
                break;
            }
            case BC_ZERO_MEM: {
                int32_t size = FETCH();
                Value addr = pop(vm);
                check_range(vm, (uint32_t)addr.as.ptr, (uint32_t)size, "zero-init");
                memset(vm->memory + (uint32_t)addr.as.ptr, 0, (size_t)size);
                break;
            }

            case BC_ADD: case BC_SUB: case BC_MUL: case BC_DIV: case BC_MOD: {
                Value b = pop(vm), a = pop(vm);
                push(vm, arith(vm, op, a, b));
                break;
            }
            case BC_NEG: { Value a = pop(vm); push(vm, negate(a)); break; }

            case BC_PTR_ADD: { Value off = pop(vm), base = pop(vm); Value r; r.tag = VAL_PTR; r.as.ptr = base.as.ptr + (int32_t)as_int64(off); push(vm, r); break; }
            case BC_PTR_SUB: { Value off = pop(vm), base = pop(vm); Value r; r.tag = VAL_PTR; r.as.ptr = base.as.ptr - (int32_t)as_int64(off); push(vm, r); break; }
            case BC_PTR_DIFF: {
                int32_t elem_size = FETCH();
                if (elem_size == 0) elem_size = 1;
                Value b = pop(vm), a = pop(vm);
                Value r; r.tag = VAL_LONG;
                r.as.l = ((int64_t)(uint32_t)a.as.ptr - (int64_t)(uint32_t)b.as.ptr) / elem_size;
                push(vm, r);
                break;
            }

            case BC_LT: case BC_GT: case BC_LE: case BC_GE: case BC_EQ: case BC_NE: {
                Value b = pop(vm), a = pop(vm);
                Value r; r.tag = VAL_BOOL; r.as.b = (int8_t)compare(a, b, op);
                push(vm, r);
                break;
            }
            case BC_NOT: { Value a = pop(vm); Value r; r.tag = VAL_BOOL; r.as.b = (int8_t)!is_truthy(a); push(vm, r); break; }

            case BC_CAST: { ValueTag to = (ValueTag)FETCH(); Value v = pop(vm); push(vm, cast_value(v, to)); break; }

            case BC_ARR_ADDR: {
                int32_t elem_size = FETCH();
                int32_t len = FETCH();
                Value idxv = pop(vm), basev = pop(vm);
                int64_t idx = as_int64(idxv);
                if (idx < 0 || idx >= len) fatal(vm, "array index out of bounds (index %lld, size %d)", (long long)idx, len);
                Value r; r.tag = VAL_PTR; r.as.ptr = basev.as.ptr + (int32_t)(idx * elem_size);
                push(vm, r);
                break;
            }
            case BC_MEMBER_ADDR: {
                int32_t off = FETCH();
                Value base = pop(vm);
                Value r; r.tag = VAL_PTR; r.as.ptr = base.as.ptr + off;
                push(vm, r);
                break;
            }

            case BC_JMP: { int32_t target = FETCH(); ip = (size_t)target; break; }
            case BC_JZ: {
                int32_t target = FETCH();
                Value v = pop(vm);
                if (!is_truthy(v)) ip = (size_t)target;
                break;
            }

            case BC_CALL: {
                int32_t func_index = FETCH();
                const BcFunc *fn = &bc->funcs[func_index];
                if (vm->call_top >= CALL_STACK_MAX) fatal(vm, "call stack overflow");
                uint32_t new_base = vm->stack_alloc_top;
                uint64_t new_top = (uint64_t)new_base + fn->frame_size;
                if (new_top > (uint64_t)(STACK_BASE + STACK_SIZE)) fatal(vm, "stack overflow (out of frame memory)");

                CallFrame *cf = &vm->call_stack[vm->call_top++];
                cf->ret_ip = (int)ip;
                cf->this_frame_base = new_base;
                cf->saved_frame_base = vm->frame_base;
                cf->saved_func_index = vm->current_func_index;

                memset(vm->memory + new_base, 0, fn->frame_size);

                for (int i = fn->nparams - 1; i >= 0; i--) {
                    const BcParam *bp = &fn->params[i];
                    if (bp->is_struct) {
                        Value addr = pop(vm);
                        check_range(vm, (uint32_t)addr.as.ptr, bp->size, "call argument (struct)");
                        memcpy(vm->memory + new_base + bp->frame_offset, vm->memory + (uint32_t)addr.as.ptr, bp->size);
                    } else {
                        Value v = pop(vm);
                        write_value(vm, new_base + bp->frame_offset, bp->tag, v);
                    }
                }
                if (fn->returns_struct) {
                    Value destptr = pop(vm);
                    write_value(vm, new_base + 0, VAL_PTR, destptr);
                }

                vm->frame_base = new_base;
                vm->stack_alloc_top = (uint32_t)new_top;
                vm->current_func_index = func_index;
                ip = (size_t)fn->entry;
                break;
            }

            case BC_CALL_NATIVE: {
                int32_t native_id = FETCH();
                int32_t argc = FETCH();
                if (argc > NATIVE_MAX_ARGS) fatal(vm, "too many arguments to a native function");
                Value args[NATIVE_MAX_ARGS];
                for (int i = argc - 1; i >= 0; i--) args[i] = pop(vm);

                switch ((NativeId)native_id) {
                    case NATIVE_PRINT:
                        for (int i = 0; i < argc; i++) native_print_one(vm, args[i]);
                        printf("\n");
                        break;
                    case NATIVE_MALLOC: {
                        uint32_t size = (uint32_t)as_int64(args[0]);
                        uint32_t addr = vm_malloc(vm, size);
                        Value r; r.tag = VAL_PTR; r.as.ptr = (int32_t)addr;
                        push(vm, r);
                        break;
                    }
                    case NATIVE_FREE:
                        vm_free(vm, (uint32_t)args[0].as.ptr);
                        break;
                    case NATIVE_STRLEN: {
                        uint32_t addr = (uint32_t)args[0].as.ptr;
                        int32_t len = 0;
                        for (;;) {
                            check_range(vm, addr + (uint32_t)len, 1, "strlen");
                            if (vm->memory[addr + (uint32_t)len] == 0) break;
                            len++;
                        }
                        Value r; r.tag = VAL_INT; r.as.i = len;
                        push(vm, r);
                        break;
                    }
                    case NATIVE_STRCMP: {
                        uint32_t a = (uint32_t)args[0].as.ptr, b = (uint32_t)args[1].as.ptr;
                        int result = 0;
                        for (;;) {
                            check_range(vm, a, 1, "strcmp");
                            check_range(vm, b, 1, "strcmp");
                            uint8_t ca = vm->memory[a], cb = vm->memory[b];
                            if (ca != cb) { result = (int)ca - (int)cb; break; }
                            if (ca == 0) { result = 0; break; }
                            a++; b++;
                        }
                        Value r; r.tag = VAL_INT; r.as.i = result;
                        push(vm, r);
                        break;
                    }
                    case NATIVE_STRCPY: {
                        uint32_t dst = (uint32_t)args[0].as.ptr, src = (uint32_t)args[1].as.ptr;
                        uint32_t i = 0;
                        for (;;) {
                            check_range(vm, src + i, 1, "strcpy (src)");
                            check_range(vm, dst + i, 1, "strcpy (dst)");
                            uint8_t c = vm->memory[src + i];
                            vm->memory[dst + i] = c;
                            if (c == 0) break;
                            i++;
                        }
                        Value r; r.tag = VAL_PTR; r.as.ptr = (int32_t)dst;
                        push(vm, r);
                        break;
                    }
                    default:
                        fatal(vm, "unknown native function id %d", native_id);
                }
                break;
            }

            case BC_RET: {
                const BcFunc *fn = &bc->funcs[vm->current_func_index];
                int has_retval = (!fn->returns_struct && fn->return_tag != VAL_VOID);
                Value retval;
                retval.tag = VAL_VOID;
                if (has_retval) retval = pop(vm);

                CallFrame *frame = &vm->call_stack[--vm->call_top];
                vm->stack_alloc_top = frame->this_frame_base;
                vm->frame_base = frame->saved_frame_base;
                vm->current_func_index = frame->saved_func_index;
                ip = (size_t)frame->ret_ip;

                if (has_retval) push(vm, retval);
                break;
            }

            case BC_HALT:
                free(vm->memory);
                return;

            default:
                fatal(vm, "invalid opcode %d at ip %zu", (int)op, ip - 1);
        }
    }
#undef FETCH
}
