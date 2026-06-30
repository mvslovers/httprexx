/* HTTPREXX.C - CGI handler for REXX Server Pages (.rexx / .rxp).
 *
 * Phase 1 skeleton (mbt v2 migration). This file currently establishes the
 * CGI entry, the httpd/httpc handshake, the UFS session, and the extension
 * router shape. The rexx370 execution glue (irxinit/irxexec/irxterm via
 * runtime LINK, the httprexx_io SAY hook) and the .rxp transpiler are added
 * in the Phase 1 implementation steps; see doc/httprexx-server-pages-spec.md.
 */
#include "clibary.h"
#include "clibppa.h"
#include "clibcrt.h"
#include "clibwto.h"
#include "clibgrt.h"
#include "libufs.h"
#include "httpcgi.h"

/* The httpd v4 macro layer (http_resp/http_printf/...) routes through the
 * HTTPX function vector and requires `httpx` in scope. Resolve it from the
 * `httpd` pointer present in each function (same convention as httplua). */
#define httpx http_get_httpx(httpd)

/* the CGI entry (@@CRT1 __start) calls main() */
int main(int argc, char **argv);

/* Send a minimal placeholder response so the skeleton is a valid CGI while
 * the real handlers are built out. Replaced by the .rexx/.rxp dispatch. */
static int dispatch(HTTPD *httpd, HTTPC *httpc, const char *script)
{
    http_resp(httpc, 200);
    http_printf(httpc, "Content-Type: text/plain\r\n");
    http_printf(httpc, "\r\n");
    http_printf(httpc, "HTTPREXX Phase 1 skeleton (mbt v2). script=%s\n",
                script ? script : "(none)");
    return 0;
}

int main(int argc, char **argv)
{
    int      rc      = 0;
    CLIBGRT *grt     = __grtget();
    CLIBCRT *crt     = __crtget();
    void    *crtapp1 = NULL;
    void    *crtapp2 = NULL;
    UFS     *ufs     = NULL;
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

    /* save the CRT app slots for our exit/external programs, then publish
     * httpd/httpc and lazily initialize the per-connection UFS session. */
    if (crt) {
        crtapp1      = crt->crtapp1;
        crtapp2      = crt->crtapp2;
        ufs          = crt->crtufs;
        crt->crtapp1 = httpd;
        crt->crtapp2 = httpc;
        crt->crtufs  = http_get_ufs(httpc);
    }

    /* Extension-based routing: HTTPD sets SCRIPT_FILENAME to the full UFS
     * path for MOD=HTTPREXX *.rexx / *.rxp. Fall back to REQUEST_PATH. */
    script = http_get_env(httpc, "SCRIPT_FILENAME");
    if (!script) {
        path = http_get_env(httpc, "REQUEST_PATH");
        if (path)
            script = strrchr(path, '/');
        if (script && *script == '/')
            script++;
    }

    rc = dispatch(httpd, httpc, script);

    if (crt) {
        crt->crtapp1 = crtapp1;
        crt->crtapp2 = crtapp2;
        crt->crtufs  = ufs;
    }

    return rc;
}
