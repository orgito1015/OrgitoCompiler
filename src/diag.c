#define _DEFAULT_SOURCE

#include "diag.h"
#include "preprocess.h" /* for sourcemap_line_text() */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void diag_list_init(DiagList *d) {
    d->items = NULL;
    d->count = 0;
    d->capacity = 0;
}

void diag_list_free(DiagList *d) {
    for (size_t i = 0; i < d->count; i++) {
        free(d->items[i].file);
        free(d->items[i].message);
    }
    free(d->items);
    d->items = NULL;
    d->count = 0;
    d->capacity = 0;
}

static void diag_grow(DiagList *d) {
    if (d->count < d->capacity) return;
    size_t new_cap = d->capacity == 0 ? 8 : d->capacity * 2;
    Diagnostic *items = (Diagnostic *)realloc(d->items, new_cap * sizeof(Diagnostic));
    if (!items) { perror("realloc"); exit(EXIT_FAILURE); }
    d->items = items;
    d->capacity = new_cap;
}

void diag_addf(DiagList *d, DiagSeverity sev, const char *file,
               size_t line, size_t col, const char *fmt, ...) {
    diag_grow(d);

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *message = (char *)malloc((size_t)needed + 1);
    if (!message) { perror("malloc"); exit(EXIT_FAILURE); }
    vsnprintf(message, (size_t)needed + 1, fmt, ap2);
    va_end(ap2);

    Diagnostic *diag = &d->items[d->count++];
    diag->severity = sev;
    diag->file = file ? strdup(file) : NULL;
    diag->line = line;
    diag->col = col;
    diag->message = message;
}

size_t diag_count_severity(const DiagList *d, DiagSeverity sev) {
    size_t n = 0;
    for (size_t i = 0; i < d->count; i++) {
        if (d->items[i].severity == sev) n++;
    }
    return n;
}

int diag_has_errors(const DiagList *d) {
    return diag_count_severity(d, DIAG_ERROR) > 0;
}

int diag_has_warnings(const DiagList *d) {
    return diag_count_severity(d, DIAG_WARNING) > 0;
}

static const char *sev_name(DiagSeverity sev) {
    switch (sev) {
        case DIAG_ERROR:   return "error";
        case DIAG_WARNING: return "warning";
        case DIAG_NOTE:    return "note";
    }
    return "?";
}

void diag_print_all(const DiagList *d, const SourceMap *map) {
    for (size_t i = 0; i < d->count; i++) {
        const Diagnostic *diag = &d->items[i];
        const char *file = diag->file ? diag->file : "<input>";

        if (diag->line > 0) {
            fprintf(stderr, "%s:%zu:%zu: %s: %s\n", file, diag->line, diag->col,
                    sev_name(diag->severity), diag->message);
        } else {
            fprintf(stderr, "%s: %s: %s\n", file, sev_name(diag->severity), diag->message);
        }

        if (diag->line > 0 && map) {
            const char *text = sourcemap_line_text(map, diag->file, diag->line);
            if (text) {
                fprintf(stderr, "    %s\n", text);
                fprintf(stderr, "    ");
                size_t col = diag->col > 0 ? diag->col : 1;
                for (size_t c = 1; c < col; c++) {
                    char ch = (c - 1 < strlen(text)) ? text[c - 1] : ' ';
                    fputc(ch == '\t' ? '\t' : ' ', stderr);
                }
                fprintf(stderr, "^\n");
            }
        }
    }
}
