# atx-vol backtesting sprint — status

**As of** 2026-07-25, HEAD `f60ce3c` on branch **`sprint/atx-vol-backtest-waves-cde`**.
**Stopped at the user's request, mid-Wave-D.** One fix round was in flight when work
stopped — see [In flight](#in-flight-when-work-stopped).

Nothing was merged into local `main` this session, per instruction. `main` still points at
`2858cab` (Wave C T4). The branch carries everything after it.

The sprint's charter: productionize the backtesting pipeline — get the economics out of a
~1000-line example file into the library, give results a real storage format, and improve
correctness and performance. **Waves A, B and C are closed. Wave D is 3 of 7 tasks in.
Wave E is planned and not started.**

---

## Where each wave stands

| Wave | Scope | Status |
|---|---|---|
| **A** | `run_diagnostics` + `run_archive` + the RunArchive binary format + schema single-source + pure-Python reader | **CLOSED** `0f485e6` |
| **B** | `listed_dispersion_pipeline` — dispersion economics under test, M1 fixed, two-route parity guarded, CLI thinned | **CLOSED** `587ee97` |
| **C** | `backtest_driver` spine; migrate all five example drivers | **CLOSED** `cb875dc` — gate PASS |
| — | pre-Wave-D hygiene: make the full Release build green | **DONE** `017fc5c` |
| **D** | `StepObserver` (L10) retires the divergence shadow loop; de-SPY `dispersion_workflow` (L12) | **T1–T3 landed**, T3 in a fix round; T4–T7 not started |
| **E** | Perf — 4 passes kept of the original 7 | Planned, not started |

### Commit chain on the branch

```
f60ce3c  feat(vol): dual-run mark-divergence equivalence arbiter        (D T3)
c438a64  test(vol): gate divergence row multiplicity and accumulation   (D T2 fix)
c464051  feat(vol): mark-divergence collector on the step observer      (D T2)
42c60d8  feat(vol): RunConfig::step_observer — the L10 substrate        (D T1)
017fc5c  fix(build): make the full Release build green                  (hygiene)
cb875dc  docs(vol): close Wave C — gate PASS, honest wave verdict       (C T8)
36f9707  refactor(vol): migrate mag7_dispersion_backtest                (C T6)
a9e62e4  test(vol): say what the PnL-track gate actually covers         (C T5 fix)
35a55cf  refactor(vol): migrate spy_dispersion_pnl                      (C T5)
--- main is here: 2858cab ---
```

Branch diff vs `main`: **16 files, +2072 / −79.**

---

## Wave C — closed, with an honest verdict

**Gate: PASS.** Full gtest 2021 ran / 1975 passed / 43 skipped / 3 failed / 7 disabled from
`build-rel` CWD, the three being the documented pre-existing reds. **All ten T2 byte
goldens re-verified from clean dirs on two independent passes — 10/10 MATCH.** Each of the
three filtered artifacts was also audited unfiltered index-by-index against T2's reference
bytes: equal line counts (166/166, 25/25, 54/54), exactly two differing lines each, every
one `wall_clock_ms`/`steps_per_s` and in-filter on both sides. No filter was widened
anywhere in the wave. Freeze intact — `kRaMinor == 0`, schema-hash pin green in-suite,
committed fixture sha256 unchanged, and the wave diff contains no `*.py` at all.

**The strongest evidence in the wave, added beyond the plan:** all five migrated drivers'
`.obj` files import `?run_timed@vol@atx@@` and import **none** of
`run_backtest`/`run_dispersion_backtest`/`tearsheet`. The shipped binaries genuinely route
through the seam, not just the source.

### The verdict, stated plainly

Wave C was chartered to close **L11** — "a strategy-agnostic 9-stage spine copy-pasted
across 5 of 6 drivers" — by building design spec §4.3's `BacktestJob`. Measured stage by
stage against the code: clock source **0/5** identical, strategy construct **0/5**, output
shape **0/5** (four genuinely distinct shapes), `EngineRunStats` **1/5**. Only stages 5+6+7
are 5/5, and four of that triple's six nodes were already library calls.

Line deltas across the five migrations: **−1, 0, +6, +6, +1 = +12.** Code-only, mag7 is −5
(its hand-built `EngineRunStats` genuinely disappears); the rest is comments recording
measured findings.

**Wave C is a wash on code size. That is the finding, not a shortfall.** Its real
deliverables are (1) the measurement that disproved L11, (2) five drivers migrated with
byte-identical output proven at the object-symbol level, (3) **ten driver byte-goldens that
did not previously exist** — before T2 not one of the five drivers had any output-byte
regression anchor, and (4) six plan errors found and reported by implementers rather than
worked around. Anyone looking for a code-reduction win will not find one; read the plan's
L11 reality-check table rather than re-litigating the abstraction.

---

## The pre-Wave-D hygiene commit — and the bug inside it

Wave C's gate found `cmake --build C:\atx\build-rel` (the **default** target) had been
exiting 1 on two `/WX` errors. Neither was in the wave's diff, and neither target had
**ever** been compiled in this build dir — every prior task built named targets. It was
load-bearing rather than cosmetic because Wave D T1 mandates a full build.

The second one was a **real bug, not a warning**. `apply_guard` in
`atx-engine/tests/core/phase4_integration_test.cpp` asserted with `ATX_ASSERT`, which is
`((void)0)` under `NDEBUG`. In Release the body vanished — which is *why* both parameters
read as unused, but the real consequence is that its
`EXPECT_DEATH(apply_guard(model.value(), 0U), ".*")` **could not pass in Release**: the
guard no longer aborted. The test was vacuous under NDEBUG, unnoticed only because the
target had never been built. Fixed with the unconditional `ATX_CHECK` — explicitly **not**
with `[[maybe_unused]]`, which would have silenced the compiler and left the test vacuous.

Byproduct: `atx-engine-core-tests.exe` now exists and ran for the first time — **326 ran /
314 passed / 12 failed**, all pre-existing death-test-style failures. Recorded, not chased;
they predate the sprint and are outside its charter.

From `017fc5c` onward, "the full build is green" is a claim that can be made, and any new
warning belongs to the wave that introduced it.

---

## Wave D — where it actually is

Wave D retires a **shadow replay loop** (`collect_mark_divergence_replay`) that re-walks the
clock and re-loads every archive to recompute mark divergence the engine already knew as it
stepped. The payoff is deleting working code, so the wave is built entirely around proving
the replacement equivalent first.

| Task | What | Result |
|---|---|---|
| T1 | `StepObserver` + `StepEvent` + `RunConfig::step_observer`, engine firing | `42c60d8`. **Approved first pass.** Suite +5/+5. |
| T2 | mark-divergence collector on the observer | `c464051`..`c438a64`. Clean after 1 fix round. Suite +7/+7 then +1/+1. |
| T3 | dual-run comparator, shadow **retained** | `f60ce3c`. Spec ✅, **2 Importants — fix round in flight.** |
| T4 | 135-session equivalence proof (controller) — **BLOCKS T5** | Not started. Prepared; see below. |
| T5 | delete the shadow loop | Blocked on T4 by design. |
| T6 | L12 — `RunSpec.index_symbol`, de-SPY `all_symbols`/`universe_at` | Not started. |
| T7 | Wave-D integration gate | Not started. |

### The three findings that carried the wave

**1. Bit-identity is structurally implied, not hoped.** T1's review established something
stronger than the implementer claimed: `ListedDispersionStrategy::on_step` **never reads
`book`** — it only writes it, after the divergence loop — and the rows are computed from the
snapshot + the frozen schedule + the policy alone. So every book difference between the
shadow (which skips transition validation, execute, hedge, settlement and expiry erase) and
the observer riding the real engine book is **provably irrelevant** to the divergence rows.
T1 *enabled* T4's proof rather than foreclosing it.

**2. The load path is the half that is genuinely unmeasured.** `live_mark` comes from the
loaded surface, and the two routes load differently — the shadow calls
`MarketSnapshot::load(path, tier)`, the engine calls
`SnapshotCache::load(path, tier, build_policy)`. Under the CLI's `Eager` default they
agree, and both reduce to *deserialize archive → `with_query_pricing(tier)`*, a
deterministic pure function of the same bytes. **That is why T4 must measure it on a route
with a nonzero row count** — and it is what makes the comparator the right instrument for
*future* divergence, which is what the deletion really rests on.

**3. T2 shipped a collector whose multiplicity and accumulation were ungated.** Every
fixture perturbed one leg on one step and the engine test ran a one-date clock, so three
mutations survived all seven tests: `out.clear()` before the push, `break` after the first
push, and hoisting the push out of the loop — exactly the properties T4 bit-compares. The
fix perturbs two non-adjacent legs (differing in symbol, side, strike, `diff` **and** bps)
and rebuilds the engine test on a two-date/two-roll clock giving 3 accumulated rows.
Mutation evidence isolates each gate: M-F → 2 failures, M-G → 2, **M-H → 1, the engine test
alone**, proving it is the two-roll clock and not the multiplicity test that closes
accumulation.

Also worth recording: T2's implementer **added a 7th test the brief did not specify**,
driving the real `run_backtest`. All five briefed tests hand-build the `StepEvent`, so as
specified the task would have shipped with **zero evidence the collector is reachable from
the engine** — the precise Wave B failure mode.

---

## In flight when work stopped

**Wave D T3, fix round 1 of 5 — dispatched, not yet returned.** Two Important findings, no
Criticals. The comparator itself was judged sound: the reviewer searched for a path where it
could report identical while the sources differ and **found none** except `n == 0`, and it
independently reproduced T3's `observer=36 shadow=36 rows MATCH` on a 3-session/2-roll
fixture.

**Important 1 is the controller's fault and is recorded as such.** My dispatch said the
comparator "must assert the compared row count is > 0 on that route and fail the gate as
vacuous if it is 0". I did not scope it; the implementer built exactly that; it is
mis-scoped. The reviewer proved it empirically rather than arguing it:

- Zero rows on `--execution configured` is **legitimately reachable**. A row exists iff
  `seed.greeks().price != leg.model_mark` (exact `!=`), and `QueryPricingRoute::ColdFallback`
  is documented as what a fast-configured surface reports outside its certified correction
  box — with `priced_surface_test.cpp` pinning that fallback as **exactly equal** to the cold
  value. A run whose legs all fall outside the box reproduces the frozen cold marks
  bit-for-bit and emits zero rows, as does any schedule authored under the fast tier.
- It patched a fixture's `trade_schedule.tsv` so each diverging leg's `model_mark` equalled
  the configured route's own `live_mark` — a well-formed schedule that passes validation —
  and got `EXIT=1`.
- **Worse than an exit code: the run's whole output is destroyed.** The `return Err`
  precedes `write_run_archive`, so the run dir afterwards contains **no `run.atxrun` at
  all** — `projected_cold`, `meta`, `diagnostics` lost. Before the change that run wrote all
  four sections and exited 0.
- **And the conditioning is keyed on the wrong axis.** On the same patched fixture, `cold`
  produced 36 rows and passed while `configured` produced 0 and failed. Neither "cold ⇒ 0"
  nor "configured ⇒ > 0" is a property of the routes — both are properties of the schedule's
  provenance. The guard *permits* the one genuinely vacuous case and *kills* a run that is
  merely uninteresting, on the **default** route.

**Ruling given, both halves:** make the guard opt-in and route-independent via
`--require-divergence-rows` (T4's proof run passes it; nothing else gains a failure mode),
**and** relocate the `return Err` to after `write_run_archive` regardless — the exit code is
recoverable, the archive is not.

**Important 2:** the MATCH line goes to stdout while the "0 rows — plumbing check" caveat
goes to stderr, so a stdout-only log shows a bare, unqualified
`mark divergence equivalence: observer=0 shadow=0 rows MATCH`. That is exactly what T4 greps
and what a ledger transcript would carry — i.e. the artifact **T5's deletion decision rests
on**. Fix is to put the qualifier on the same stdout line.

**Also in flight:** a background PowerShell job hashing `parity-full`'s two 696 MB
`definitions*.tsv` files to decide whether the T4 corpus copy can skip one. Harmless,
read-only, no output yet.

---

## T4 is prepared — read this before running it

T4 is the 135-session equivalence proof and it **blocks T5**. Two things were settled before
stopping:

1. **Run on COPIES ONLY. Never on `parity-full` itself.** The plan's Step 1 says to run cold
   directly on it. That is unsafe: `run-projected-backtest` writes `projected_cold`,
   `mark_divergence`, `meta` and `diagnostics`, and `run_archive`'s merge-write carries
   sections forward only if their name is **not** in the incoming write set — *"on a name
   collision the NEW section wins."* Running in place would overwrite the pinned cold
   goldens the proof exists to compare against. A copy is free and correct:
   `run_identity_hash` folds only `run_spec.tsv` + `universe_schedule.tsv`, and the
   manifest's archive paths are absolute, so a metadata-only copy runs correctly and keeps
   the same identity. (`parity-full` is 1.4 GB, dominated by two 696 MB `definitions*.tsv`;
   `definitions-orig.tsv` is a backup the run does not read.)
2. **The escalation path is pre-decided, so nobody improvises at the gate.** If
   `--execution configured` cannot complete on the corpus, or yields N == 0, fall back to the
   **perturbed-`model_mark` copied-schedule corpus**. Do **not** fall back to "T2's gtest
   plus T3's RED probe suffice" — a gtest that hand-builds a `StepEvent` cannot prove the
   engine fires the hook where the shadow looped, and that is the entire claim T5 rests on.
   If both routes fail to produce a nonzero-row bit-comparison, **T5 is not dispatched and
   the shadow stays**, recorded as a wave shortfall. Deleting a shadow whose replacement was
   never proven on a nonzero corpus is precisely Wave B's defect class.

The T4 invocation to use is in `task-3-report.md`; it needs the `--require-divergence-rows`
flag from the in-flight fix round appended to it.

---

## Residual risks carried into Wave E and the final review

1. **The largest one, from Wave C: no committed golden pins any driver's bytes.** All ten of
   Wave C's goldens live in session scratchpad. `mag7/engine_metrics.csv` even embeds an
   absolute `# db_root=` path, so **four** of the ten die with the scratchpad, not one.
   Change `spy_dispersion_pnl.cpp`'s `%.10g` to `%.9g`, or delete a `Meta` key, and the
   entire suite still passes. Closing it needs committed driver-produced golden fixtures —
   a separate task, and the named item for the final whole-sprint review.
2. **Contract comments rot.** Six `file:line` citations in contract comments went stale
   inside Wave C alone, one of them born stale in the very commit that fixed two others.
   Standing recommendation: **no line numbers in contract comments — name the symbol.**
3. **The box is shared with other sessions' worktrees.** One link-stage
   `llvm-objdump … OutOfMemoryException` was observed and judged purely environmental
   (retry with no source change succeeded). This matters for **Wave E**, whose measurement
   protocol requires a quiet box: every Wave E task must confirm quiet before measuring and
   discard any pair of numbers straddling foreign load.
4. **`run_timed`'s `Err` route is pinned by nothing executable** — the "error propagates
   verbatim" contract is provably true from `ATX_TRY_IMPL` and unreachable by any byte gate,
   but the same hole exists for all five migrated drivers.
5. **`atx-engine-core-tests`: 12 pre-existing failures**, newly visible. Outside this
   sprint's charter entirely; recorded so they are not rediscovered as a regression.

---

## Tooling hazard — corrected this session

The earlier status doc claimed "`diff` and `grep` via the Bash tool return WRONG answers".
That overstated it. Measured on a purpose-built fixture:

- **`diff` prints the correct textual difference but returns exit code 0 on differing
  files**, while `rtk proxy diff` correctly returns 1. Reading its output is safe; branching
  on `$?`, `&&`, `||`, or `if diff a b; then` silently concludes "identical".
- **`grep` is clean** — correct counts, exit 0 on match and 1 on no match, including through
  a pipe. The earlier claim that a piped `grep -c` miscounted **does not reproduce** and is
  withdrawn.

The operative rule is unchanged and still binding: establish and verify every byte
comparison in PowerShell (`Get-FileHash`, `Compare-Object`) and **print both values**. That
protocol earned itself during Wave C's gate — a case-insensitive PowerShell variable
collision (`$ref` clobbering `$REF`) made two audits print `ref lines = 0 / COUNT DIFFERS`,
indistinguishable from a real gate failure. Printing both values plus line counts is what
caught it; a boolean-only verdict would have produced a false GATE FAIL.

Separately, the **Grep tool** (not just Bash grep) still mis-renders characters — it showed
`//` as `\` in a header twice this sprint. Use Read when exact characters matter.

---

## Process notes

**Eleven plan errors have now been found and reported by implementers rather than worked
around** — including three separate briefs specifying a test that already existed verbatim,
a "RED" that was actually a positive control, a probe order that was not runnable as written
(pre-migration the driver did not consume the library, so the artifact half would have been
a false green), pinned floating-point literals unreachable under the `EXPECT_EQ` the same
brief demanded, and a fixture the brief pinned that no longer exists. That is the process
working.

Two implementer reports also **corrected their own claims** when reviewers pushed back
(a false lemma about `EXPECT_EQ` on a bps metric; a schedule-identity argument that was
wrong because `create` takes its schedule by value). Those corrections were itemised in the
reports so they are auditable rather than silent.

The full task-by-task record — every hash, every mutation, every ruling — is in
`.superpowers/sdd/backtest-wave-c/progress.md` (1139 lines; it covers Waves C, D and E
despite the directory name).
