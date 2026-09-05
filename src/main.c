#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "codegen.h"
#include "diag.h"
#include "lexer.h"
#include "optimize.h"
#include "parser.h"
#include "preprocess.h"
#include "typecheck.h"
#include "types.h"
#include "vm.h"

static size_t g_diags_printed = 0;

static void flush_diags(DiagList *diags, const SourceMap *srcmap) {
    if (diags->count <= g_diags_printed) return;
    DiagList slice;
    slice.items = diags->items + g_diags_printed;
    slice.count = diags->count - g_diags_printed;
    slice.capacity = 0;
    diag_print_all(&slice, srcmap);
    g_diags_printed = diags->count;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "OrgitoCompiler - a C-subset compiler and virtual machine\n");
    fprintf(stderr, "Usage: %s [options] <file.oc>\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -O0              disable optimizations\n");
    fprintf(stderr, "  -O1              constant folding, dead-code elimination, bytecode peephole cleanup (default)\n");
    fprintf(stderr, "  -Wall            show warnings (default)\n");
    fprintf(stderr, "  -Werror          treat warnings as errors\n");
    fprintf(stderr, "  --emit-ast       print the parsed AST and exit (no typecheck/codegen)\n");
    fprintf(stderr, "  --emit-bytecode  print a disassembly of the compiled bytecode and exit (no execution)\n");
    fprintf(stderr, "  -h, --help       show this message\n");
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int optimize = 1;
    int werror = 0;
    int emit_ast = 0;
    int emit_bytecode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O0") == 0) optimize = 0;
        else if (strcmp(argv[i], "-O1") == 0) optimize = 1;
        else if (strcmp(argv[i], "-Wall") == 0) { /* warnings are always collected; this is the default */ }
        else if (strcmp(argv[i], "-Werror") == 0) werror = 1;
        else if (strcmp(argv[i], "--emit-ast") == 0) emit_ast = 1;
        else if (strcmp(argv[i], "--emit-bytecode") == 0) emit_bytecode = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option '%s'\n\n", argv[i]); print_usage(argv[0]); return 1; }
        else if (!path) path = argv[i];
        else { fprintf(stderr, "unexpected extra argument '%s'\n", argv[i]); return 1; }
    }
    if (!path) { print_usage(argv[0]); return 1; }

    types_init();

    DiagList diags;
    diag_list_init(&diags);

    SourceMap *srcmap = NULL;
    char *source = preprocess_file(path, &diags, &srcmap);
    flush_diags(&diags, srcmap);
    if (diag_has_errors(&diags)) {
        free(source);
        sourcemap_free(srcmap);
        diag_list_free(&diags);
        return 1;
    }

    Lexer lex;
    lexer_init(&lex, source, &diags, srcmap);
    Parser parser;
    parser_init(&parser, &lex, &diags, srcmap);
    Program prog = parse_program(&parser);
    flush_diags(&diags, srcmap);

    if (!diag_has_errors(&diags)) {
        typecheck_program(&prog, &diags, srcmap);
        flush_diags(&diags, srcmap);
    }

    int fatal_now = diag_has_errors(&diags) || (werror && diag_has_warnings(&diags));
    if (fatal_now) {
        program_free(&prog);
        free(source);
        sourcemap_free(srcmap);
        diag_list_free(&diags);
        return 1;
    }

    if (optimize) optimize_ast(&prog);

    if (emit_ast) {
        ast_dump_program(&prog);
        program_free(&prog);
        free(source);
        sourcemap_free(srcmap);
        diag_list_free(&diags);
        return 0;
    }

    Bytecode bc;
    bc_init(&bc);
    codegen_program(&bc, &prog, &diags, srcmap);
    codegen_finish(&bc, &diags);
    flush_diags(&diags, srcmap);

    if (diag_has_errors(&diags) || (werror && diag_has_warnings(&diags))) {
        bc_free(&bc);
        program_free(&prog);
        free(source);
        sourcemap_free(srcmap);
        diag_list_free(&diags);
        return 1;
    }

    if (optimize) optimize_bytecode(&bc);

    if (emit_bytecode) {
        codegen_disassemble(&bc);
        bc_free(&bc);
        program_free(&prog);
        free(source);
        sourcemap_free(srcmap);
        diag_list_free(&diags);
        return 0;
    }

    program_free(&prog);
    free(source);
    sourcemap_free(srcmap);
    diag_list_free(&diags);

    vm_execute(&bc); /* exits the process directly (status 2) on a fatal runtime error */
    bc_free(&bc);
    return 0;
}
