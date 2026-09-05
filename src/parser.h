#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "diag.h"
#include "lexer.h"
#include "preprocess.h"

#define PARSER_MAX_DEPTH 2000

typedef struct {
    Lexer *lex;
    DiagList *diags;
    const SourceMap *srcmap;
    int depth;         /* expression recursion depth guard      */
    int stmt_depth;    /* statement/block recursion depth guard */
    int loop_depth;    /* enclosing loops (break/continue)      */
    int switch_depth;  /* enclosing switches (break)             */
} Parser;

void parser_init(Parser *p, Lexer *lex, DiagList *diags, const SourceMap *srcmap);

/* Parses top-level struct/global/function declarations until EOF. Never
 * aborts: a malformed declaration is reported and the parser resyncs to
 * the next likely top-level boundary and keeps going, so a single run can
 * surface multiple independent syntax errors. Check diag_has_errors()
 * before trusting/using the returned Program. */
Program parse_program(Parser *p);

#endif /* PARSER_H */
