#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>
#include <stdint.h>
#include "diag.h"
#include "preprocess.h"

/* Copies `src` into a fixed `cap`-sized buffer, truncating rather than
 * overflowing and always NUL-terminating. Used everywhere an identifier
 * (LEX_MAX_IDENT) is copied into a fixed-size field - plain
 * strncpy(dst, src, cap - 1) leaves dst possibly unterminated from GCC's
 * point of view (triggering -Wstringop-truncation) even though every
 * caller's destination is freshly zeroed first. */
static inline void copy_bounded(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

typedef enum {
    TOK_EOF = 0,
    TOK_INT_LIT,
    TOK_LONG_LIT,
    TOK_FLOAT_LIT,
    TOK_DOUBLE_LIT,
    TOK_CHAR_LIT,
    TOK_IDENT,
    TOK_STRING,
    /* keywords */
    TOK_KW_VOID, TOK_KW_BOOL, TOK_KW_CHAR, TOK_KW_INT, TOK_KW_LONG,
    TOK_KW_FLOAT, TOK_KW_DOUBLE, TOK_KW_STRUCT,
    TOK_KW_TRUE, TOK_KW_FALSE, TOK_KW_NULL,
    TOK_KW_IF, TOK_KW_ELSE, TOK_KW_WHILE, TOK_KW_FOR,
    TOK_KW_BREAK, TOK_KW_CONTINUE, TOK_KW_RETURN,
    TOK_KW_SWITCH, TOK_KW_CASE, TOK_KW_DEFAULT, TOK_KW_SIZEOF,
    /* arithmetic */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    /* comparison */
    TOK_LT, TOK_GT, TOK_LE, TOK_GE, TOK_EQ, TOK_NE,
    /* logical */
    TOK_AND, TOK_OR, TOK_NOT,
    /* assignment family */
    TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN, TOK_PERCENT_ASSIGN, TOK_INCR, TOK_DECR,
    /* punctuation */
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET, TOK_COMMA, TOK_SEMI,
    TOK_DOT, TOK_ARROW, TOK_AMP, TOK_COLON
} TokenKind;

#define LEX_MAX_IDENT  64
/* Maximum bytes of a string literal after escape processing (incl. NUL). */
#define LEX_MAX_STRING 256

typedef struct {
    TokenKind kind;
    int32_t  int_value;                 /* TOK_INT_LIT    */
    int64_t  long_value;                /* TOK_LONG_LIT   */
    float    float_value;               /* TOK_FLOAT_LIT  */
    double   double_value;              /* TOK_DOUBLE_LIT */
    int8_t   char_value;                /* TOK_CHAR_LIT   */
    char     text[LEX_MAX_IDENT];       /* TOK_IDENT      */
    char     str_text[LEX_MAX_STRING];  /* TOK_STRING     */
    size_t   str_len;                   /* TOK_STRING: length in str_text, excluding the NUL */
    size_t   line;                      /* merged-buffer line (see preprocess.h) */
    size_t   col;
} Token;

typedef struct {
    const char *input;
    size_t pos;
    char   current;
    size_t line;
    size_t col;
    Token  current_token;
    Token  lookahead[2];     /* lookahead[0] = token after current, [1] = after that */
    int    lookahead_count;  /* how many of lookahead[] are valid, 0..2 */
    DiagList        *diags;
    const SourceMap *srcmap;  /* may be NULL: diagnostics fall back to raw merged line numbers */
} Lexer;

void lexer_init(Lexer *lex, const char *input, DiagList *diags, const SourceMap *srcmap);

/* Advances past current_token, scanning the next one into it. Never
 * aborts: an unrecognized character or malformed literal is reported to
 * `diags` and skipped so scanning can continue. */
void lexer_next(Lexer *lex);

/* Returns (without consuming) the token following current_token. */
const Token *lexer_peek(Lexer *lex);

/* Returns (without consuming) the token after that one. */
const Token *lexer_peek2(Lexer *lex);

/* Human-readable name for diagnostics, e.g. "';'" or "identifier". */
const char *token_kind_name(TokenKind k);

#endif /* LEXER_H */
