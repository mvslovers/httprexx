# HTTPREXX Server Pages — Phase 2 Status & Decisions

Living working document. Tracks scope, decisions, evidence, and progress for
Phase 2 (spec: `httprexx-server-pages-spec.md` §10). Updated as we go so the
current state is always visible.

**Last updated:** 2026-07-03

---

## Phase 2 scope (spec §10) — the five items

| # | Item | Decision | Blocked on rexx370? |
|---|------|----------|---------------------|
| 1 | Compiled-exec cache | **Deferred** | Yes — needs rexx370 to export a compile/run-from-image path as load modules; not in focus. Re-evaluate now that #208 makes `.rxp` compile (see Perf below). |
| 2 | literals (make `.rxp` bytecode-compilable) | **Done — resolved UPSTREAM in rexx370** | Fixed in the engine (rexx370 #208 + #212), not in HTTPREXX |
| 3 | error line mapping | **Deferred** | Yes — rexx370 SIGL line tracking is deferred (always 0) |
| 4 | EXECIO → UFS | **Deferred** | Yes — EXECIO is not implemented in rexx370 |
| 5 | `http_flush` streaming valve | **Next** | No — pure HTTPREXX, automatic buffer-threshold flush |

---

## #2 resolved upstream — the fix moved to the right layer

The original Phase 2 plan chunked long `SAY` literals in the **HTTPREXX transpiler**
(issue #4, PR #5) to dodge rexx370's 63-byte bytecode constant limit
(`IRXBC_STR_MAX`). That worked, but adversarial testing found it could regress a
large page to a fatal 500 (the program-wide `BCOM_MAX_CONSTS = 512` table overflow
returned a *fatal* `IRXBC_ERR_STOR`). The investigation drove the fix into rexx370
itself, where it belongs:

- **rexx370 #208** (`e3d187c`, main): the bytecode compiler now **chunks a >63-byte
  literal into ≤63-byte constants + `CONCAT` internally**, instead of failing.
- **rexx370 #212 / #213** (`caaa13c`, main): compile-time fixed-table overflow now
  returns the new **`IRXBC_ERR_CAPACITY`** and is fallback-eligible in
  `bc_err_is_fallback()` → large pages **fall back to the interpreter and render**
  instead of a fatal 500. Scoped cleanly: genuine OOM and execute-time errors stay
  fatal.

**Consequence for HTTPREXX:** no transpiler change needed. `src/rxptrans.c` stays
simple (one `say '…' || …` per line, one unchunked literal). **PR #5 and issue #4
are closed** as superseded/redundant.

**Verified host-side against rexx370 main** (unchunked transpiler output):
- small/normal page → compiles natively to bytecode (`exec=1`, byte-identical to
  the interpreter);
- 50 KB page (303 distinct lines) → falls back gracefully and renders 50 477 B
  (previously a fatal 500 under the chunking approach).

**Pending (deploy):** rexx370's IRX* load modules must be rebuilt + redeployed to the
MVS LINKLIB for #208/#212 (and the earlier 63-byte STRTOOLONG fix) to take effect on
the target.

---

## Deferred items — rexx370 prerequisites (unchanged)

- **#3 error line mapping** — rexx370 line tracking deferred (`irx#bvm.c`: "SIGL —
  line tracking not yet available; set to 0"); `wkbi_sigl` is always 0.
- **#4 EXECIO → UFS** — `RXFREAD_DS`/`RXFWRITE_DS` exist only as `#define`s
  (`irxwkblk.h`); no EXECIO in the tokenizer/parser/VM.
- **#1 compiled-exec cache** — the real build/run entries (`IRXBCOMP`/`IRXBEXEC`) are
  not installed load modules; reentrant sharing of the `RX37` image unconfirmed.
  Worth a fresh cost/benefit read now that `.rxp` actually compiles (Perf below).

---

## Performance re-analysis (2026-07-03)

_(to be filled in — measuring interpreter vs bytecode for a realistic `.rxp` now
that rexx370 #208 makes it compile, and estimating the compile fraction to re-judge
the deferred cache #1)_

---

## Status log

- **2026-07-02** — Phase 2 read-through; cache's spec mechanism did not match
  rexx370 (wrong primitives; IRXBCOMP/IRXBEXEC not installed).
- **2026-07-03** — Scope: do #2 + #5; defer #1/#3/#4. Measured the #2 gate (host):
  chunking is a standalone win, but adversarial testing found a fatal regression on
  large pages (>512 distinct constants) via rexx370's non-fallback table overflow.
- **2026-07-03** — Filed rexx370 issues; maintainer fixed them in the engine:
  **#208** (compiler chunks long literals) + **#212/#213** (`IRXBC_ERR_CAPACITY`
  fallback). Both on rexx370 main. HTTPREXX transpiler chunking is therefore
  redundant → **PR #5 and issue #4 closed**, transpiler reverted to simple form.
  Verified host-side. **Next:** perf re-analysis, then #5 (`http_flush`).
