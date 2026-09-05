#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE
} DiagSeverity;

typedef struct {
    DiagSeverity severity;
    char  *file;     /* heap-owned; NULL means "<input>" */
    size_t line;     /* 1-based; 0 = unknown/not shown */
    size_t col;      /* 1-based; 0 = unknown/not shown */
    char  *message;  /* heap-owned, already fully formatted */
} Diagnostic;

typedef struct {
    Diagnostic *items;
    size_t count;
    size_t capacity;
} DiagList;

/* Opaque here; fully defined in preprocess.h. diag.c only needs a pointer
 * to look up source text for the caret line via sourcemap_line_text(). */
typedef struct SourceMap SourceMap;

void diag_list_init(DiagList *d);
void diag_list_free(DiagList *d);

/* Records one diagnostic. `file` may be NULL ("<input>"). `line`/`col` of 0
 * suppress the location prefix and the caret line. */
void diag_addf(DiagList *d, DiagSeverity sev, const char *file,
               size_t line, size_t col, const char *fmt, ...);

size_t diag_count_severity(const DiagList *d, DiagSeverity sev);
int    diag_has_errors(const DiagList *d);
int    diag_has_warnings(const DiagList *d);

/* Prints every diagnostic to stderr in recorded order, each followed by a
 * source line + '^' caret when `map` can supply the text for (file,line).
 * `map` may be NULL (no source text is shown, only the message line). */
void diag_print_all(const DiagList *d, const SourceMap *map);

#endif /* DIAG_H */
