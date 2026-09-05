#ifndef VM_H
#define VM_H

#include "codegen.h"

/* Executes the given bytecode to completion. Returns normally (0 exit
 * status, left to main.c) on a clean BC_HALT. A fatal runtime error (null
 * deref, out-of-bounds access, division by zero, stack overflow, double
 * free, ...) is reported to stderr as "VM error: ..." and exits the
 * process with status 2 - matching the compile-error(1)/runtime-error(2)
 * exit code convention used by main.c. */
void vm_execute(const Bytecode *bc);

#endif /* VM_H */
