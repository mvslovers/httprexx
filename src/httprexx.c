/* HTTPREXX.C - CGI handler for REXX Server Pages (.rexx / .rxp).
 *
 * Drives the rexx370 IRX services (installed as load modules) to run a REXX
 * program per request and capture its SAY output into the HTTP response:
 *
 *   read .rexx/.rxp from UFS  ->  (.rxp: transpile to REXX)  ->  IRXINIT a fresh
 *   LPE  ->  overwrite IRXEXTE.io_routine with httprexx_io  ->  IRXEXEC the
 *   in-storage source  ->  IRXTERM  ->  flush the buffered page to httpc.
 *
 * The IRX services are reached at runtime via the MVS LINK service (__linkds);
 * rexx370 is not linked in -- only its struct layouts are reproduced locally in
 * irxbind.h. See doc/rexx370-bindings.md for the full contract.
 */
#include "clibary.h"
#include "clibppa.h"
#include "clibcrt.h"
#include "clibwto.h"
#include "clibgrt.h"
#include "cliblink.h"
#include "libufs.h"
#include "httpcgi.h"

#include <stdlib.h>
#include <string.h>

#include "irxbind.h"
#include "rxptrans.h"

/* The httpd v4 macro layer (http_resp/http_send/...) routes through the HTTPX
 * vector and needs `httpx` in scope; resolve it from the `httpd` pointer that
 * every function below holds (same convention as httplua). */
#define httpx http_get_httpx(httpd)

/* Caps. The output buffer is fixed so the SAY path never reallocs from inside
 * IRXEXEC's call context; overflow is flagged and the request fails cleanly.
 * (Streaming large responses is a Phase-2 concern -- spec section 6.) */
#define HRX_SRC_MAX  (64u * 1024u)   /* max .rexx/.rxp source read from UFS */
#define HRX_OUT_MAX  (32u * 1024u)   /* max rendered page (buffered)        */

/* The IRXTERM R0 shim (asm/htrxterm.asm). */
extern int httprexx_irxterm(void *env) asm("HRXTERM");

/* the CGI entry (@@CRT1 __start) calls main() */
int main(int argc, char **argv);

/* Request context, reached from httprexx_io via the ENVBLOCK user field. */
typedef struct {
    char  *buf;        /* fixed output buffer (no realloc on the SAY path) */
    size_t cap;
    size_t len;
    int    overflow;
} reqctx_t;

/* ------------------------------------------------------------------ */
/*  I/O replaceable routine: SAY -> buffer (heap-free)               */
/* ------------------------------------------------------------------ */

/* Bound to IRXEXTE.io_routine after IRXINIT. Runs nested inside IRXEXEC's call
 * context, so it touches no heap: it only memcpy's into the pre-allocated
 * buffer. Recovers the request context from env->userfield (no globals). */
static int httprexx_io(int function, IRX_LSTR *data, IRX_ENVBLOCK *env)
{
    reqctx_t *ctx = env ? (reqctx_t *)env->userfield : NULL;

    switch (function) {
    case RXFWRITE:                       /* SAY: append the line + newline */
        if (ctx && data) {
            size_t n = data->len;
            if (ctx->len + n + 1 > ctx->cap) {
                ctx->overflow = 1;       /* drop -- do not grow */
            } else {
                if (n && data->pstr) {
                    memcpy(ctx->buf + ctx->len, data->pstr, n);
                    ctx->len += n;
                }
                ctx->buf[ctx->len++] = '\n';
            }
        }
        return 0;

    case RXFTWRITE:                      /* trace / error -> log, not body */
    case RXFWRITERR:
        if (data && data->pstr && data->len) {
            wtof("HTTPREXX: %.*s", (int)data->len, (char *)data->pstr);
        }
        return 0;

    case RXFREAD:                        /* PULL: EOF in Phase 1 (spec 4.2) */
    case RXFREADP:
        if (data) {
            data->len = 0;               /* empty line */
        }
        return 0;

    default:                             /* EXECIO / open / close: Phase 2 */
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  in-storage source (INSTBLK)                                      */
/* ------------------------------------------------------------------ */

/* Build an INSTBLK over a REXX source buffer: one entry per source line
 * (newline excluded). The source must outlive the IRXEXEC call -- the entries
 * point into it. Returns the malloc'd entry array (caller frees) and fills
 * *ib; returns NULL on OOM or an empty program. */
static IRX_INSTBLK_ENTRY *build_instblk(char *src, size_t len,
                                        IRX_INSTBLK *ib, int *nlines_out)
{
    IRX_INSTBLK_ENTRY *ent;
    int    nlines = 0;
    int    k;
    size_t i;
    size_t start;

    /* count lines (a trailing newline adds no empty final line) */
    start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || src[i] == '\n') {
            if (i == len && i == start) {
                break;
            }
            nlines++;
            start = i + 1;
        }
    }
    if (nlines == 0) {
        return NULL;
    }

    ent = (IRX_INSTBLK_ENTRY *)malloc((size_t)nlines * sizeof(IRX_INSTBLK_ENTRY));
    if (!ent) {
        return NULL;
    }

    k = 0;
    start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || src[i] == '\n') {
            if (i == len && i == start) {
                break;
            }
            ent[k].stmt = src + start;
            ent[k].len  = (int)(i - start);
            k++;
            start = i + 1;
        }
    }

    memset(ib, 0, sizeof(*ib));
    memcpy(ib->acronym, IRX_INSTBLK_ID, 8);
    ib->hdrlen  = IRX_INSTBLK_HDRLEN;
    ib->address = ent;
    ib->usedlen = nlines * (int)sizeof(IRX_INSTBLK_ENTRY);
    memset(ib->member, ' ', 8);          /* blank for PARSE SOURCE */
    memset(ib->ddname, ' ', 8);
    memset(ib->subcom, ' ', 8);

    *nlines_out = nlines;
    return ent;
}

/* ------------------------------------------------------------------ */
/*  responses                                                        */
/* ------------------------------------------------------------------ */

static int send_error(HTTPD *httpd, HTTPC *httpc, int code)
{
    http_resp(httpc, code);
    http_printf(httpc, "Content-Type: text/plain\r\n");
    http_printf(httpc, "\r\n");
    http_printf(httpc, "HTTPREXX: error processing request (%d)\n", code);
    return 0;
}

/* default content type text/html (spec section 6). The body is emitted with
 * http_printf (the text path), so httpd translates EBCDIC->ASCII on the wire --
 * same as httplua. `body` must be NUL-terminated; a rendered page is text. */
static int send_page(HTTPD *httpd, HTTPC *httpc, const char *body)
{
    http_resp(httpc, 200);
    http_printf(httpc, "Content-Type: text/html\r\n");
    http_printf(httpc, "\r\n");
    http_printf(httpc, "%s", body);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  run an in-storage REXX program                                   */
/* ------------------------------------------------------------------ */

static int run_rexx(HTTPD *httpd, HTTPC *httpc, char *program, size_t prog_len,
                    char *query)
{
    reqctx_t           ctx;
    IRX_INSTBLK        ib;
    IRX_INSTBLK_ENTRY *ent = NULL;
    int                nlines = 0;
    int                ok = 0;
    int                prc = 0;
    int                rc;

    /* IRXINIT parameters (the VLIST holds the ADDRESS of each, CALL,VL style) */
    static const char fcode[] = "INITENVB";
    void         *p_parmmod = NULL;
    void         *p_userfld = NULL;
    void         *p_wkblk   = NULL;
    void         *p_resv    = NULL;
    IRX_ENVBLOCK *env       = NULL;
    int           reason    = 0;
    unsigned      vinit[7];

    /* IRXEXEC parameters */
    void               *p_execblk = NULL;
    void               *p_argtab  = NULL;
    int                 p_flags   = IRXEXEC_SUBROUTINE;
    void               *p_instblk;
    void               *p_resv5   = NULL;
    void               *p_evalblk = NULL;
    void               *p_wkarea  = NULL;
    void               *p_usrfld  = NULL;
    void               *p_envblk;
    int                 rexxrc    = 0;
    IRX_ARGTABLE_ENTRY  argt[2];
    unsigned            vexec[10];

    ctx.buf = (char *)malloc(HRX_OUT_MAX + 1);   /* +1 for the NUL terminator */
    if (!ctx.buf) {
        return send_error(httpd, httpc, 500);
    }
    ctx.cap = HRX_OUT_MAX;
    ctx.len = 0;
    ctx.overflow = 0;

    ent = build_instblk(program, prog_len, &ib, &nlines);
    if (!ent) {
        free(ctx.buf);
        return send_page(httpd, httpc, "");       /* empty program -> empty page */
    }

    /* --- IRXINIT: create a fresh LPE --- */
    vinit[0] = (unsigned)fcode;
    vinit[1] = (unsigned)&p_parmmod;
    vinit[2] = (unsigned)&p_userfld;
    vinit[3] = (unsigned)&p_wkblk;
    vinit[4] = (unsigned)&p_resv;
    vinit[5] = (unsigned)&env;
    vinit[6] = (unsigned)&reason | 0x80000000U;   /* last slot: VL marker */
    rc = __linkds("IRXINIT", NULL, vinit, &prc);
    if (rc != 0 || prc != 0 || env == NULL) {
        wtof("HTTPREXX: IRXINIT failed link=%d rc=%d", rc, prc);
        goto done;                                /* ok stays 0 -> 500 */
    }

    /* --- overwrite the I/O routine and bind the request context --- */
    {
        IRX_IRXEXTE *exte = (IRX_IRXEXTE *)env->irxexte;
        if (exte) {
            exte->io_routine = (void *)httprexx_io;
            exte->irxinout   = (void *)httprexx_io;
        }
        env->userfield = &ctx;
    }

    /* --- IRXEXEC: run the in-storage source --- */
    p_instblk = &ib;
    p_envblk  = env;
    if (query && *query) {
        argt[0].str = query;
        argt[0].len = (int)strlen(query);
        memset(&argt[1], 0xFF, sizeof(argt[1]));  /* 8-byte 0xFF terminator */
        p_argtab = argt;
    }
    vexec[0] = (unsigned)&p_execblk;
    vexec[1] = (unsigned)&p_argtab;
    vexec[2] = (unsigned)&p_flags;
    vexec[3] = (unsigned)&p_instblk;
    vexec[4] = (unsigned)&p_resv5;
    vexec[5] = (unsigned)&p_evalblk;
    vexec[6] = (unsigned)&p_wkarea;
    vexec[7] = (unsigned)&p_usrfld;
    vexec[8] = (unsigned)&p_envblk;
    vexec[9] = (unsigned)&rexxrc | 0x80000000U;   /* last slot: VL marker */
    rc = __linkds("IRXEXEC", NULL, vexec, &prc);

    /* --- IRXTERM: free the LPE (always, even if IRXEXEC failed) --- */
    httprexx_irxterm(env);

    if (rc != 0 || prc != 0) {
        wtof("HTTPREXX: IRXEXEC failed link=%d rc=%d rexxrc=%d", rc, prc, rexxrc);
        goto done;
    }
    if (ctx.overflow) {
        wtof("HTTPREXX: output exceeded %u bytes", (unsigned)HRX_OUT_MAX);
        goto done;
    }
    ok = 1;

done:
    free(ent);
    if (ok) {
        ctx.buf[ctx.len] = '\0';
        rc = send_page(httpd, httpc, ctx.buf);
    } else {
        rc = send_error(httpd, httpc, 500);
    }
    free(ctx.buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  loader & routing                                                 */
/* ------------------------------------------------------------------ */

/* Read up to HRX_SRC_MAX bytes of a UFS file into a malloc'd buffer. */
static char *read_ufs(UFS *ufs, const char *path, size_t *len_out)
{
    UFSFILE *fp;
    char    *buf;
    UINT32   n;
    size_t   total = 0;

    if (!ufs) {
        return NULL;
    }
    fp = ufs_fopen(ufs, path, "r");
    if (!fp) {
        return NULL;
    }
    buf = (char *)malloc(HRX_SRC_MAX);
    if (!buf) {
        ufs_fclose(&fp);
        return NULL;
    }
    while (total < HRX_SRC_MAX &&
           (n = ufs_fread(buf + total, 1, (UINT32)(HRX_SRC_MAX - total), fp)) > 0) {
        total += n;
    }
    ufs_fclose(&fp);
    *len_out = total;
    return buf;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

static int dispatch(HTTPD *httpd, HTTPC *httpc, const char *script)
{
    CLIBCRT *crt = __crtget();
    UFS     *ufs = crt ? (UFS *)crt->crtufs : NULL;
    char    *src = NULL;
    size_t   src_len = 0;
    char    *query;
    int      rc;

    if (!script || !*script) {
        return send_error(httpd, httpc, 404);
    }
    src = read_ufs(ufs, script, &src_len);
    if (!src) {
        return send_error(httpd, httpc, 404);
    }

    query = http_get_env(httpc, "QUERY_STRING");

    if (ends_with(script, ".rxp")) {
        char  *prog = NULL;
        size_t prog_len = 0;
        if (rxp_transpile(src, src_len, &prog, &prog_len) != 0) {
            free(src);
            return send_error(httpd, httpc, 500);
        }
        free(src);
        rc = run_rexx(httpd, httpc, prog, prog_len, query);
        free(prog);
    } else {
        /* .rexx (or anything routed here): run the source directly */
        rc = run_rexx(httpd, httpc, src, src_len, query);
        free(src);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  entry                                                            */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int      rc      = 0;
    CLIBGRT *grt     = __grtget();
    CLIBCRT *crt     = __crtget();
    void    *crtapp1 = NULL;
    void    *crtapp2 = NULL;
    void    *crtufs  = NULL;
    HTTPD   *httpd   = grt->grtapp1;
    HTTPC   *httpc   = grt->grtapp2;
    char    *path    = NULL;
    char    *script  = NULL;

    if (!httpd) {
        wtof("This program %s must be called by the HTTPD web server%s",
             argv[0], "");
        printf("This program %s must be called by the HTTPD web server%s",
               argv[0], "\n");
        return 12;
    }

    /* save the CRT app slots, publish httpd/httpc, init the UFS session */
    if (crt) {
        crtapp1      = crt->crtapp1;
        crtapp2      = crt->crtapp2;
        crtufs       = crt->crtufs;
        crt->crtapp1 = httpd;
        crt->crtapp2 = httpc;
        crt->crtufs  = http_get_ufs(httpc);
    }

    /* Extension routing: HTTPD sets SCRIPT_FILENAME to the full UFS path for
     * MOD=HTTPREXX *.rexx / *.rxp. Fall back to the REQUEST_PATH basename. */
    script = http_get_env(httpc, "SCRIPT_FILENAME");
    if (!script) {
        path = http_get_env(httpc, "REQUEST_PATH");
        if (path) {
            script = strrchr(path, '/');
        }
        if (script && *script == '/') {
            script++;
        }
    }

    rc = dispatch(httpd, httpc, script);

    if (crt) {
        crt->crtapp1 = crtapp1;
        crt->crtapp2 = crtapp2;
        crt->crtufs  = crtufs;
    }
    return rc;
}
