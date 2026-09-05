#ifndef BUILTINS_H
#define BUILTINS_H

/* Canonical list of native (builtin) functions, shared between typecheck.c
 * (which registers their signatures into the ordinary function namespace,
 * so arity/type checking on calls "just works" like any other function)
 * and codegen.c/vm.c (which use the NativeId as CALL_NATIVE's operand and
 * to index the VM's native dispatch table). Keep name/arity here in sync
 * with typecheck.c's registration of each one's parameter/return types. */
typedef enum {
    NATIVE_PRINT = 0,  /* print(a, b, ...): variadic, any argument types, returns void */
    NATIVE_MALLOC,     /* malloc(long size) -> void*                                  */
    NATIVE_FREE,       /* free(void* ptr) -> void                                     */
    NATIVE_STRLEN,     /* strlen(char* s) -> int                                      */
    NATIVE_STRCMP,     /* strcmp(char* a, char* b) -> int                             */
    NATIVE_STRCPY,     /* strcpy(char* dst, char* src) -> char*                       */
    NATIVE_COUNT
} NativeId;

typedef struct {
    const char *name;
    int fixed_arity; /* -1 = variadic (print only) */
} NativeSig;

extern const NativeSig NATIVE_SIGS[NATIVE_COUNT];

#endif /* BUILTINS_H */
