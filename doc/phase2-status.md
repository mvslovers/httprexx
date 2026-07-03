# HTTPREXX Server Pages — Phase 2 Status & Decisions

Living working document. Tracks scope, decisions, evidence, and progress for
Phase 2 (spec: `httprexx-server-pages-spec.md` §10). Updated as we go so the
current state is always visible.

**Last updated:** 2026-07-03

---

## Phase 2 scope (spec §10) — the five items

| # | Item | Decision | Blocked on rexx370? |
|---|------|----------|---------------------|
| 1 | Compiled-exec cache | **Deferred** | Yes — needs a rexx370 compiler exposed as load modules; explicitly **not in focus** upstream right now |
| 2 | literals (make `.rxp` bytecode-compilable) | **Implemented (PR #5, draft); gated on rexx370#212** — see "Post-impl finding" below | No for chunking itself; the safety gate needs rexx370#212 |
| 3 | error line mapping | **Deferred** | Yes — rexx370 SIGL line tracking is deferred (always 0) |
| 4 | EXECIO → UFS | **Deferred** | Yes — EXECIO is not implemented in rexx370 at all |
| 5 | `http_flush` streaming valve | **Do now** | No — pure HTTPREXX, automatic buffer-threshold flush |

**Current focus:** measure first (item 2 gate), evaluate, then build #5.

---

## Evidence behind the decisions

All file:line refs are into the sibling `../rexx370` checkout.

### #1 Cache — deferred (maintainer decision: rexx370 compiler not in focus)
- The spec (§8/§4.4/§4.5) names the wrong primitives: INSTBLK is **source
  lines**, `irx_load_dispatch`/IRXLOAD loads **source**, and
  `irx_exec_dispatch`/IRXEXEC **reconstructs source and recompiles every call**
  (`src/irx#exec.c:208-256`).
- The real compiled image is `struct irx_bc_execblk` (magic `"RX37"`,
  `include/irxexbl.h`) — flat, position-independent, genuinely cacheable. But it
  is compiled fresh and **freed at the end of every `irx_exec_run`**
  (`src/irx#exec.c:374-413`); there is no persistence.
- The real build/run entries are `irx_bc_compile` (alias `IRXBCOMP`) and
  `irx_bc_execute` (alias `IRXBEXEC`), `include/irxbvm.h:75,106` — but **neither
  has an asm shim** in `../rexx370/asm/`, so they are not installed load modules
  and HTTPREXX's runtime-LINK model cannot reach them.
- `wkbi_cache` ("exec LRU cache", `include/irxwkblk.h:167`) is a declared but
  **unused** stub.
- → Cache needs upstream rexx370 work (export IRXBCOMP/IRXBEXEC as load modules
  **and** confirm the RX37 image is read-only during execute for reentrant
  sharing — spec Open Point 1). Maintainer: compiler is not the current focus.

### #2 literals — do now, but as transpiler literal-chunking
- Root cause: rexx370 bytecode caps each constant/symbol at 63 bytes
  (`IRXBC_STR_MAX`, `include/irxexbl.h:70`). Ordinary HTML `SAY` literals in
  transpiled `.rxp` exceed that → `IRXBC_ERR_STRTOOLONG` → the engine falls back
  to the token-walk interpreter (`src/irx#exec.c:294-299, 374-396`).
- The spec's mechanism (§5 "literals-in-stem": set a `_LIT.` stem in the
  variable pool before execution) is **also blocked** — it needs vpool-set-from-C
  (IRXEXCOM), which is not an installed load module (same wall that deferred CGI
  pool variables in Phase 1; bindings flag #3). No `irxexcom`/`irxvpol` shim in
  `../rexx370/asm/`.
- Unblocked alternative that reaches the same goal: the **transpiler chunks each
  HTML line into ≤63-byte string literals joined with `||`** — pure C, no vpool.

### #3 error line mapping — deferred (verified 2026-07-03)
- rexx370 line tracking is explicitly deferred: `src/irx#bvm.c:2645` "SIGL — line
  tracking not yet available; set to 0" (also :2739, :3649-3650). `wkbi_sigl` is
  always 0. With no real error line from the engine, HTTPREXX has nothing to map.

### #4 EXECIO → UFS — deferred (verified 2026-07-03)
- `RXFREAD_DS`/`RXFWRITE_DS` exist only as `#define`s (`include/irxwkblk.h:57-58`).
  No EXECIO instruction in the tokenizer/parser/VM; the engine never emits those
  codes. Nothing for HTTPREXX to map until rexx370 implements EXECIO.

### #5 `http_flush` — do now
- Pure HTTPREXX buffer mechanic. Automatic flush when the response buffer crosses
  a threshold (commit header, stream from there); default remains a single flush
  at end of request. No rexx370 dependency.

---

## Open wrinkle on #2 — payoff is entangled with the (deferred) cache

Bytecode's big win comes **with** the cache (compile once, reuse). Without the
cache, `.rxp` still compiles on **every** request; #2 only flips the outcome from
"compile fails → interpret" to "compile succeeds → run bytecode". Whether that is
net faster is unmeasured. A cheaper immediate alternative exists: **disable
bytecode for the `.rxp` route** (`REXX370_BYTECODE=0`), skipping the doomed
compile entirely.

→ Decision: **measure before building #2** (see below).

---

## Measurement plan (item-2 gate)

Host-side, no MVS needed. Driver based on `../rexx370/test/bench/tstcps_host.c`
(times `irx_exec_run`; honors `REXX370_BYTECODE` via `getenv`; one fresh LPE per
run = faithful per-request model). Loop the run N times to average out noise.

Two experiments (per the pre-work analysis):
1. **Realistic `.rxp`** (HTML lines >63 bytes → triggers fallback), bytecode
   `=1` vs `=0` → cost of the **doomed compile** that `.rxp` pays every request.
2. **Short-literal `.rexx`** (bytecode compiles successfully), `=1` vs `=0` →
   bytecode-execute vs interpret-execute — the real gate for the whole bytecode
   branch.

Reframes the earlier `demo.rxp` note ("422 ms TTFB, ~85% REXX-side"): that figure
is doomed-compile + fallback + interpret + SAY I/O combined; these experiments
split it.

### Results (2026-07-03, host = macOS ARM, clang -O2)

Driver: `scratchpad/bcbench.c` over `page_html.rexx` (today's transpiler, long
literals) and `page_short.rexx` (#2 chunking, literals ≤63 B). Both emit
**byte-identical** output (46 lines, 2451 B — verified). BCDEBUG confirms:
`page_html` → `exec=0 fallback=1` (falls back to interpreter); `page_short` →
`exec=1 fallback=0` (compiles + runs bytecode). 20 000 iters × 3 trials.

| Cell | Page | `REXX370_BYTECODE` | What it models | per-iter |
|------|------|-----|----------------|----------|
| A | page_html | 1 | **Today's `.rxp`**: doomed compile → fallback → interpret | **~58 µs** |
| B | page_html | 0 | Disable bytecode for `.rxp`: straight interpret | ~47 µs |
| C | page_short | 1 | **#2** chunking + successful bytecode execute | **~36 µs** |
| D | page_short | 0 | #2 with bytecode off (sanity ≈ B) | ~48 µs |

**Derived (ratios are the transferable part; absolute µs are host-only):**
- **Doomed-compile tax today** = A − B ≈ **11 µs (~19% of every `.rxp` request)** —
  pure waste: compile attempt that always gets thrown away.
- **#2 + bytecode vs today** = A → C ≈ **−38% per request** — the fastest cell.
- **Bytecode-execute vs interpret (the real gate)** = C vs D: bytecode wins by
  ~25% **even though C still pays a (successful) compile every request**. So
  bytecode execution genuinely beats the token-walk interpreter for this workload.
- **Free immediate win**: A → B ≈ **−19%**, zero code — just don't attempt
  bytecode on the `.rxp` route.

**Conclusions:**
1. Today's `.rxp` (cell A) is the **worst** of all four options.
2. **#2 pays off now, without the cache** (C beats A by ~38% and beats the
   interpreter B/D by ~24%). This resolves the earlier "payoff entangled with the
   deferred cache" wrinkle: it is *not* entangled — chunking + bytecode is a
   standalone per-request win.
3. There is a **zero-code stopgap** (disable bytecode for `.rxp`, A→B, ~19%) if #2
   is not ready immediately.
4. The **cache stacks on top of #2 later**: it would remove the compile portion of
   cell C (a cache hit ≈ bytecode-execute only, < 36 µs). Sequencing confirmed:
   #2 now, cache when rexx370 exposes IRXBCOMP/IRXBEXEC.

**Caveats:** host measurement — absolute µs do not transfer to MVS/Hercules; MVS
storage allocation (GETMAIN vs host malloc) likely makes the *compile* cost
relatively larger, which would only strengthen (1)/(3)/(4). A live-MVS run is the
ground truth if we want certainty before committing. Harness + inputs in
`scratchpad/` (`bcbench.c`, `genpages.py`, `page_html.rexx`, `page_short.rexx`).

---

## Post-implementation finding — chunking can regress large pages to a fatal 500

Adversarial stress testing (after the PR) found a real regression the unit tests
missed. rexx370's bytecode compiler has **fixed, program-wide** tables
(`irx#bcom.c`): `BCOM_MAX_CONSTS = 512`, `BCOM_MAX_CODE = 16384`. Overflowing them
returns `IRXBC_ERR_STOR`, which is **not** in `bc_err_is_fallback()` (that covers
only `UNSUP` / `STRTOOLONG` / `PARSE_COMPOUND`) → `irx_exec_run` returns it
**fatally** instead of falling back to the interpreter.

Chunking exposes this: before, a long HTML line produced one >63-byte constant →
`STRTOOLONG` → graceful fallback → page renders. After, the line becomes many
≤63-byte constants that compile *past* that point until the 512-constant table
overflows → `IRXBC_ERR_STOR` → **fatal 500**. The table is per-program, so the
limit is **cumulative** across the whole page.

**Measured (host, distinct content so the compiler's constant dedup doesn't hide
it):**
- Single line: 508 distinct chunks (~32 KB) → `exec=1`, correct. 635 chunks
  (~40 KB) → **fatal rc=20**.
- Realistic multi-line page `page_big.rxp` (50 KB, 303 distinct lines — a report
  with 300 entries): **OLD renders 50 477 B via fallback; NEW returns fatal
  rc=20 (0 B).**

Threshold ≈ **>512 distinct 63-byte chunks ≈ >32 KB of distinct page content**,
cumulative. That is a plausible real page, not a pathological one.

(Note: an earlier sweep with all-identical bytes wrongly suggested the limit was
~4800 chunks / ~300 KB — the compiler **deduplicates identical constants**, hiding
the 512-limit. Distinct content is the correct test.)

**Proper fix (upstream, same pattern as the 63-byte STRTOOLONG fix):** filed as
**rexx370#212**. Classify the compile-time *capacity* overflow (raised before any
bytecode runs, so side-effect-free to retry) as fallback-eligible in rexx370's
`bc_err_is_fallback()`. Then table overflow → graceful fallback to the interpreter
(which has no such limits) → page renders. Benefits all rexx370 callers.

Subtlety noted in rexx370#212: `IRXBC_ERR_STOR=20` is overloaded (compile-time
table overflow *and* real alloc failure *and* execute-time VM errors). Since
`bc_err_is_fallback()` is consulted only on the compile return (`irx#exec.c:376`),
execute-time `STOR` stays fatal regardless. Preferred fix there is a **distinct
capacity code** rather than blanket-classifying `STOR`.

**Consequence for PR #5:** chunking is a strict win only *once rexx370 falls back
on `STOR`*. Until then, merging #5 alone trades "most pages faster" for "pages
> ~32 KB distinct content return 500 instead of rendering." Options: (a) land the
rexx370 `STOR`-fallback fix first, then #5 is strictly safe; (b) interim
transpiler guard — track emitted constants and stop chunking past a safe budget
(~400), letting the remaining long literals fall back via `STRTOOLONG` (adds
cross-line state; conservative, since it can't see the compiler's dedup);
(c) hold #5 until (a). **Recommendation: (a).**

## Status log

- **2026-07-02** — Phase 2 read-through; established the cache's spec mechanism
  does not match rexx370 (wrong primitives; IRXBCOMP/IRXBEXEC not installed).
- **2026-07-03** — Maintainer decisions: defer #1 (cache); do #2 (literals) and
  #5 (`http_flush`); verified #3 and #4 are rexx370-blocked. Agreed to measure
  the #2 gate first. Created this status doc; building the measurement harness.
- **2026-07-03** — Ran the #2-gate measurement (host). Result: #2 is a standalone
  ~38%/request win (not entangled with the cache); today's `.rxp` is the worst
  cell; a zero-code ~19% stopgap exists (disable bytecode on the `.rxp` route).
  **Next:** implement #2 (transpiler literal-chunking), then #5 (`http_flush`).
- **2026-07-03** — Implemented #2 (issue #4, PR #5, branch
  `feature/rxp-literal-chunking`): `line_close_lit` in `src/rxptrans.c` splits
  literals into ≤63 value-byte `||`-joined chunks; 5 new tests (17/17 host green);
  spec §5/§10 corrected (v1.2). CI green. End-to-end verified on a small page:
  old `fallback=1` → new `exec=1`, byte-identical render.
- **2026-07-03** — Adversarial stress test (advisor-prompted) found a regression:
  chunking a large page (>512 distinct 63-byte chunks ≈ >32 KB content) overflows
  rexx370's program-wide constants table → `IRXBC_ERR_STOR` → **fatal 500** where
  the unchunked page rendered via fallback. Confirmed on a realistic 50 KB page.
  Root cause + fix in "Post-impl finding" above. **#2 is NOT done**: PR #5 needs
  the rexx370 `STOR`-fallback fix before it is strictly safe to merge. **Next:**
  decide the gate (recommend the upstream rexx370 fix), then #5 (`http_flush`).
- **2026-07-03** — Maintainer chose the upstream route. Filed **rexx370#212**
  (compile-time table overflow should be fallback-eligible). Converted **PR #5 to
  draft**, linked the blocker. #2 stays open pending rexx370#212 + redeploy.
  **Next:** proceed with #5 (`http_flush`), which is independent of this gate.
