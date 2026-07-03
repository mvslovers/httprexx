/* tstrxp.c - host unit tests for the .rxp transpiler (rxptrans.c).
 *
 * Pure C, no MVS services: runs natively via `make test-host` (fast inner
 * loop) and as an MVS load module via `make test`. The expected REXX output is
 * compared byte-for-byte. All strings use character literals only, so the test
 * is correct under both ASCII (host) and EBCDIC (cc370).
 */
#include <mbtcheck.h>

#include <stdlib.h>
#include <string.h>

#include "rxptrans.h"

/* Building blocks for the literal-chunking cases. RXP_LIT_CHUNK is 63, so these
 * let the expected strings be written exactly at the chunk boundary. */
#define A7  "aaaaaaa"                     /*  7 */
#define A56 A7 A7 A7 A7 A7 A7 A7 A7       /* 56 */
#define A62 A56 "aaaaaa"                  /* 62 */
#define A63 A62 "a"                       /* 63 = one full chunk */

/* Compare transpiler output against the expected REXX source, dumping both on
 * a mismatch so a failing case is easy to diagnose. */
static int xp_eq(const char *in, const char *want)
{
    char  *out = NULL;
    size_t out_len = 0;
    int    rc;
    int    ok;

    rc = rxp_transpile(in, strlen(in), &out, &out_len);
    if (rc != 0) {
        printf("    rxp_transpile returned %d (expected success)\n", rc);
        return 0;
    }
    ok = (out_len == strlen(want)) && (strcmp(out, want) == 0);
    if (!ok) {
        printf("    --- expected (%lu) ---\n%s\n    --- got (%lu) ---\n%s\n",
               (unsigned long)strlen(want), want, (unsigned long)out_len, out);
    }
    free(out);
    return ok;
}

int main(void)
{
    printf("=== HTTPREXX .rxp transpiler tests ===\n");

    /* 1. The spec's hello.rxp (section 5) must transpile byte-exact. */
    CHECK(xp_eq(
        "<?rexx parse arg name; if name = '' then name = 'World' ?>\n"
        "<h1>Hello, <?rexx= name ?>!</h1>\n"
        "<?rexx items = 'Hercules MVS REXX'\n"
        "do i = 1 to words(items) ?>\n"
        "  <li><?rexx= word(items, i) ?></li>\n"
        "<?rexx end ?>\n",

        "parse arg name; if name = '' then name = 'World'\n"
        "say '<h1>Hello, ' || (name) || '!</h1>'\n"
        "items = 'Hercules MVS REXX'\n"
        "do i = 1 to words(items)\n"
        "say '  <li>' || (word(items, i)) || '</li>'\n"
        "end\n"),
        "hello.rxp transpiles byte-exact");

    /* 2. Mid-line statement tag splits the line into two SAYs around it. */
    CHECK(xp_eq(
        "foo<?rexx call sub ?>bar\n",
        "say 'foo'\n"
        "call sub\n"
        "say 'bar'\n"),
        "mid-line statement tag splits into two SAYs");

    /* 3. Quote-doubling escaper for embedded single quotes. */
    CHECK(xp_eq(
        "it's a <?rexx= x ?>!\n",
        "say 'it''s a ' || (x) || '!'\n"),
        "embedded single quote is doubled");

    /* 4. A blank literal line is preserved as say ''. */
    CHECK(xp_eq(
        "a\n\nb\n",
        "say 'a'\n"
        "say ''\n"
        "say 'b'\n"),
        "blank literal line -> say ''");

    /* 5. Pure literal lines (HTML with < that is not a tag). */
    CHECK(xp_eq(
        "<html>\n</html>\n",
        "say '<html>'\n"
        "say '</html>'\n"),
        "literal angle brackets pass through");

    /* 6. Expression-only line. */
    CHECK(xp_eq(
        "<?rexx= 1 + 1 ?>\n",
        "say (1 + 1)\n"),
        "expression-only line -> say (expr)");

    /* 7. Adjacent expressions with no literal between them. */
    CHECK(xp_eq(
        "<?rexx= a ?><?rexx= b ?>\n",
        "say (a) || (b)\n"),
        "adjacent expressions concatenate");

    /* 8. A statement-only line emits no stray say '' (newline swallowed). */
    CHECK(xp_eq(
        "<?rexx say 'hi' ?>\n",
        "say 'hi'\n"),
        "statement-only line: no stray blank SAY");

    /* 9. Only a newline immediately after ?> is swallowed; a following tag is
     *    not, so the statement and the next SAY land on separate lines. */
    CHECK(xp_eq(
        "<?rexx a ?><?rexx= x ?>\n",
        "a\n"
        "say (x)\n"),
        "swallow applies only to a trailing newline");

    /* 10. No trailing newline still flushes the final line. */
    CHECK(xp_eq(
        "tail",
        "say 'tail'\n"),
        "final line without newline is flushed");

    /* 11. Empty input -> empty program. */
    CHECK(xp_eq("", ""), "empty input -> empty output");

    /* 12. An unterminated tag is a transpile error. */
    {
        char  *out = NULL;
        size_t out_len = 0;
        int    rc = rxp_transpile("<?rexx no close", 15, &out, &out_len);
        CHECK(rc == -1, "unterminated tag -> error");
        if (rc == 0) {
            free(out);
        }
    }

    /* 13. A literal of exactly RXP_LIT_CHUNK (63) bytes stays a single part —
     *     boundary, must not split. */
    CHECK(xp_eq(
        A63 "\n",
        "say '" A63 "'\n"),
        "literal of exactly 63 bytes is one chunk");

    /* 14. 64 bytes crosses the boundary: split into 63 + 1, joined by ||. */
    CHECK(xp_eq(
        A63 "a\n",
        "say '" A63 "' || 'a'\n"),
        "literal of 64 bytes splits into 63 + 1");

    /* 15. 130 bytes -> 63 + 63 + 4. */
    CHECK(xp_eq(
        A63 A63 "aaaa\n",
        "say '" A63 "' || '" A63 "' || 'aaaa'\n"),
        "literal of 130 bytes splits into 63 + 63 + 4");

    /* 16. A single quote landing as the 63rd value byte is doubled inside its
     *     chunk (chunk size is counted in value bytes, not source bytes). */
    CHECK(xp_eq(
        A62 "'b\n",
        "say '" A62 "''' || 'b'\n"),
        "embedded quote at the chunk boundary is doubled in-chunk");

    /* 17. Chunked literal composes with an expression part: the split literal
     *     and the (expr) interleave with the same || joins. */
    CHECK(xp_eq(
        A63 "a<?rexx= x ?>z\n",
        "say '" A63 "' || 'a' || (x) || 'z'\n"),
        "chunked literal composes with an expression");

    return mbt_test_summary("TSTRXP");
}
