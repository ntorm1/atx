# atx-vol backtesting sprint — status

**As of** 2026-07-25, HEAD `876e0d5` on branch **`sprint/atx-vol-backtest-waves-cde`**.
**Wave D is CLOSED.** Every commit on the branch has been reviewed. Wave E is in progress.

Nothing has been merged into local `main`, per instruction. `main` still points at
`2858cab` (Wave C T4). The branch carries everything after it.

The sprint's charter: productionize the backtesting pipeline — get the economics out of a
~1000-line example file into the library, give results a real storage format, and improve
correctness and performance. **Waves A, B, C and D are closed. Wave E is under way.**

---

## Where each wave stands

| Wave | Scope | Status |
|---|---|---|
| **A** | `run_diagnostics` + `run_archive` + the RunArchive binary format + schema single-source + pure-Python reader | **CLOSED** `0f485e6` |
| **B** | `listed_dispersion_pipeline` — dispersion economics under test, M1 fixed, two-route parity guarded, CLI thinned | **CLOSED** `587ee97` |
| **C** | `backtest_driver` spine; migrate all five example drivers | **CLOSED** `cb875dc` — gate PASS |
| — | pre-Wave-D hygiene: make the full Release build green | **DONE** `017fc5c` |
| **D** | `StepObserver` (L10) retires the divergence shadow loop; de-SPY `dispersion_workflow` (L12) | **CLOSED** `876e0d5` — gate PASS, whole-branch review Approved |
| **E** | Perf — 4 passes kept of the original 7 | **In progress** — T1 running |

### Commit chain on the branch

```
876e0d5  docs(vol): Wave D whole-branch review — Approved, 0 Critical
5ab3778  docs(vol): Wave D T7 gate — Steps 1-4 GREEN, all six L12 sites executed
20d0c00  docs(vol): close Wave D T6
0a895b8  refactor(vol): RunSpec.index_symbol replaces the SPY hardcode     (D T6)
3e1db2b  docs(vol): close Wave D T5
d955e93  refactor(vol)!: delete the mark-divergence shadow loop            (D T5)
47c0d35  docs(vol): close the T3 fix round — re-review clean
819c442  docs(vol): Wave D T4 — 135-session equivalence proof GREEN
f4e6350  docs(vol): correct the stop point
6c389de  docs(vol): sprint status at the Wave D stop point
5c227e8  fix(vol): vacuity gate opt-in and non-destructive                 (D T3 fix 1)
f60ce3c  feat(vol): dual-run mark-divergence equivalence arbiter           (D T3)
c438a64  test(vol): gate divergence row multiplicity and accumulation      (D T2 fix)
c464051  feat(vol): mark-divergence collector on the step observer         (D T2)
42c60d8  feat(vol): RunConfig::step_observer — the L10 substrate           (D T1)
017fc5c  fix(build): make the full Release build green                     (hygiene)
cb875dc  docs(vol): close Wave C — gate PASS, honest wave verdict          (C T8)
36f9707  refactor(vol): migrate mag7_dispersion_backtest                   (C T6)
a9e62e4  test(vol): say what the PnL-track gate actually covers            (C T5 fix)
35a55cf  refactor(vol): migrate spy_dispersion_pnl                         (C T5)
--- main is here: 2858cab ---
```

Branch diff vs `main`: **20 commits, 24 files, +3843 / −209.**

---

## Wave D — closed

Wave D retired a **shadow replay loop** (`collect_mark_divergence_replay`) that re-walked
the clock and re-loaded every archive to recompute mark divergence the engine already knew
as it stepped. The payoff is deleting working code, so the wave was built entirely around
proving the replacement equivalent *first*.

| Task | What | Result |
|---|---|---|
| T1 | `StepObserver` + `StepEvent` + `RunConfig::step_observer`, engine firing | `42c60d8`. Approved first pass. |
| T2 | mark-divergence collector on the observer | `c464051`..`c438a64`. Clean after 1 fix round. |
| T3 | dual-run bit-exact comparator, shadow retained | `f60ce3c`..`5c227e8`. Re-review clean, 0 Crit / 0 Imp. |
| T4 | **135-session equivalence proof** (controller) | `819c442`. **N = 137 rows, bit-exact.** |
| T5 | **delete the shadow loop** | `d955e93`. **−193 lines (−150 code).** Review clean first pass. |
| T6 | L12 — `RunSpec.index_symbol`, de-SPY `all_symbols`/`universe_at` | `0a895b8`. Approved, 1 Important (report, not code). |
| T7 | wave gate + whole-branch review | `5ab3778`, `876e0d5`. **Gate PASS.** |

**Suite: 2020 → 2044 ran, 1974 → 1998 passed.** Skipped/failed/disabled unchanged; the 3
failures remain exactly the documented pre-existing reds. Every test the wave added passes.

### The wave's central claim, and the three independent proofs behind it

The claim is that the observer-derived `mark_divergence` section is the same artifact the
shadow produced. Three different agents established it three different ways:

1. **T4, on the production corpus:** `observer=137 shadow=137 rows MATCH` across 7 rolls
   and 11 underlyings, 137 *distinct* bps values spanning 2.3e-12 to 795.8 bps, determinism
   proven on an independent pass from a fresh copy.
2. **T5's reviewer, inside the container:** per-section **binary payload crc32c + sha256**
   compared pre/post deletion — `mark_divergence`, `projected_cold` and `projected_nodiv`
   all binary-identical. This caught what no gate in the brief could: a TSV dump renders
   *decoded* rows, so a changed dictionary-encoding order would produce identical text and
   a different `run.atxrun`.
3. **T7, after the deletion:** `MD-CFG = 9e958a90ae15ac74` at N=137, byte-identical to what
   T4 measured while both sources still existed.

### Wave D's honest weak spots

- **The arena staging has no automated test.** T5 fills `MarkDivergenceArena` from the
  observer's rows in registry order; a transposition of two same-typed columns would be
  invisible to any gate where both sources already agreed. The same exposure the shadow
  had, so not a regression — but not closed either.
- **`verify` could not run on the parity corpus at all** (it needs `quality.tsv`, which
  that corpus has never had — proven by running `verify` read-only against the untouched
  source and getting the identical error). The gate was satisfied on a different corpus
  and the substitution is stated, not hidden.
- **The pinned positional row order of `mark_divergence` has no in-tree consumer.** Python
  reads by column name, then sorts by bps and takes the top 12. The ordering is enforced
  solely by the out-of-process section sha256.
- **`StepEvent::step_index` has no production reader** — tests only. Kept as a recorded
  decision (it is a natural member of a public step-observation API), not an oversight.

### Two Important findings from the whole-branch review

**1. The evidence-channel contract was weakened, and two earlier passes missed it.** The
retained `all_rolls_consumed()` gate cannot distinguish *"the observer ran and found zero
divergences"* from *"the observer was never installed"* — but the comment T5 moved onto it
claims exactly that distinction. The shadow's gate could make it, because it interrogated
the same object its collection loop read. The T5 dispatch named this as the highest-value
check in that review, T5's reviewer examined it and accepted the carry, and the overclaim
still survived to a third pair of eyes. Latent (T4 and T7 both prove the observer fires),
and **being fixed rather than carried.**

**2. The Python `RunSpec` binding omits `index_symbol`** while `read_run_spec` is bound and
the bound `all_symbols`/`universe_at` take no index argument — so a non-default index would
silently give the C++ CLI and Python different universes. Inert today. The distinction from
the deliberate `step_observer` omission is the point: that one was recorded as a decision,
this one was not. Python is out of scope this sprint, so it is a **named follow-up**.

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

## Residual risks carried into Wave E and the final review

0. **Two named follow-ups out of Wave D**, both out of this sprint's scope rather than
   forgotten: expose `index_symbol` on the Python `RunSpec` binding and thread it through
   the bound `all_symbols`/`universe_at` (Important-2 above); and reconcile
   `build_corpus_command`, which writes `occ_ess_inventory.tsv` only when `occ_ess_root` is
   set, with `build_schedule_command`, which calls `verify_occ_ess_evidence`
   **unconditionally** — so a corpus legitimately built without an OCC ESS root can never
   run `build-schedule`.
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

## Tooling hazard — five ways a byte gate has silently lied

This is the sprint's most transferable finding, and it kept growing. Every one of these was
observed, not theorised, and each produced a gate verdict that was **wrong in a way that
looked right**. All five are now enumerated in `SPRINT-CONSTRAINTS.md` and handed to every
subagent.

| # | How it lied | Evidence |
|---|---|---|
| 1 | Bash **`diff` returns exit 0 on differing files** (it prints the correct difference). `if diff a b; then`, `&&`, `\|\|`, `$?` all conclude "identical". `rtk proxy diff` returns 1 correctly. | measured on a purpose-built fixture |
| 2 | **PowerShell `>` redirecting a native exe** re-encodes with a UTF-8 BOM and CRLF | same `projected_cold` dump: `E0C2ABB1…` (FAIL) via PowerShell vs `cbabca44…` (PASS) via bash |
| 3 | **Unrecorded capture convention** — a hash pinned under CRLF normalisation reads as drift when re-checked raw | `AFD4C06E…` (CRLF) vs `39D47B8B…` (raw LF), same artifact |
| 4 | **A PowerShell helper named after a built-in alias** — `function H` resolved to `Get-History`, so every hash was `$null` | six gates "passed" comparing `$null` to `$null` |
| 5 | **`printf` interpreting backslashes** in Windows paths (`\a`, `\r`) | `C:\atx-data\…` silently became `C:tx-data\…` |

Two of these were found by subagents reporting against their own work.

**The unifying rule, and the one worth keeping: a comparison that cannot be shown to fail
has not been run.** Capture bytes with bash `>`, hash with PowerShell `Get-FileHash`, print
both the computed and the expected value, record which convention a pin was taken under,
and include a negative control that demonstrably returns `False`.

That protocol earned itself twice. In Wave C's gate a case-insensitive PowerShell variable
collision (`$ref` clobbering `$REF`) made two audits print `ref lines = 0 / COUNT DIFFERS`,
indistinguishable from a real failure — printing both values plus line counts is what
caught it. And in Wave D's gate, `trade_schedule.tsv` matched its golden **only because
`build-schedule` had failed and left the copied file untouched**: a copy-integrity check
masquerading as a regeneration gate, caught only by noticing the mtime had not moved.

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
