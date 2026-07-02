/* rxptrans.h - REXX Server Pages (.rxp) transpiler.
 *
 * Transforms a .rxp template (HTML interleaved with REXX tags) into a single
 * REXX program whose SAY output is the rendered page. See section 5 of
 * doc/httprexx-server-pages-spec.md for the model. The transpiler is pure C
 * (no interpreter, no MVS services), so it cross-compiles for the host and is
 * unit-tested there (test/tstrxp.c).
 *
 * Tag dialect (section 3):
 *   <?rexx  s ?>   statement block, passes through verbatim (no output)
 *   <?rexx= e ?>   expression, its value is written to the output
 *   (anything)     literal text, emitted verbatim
 *
 * Phase 1 scanner rule (section 5.1): no "?>" inside REXX code (string
 * literals or block comments that contain "?>" are deferred to Phase 3).
 */
#ifndef RXPTRANS_H
#define RXPTRANS_H

#include <stddef.h>

/* Transpile `src` (src_len bytes) into a REXX program.
 *
 * On success returns 0, stores a malloc'd NUL-terminated REXX source buffer in
 * *out (the caller frees it) and its length (excluding the NUL) in *out_len.
 *
 * Returns -1 on an allocation failure or a malformed template (an unterminated
 * "<?rexx ... ?>" tag); in that case *out is not modified.
 */
int rxp_transpile(const char *src, size_t src_len, char **out, size_t *out_len);

#endif /* RXPTRANS_H */
