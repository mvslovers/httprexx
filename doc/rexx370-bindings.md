# rexx370 IRX Bindings for HTTPREXX (Phase 1)

How HTTPREXX drives the rexx370 REXX interpreter. This is the confirmed,
read-from-source contract behind the execution glue; it complements the design
in `httprexx-server-pages-spec.md` with the concrete call sequences (which the
spec deliberately leaves out).

All entry-point names, struct fields, and register conventions below were read
from the rexx370 sources (`../rexx370`), not assumed.

---

## 0. Consumption model: runtime LINK, vendored headers (no rexx370 build dep)

The IRX services are **already installed as load modules** on the target system
(`IRXINIT`, `IRXEXEC`, `IRXTERM`, plus the data modules `IRXANCHR`/`IRXPARMS`
they load). HTTPREXX therefore:

- **does not** statically link rexx370 and **does not** declare it as an mbt
  dependency — only its **headers are vendored** under `include/` for the struct
  layouts and function codes;
- invokes the services **at runtime via the MVS LINK service** (`__linkds`,
  `cliblink.h`), exactly as the old brexx370-based module LINKed to `BREXX`.

This honors spec §2 ("No changes to rexx370") completely. It is the only model
that works without a `[lib]` export, because rexx370 builds each IRX service as
a self-contained load module (each NCAL-links the whole C core) and ships no
static archive. The C convenience wrappers `irx_exec_run()` / `vpool_set()` are
plain C functions (no load-module entry), so they are **not** reachable this way
— HTTPREXX drives the standard `IRXEXEC` interface instead.

> Vendored headers needed (struct layouts only): the IRX control blocks
> (`envblock`, `irxexte`, `instblk`/`instblk_entry`, `argtable_entry`, the
> `IRXEXEC` plist), the I/O function codes (`irxwkblk.h`: `RXFWRITE` …), the I/O
> routine signature (`irxio.h`), and the `lstring370` string header for the
> `PLstr` the I/O routine receives. These are copied verbatim from rexx370 /
> lstring370 and refreshed if those projects bump the layouts.

---

## 1. IRXINIT — create a Language Processor Environment

`IRXINIT` (load module) takes a **7-slot VL parameter list** in R1. Each slot is
a pointer to the value; the last slot's high bit marks end-of-list.

```
CALL IRXINIT,(FCODE,PARMMOD,USERFLD,WKBLKEXT,RESERVED,ENVBLK,REASON),VL
  P1 FCODE     -> 8-char function "INITENVB"
  P2 PARMMOD   -> PARMBLOCK (NULL = defaults; MODNAMET I/O override is a no-op
                  in rexx370, so we do not bother building one — see §2)
  P3 USERFLD   -> user word (we set the request context here too; but the
                  reliable hook is ENVBLOCK_USERFIELD, set in §2)
  P4 WKBLKEXT  -> NULL
  P5 RESERVED  -> NULL
  P6 ENVBLK    -> OUT: receives the new ENVBLOCK pointer
  P7 REASON    -> OUT: reason code (last slot, VL bit set)
R15 = 0 ok / 4 no-env / 20 error
```

Invocation:

```c
rc = __linkds("IRXINIT", NULL, vlist7, &prc);   /* prc = IRXINIT R15 */
/* env = *(struct envblock **)ENVBLK-slot value */
```

`__linkds` (`cliblink.h`): `int __linkds(const char *pgm, void *dcb, void *r1,
int *prc)` — `r1` is the VL list, `prc` receives the linked program's return
code, and ABENDs are trapped (ESTAE) rather than dumping.

---

## 2. The SAY hook — overwrite `IRXEXTE.io_routine` after init

rexx370 wires its default I/O routine (`irxinout`, writes SYSTSPRT) into
`ENVBLOCK -> envblock_irxexte -> io_routine` during IRXINIT. The TSO/E way to
replace it is the MODNAMET table passed to IRXINIT, **but rexx370 does not
implement the MODNAMET override yet** (`src/irx#init.c`: "non-blank entries …
are intentionally ignored in this phase").

**Fallback (flagged):** after IRXINIT returns, HTTPREXX overwrites the pointer
directly, before IRXEXEC:

```c
struct irxexte *exte = (struct irxexte *)env->envblock_irxexte;
exte->io_routine = (void *)httprexx_io;     /* also set exte->irxinout to match */
env->envblock_userfield = reqctx;           /* recovered inside httprexx_io     */
```

This is safe and verified: the SAY opcode reads the pointer **by indirection at
call time** (`src/irx#bvm.c`, `OP_SAY`: `io_fn = exte->io_routine; io_fn(RXFWRITE,
…)`), with no per-exec caching — so a post-init overwrite redirects every SAY.
When rexx370 implements MODNAMET, this collapses to the standard path with no
HTTPREXX change.

### The I/O routine

```c
/* irxio.h:  int irxinout(int function, PLstr data, struct envblock *envblock); */
int httprexx_io(int function, PLstr data, struct envblock *env);
```

Recovers the request context from `env->envblock_userfield` (no globals → keeps
reentrancy). Function codes from `irxwkblk.h`:

| Code         | Op            | Phase 1 behavior                                   |
|--------------|---------------|----------------------------------------------------|
| `RXFWRITE`   | `SAY`         | append `data` bytes + `\n` to the request buffer   |
| `RXFTWRITE`  | trace         | `wtof` (log), not the body                          |
| `RXFWRITERR` | error line    | `wtof` (log), not the body                          |
| `RXFREAD`/`RXFREADP` | `PULL` | **return EOF** (spec §4.2 sanctions EOF default)   |
| others       | EXECIO/open   | unsupported (Phase 2)                               |

`data` is a `PLstr` (lstring370 `Lstr *`): read its byte pointer and length via
the lstring header to copy the SAY text into the buffer.

> **Flagged Phase-1 gap — POST body / PULL.** rexx370 has no data-stack API yet
> and the default `irxinout` stubs RXFREAD/RXFREADP, so the spec's "POST body via
> stack/PULL" (§7) is **not** wired in Phase 1; `httprexx_io` returns EOF for
> PULL. When needed, the body can be fed directly through `httprexx_io`'s
> RXFREAD path (no stack API required) — deferred, not on the hello path.

---

## 3. IRXEXEC — run an in-storage exec

`IRXEXEC` (load module) takes a **10-slot VL parameter list** in R1:

```
CALL IRXEXEC,(EXECBLK,ARGTAB,FLAGS,INSTBLK,RESV5,EVALBLK,WKAREA,USRFLD,ENVBLK,REXXRC),VL
  P1 EXECBLK  -> NULL (using in-storage INSTBLK instead)
  P2 ARGTAB   -> ARGTABLE: the query string for PARSE ARG (or NULL)
  P3 FLAGS    -> fullword; IRXEXEC_SUBROUTINE (0x40000000) call type
  P4 INSTBLK  -> in-storage REXX source (the .rexx text, or transpiled .rxp)
  P5 RESV5    -> NULL
  P6 EVALBLK  -> NULL (no function result wanted)
  P7 WKAREA   -> NULL
  P8 USRFLD   -> NULL
  P9 ENVBLK   -> the ENVBLOCK from IRXINIT   *** pass the env HERE ***
  P10 REXXRC  -> OUT: the REXX program's exit code (last slot, VL bit set)
R15 = 0 ok / 28 NOENV / 32 BADPLIST / …
```

**The ENVBLOCK goes in P9, not R0.** `__linkds` cannot control R0, but the
IRXEXEC env resolution is P9-first (`include/irxexec.h`), so P9 is authoritative.
Every P1–P9 *slot address* must be non-NULL (a slot may *point to* NULL); P10
omitted is allowed (VL bit then sits on P9).

```c
rc = __linkds("IRXEXEC", NULL, vlist10, &prc);   /* prc = IRXEXEC R15; *REXXRC = exec exit */
```

### INSTBLK (in-storage source) — `include/irx.h`

128-byte header (eye-catcher `IRXINSTB`, hdrlen 128, `instblk_address` → entry
array, `instblk_usedlen`) followed by an array of `instblk_entry { void *stmt;
int len; }`, one per REXX source line. Build: copy the source into one flat
buffer, then one entry per line pointing at the line text and its length
(newline not included in the entry).

### ARGTABLE — `include/irx.h`

Array of `argtable_entry { void *str; int len; }` terminated by 8 bytes of
`0xFF`. One entry holding the query string feeds `PARSE ARG`; no query → P2 NULL.

---

## 4. IRXTERM — tear down the LPE (needs a tiny asm glue)

`IRXTERM` takes the ENVBLOCK **in R0** (no parameter list). `__linkds` cannot set
R0, so HTTPREXX provides a minimal own-asm/inline-asm shim: LOAD `EP=IRXTERM` at
runtime (the module is installed, not statically linked, so a static `=V` would
not resolve under NCAL), set `R0 = env`, `BALR`. `R15` = 0 ok / 20 bad env.

```c
int httprexx_irxterm(struct envblock *env);   /* shim: LOAD EP=IRXTERM; R0=env; BALR */
```

One fresh LPE per request: IRXINIT → patch io_routine → IRXEXEC → IRXTERM, with
the request buffer flushed to `httpc` after IRXTERM. No cross-request state.

---

## 5. Routing decision

Extension routing: **`MOD=HTTPREXX *.rexx`** and **`MOD=HTTPREXX *.rxp`** in the
httpd parmlib. httpd then sets `SCRIPT_FILENAME` to the full UFS path; `main()`
reads it (falling back to the `REQUEST_PATH` basename) and branches on the
extension — `.rexx` runs the source directly, `.rxp` is transpiled first
(`rxp_transpile`). Both are read from UFS via the public libufs API
(`ufs_fopen`/`ufs_fclose`), like httplua's `readable()`.

---

## 6. End-to-end call sequence (per request)

```
read file from UFS (libufs)                         ── .rexx: source as-is
                                                    ── .rxp:  rxp_transpile(...)
reqctx = { httpc, output buffer }
__linkds("IRXINIT", …, vlist7)        -> env
env->envblock_irxexte->io_routine = httprexx_io
env->envblock_userfield            = &reqctx
build INSTBLK (source) + ARGTABLE (query string)
__linkds("IRXEXEC", …, vlist10 with ENVBLK=env in P9)
   └─ SAY -> httprexx_io(RXFWRITE) -> reqctx.buffer
httprexx_irxterm(env)                 -> free LPE
flush reqctx.buffer -> httpc (default Content-Type: text/html, lazy)
```

---

## 7. Phase-1 flags (deviations / deferrals, all sanctioned by the spec)

1. **MODNAMET I/O override is a rexx370 no-op** → SAY is hooked by overwriting
   `IRXEXTE.io_routine` post-init (§2). Safe (verified by-indirection dispatch);
   collapses to the standard path once rexx370 implements MODNAMET.
2. **No data stack in rexx370** → POST body via `PULL` is not wired; `httprexx_io`
   returns EOF for RXFREAD/RXFREADP (spec §4.2 default). hello path unaffected.
3. **CGI metadata pool variables deferred** → `vpool_set` is not a load-module
   entry (unreachable via runtime LINK); the standard `IRXEXCOM`/SHVBLOCK path is
   a follow-up. The query string still reaches the program via `PARSE ARG`
   (ARGTABLE), which is all `hello.rexx`/`hello.rxp` need.
4. **IRXTERM needs a tiny asm/inline-asm shim** (R0 convention) — HTTPREXX's own
   code, not a rexx370 change.
