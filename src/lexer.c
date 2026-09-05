#define _DEFAULT_SOURCE

#include "lexer.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void lex_errorv(Lexer *lex, size_t line, size_t col, const char *fmt, va_list ap) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    const char *file;
    size_t orig_line;
    sourcemap_resolve(lex->srcmap, line, &file, &orig_line);
    diag_addf(lex->diags, DIAG_ERROR, file, orig_line, col, "%s", buf);
}

static void lex_error(Lexer *lex, size_t line, size_t col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    lex_errorv(lex, line, col, fmt, ap);
    va_end(ap);
}

static void advance(Lexer *lex) {
    if (lex->current == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    lex->pos++;
    lex->current = lex->input[lex->pos];
}

static char peek_char(const Lexer *lex, size_t ahead) {
    size_t idx = lex->pos + ahead;
    /* input is NUL-terminated; reading exactly the NUL is fine. */
    size_t len = strlen(lex->input);
    if (idx > len) return '\0';
    return lex->input[idx];
}

static void skip_ws_and_comments(Lexer *lex) {
    for (;;) {
        while (lex->current == ' ' || lex->current == '\t' ||
               lex->current == '\r' || lex->current == '\n') {
            advance(lex);
        }
        if (lex->current == '/' && peek_char(lex, 1) == '/') {
            while (lex->current != '\n' && lex->current != '\0') advance(lex);
            continue;
        }
        if (lex->current == '/' && peek_char(lex, 1) == '*') {
            size_t start_line = lex->line, start_col = lex->col;
            advance(lex); advance(lex);
            int closed = 0;
            while (lex->current != '\0') {
                if (lex->current == '*' && peek_char(lex, 1) == '/') {
                    advance(lex); advance(lex);
                    closed = 1;
                    break;
                }
                advance(lex);
            }
            if (!closed) {
                lex_error(lex, start_line, start_col, "unterminated block comment");
            }
            continue;
        }
        break;
    }
}

typedef struct { const char *name; TokenKind kind; } Keyword;

static const Keyword KEYWORDS[] = {
    { "void", TOK_KW_VOID }, { "bool", TOK_KW_BOOL }, { "char", TOK_KW_CHAR },
    { "int", TOK_KW_INT }, { "long", TOK_KW_LONG }, { "float", TOK_KW_FLOAT },
    { "double", TOK_KW_DOUBLE }, { "struct", TOK_KW_STRUCT },
    { "true", TOK_KW_TRUE }, { "false", TOK_KW_FALSE }, { "NULL", TOK_KW_NULL },
    { "if", TOK_KW_IF }, { "else", TOK_KW_ELSE }, { "while", TOK_KW_WHILE },
    { "for", TOK_KW_FOR }, { "break", TOK_KW_BREAK }, { "continue", TOK_KW_CONTINUE },
    { "return", TOK_KW_RETURN }, { "switch", TOK_KW_SWITCH }, { "case", TOK_KW_CASE },
    { "default", TOK_KW_DEFAULT }, { "sizeof", TOK_KW_SIZEOF },
};
#define NUM_KEYWORDS (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static void scan_ident_or_keyword(Lexer *lex, Token *tok) {
    size_t line = lex->line, col = lex->col;
    char buf[LEX_MAX_IDENT * 4];
    size_t n = 0;
    while (isalnum((unsigned char)lex->current) || lex->current == '_') {
        if (n + 1 < sizeof(buf)) buf[n++] = lex->current;
        advance(lex);
    }
    buf[n] = '\0';

    for (size_t i = 0; i < NUM_KEYWORDS; i++) {
        if (strcmp(buf, KEYWORDS[i].name) == 0) {
            tok->kind = KEYWORDS[i].kind;
            tok->line = line;
            tok->col = col;
            return;
        }
    }

    tok->kind = TOK_IDENT;
    if (n >= LEX_MAX_IDENT) {
        lex_error(lex, line, col, "identifier '%s' exceeds maximum length of %d", buf, LEX_MAX_IDENT - 1);
        n = LEX_MAX_IDENT - 1;
    }
    memcpy(tok->text, buf, n);
    tok->text[n] = '\0';
    tok->line = line;
    tok->col = col;
}

static void scan_number(Lexer *lex, Token *tok) {
    size_t line = lex->line, col = lex->col;
    char buf[128];
    size_t n = 0;
    int is_float_syntax = 0;

    while (isdigit((unsigned char)lex->current)) {
        if (n + 1 < sizeof(buf)) buf[n++] = lex->current;
        advance(lex);
    }
    if (lex->current == '.' && isdigit((unsigned char)peek_char(lex, 1))) {
        is_float_syntax = 1;
        if (n + 1 < sizeof(buf)) buf[n++] = lex->current;
        advance(lex);
        while (isdigit((unsigned char)lex->current)) {
            if (n + 1 < sizeof(buf)) buf[n++] = lex->current;
            advance(lex);
        }
    }
    if (lex->current == 'e' || lex->current == 'E') {
        size_t save_n = n;
        char save_cur = lex->current;
        size_t save_pos = lex->pos, save_line = lex->line, save_col = lex->col;
        if (n + 1 < sizeof(buf)) buf[n++] = lex->current;
        advance(lex);
        if (lex->current == '+' || lex->current == '-') {
            if (n + 1 < sizeof(buf)) buf[n++] = lex->current;
            advance(lex);
        }
        if (isdigit((unsigned char)lex->current)) {
            is_float_syntax = 1;
            while (isdigit((unsigned char)lex->current)) {
                if (n + 1 < sizeof(buf)) buf[n++] = lex->current;
                advance(lex);
            }
        } else {
            /* not actually an exponent (e.g. "3e" with no digits) - rewind */
            n = save_n;
            lex->pos = save_pos; lex->line = save_line; lex->col = save_col;
            lex->current = save_cur;
        }
    }
    buf[n] = '\0';

    if (n >= sizeof(buf) - 1) {
        lex_error(lex, line, col, "numeric literal too long");
    }

    if (is_float_syntax) {
        double d = strtod(buf, NULL);
        if (lex->current == 'f' || lex->current == 'F') {
            advance(lex);
            tok->kind = TOK_FLOAT_LIT;
            tok->float_value = (float)d;
        } else {
            tok->kind = TOK_DOUBLE_LIT;
            tok->double_value = d;
        }
    } else {
        unsigned long long v = strtoull(buf, NULL, 10);
        int overflowed = (v == 0xFFFFFFFFFFFFFFFFULL && buf[0] != '0');
        if (lex->current == 'L' || lex->current == 'l') {
            advance(lex);
            tok->kind = TOK_LONG_LIT;
            tok->long_value = (int64_t)v;
        } else if (v > 2147483647ULL || overflowed) {
            tok->kind = TOK_LONG_LIT;
            tok->long_value = (int64_t)v;
        } else {
            tok->kind = TOK_INT_LIT;
            tok->int_value = (int32_t)v;
        }
    }
    tok->line = line;
    tok->col = col;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Consumes and decodes one (possibly escaped) character; returns -1 on a
 * malformed escape (already reported) or unterminated input. */
static int scan_escape_or_char(Lexer *lex, size_t line, size_t col) {
    if (lex->current == '\\') {
        advance(lex);
        char c = lex->current;
        advance(lex);
        switch (c) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return '\0';
            case '\\': return '\\';
            case '\'': return '\'';
            case '"': return '"';
            case 'x': {
                int hi = hexval(lex->current);
                if (hi < 0) { lex_error(lex, line, col, "invalid hex escape"); return -1; }
                advance(lex);
                int lo = hexval(lex->current);
                int value = hi;
                if (lo >= 0) { value = hi * 16 + lo; advance(lex); }
                return value & 0xFF;
            }
            default:
                lex_error(lex, line, col, "unknown escape sequence '\\%c'", c);
                return -1;
        }
    }
    if (lex->current == '\0' || lex->current == '\n') {
        return -1;
    }
    char c = lex->current;
    advance(lex);
    return (unsigned char)c;
}

static void scan_char_literal(Lexer *lex, Token *tok) {
    size_t line = lex->line, col = lex->col;
    advance(lex); /* opening ' */
    int value = scan_escape_or_char(lex, line, col);
    if (value < 0) {
        lex_error(lex, line, col, "unterminated or invalid character literal");
    } else if (lex->current != '\'') {
        lex_error(lex, line, col, "character literal must contain exactly one character");
        while (lex->current != '\'' && lex->current != '\n' && lex->current != '\0') advance(lex);
        if (lex->current == '\'') advance(lex);
    } else {
        advance(lex); /* closing ' */
    }
    tok->kind = TOK_CHAR_LIT;
    tok->char_value = (int8_t)(value < 0 ? 0 : value);
    tok->line = line;
    tok->col = col;
}

static void scan_string_literal(Lexer *lex, Token *tok) {
    size_t line = lex->line, col = lex->col;
    advance(lex); /* opening " */
    size_t n = 0;
    int terminated = 0;
    while (lex->current != '\0') {
        if (lex->current == '"') { advance(lex); terminated = 1; break; }
        if (lex->current == '\n') break;
        int value = scan_escape_or_char(lex, line, col);
        if (value < 0) break;
        if (n + 1 >= LEX_MAX_STRING) {
            lex_error(lex, line, col, "string literal exceeds maximum length of %d bytes", LEX_MAX_STRING - 1);
            while (lex->current != '"' && lex->current != '\n' && lex->current != '\0') advance(lex);
            break;
        }
        tok->str_text[n++] = (char)value;
    }
    if (!terminated && lex->current != '"') {
        lex_error(lex, line, col, "unterminated string literal");
    } else if (lex->current == '"') {
        advance(lex);
    }
    tok->str_text[n] = '\0';
    tok->str_len = n;
    tok->kind = TOK_STRING;
    tok->line = line;
    tok->col = col;
}

static void scan_token(Lexer *lex, Token *tok) {
    for (;;) {
        skip_ws_and_comments(lex);
        if (lex->current == '\0') {
            tok->kind = TOK_EOF;
            tok->line = lex->line;
            tok->col = lex->col;
            return;
        }
        if (isalpha((unsigned char)lex->current) || lex->current == '_') {
            scan_ident_or_keyword(lex, tok);
            return;
        }
        if (isdigit((unsigned char)lex->current)) {
            scan_number(lex, tok);
            return;
        }
        if (lex->current == '"') {
            scan_string_literal(lex, tok);
            return;
        }
        if (lex->current == '\'') {
            scan_char_literal(lex, tok);
            return;
        }

        size_t line = lex->line, col = lex->col;
        char c = lex->current;
        advance(lex);

#define TOK1(k) do { tok->kind = (k); tok->line = line; tok->col = col; return; } while (0)
#define TOK2(next, k2, k1) do { \
            if (lex->current == (next)) { advance(lex); tok->kind = (k2); } \
            else { tok->kind = (k1); } \
            tok->line = line; tok->col = col; return; \
        } while (0)

        switch (c) {
            case '+':
                if (lex->current == '+') { advance(lex); TOK1(TOK_INCR); }
                TOK2('=', TOK_PLUS_ASSIGN, TOK_PLUS);
            case '-':
                if (lex->current == '-') { advance(lex); TOK1(TOK_DECR); }
                if (lex->current == '>') { advance(lex); TOK1(TOK_ARROW); }
                TOK2('=', TOK_MINUS_ASSIGN, TOK_MINUS);
            case '*': TOK2('=', TOK_STAR_ASSIGN, TOK_STAR);
            case '/': TOK2('=', TOK_SLASH_ASSIGN, TOK_SLASH);
            case '%': TOK2('=', TOK_PERCENT_ASSIGN, TOK_PERCENT);
            case '<': TOK2('=', TOK_LE, TOK_LT);
            case '>': TOK2('=', TOK_GE, TOK_GT);
            case '=': TOK2('=', TOK_EQ, TOK_ASSIGN);
            case '!': TOK2('=', TOK_NE, TOK_NOT);
            case '&': TOK2('&', TOK_AND, TOK_AMP);
            case '|':
                if (lex->current == '|') { advance(lex); TOK1(TOK_OR); }
                lex_error(lex, line, col, "unexpected character '|' (bitwise operators are not supported)");
                continue;
            case '(': TOK1(TOK_LPAREN);
            case ')': TOK1(TOK_RPAREN);
            case '{': TOK1(TOK_LBRACE);
            case '}': TOK1(TOK_RBRACE);
            case '[': TOK1(TOK_LBRACKET);
            case ']': TOK1(TOK_RBRACKET);
            case ',': TOK1(TOK_COMMA);
            case ';': TOK1(TOK_SEMI);
            case '.': TOK1(TOK_DOT);
            case ':': TOK1(TOK_COLON);
            default:
                lex_error(lex, line, col, "unexpected character '%c'", c);
                continue;
        }
#undef TOK1
#undef TOK2
    }
}

void lexer_init(Lexer *lex, const char *input, DiagList *diags, const SourceMap *srcmap) {
    lex->input = input;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->current = input[0];
    lex->lookahead_count = 0;
    lex->diags = diags;
    lex->srcmap = srcmap;
    scan_token(lex, &lex->current_token);
}

void lexer_next(Lexer *lex) {
    if (lex->lookahead_count > 0) {
        lex->current_token = lex->lookahead[0];
        lex->lookahead[0] = lex->lookahead[1];
        lex->lookahead_count--;
    } else {
        scan_token(lex, &lex->current_token);
    }
}

const Token *lexer_peek(Lexer *lex) {
    if (lex->lookahead_count < 1) {
        scan_token(lex, &lex->lookahead[lex->lookahead_count]);
        lex->lookahead_count++;
    }
    return &lex->lookahead[0];
}

const Token *lexer_peek2(Lexer *lex) {
    lexer_peek(lex);
    if (lex->lookahead_count < 2) {
        scan_token(lex, &lex->lookahead[lex->lookahead_count]);
        lex->lookahead_count++;
    }
    return &lex->lookahead[1];
}

const char *token_kind_name(TokenKind k) {
    switch (k) {
        case TOK_EOF: return "end of file";
        case TOK_INT_LIT: case TOK_LONG_LIT: case TOK_FLOAT_LIT: case TOK_DOUBLE_LIT:
            return "number";
        case TOK_CHAR_LIT: return "character literal";
        case TOK_IDENT: return "identifier";
        case TOK_STRING: return "string literal";
        case TOK_KW_VOID: return "'void'";
        case TOK_KW_BOOL: return "'bool'";
        case TOK_KW_CHAR: return "'char'";
        case TOK_KW_INT: return "'int'";
        case TOK_KW_LONG: return "'long'";
        case TOK_KW_FLOAT: return "'float'";
        case TOK_KW_DOUBLE: return "'double'";
        case TOK_KW_STRUCT: return "'struct'";
        case TOK_KW_TRUE: return "'true'";
        case TOK_KW_FALSE: return "'false'";
        case TOK_KW_NULL: return "'NULL'";
        case TOK_KW_IF: return "'if'";
        case TOK_KW_ELSE: return "'else'";
        case TOK_KW_WHILE: return "'while'";
        case TOK_KW_FOR: return "'for'";
        case TOK_KW_BREAK: return "'break'";
        case TOK_KW_CONTINUE: return "'continue'";
        case TOK_KW_RETURN: return "'return'";
        case TOK_KW_SWITCH: return "'switch'";
        case TOK_KW_CASE: return "'case'";
        case TOK_KW_DEFAULT: return "'default'";
        case TOK_KW_SIZEOF: return "'sizeof'";
        case TOK_PLUS: return "'+'";
        case TOK_MINUS: return "'-'";
        case TOK_STAR: return "'*'";
        case TOK_SLASH: return "'/'";
        case TOK_PERCENT: return "'%'";
        case TOK_LT: return "'<'";
        case TOK_GT: return "'>'";
        case TOK_LE: return "'<='";
        case TOK_GE: return "'>='";
        case TOK_EQ: return "'=='";
        case TOK_NE: return "'!='";
        case TOK_AND: return "'&&'";
        case TOK_OR: return "'||'";
        case TOK_NOT: return "'!'";
        case TOK_ASSIGN: return "'='";
        case TOK_PLUS_ASSIGN: return "'+='";
        case TOK_MINUS_ASSIGN: return "'-='";
        case TOK_STAR_ASSIGN: return "'*='";
        case TOK_SLASH_ASSIGN: return "'/='";
        case TOK_PERCENT_ASSIGN: return "'%='";
        case TOK_INCR: return "'++'";
        case TOK_DECR: return "'--'";
        case TOK_LPAREN: return "'('";
        case TOK_RPAREN: return "')'";
        case TOK_LBRACE: return "'{'";
        case TOK_RBRACE: return "'}'";
        case TOK_LBRACKET: return "'['";
        case TOK_RBRACKET: return "']'";
        case TOK_COMMA: return "','";
        case TOK_SEMI: return "';'";
        case TOK_DOT: return "'.'";
        case TOK_ARROW: return "'->'";
        case TOK_AMP: return "'&'";
        case TOK_COLON: return "':'";
    }
    return "?";
}
