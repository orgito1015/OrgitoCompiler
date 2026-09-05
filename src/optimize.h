#ifndef OPTIMIZE_H
#define OPTIMIZE_H

#include "ast.h"
#include "codegen.h"

/* AST-level pass: folds constant subexpressions (literal arithmetic,
 * comparisons, unary ops) into a single literal node, and removes
 * statements that can never run because they follow a return/break/
 * continue within the same statement list (recursively, through every
 * nested block/if/while/for/switch). Mutates `prog` in place. */
void optimize_ast(Program *prog);

/* Bytecode-level peephole pass: collapses a JMP/JZ whose target is itself
 * an unconditional JMP into a direct jump to that JMP's own target.
 * Mutates `bc` in place. */
void optimize_bytecode(Bytecode *bc);

#endif /* OPTIMIZE_H */
