#ifndef TYPECHECK_H
#define TYPECHECK_H

#include "ast.h"
#include "diag.h"
#include "preprocess.h"

/* Walks the whole program, resolving every identifier against a scoped
 * environment (locals -> globals -> functions/natives -> structs),
 * annotating every Expr's `type` field, and enforcing assignment/operand/
 * argument/member/cast compatibility. Also emits warnings for unused
 * locals/parameters and for code following return/break/continue in the
 * same statement list. Never aborts early - keeps walking so one run
 * reports every independent problem it finds. Codegen must not run unless
 * diag_has_errors(diags) is false afterwards. */
void typecheck_program(Program *prog, DiagList *diags, const SourceMap *srcmap);

#endif /* TYPECHECK_H */
