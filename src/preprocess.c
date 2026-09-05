#define _DEFAULT_SOURCE

#include "preprocess.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#endif

/* ---------- growable string buffer ---------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynStr;

static void dynstr_init(DynStr *s) { s->data = NULL; s->len = 0; s->cap = 0; }

static void dynstr_reserve(DynStr *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    size_t new_cap = s->cap == 0 ? 4096 : s->cap * 2;
    while (new_cap < s->len + extra + 1) new_cap *= 2;
    char *data = (char *)realloc(s->data, new_cap);
    if (!data) { perror("realloc"); exit(EXIT_FAILURE); }
    s->data = data;
    s->cap = new_cap;
}

static void dynstr_append(DynStr *s, const char *text, size_t n) {
    dynstr_reserve(s, n);
    memcpy(s->data + s->len, text, n);
    s->len += n;
    s->data[s->len] = '\0';
}

static void dynstr_append_char(DynStr *s, char c) { dynstr_append(s, &c, 1); }

/* ---------- source map ---------- */

typedef struct {
    char   *file;
    char  **lines;
    size_t  line_count;
    size_t  line_cap;
} SourceFile;

typedef struct {
    int    file_index;
    size_t orig_line;
} MapEntry;

struct SourceMap {
    SourceFile *files;
    size_t file_count;
    size_t file_cap;

    MapEntry *map;      /* map[0] unused; valid for 1..map_count */
    size_t map_count;
    size_t map_cap;
};

typedef struct {
    char *paths[64];
    int count;
} IncludeStack;

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (!r) { perror("malloc"); exit(EXIT_FAILURE); }
    memcpy(r, s, n);
    return r;
}

static char *canonicalize_path(const char *path) {
#ifdef _WIN32
    char buf[PATH_MAX];
    DWORD n = GetFullPathNameA(path, PATH_MAX, buf, NULL);
    if (n > 0 && n < PATH_MAX) return xstrdup(buf);
    return xstrdup(path);
#else
    char buf[PATH_MAX];
    if (realpath(path, buf)) return xstrdup(buf);
    return xstrdup(path);
#endif
}

static void directory_of(const char *path, char *out, size_t out_size) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *cut = slash;
    if (bslash && (!cut || bslash > cut)) cut = bslash;
    if (!cut) {
        snprintf(out, out_size, ".");
        return;
    }
    size_t n = (size_t)(cut - path);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static void join_path(const char *dir, const char *rel, char *out, size_t out_size) {
    if (strcmp(dir, ".") == 0) {
        snprintf(out, out_size, "%s", rel);
    } else {
        snprintf(out, out_size, "%s/%s", dir, rel);
    }
}

static int sourcemap_add_file(struct SourceMap *map, const char *display_name,
                               char **lines, size_t line_count) {
    if (map->file_count >= map->file_cap) {
        size_t new_cap = map->file_cap == 0 ? 8 : map->file_cap * 2;
        SourceFile *files = (SourceFile *)realloc(map->files, new_cap * sizeof(SourceFile));
        if (!files) { perror("realloc"); exit(EXIT_FAILURE); }
        map->files = files;
        map->file_cap = new_cap;
    }
    SourceFile *sf = &map->files[map->file_count];
    sf->file = xstrdup(display_name);
    sf->lines = lines;
    sf->line_count = line_count;
    sf->line_cap = line_count;
    return (int)map->file_count++;
}

static void sourcemap_add_map_entry(struct SourceMap *map, int file_index, size_t orig_line) {
    if (map->map_count + 1 >= map->map_cap) {
        size_t new_cap = map->map_cap == 0 ? 256 : map->map_cap * 2;
        MapEntry *m = (MapEntry *)realloc(map->map, new_cap * sizeof(MapEntry));
        if (!m) { perror("realloc"); exit(EXIT_FAILURE); }
        map->map = m;
        map->map_cap = new_cap;
    }
    map->map_count++;
    map->map[map->map_count].file_index = file_index;
    map->map[map->map_count].orig_line = orig_line;
}

/* Reads the whole file and splits it into individually heap-owned lines
 * (trailing '\r'/'\n' stripped). Returns 0 on failure to open. */
static int read_lines(const char *path, char ***out_lines, size_t *out_count) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return 0; }
    rewind(f);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { perror("malloc"); exit(EXIT_FAILURE); }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';

    size_t cap = 64, count = 0;
    char **lines = (char **)malloc(cap * sizeof(char *));
    if (!lines) { perror("malloc"); exit(EXIT_FAILURE); }

    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            size_t end = i;
            if (end > start && buf[end - 1] == '\r') end--;
            size_t len = end - start;
            char *line = (char *)malloc(len + 1);
            if (!line) { perror("malloc"); exit(EXIT_FAILURE); }
            memcpy(line, buf + start, len);
            line[len] = '\0';
            if (count >= cap) {
                cap *= 2;
                lines = (char **)realloc(lines, cap * sizeof(char *));
                if (!lines) { perror("realloc"); exit(EXIT_FAILURE); }
            }
            lines[count++] = line;
            start = i + 1;
            if (i == n) break;
        }
    }
    free(buf);
    *out_lines = lines;
    *out_count = count;
    return 1;
}

/* Parses a line of the form (leading whitespace) #include "path" ; returns
 * a heap-owned copy of `path` on success, NULL if the line isn't an
 * #include directive at all. Sets *malformed if it looks like an #include
 * but is missing the closing quote. */
static char *parse_include_line(const char *line, int *malformed) {
    *malformed = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    const char *kw = "include";
    size_t kwlen = strlen(kw);
    if (strncmp(p, kw, kwlen) != 0) return NULL;
    p += kwlen;
    if (*p != ' ' && *p != '\t') return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') { *malformed = 1; return NULL; }
    p++;
    const char *start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') { *malformed = 1; return NULL; }
    size_t len = (size_t)(p - start);
    char *result = (char *)malloc(len + 1);
    if (!result) { perror("malloc"); exit(EXIT_FAILURE); }
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

static int process_file(const char *path, DiagList *diags, struct SourceMap *map,
                         DynStr *merged, IncludeStack *stack) {
    char *canonical = canonicalize_path(path);

    for (int i = 0; i < stack->count; i++) {
        if (strcmp(stack->paths[i], canonical) == 0) {
            DynStr chain;
            dynstr_init(&chain);
            for (int j = 0; j < stack->count; j++) {
                dynstr_append(&chain, stack->paths[j], strlen(stack->paths[j]));
                dynstr_append(&chain, " -> ", 4);
            }
            dynstr_append(&chain, path, strlen(path));
            diag_addf(diags, DIAG_ERROR, path, 0, 0, "include cycle detected: %s", chain.data);
            free(chain.data);
            free(canonical);
            return 0;
        }
    }

    char **lines = NULL;
    size_t line_count = 0;
    if (!read_lines(path, &lines, &line_count)) {
        diag_addf(diags, DIAG_ERROR, path, 0, 0, "cannot open include file '%s'", path);
        free(canonical);
        return 0;
    }

    if (stack->count >= 64) {
        diag_addf(diags, DIAG_ERROR, path, 0, 0, "#include nesting too deep");
        free(canonical);
        for (size_t i = 0; i < line_count; i++) free(lines[i]);
        free(lines);
        return 0;
    }
    stack->paths[stack->count++] = canonical;

    int file_index = sourcemap_add_file(map, path, lines, line_count);

    char dir[PATH_MAX];
    directory_of(path, dir, sizeof(dir));

    for (size_t i = 0; i < line_count; i++) {
        size_t orig_line = i + 1;
        int malformed = 0;
        char *inc_path = parse_include_line(lines[i], &malformed);
        if (malformed) {
            diag_addf(diags, DIAG_ERROR, path, orig_line, 1, "malformed #include directive (expected #include \"file.oc\")");
            continue;
        }
        if (inc_path) {
            char child[PATH_MAX * 2];
            join_path(dir, inc_path, child, sizeof(child));
            free(inc_path);
            process_file(child, diags, map, merged, stack);
            continue;
        }
        dynstr_append(merged, lines[i], strlen(lines[i]));
        dynstr_append_char(merged, '\n');
        sourcemap_add_map_entry(map, file_index, orig_line);
    }

    stack->count--;
    return 1;
}

char *preprocess_file(const char *entry_path, DiagList *diags, struct SourceMap **out_map) {
    struct SourceMap *map = (struct SourceMap *)calloc(1, sizeof(struct SourceMap));
    if (!map) { perror("calloc"); exit(EXIT_FAILURE); }

    DynStr merged;
    dynstr_init(&merged);

    IncludeStack stack;
    stack.count = 0;

    process_file(entry_path, diags, map, &merged, &stack);

    if (!merged.data) dynstr_append(&merged, "", 0);

    *out_map = map;
    return merged.data;
}

void sourcemap_resolve(const struct SourceMap *map, size_t merged_line,
                        const char **out_file, size_t *out_orig_line) {
    if (map && merged_line >= 1 && merged_line <= map->map_count) {
        const MapEntry *e = &map->map[merged_line];
        *out_file = map->files[e->file_index].file;
        *out_orig_line = e->orig_line;
        return;
    }
    *out_file = (map && map->file_count > 0) ? map->files[0].file : NULL;
    *out_orig_line = merged_line;
}

const char *sourcemap_line_text(const struct SourceMap *map, const char *file, size_t line) {
    if (!map || !file) return NULL;
    for (size_t i = 0; i < map->file_count; i++) {
        if (strcmp(map->files[i].file, file) == 0) {
            if (line >= 1 && line <= map->files[i].line_count) {
                return map->files[i].lines[line - 1];
            }
            return NULL;
        }
    }
    return NULL;
}

void sourcemap_free(struct SourceMap *map) {
    if (!map) return;
    for (size_t i = 0; i < map->file_count; i++) {
        for (size_t j = 0; j < map->files[i].line_count; j++) free(map->files[i].lines[j]);
        free(map->files[i].lines);
        free(map->files[i].file);
    }
    free(map->files);
    free(map->map);
    free(map);
}
