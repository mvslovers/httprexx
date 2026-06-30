# HTTPREXX Phase 1 — Manual Smoke Test

End-to-end rendering runs on the live MVS/Hercules system and is a **manual**
check. This checklist reproduces it: deploy HTTPREXX, configure routing, upload
the two sample pages, and confirm the rendered output.

The build itself is already verified off-target: the skeleton and full handler
compile and link clean under mbt v2 (cc370 + as370 + ld370), and the `.rxp`
transpiler passes its host unit tests (`make test-host`, 12/12). What only the
target can show is the IRX runtime path (IRXINIT → SAY hook → IRXEXEC → IRXTERM).

---

## Prerequisites

1. **rexx370 IRX modules installed.** `IRXINIT`, `IRXEXEC`, `IRXTERM`,
   `IRXANCHR`, `IRXPARMS` must be loadable by the httpd address space (LINKLST,
   LPA, or the httpd STEPLIB). HTTPREXX LINKs/LOADs them by name at runtime.
2. **httpd 4.0.0-dev (UFS-backed) running**, with a DOCROOT on UFS.
3. **HTTPREXX deployed** to the httpd load library (the one httpd loads MOD=
   modules from, e.g. `HTTPD.LINKLIBT`). `make deploy` packs and RECEIVEs into
   the deploy LINKLIB; copy member `HTTPREXX` from there into the httpd load
   library (IEBCOPY), then restart httpd — MOD= modules are loaded at STARTUP.

---

## 1. Configure routing (httpd parmlib, DD:HTTPPRM)

Add extension routes so httpd sets `SCRIPT_FILENAME` to the full UFS path:

```
MOD=HTTPREXX *.rexx
MOD=HTTPREXX *.rxp
```

Restart httpd to load the module and the new directives.

## 2. Upload the sample pages to the UFS DOCROOT

`examples/hello.rexx` and `examples/hello.rxp` (in this repo). Place them under
DOCROOT, EBCDIC, e.g. `/hello.rexx` and `/hello.rxp`.

## 3. Run the requests

### `.rexx` — direct execution

```sh
curl -s 'http://HOST:PORT/hello.rexx'
curl -s 'http://HOST:PORT/hello.rexx?name=Mike'
```

Expected body (default, then with the query string):

```html
<html><body>
<h1>Hello, World!</h1>
<p>Rendered by a .rexx script under HTTPREXX.</p>
</body></html>
```

```html
<html><body>
<h1>Hello, Mike!</h1>
<p>Rendered by a .rexx script under HTTPREXX.</p>
</body></html>
```

`Content-Type: text/html`, HTTP 200.

### `.rxp` — transpiled Server Page

```sh
curl -s 'http://HOST:PORT/hello.rxp'
curl -s 'http://HOST:PORT/hello.rxp?name=Mike'
```

The transpiler turns `hello.rxp` into this REXX (verified by the host test):

```rexx
parse arg name; if name = '' then name = 'World'
say '<h1>Hello, ' || (name) || '!</h1>'
items = 'Hercules MVS REXX'
do i = 1 to words(items)
say '  <li>' || (word(items, i)) || '</li>'
end
```

Expected body (default; with `?name=Mike` the heading becomes `Hello, Mike!`):

```html
<h1>Hello, World!</h1>
  <li>Hercules</li>
  <li>MVS</li>
  <li>REXX</li>
```

`Content-Type: text/html`, HTTP 200.

## 4. Concurrency (optional)

Fire several `.rxp` requests in parallel; each must render independently (one
fresh LPE per request, no global lock):

```sh
for i in 1 2 3 4 5; do curl -s "http://HOST:PORT/hello.rxp?name=W$i" & done; wait
```

---

## What success proves

- IRXINIT builds an LPE reachable via `__linkds`, and the post-init overwrite of
  `IRXEXTE.io_routine` actually redirects SAY (the MODNAMET override is a no-op
  in rexx370, so this fallback is what makes SAY land in the buffer).
- IRXEXEC runs the in-storage INSTBLK with the ENVBLOCK passed in P9, and the
  query string reaches the program via `PARSE ARG` (the ARGTABLE).
- The `HRXTERM` asm shim tears the LPE down (R0 linkage) with no leak.
- Buffered output yields a clean `text/html` page (or a clean 500 on failure —
  nothing half-sent).

## Diagnostics

On failure HTTPREXX returns HTTP 500 and writes a `wtof` line to the system log:

- `HTTPREXX: IRXINIT failed link=.. rc=..` — LPE not created (IRX modules not
  installed/loadable, or init error).
- `HTTPREXX: IRXEXEC failed link=.. rc=.. rexxrc=..` — exec error (RC is the
  IRXEXEC return code).
- `HTTPREXX: output exceeded 32768 bytes` — the page is larger than the fixed
  buffer (Phase 2 adds streaming).
- A 404 means the path was not found in UFS (check DOCROOT and the upload).

## Out of scope for this smoke test (flagged Phase-1 deferrals)

These are intentionally not exercised (see doc/rexx370-bindings.md section 7):

- **CGI metadata as pool variables** (`REQUEST_METHOD`, `HTTP.` stem) — needs the
  `IRXEXCOM` path; the query string via `PARSE ARG` is wired and is all the
  samples use.
- **POST body via `PULL`** — rexx370 has no data-stack API yet; `httprexx_io`
  returns EOF for `PULL` (sanctioned by spec section 4.2).
- **Header override / streaming / compiled-exec cache** — Phase 2/3.
