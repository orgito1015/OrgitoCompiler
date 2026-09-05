#ifndef PREPROCESS_H
#define PREPROCESS_H

#include <stddef.h>
#include "diag.h"

/* Fully defined here (diag.h only forward-declares the name). */
struct SourceMap;

/*
 * Preprocesses `entry_path`, expanding `#include "relative/path.oc"`
 * directives textually and recursively (the directive must be the first
 * non-whitespace text on its line; the included path is resolved relative
 * to the directory of the file containing the directive). Cycle detection
 * covers the currently-open include chain (A includes B includes A is
 * rejected; A included from two different places is not an error).
 *
 * Returns a heap-owned, NUL-terminated buffer holding every real source
 * line from every file, in inclusion order, each followed by '\n' (the
 * `#include` directive lines themselves are NOT copied into the buffer).
 * Line numbers in this buffer are a single contiguous count starting at 1;
 * `*out_map` lets callers translate any such "merged" line back to the
 * file+line it actually came from, and fetch that file's raw text for
 * diagnostics.
 *
 * On a missing file or an include cycle, an error is appended to `diags`
 * for that #include line (processing continues for the rest of that file,
 * so multiple bad includes in one run all get reported) and the return
 * value may still be non-NULL; callers should still check
 * diag_has_errors(diags) before proceeding to lex/parse the result.
 *
 * `*out_map` is always allocated on a non-NULL return, even if `diags`
 * gained errors; free it with sourcemap_free().
 */
char *preprocess_file(const char *entry_path, DiagList *diags, struct SourceMap **out_map);

/* Resolves a 1-based merged-buffer line number to the original file path
 * and 1-based line number within that file. `*out_file` is a pointer owned
 * by the map (valid until sourcemap_free); if `merged_line` is out of
 * range, `*out_file` is set to entry_path's basename-less path and
 * `*out_orig_line` to `merged_line` unchanged (best-effort fallback). */
void sourcemap_resolve(const struct SourceMap *map, size_t merged_line,
                        const char **out_file, size_t *out_orig_line);

/* Raw text (no trailing newline) of 1-based `line` of `file`, or NULL if
 * that file/line isn't known to the map. `file` is matched by the exact
 * string previously returned via sourcemap_resolve (or passed to
 * diag_addf by a caller that resolved it itself). */
const char *sourcemap_line_text(const struct SourceMap *map, const char *file, size_t line);

void sourcemap_free(struct SourceMap *map);

#endif /* PREPROCESS_H */
