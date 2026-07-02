/* rxptrans.c - REXX Server Pages (.rxp) transpiler. See rxptrans.h.
 *
 * Model (spec section 5): each source line that produces output becomes one
 * `say` built with `||` concatenation; statement tags pass through verbatim.
 *
 *   - Literal text and <?rexx= e ?> expressions on a source line are coalesced
 *     into a single  say lit1 || (e1) || lit2 || ...  . `||` joins with no
 *     separating blank, so the output is byte-exact and the line's trailing
 *     newline is supplied by SAY.
 *   - <?rexx s ?> emits `s` verbatim (trimmed), as its own line(s); a statement
 *     tag in the middle of a line flushes the parts accumulated so far as one
 *     `say`, so it splits the line into two SAYs around the statement.
 *   - The single newline immediately after a statement tag's `?>` is swallowed,
 *     so a line that is just a statement tag emits no stray `say ''`.
 *   - A blank literal line is preserved as `say ''`.
 *
 * Literal escaping is quote-doubling: REXX has no backslash escapes, so a
 * literal becomes a single-quoted string with embedded `'` doubled.
 *
 * All character handling uses character literals (no hardcoded code points), so
 * the scanner is correct under both EBCDIC (cc370) and ASCII (host tests).
 */
#include "rxptrans.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  growable byte buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    err;   /* sticky: set on allocation failure */
} obuf;

static void obuf_init(obuf *o)
{
    o->buf = NULL;
    o->len = 0;
    o->cap = 0;
    o->err = 0;
}

static void obuf_reserve(obuf *o, size_t extra)
{
    size_t want;
    size_t ncap;
    char  *nb;

    if (o->err) {
        return;
    }
    want = o->len + extra + 1;   /* +1 keeps room for a trailing NUL */
    if (want <= o->cap) {
        return;
    }
    ncap = o->cap ? o->cap : 256;
    while (ncap < want) {
        ncap *= 2;
    }
    nb = (char *)realloc(o->buf, ncap);
    if (!nb) {
        o->err = 1;
        return;
    }
    o->buf = nb;
    o->cap = ncap;
}

static void obuf_put(obuf *o, const char *s, size_t n)
{
    obuf_reserve(o, n);
    if (o->err) {
        return;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void obuf_putc(obuf *o, char c)
{
    obuf_reserve(o, 1);
    if (o->err) {
        return;
    }
    o->buf[o->len++] = c;
}

static void obuf_puts(obuf *o, const char *s)
{
    obuf_put(o, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/*  say-line assembly                                                */
/* ------------------------------------------------------------------ */

/* current output line being assembled */
typedef struct {
    obuf parts;     /* the parts joined by " || " so far                 */
    obuf lit;       /* pending raw literal not yet escaped into parts     */
    int  nparts;    /* number of parts added (to insert " || " and to     */
                    /* distinguish an empty line from a non-empty one)    */
} line_t;

static void line_init(line_t *ln)
{
    obuf_init(&ln->parts);
    obuf_init(&ln->lit);
    ln->nparts = 0;
}

/* append an already-formatted part (a quoted literal or a "(expr)") */
static void line_add(line_t *ln, const char *s, size_t n)
{
    if (ln->nparts > 0) {
        obuf_puts(&ln->parts, " || ");
    }
    obuf_put(&ln->parts, s, n);
    ln->nparts++;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* close the pending literal: if non-empty, escape it into a REXX single-quoted
 * string (doubling embedded quotes) and add it as a part. */
static void line_close_lit(line_t *ln)
{
    obuf   q;
    size_t i;

    if (ln->lit.len == 0) {
        return;
    }
    obuf_init(&q);
    obuf_putc(&q, '\'');
    for (i = 0; i < ln->lit.len; i++) {
        char c = ln->lit.buf[i];
        obuf_putc(&q, c);
        if (c == '\'') {
            obuf_putc(&q, '\'');
        }
    }
    obuf_putc(&q, '\'');
    if (!q.err) {
        line_add(ln, q.buf, q.len);
    } else {
        ln->parts.err = 1;
    }
    free(q.buf);
    ln->lit.len = 0;
}

/* add an expression tag's content (trimmed) as a parenthesized part. An empty
 * expression is ignored (it would produce invalid "()"). */
static void line_add_expr(line_t *ln, const char *s, size_t n)
{
    obuf   e;
    size_t a = 0;
    size_t b = n;

    line_close_lit(ln);
    while (a < b && is_ws(s[a])) {
        a++;
    }
    while (b > a && is_ws(s[b - 1])) {
        b--;
    }
    if (a == b) {
        return;
    }
    obuf_init(&e);
    obuf_putc(&e, '(');
    obuf_put(&e, s + a, b - a);
    obuf_putc(&e, ')');
    if (!e.err) {
        line_add(ln, e.buf, e.len);
    } else {
        ln->parts.err = 1;
    }
    free(e.buf);
}

/* flush the assembled line. emit_blank governs an empty line: at a hard newline
 * an empty line is a blank output line (`say ''`); before a statement tag or at
 * EOF an empty line emits nothing. */
static void line_flush(line_t *ln, obuf *out, int emit_blank)
{
    line_close_lit(ln);
    if (ln->nparts > 0) {
        obuf_puts(out, "say ");
        obuf_put(out, ln->parts.buf, ln->parts.len);
        obuf_putc(out, '\n');
    } else if (emit_blank) {
        obuf_puts(out, "say ''\n");
    }
    ln->parts.len = 0;
    ln->nparts = 0;
}

/* emit a statement tag's content verbatim (trimmed of surrounding whitespace),
 * preserving any internal newlines, as its own line(s). */
static void emit_stmt(obuf *out, const char *s, size_t n)
{
    size_t a = 0;
    size_t b = n;

    while (a < b && is_ws(s[a])) {
        a++;
    }
    while (b > a && is_ws(s[b - 1])) {
        b--;
    }
    if (a == b) {
        return;   /* empty statement tag -> nothing */
    }
    obuf_put(out, s + a, b - a);
    obuf_putc(out, '\n');
}

/* ------------------------------------------------------------------ */
/*  transpiler                                                       */
/* ------------------------------------------------------------------ */

int rxp_transpile(const char *src, size_t src_len, char **out_p, size_t *out_len_p)
{
    static const char OPEN[] = { '<', '?', 'r', 'e', 'x', 'x' };
    static const size_t OPENLEN = sizeof(OPEN);

    obuf   out;
    line_t ln;
    size_t i = 0;
    int    swallow_nl = 0;
    int    failed;

    obuf_init(&out);
    line_init(&ln);

    while (i < src_len) {
        /* consume one swallowed newline immediately after a statement ?> */
        if (swallow_nl) {
            swallow_nl = 0;
            if (src[i] == '\n') {
                i++;
                continue;
            }
            /* otherwise fall through and process src[i] normally */
        }

        /* opening tag? */
        if (i + OPENLEN <= src_len && memcmp(src + i, OPEN, OPENLEN) == 0) {
            int    is_expr = 0;
            size_t cstart;
            size_t j;

            i += OPENLEN;
            if (i < src_len && src[i] == '=') {
                is_expr = 1;
                i++;
            }
            cstart = i;

            /* find the closing "?>" (Phase 1: no "?>" inside REXX code) */
            j = cstart;
            while (j + 1 < src_len && !(src[j] == '?' && src[j + 1] == '>')) {
                j++;
            }
            if (!(j + 1 < src_len && src[j] == '?' && src[j + 1] == '>')) {
                /* unterminated tag */
                free(out.buf);
                free(ln.parts.buf);
                free(ln.lit.buf);
                return -1;
            }

            if (is_expr) {
                line_add_expr(&ln, src + cstart, j - cstart);
            } else {
                /* mid-line statement: flush the parts so far as the first SAY */
                line_flush(&ln, &out, 0);
                emit_stmt(&out, src + cstart, j - cstart);
                swallow_nl = 1;
            }
            i = j + 2;   /* past "?>" */
            continue;
        }

        /* literal character */
        if (src[i] == '\n') {
            line_flush(&ln, &out, 1);
        } else {
            obuf_putc(&ln.lit, src[i]);
        }
        i++;
    }

    /* end of input: flush any trailing parts (no blank-line emit) */
    line_flush(&ln, &out, 0);

    failed = out.err || ln.parts.err || ln.lit.err;
    free(ln.parts.buf);
    free(ln.lit.buf);

    if (failed) {
        free(out.buf);
        return -1;
    }

    /* NUL-terminate for callers that treat the result as a C string */
    obuf_putc(&out, '\0');
    if (out.err) {
        free(out.buf);
        return -1;
    }

    *out_p = out.buf;
    *out_len_p = out.len - 1;   /* exclude the NUL */
    return 0;
}
