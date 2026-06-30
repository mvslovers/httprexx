# HTTPREXX Server Pages (`.rexx` / `.rxp`) — Specification

**Project:** httprexx (rexx370 edition) · **Status:** Final · **Version:** 1.1
**Engine:** rexx370 (TSO/E V2-compatible, modular, **reentrant**) — *not* brexx370.

A CGI module that either executes `.rexx` files from the UFS directly, or renders
**REXX Server Pages** (`.rxp`) — PHP-/JSP-style mixing of HTML and REXX via tags,
transpiled into a single REXX program. Sibling to the HTTPLUA Server Pages spec; the
structure is deliberately parallel, the engine-specific mechanics differ.

---

## 1. Motivation: why rexx370 changes the design

The existing `httprexx` was built for **brexx370**, which is neither modular nor
reentrant. That forced a heavy execution model:

- A server-wide `lock(link_rexx, LOCK_EXC)` so **only one REXX runs at a time** — the
  worker pool is serialized down to one REXX request.
- Three SVC 99 temp datasets per request (STDIN/STDOUT/STDERR); `SAY` writes to the
  STDOUT dataset, which is then read back and sniffed.
- Request variables passed through a `DD:HTTPVARS` dataset.
- `__linkds("BREXX", …)` to a load module, plus a separate `HTTPSAY` module the script
  must call explicitly for direct output.

**rexx370** removes every one of those constraints, because it is reentrant and
implements the TSO/E V2 programming services and replaceable routines:

| Old (brexx370)                         | New (rexx370)                                              |
|----------------------------------------|-----------------------------------------------------------|
| global `LOCK_EXC` around all REXX      | reentrant LPE per request — concurrent workers            |
| `__linkds("BREXX")` to a load module   | `irx_exec_run()` from an in-storage source buffer         |
| `SAY` → STDOUT dataset → read back      | I/O replaceable routine → in-memory buffer (no dataset)  |
| request vars via `DD:HTTPVARS` dataset  | request vars set directly in the variable pool           |
| separate `HTTPSAY` call module          | just `SAY` — the I/O routine captures it (one output path) |

The key enabler is exactly the advantage noted up front: REXX routes all line I/O
(`SAY`, `PULL`, trace, error) through a single **I/O replaceable routine**, which the
caller overrides at environment-init time. We replace it once and every `SAY` lands in
the HTTP response.

---

## 2. Goal & Scope

**In scope:** `.rexx` direct execution from UFS; `.rxp` tag dialect and transpiler;
loader and routing in HTTPREXX; the I/O-routine and variable-pool bindings; the output
and content-type model; an optional compiled-exec cache.

**Out of scope / non-goals:**

- No changes to **rexx370**. The interpreter stays a generic, IBM-compatible REXX; web
  semantics live in the CGI glue.
- No brexx370 compatibility path. This module targets rexx370 only.
- No include/layout mechanism, no template inheritance (Phase 3).
- **No environment pooling** across requests (reusing an LPE): one fresh LPE per request.
  rexx370's reentrancy makes this cheap and safe; pooling would risk variable/state
  leakage.
- **No sessions / application state** (separate spec if ever needed, UFS-backed).

---

## 3. Tag Syntax (`.rxp`)

Explicit `rexx` to avoid `<?xml ?>` collision:

| Tag              | Meaning                                   |
|------------------|-------------------------------------------|
| `<?rexx  ... ?>` | Statement block, **no** output            |
| `<?rexx= ... ?>` | Expression, result is written to output   |
| (anything else)  | Literal text, emitted verbatim            |

Extensions: **`.rexx`** = script mode (direct execution), **`.rxp`** = server pages
(transpiled). Future (Phase 3): `<?rexx== ?>` raw vs. `<?rexx= ?>` HTML-escaped.

---

## 4. Architecture

Everything lives in **HTTPREXX**. It drives rexx370 through the documented IRX services.

### 4.1 Per-request environment (reentrant)

For each request the module builds a Language Processor Environment and tears it down:

```c
/* 1. parmblock names HTTPREXX's I/O routine in its MODNAMET, so SAY/PULL/trace
      are routed to us instead of the default IRXINOUT (which writes SYSTSPRT). */
irxinit(parmblock, &env);                 /* IRXINITC: fresh reentrant LPE      */
env->userfield = reqctx;                  /* request context (httpd/httpc/buffer)*/
/* set request variables in the pool (REQUEST_METHOD, REQUEST_PATH, HTTP. stem…) */
irx_exec_run(source, source_len, args, args_len, &rc, env);   /* in-storage exec */
irxterm(env);                             /* IRXTERMC: free the LPE             */
```

The query string is passed as `args` and reaches the program via `PARSE ARG` — no
accessor needed. No custom functions are registered (see §4.3, §7).

No global lock: each worker runs its own `env`, with its own variable pool, I/O binding,
and stack. This is the direct analogue of HTTPLUA's fresh `lua_State` per request — but
here reentrancy is a property of the engine, not a workaround.

### 4.2 The I/O replaceable routine (the SAY hook)

rexx370 wires the I/O routine into `IRXEXTE.io_routine` during IRXINIT and lets the
caller override it via MODNAMET. HTTPREXX supplies `httprexx_io(function, data, env)`
handling the function codes from `irxwkblk.h`:

| Code         | REXX op                | HTTPREXX behavior                              |
|--------------|------------------------|------------------------------------------------|
| `RXFWRITE`   | `SAY`                  | append `data` + newline to the request buffer  |
| `RXFTWRITE`  | trace output           | request buffer or log (config)                 |
| `RXFWRITERR` | error message          | STDERR/log (`wtof`), not the response body     |
| `RXFREAD`/`RXFREADP` | `PULL` from terminal | EOF by default; optionally the POST body (§9, open) |
| `RXFREAD_DS`/`RXFWRITE_DS` | `EXECIO`  | map to UFS (Phase 2) or unsupported initially  |

The routine recovers the request context from `env->userfield` (set at init) — **no
globals**, which is what makes it reentrant. This is the REXX analogue of HTTPLUA's `_out`
vararg, using the standard ENVBLOCK user field — except here it is the *only* output path
(unlike brexx370, which needed a separate `HTTPSAY` module beside the dataset path).

### 4.3 Literal & expression emit (`.rxp`): just `SAY`

No custom writer function. `SAY` already routes through the replaced I/O routine into the
buffer, so it *is* the direct output path. Its one quirk — it appends a newline — is a
transpiler concern, not a reason for a BIF: the transpiler **coalesces each source line
into a single `SAY`** using REXX concatenation (`||`, which joins with no separating
blank). One template output line → one `SAY` → one newline, which mirrors the source and
is exactly what HTML wants. If truly newline-free output is ever needed (JSON fragments,
`<pre>`), `CHAROUT` is the standard REXX primitive — still no custom function.

### 4.4 Loader & routing

`.rexx`/`.rxp` are read from UFS via the public libufs API (`ufs_fopen`/`ufs_fclose`).
The router in `main()` selects by **file extension only**:

```
Request → main()
            ├─ ".rexx" → read source         → irx_exec_run(...)        ──┐
            ├─ ".rxp"  → read + transpile     → irx_exec_run(...)        ──┤
            └─ (cache hit, Phase 2) → irx_exec_dispatch(cached INSTBLK) ──┘
                                                                           ▼
                                       SAY → I/O routine → request buffer → flush → httpc
```

Server configuration: `MOD=HTTPREXX *.rexx` and `MOD=HTTPREXX *.rxp` (`CGI=` works as a
deprecated alias but emits `HTTPD410W`).

### 4.5 Why the transpiler is in C

Same reasoning as HTTPLUA: bootstrap avoidance. The transpiler is a pure C string
transform that produces the REXX source fed to `irx_exec_run()` (or, in Phase 2, the
compiler producing an INSTBLK). It needs no running interpreter, and the Phase-2 cache can
only persist in C anyway.

---

## 5. Transpilation Model (`.rxp`)

A `.rxp` file becomes a single REXX program. Each source line that produces output becomes
one `SAY` built with `||`; statement tags pass through verbatim.

**Input** (`hello.rxp`):

```rexx
<?rexx parse arg name; if name = '' then name = 'World' ?>
<h1>Hello, <?rexx= name ?>!</h1>
<?rexx items = 'Hercules MVS REXX'
do i = 1 to words(items) ?>
  <li><?rexx= word(items, i) ?></li>
<?rexx end ?>
```

**Generated program** (conceptual):

```rexx
parse arg name; if name = '' then name = 'World'
say '<h1>Hello, ' || (name) || '!</h1>'
items = 'Hercules MVS REXX'
do i = 1 to words(items)
say '  <li>' || (word(items, i)) || '</li>'
end
```

**Mapping rules (per source line):**

- A line's literal runs and `<?rexx= e ?>` expressions are concatenated into one
  `say a || (e) || b || …`. `||` joins with no separating blank, so output is byte-exact;
  the line's trailing newline comes from `SAY`. The parenthesized expression is the natural
  hook for later HTML escaping.
- `<?rexx s ?>` (statement tag) → `s` verbatim, no `SAY`.
- A statement tag in the middle of a line splits it into two `SAY`s around the statement
  (and thus two output lines). Mid-line statement tags are unusual; documented, accepted.

**Literal escaping:** a small escaper produces valid REXX string literals (pick a quote
character, double embedded occurrences). REXX has no `\`-escapes, so this is just
quote-doubling.

**Alternative without escaping (Phase 2):** place literals in a stem set in the variable
pool before execution (`_LIT.1`, `_LIT.2`, …) and reference them in the `SAY`
(`say _lit.1 || (e) || _lit.2`). The HTML bytes then never pass through the REXX
tokenizer, and the compiled program stays small. Trade-off: a cached compiled exec must
carry its literal table alongside it.

### 5.1 Parser: deliberately limited scanner in Phase 1

Phase 1 implements only a simple scanner. Rule: **no `?>` inside REXX code.** REXX is
*simpler* to scan than Lua here — there are no line comments and no long brackets, only
two constructs can contain `?>`:

```rexx
x = '?>'        /* string literal (single or double quoted) */
/* ?> */        /* block comment (nestable in REXX)          */
```

Handling these correctly needs string- and (nestable) comment-tracking — a small REXX
lexer, deferred to Phase 3. Phase 1 documents the rule and does not support them.

---

## 6. Output & Content-Type Model (buffered)

Output is **buffered**, not streamed. `SAY` (via the replaced I/O routine) appends to an
in-memory request buffer hanging off `env->userfield`; the flush to `httpc` happens at the
end of the request.

- **Clean error semantics.** If the REXX program fails or abends mid-render, nothing has
  been sent — HTTPREXX returns a clean `500` instead of `200` plus a half page. (The old
  module already buffered via the STDOUT dataset; this keeps the property, in memory.)
- **Content-Type: default, lazily committed.** For `.rxp`/`.rexx`, `text/html` is the
  default, committed lazily (until the first byte is flushed). No header-setting function
  is provided in the first cut. A script that needs to override the header uses the
  standard CGI convention — emit header lines, then a blank line, then the body — which
  HTTPREXX parses from the leading buffer (the old module already half-did this by sniffing
  `HTTP/…` status lines). The `<`-sniffing content-type heuristic is otherwise dropped.

**Escape valve (Phase 2) `http_flush`**: for very large responses, an explicit flush
commits the header and switches to streaming from that point on. Default is a single flush
at end of request.

Error/trace output (`RXFWRITERR`, `RXFTWRITE`) is logged via `wtof`, not mixed into the
response body.

---

## 7. Request Context Exposure

Request data reaches the REXX program through **stock REXX mechanisms only** — no external
functions in the first cut:

- **Query string → `PARSE ARG`.** Passed as the `args` argument of `irx_exec_run`; the
  program reads it with `arg` / `parse arg`. This is the natural REXX path and needs no
  accessor.
- **CGI metadata → plain pool variables** (`vpool_set`): `REQUEST_METHOD`, `REQUEST_PATH`,
  … and request headers as a `HTTP.` stem, set before execution. The program references
  them as ordinary variables (`if REQUEST_METHOD = 'POST' then …`). REXX symbols cannot
  contain `-`, so header names are mapped `Content-Type` → `HTTP.CONTENT_TYPE` (the same
  mapping the old module did for `DD:HTTPVARS`).
- **Request body → data stack + `PULL`.** For POST/PUT, HTTPREXX pushes the body lines onto
  the REXX data stack (the `RXFREAD` path); the program reads them with `PULL` / `parse
  pull`. This is the idiomatic REXX way to consume streamed input.

External functions or an `ADDRESS HTTP` host command environment are *not* required for the
first cut and are deferred to Phase 3 as ergonomic conveniences only.

---

## 8. Cross-Request State

**Phase 1 requires nothing across request boundaries.** The LPE, the request buffer, and
all per-request storage live and die with the request. The feature is correct with zero
persistent state, and — unlike the old module — needs **no global lock**.

The **only** cross-request state is the **Phase-2 compiled-exec cache**, a pure
performance optimization. rexx370 compiles REXX source into a self-contained bytecode
image (the `RX37` EXECBLK: header + constants table + symbol table + bytecode) reachable
as an INSTBLK via `irx_load_dispatch` (LOAD/FREE). The cache holds that compiled image
keyed by UFS path + `mtime`; on a hit the module runs it under a fresh per-request env via
`irx_exec_dispatch`.

**Anchor:** the existing `cgictx` registrar (as in HTTPLUA / mvsMF):

```c
HRXCTX *ctx = http_cgictx_get(httpd, "HTTPREXX", sizeof(HRXCTX));
if (!ctx) { /* array full / no core -> run without cache (= Phase-1 behavior) */ }
```

The eyecatcher `"HTTPREXX"` is exactly 8 bytes (no padding). The block is `__getm()`'d in
subpool 0 (address-space lifetime), find-or-create per eyecatcher, `NULL` on a full array
without ABEND. `HRXCTX` holds the cache root + a latch word.

**Forced consequences (same two pitfalls as HTTPLUA):**

- **Cache entries need SP0 lifetime.** The compiled images and hash nodes must be `__getm()`
  (SP0), not `malloc()` — malloc storage is reclaimed at request teardown (`@@exit`).
- **Mutation is HTTPREXX's own concern.** The registrar lock covers only the block's
  find-or-create. Insert/replace of cache entries across concurrent workers needs an own
  latch (a compare-and-swap word in `HRXCTX`, or a private ENQ) around insert/replace, with
  lock-free reads.

**Sharing model — confirm before Phase 2:** a cached INSTBLK is intended to be **read-only,
reentrant-shareable** across concurrent envs, with all mutable state (variable pool, stack)
living in the per-request env. This must be verified against the rexx370 VM (that the
INSTBLK / bytecode image is not mutated during execution). If it is not fully read-only, the
cache stores source-or-tokens and each request re-instantiates — still cheaper than
re-tokenizing from text, but not zero-copy.

**Graceful degradation & config:** `NULL` from `http_cgictx_get()` → run without a cache.
`CGI_CONTEXT_POINTERS` must cover all context-using CGIs (mvsMF + HTTPLUA + HTTPREXX + …).

---

## 9. EBCDIC

Non-critical. The delimiters `< ? = >` are all in the invariant set. rexx370 runs EBCDIC
(CP037/1047) and tokenizes EBCDIC source. The whole pipeline stays EBCDIC — the transpiler
scans EBCDIC bytes and emits EBCDIC REXX source; EBCDIC→ASCII happens centrally on the wire
in httpd/`httpxlat`, not in the template.

---

## 10. Phases

**Phase 1 — PoC.** Per-request LPE via `irxinit`/`irxterm`; `httprexx_io` I/O routine
(SAY → buffer) bound through `env->userfield`; query string via `PARSE ARG`, CGI metadata
as pool variables, POST body via the stack/`PULL`; **no external functions**; `.rexx`
direct execution and the `.rxp` transpiler (scanner, escaper, per-line `SAY` coalescing
with `||`); extension router; buffered output with default lazy content-type. **No**
cross-request state, **no** global lock. *Goal:* `hello.rexx` and `hello.rxp` render
end-to-end, under concurrent workers.

**Phase 2 — Performance & diagnostics.** Compiled-exec cache (`cgictx` anchor, INSTBLK/
`RX37` image keyed by path+mtime, latch); literals-in-stem; error line mapping
(source-newline preservation); `EXECIO` (`RXFREAD_DS`/`RXFWRITE_DS`) mapped to UFS;
`http_flush` streaming valve.

**Phase 3 — Hardening & convenience.** Optional ergonomic helpers (`ADDRESS HTTP` host
command environment via SUBCOMTB, `CHAROUT`-based newline-free emit); escaped-vs-raw output
(`<?rexx=` / `<?rexx==`); include/layout; sandboxing (restrict host command environments
and the function search order).

---

## 11. Design Decisions

**D1 — Target rexx370, drop brexx370.**
*Rationale:* reentrancy + IRX programming services remove the global lock, the temp
datasets, and the LINK-to-load-module model. *Consequence:* per-request LPE; in-storage
exec; the whole old `link_rexx` machinery is retired.

**D2 — Output is `SAY` through the replaced I/O routine, buffered. No writer function.**
*Rationale:* `SAY` already funnels through the I/O routine; overriding it (MODNAMET) sends
output to the HTTP buffer with no dataset, making `SAY` itself the single output path. Its
per-line newline is handled by the transpiler coalescing each source line into one
`say … || …`. *Consequence:* clean `500` on error; default lazy content-type; no `_OUT`/
`HTTPSAY`; `CHAROUT` is the standard fallback if newline-free emit is ever needed.

**D3 — Request context bound via `ENVBLOCK_USERFIELD`, not globals.**
*Rationale:* the I/O routine must find the request without static state, or reentrancy
breaks. *Consequence:* `userfield = reqctx`; recovered inside `httprexx_io`.

**D6 — No external REXX functions in the first cut.**
*Rationale:* every invented function (`_OUT`, `http_var`, `http_header`) duplicated a stock
mechanism. *Consequence:* output = `SAY`; query string = `PARSE ARG`; CGI metadata = pool
variables read directly; POST body = stack + `PULL`; header override = CGI-style leading
header block. Helpers are a Phase-3 convenience, not a dependency.

**D4 — Transpiler in C.**
*Rationale:* bootstrap avoidance; locality to the loader; the cache persists only in C.
*Consequence:* a pure C path producing the REXX source for `irx_exec_run` (or the INSTBLK).

**D5 — Only cross-request state = the Phase-2 compiled-exec cache, anchored via `cgictx`.**
*Rationale:* explicit address-space lifetime via `http_cgictx_get`, no ABI change.
*Consequence:* `HRXCTX` keyed by `"HTTPREXX"`; entries SP0 `__getm()`; own insert/replace
latch; graceful degradation on `NULL`; INSTBLK reentrant-shareability to be confirmed.

---

## 12. Open Points

1. **INSTBLK reentrancy:** confirm the rexx370 compiled image is read-only/shareable across
   concurrent environments before building the shared cache (§8). Decides zero-copy vs.
   re-instantiate.
2. **POST body framing:** the body reaches the program via the stack/`PULL` (§7) — confirm
   line framing (RECFM-like splitting vs. raw) and how an empty body PULLs.
3. **Header override convention:** finalize the CGI-style leading-header-block parsing for
   `.rexx` (and whether `.rxp` supports it at all, given its literal-first structure).
4. **Extension final:** `.rexx` + `.rxp` (`MOD=HTTPREXX *.rxp`).
5. **Config sizing:** `CGI_CONTEXT_POINTERS` must cover mvsMF + HTTPLUA + HTTPREXX + ….
6. **XSS default:** should `<?rexx=` eventually escape rather than emit raw?

---

## Revision History

- **1.1** — Removed all external REXX functions from the first cut. Output is plain `SAY`
  through the replaced I/O routine (single output path; the transpiler coalesces each source
  line into one `say … || …`), replacing the `_OUT` BIF. Request input via `PARSE ARG`
  (query string), pool variables (CGI metadata), and the stack/`PULL` (POST body), replacing
  `http_var`. Header override via the CGI-style leading-header-block convention instead of an
  `http_header` BIF. Added decision D6; helpers (`ADDRESS HTTP`, `CHAROUT`) demoted to
  Phase-3 conveniences.
- **1.0** — Final. Targets rexx370 (reentrant LPE per request, no global lock); I/O
  replaceable routine for buffered output bound via `ENVBLOCK_USERFIELD`; request context via
  the variable pool; `.rexx` direct execution and `.rxp` transpilation via libufs +
  `irx_exec_run`; routing by extension (`MOD=HTTPREXX`); cross-request compiled-exec cache
  anchored via `http_cgictx_get` (eyecatcher `"HTTPREXX"`, SP0 lifetime, own mutation latch,
  graceful degradation); EBCDIC end-to-end; Phase-1 PoC carries no cross-request state.
