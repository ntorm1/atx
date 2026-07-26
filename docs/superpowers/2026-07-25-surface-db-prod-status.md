# Surface DB Production — Sprint Status

**Date:** 2026-07-25 (stopped by operator)
**Branch:** `feat/surface-db-prod` — worktree `C:\atx\.claude\worktrees\surface-db-prod`
**Tip:** `80bff54` · **56 commits** ahead of merge-base `717e08d`
**Plan:** `docs/superpowers/plans/2026-07-22-surface-db-production.md`
**Run log:** `atx-vol/research/2026-07-surface-db-production-run.md`
**Ledger:** `.superpowers/sdd/surface-db-prod/progress.md` (gitignored)

**Money: $0.0000 spent of the $100 budget.** Five paid Databento calls; every one preceded
by its own logged free `get_cost` preflight under a hard cap.

---

## Bottom line

The production data run is **complete**. The library, both CLIs, and the tooling are
complete and reviewed. **One Critical defect is open** and the branch should not merge
until it is closed — see *Open items* first.

This session did not merely execute the plan; the run found six real defects in code that
had already passed review, and fixing four of them is most of what happened. That is the
run doing its job.

---

## The production artifact

`C:/atx-data/surface-db/prod-2026-07` — 2026-07-01 .. 2026-07-24, 51 names.

```
$ atx-vol-surface-db verify --db C:/atx-data/surface-db/prod-2026-07 --min-cells 850
partitions 17 / partitions_in_db 17 / symbols 51
cells_checked 867
cells_ok        858        <- 99.0% coverage
cells_unmappable  9
cells_non_finite  0
cells_checksum    0
symbols_disabled  0
verdict FAILED             ($? = 1, because 9 requested cells are genuinely absent)
```

17 partitions, 858 surfaces, 6.7 MB. **Every stored surface passes its payload CRC and
evaluates finite at the money.** All 9 failures carry a captured reason.

Source hive `C:/atx-data/opra-hive`: 140 date partitions, 470 MB, and **867 of 867 cells
present** for 2026-07 — a confirming dry-run reports `to_pull=0`.

Query spot-check at each name's own one-month forward: **SPY 15.3% < JPM 22.3% <
NVDA 38.6%** — the ordering the economics require (index < bank < high-beta semi).

### The 9 residual failures

| date / symbol | model | decoded mask | character |
|---|---|---|---|
| SPY 07-15, SPY 07-22 | convex-dense | `CarryGap\|Butterfly` | **marginal** — miss the no-arb bound by 0.000000 and 0.000107, slopes at −1.0 |
| AAPL 07-14 | essvi | `CarryGap\|InvalidDomain` | marginal |
| PG 07-09 | essvi | `CarryGap\|InversionResidual` | marginal |
| MCD 07-01, COST 07-08, UNH 07-15 | essvi | `CarryGap\|Calendar\|Butterfly\|StrikeMonotonicity` | **genuinely arbitrage-violating** — 18-21 butterfly *and* 17-28 calendar violations |
| KO 07-20, JPM 07-23 | — | — | same class |

They are left failing deliberately. Tuning admission thresholds to manufacture a clean
number would defeat the instrumentation this run exists to have added.

---

## What the run found and fixed

| # | Defect | Severity | Commit |
|---|---|---|---|
| A | **The fitter's diagnostic was computed and thrown away twice.** 9 cells failed and the operator got a bare count. Root-causing needed a multi-hour source investigation that still could not name the predicate. | Important | `069669e` |
| B | **The curve family was pinned for every symbol**, which switched off both fallback ladders — one attempt per cell, no recovery. | Important | `dba34a6`, `08ba2d9`, `22cabea` |
| — | FIX-A minors: an allocation entered the OOM handler; a false doc comment; a probabilistic ordering test | Minor | `d7fcb01` |
| C | **A punctuated ticker lost all its data silently**, and the loss went quiet on every resume after the first. | Important ×2 | `4b83967`, `83beb4d` |
| D | **A resume re-fitted 49 healthy cells for every failing one.** | Important | `80bff54` |

**FIX-B is the one with a measurable payoff.** Unpinning restored the fallback ladder and
recovered 7 of 10 lost cells on a like-for-like rebuild — coverage 77.8% → 93.3% on the
smoke set, and 99.0% at production width.

Flipping that default **changes fitted output everywhere**, not just runtime: the
classified profile's calibration is applied only when the auto route populates
`decision_`, so pinned cells were fitting with a default-constructed `CalibOpts`
(`max_outer_iter` 4 vs 50, `huber_k` 1.5 vs 2.0, `residual_disable` true vs false). That is
documented in `atx-vol/docs/surface-db-build.md`.

**FIX-C recovered BRK.B**, which had been silently absent: the loader derived two names for
one underlier (`BRK.B` from the column, `BRKB` from the OSI root), filed all 1,838 quotes
under one, and handed the fitter the empty other. `symbols_enabled` went 50 → 51.

---

## Review record

Every task was reviewed, and every review that found something was followed by a fix round
and a scoped re-review.

| task | review outcome | re-review |
|---|---|---|
| FIX-A | Spec ✅, Approved, 0 Critical / 0 Important, 3 Minor | — |
| FIX-B | Spec ✅, **3 Important** (all doc/report) | **all 7 addressed, 0 open** |
| FIX-C | Spec ✅ on deliverables, **1 Critical** | **all 7 addressed, 0 open** |
| FIX-D | Spec ✅, **1 Critical + 4 Important** | **NOT DONE — open** |

The reviews earned their cost. Three examples:

- **FIX-C's Critical was a regression this branch introduced.** Making the ticker dotted
  broke an economics check that had been tautologically satisfied, and because the caller
  wraps it in `ATX_TRY`, one punctuated name aborted an entire date's join for *every*
  name — where before it was dropped and counted. Reachable from a committed example
  against a committed universe file.
- **A false mechanism was caught and corrected in public.** An implementer reported that
  the manifest does not round-trip a pinned curve's `parametric` numerics, and this
  controller propagated it into the run log. It is false — the serializer round-trips ~30
  such fields. Corrected in place at `f2a75aa` rather than edited away, so nobody hunts a
  serializer bug that does not exist. The conclusion survived by a larger mechanism.
- **Verification claims were checked, not accepted.** One "pre-existing failure" proof was
  found unsound (its transcript never showed the object recompiling) and then established
  independently two other ways. One agent's own bite-test construction was wrong and the
  implementer said so. Two stale-binary traps were caught.

---

## OPEN ITEMS

### 1. 🔴 CRITICAL — the branch currently emits a false `TOTAL FIT FAILURE`

**This is the blocker.** FIX-D (`80bff54`) is committed with its Critical unfixed.

Carried cells increment `n_carried`, deliberately not `n_ok`. So on exactly the shape
FIX-D targets — permanent failures plus healthy carried siblings — the counters become
`cells_to_fit > 0, cells_ok == 0`, which trips `is_total_fit_failure`
(`surface_db_build.cpp:383`). The CLI then **exits 3** printing:

> `TOTAL FIT FAILURE: N cells scheduled, 0 fitted.`
> `Most likely cause: the carry rate does not match the hive… Re-run with the matching --r`

…on a **healthy, fully converged database**. An operator who follows that guidance and
changes `--r` invalidates every surface in the database.

The predicate's own header forbids exactly this ("Deliberately NARROW — the two
neighbouring shapes are both healthy and must not be swept in"); FIX-D creates a third
healthy shape it cannot see. The suite stayed green because the predicate is unit-tested
only on hand-built reports. **It surfaces only on the second post-FIX-D resume** — after
the change has been trusted.

Fix in flight: widen to `… && cells_carried == 0`, audit `is_total_config_failure`'s
identical clause, add an end-to-end case.

**A partial fix is UNCOMMITTED in the working tree** — 7 files, +122/−20, including the
widened predicate at `surface_db_build.cpp:393`. It is **unbuilt and untested**; it was
left uncommitted deliberately rather than committing unverified code. Either finish and
verify it, or discard it — do not assume it is correct.

Also open from the same review: `cells_carried` is invisible on every operator-facing
surface (CSV, terminal, Python bindings); `success_rate` reads 0% for carried symbols; the
"FNV-1a/64" constant is not FNV's prime (`0x1000'0000'01b3` vs `0x100000001b3` — stable,
so not a determinism break, but cheap to fix now and a **migration** once production
manifests carry fingerprints); and `write_partition` stamps a carry-blessing it cannot
verify on a public API.

### 2. 🟠 BRK.B is fixed at the seam, not end to end

`databento_spy_dispersion_definitions.cpp:355-357` filters definitions with an **exact**
root match. Fed the dotted universe, every BRK.B definition is still rejected, so under
`SkipUnlisted` the name remains silently absent from every dispersion basket. The
surface-db path is fixed; the listed-dispersion path is not.

### 3. 🟠 Five copies of the ticker→OSI-root rule, and they disagree

`pull_opra_hive.py` does `dotstrip(ticker) == striptrailingdigits(root)`; the two new C++
helpers do `dotstrip(ticker) == root` (strictly narrower); `build_ochain.cpp`'s `strip_dot`
strips both sides; `databento_pull.cpp:264` writes the **root** convention that
`build_ochain.cpp:97` documents as canonical — the opposite of what the Python tool writes.
**The producer normalises more aggressively than the consumer validates.** Duplicating this
rule is how the original defect was manufactured. Promote one `osi_root_of` and make the
trailing-digit question a named policy.

### 4. 🟠 A hard `CHECK` crash under memory pressure, not root-caused

A 6-date chunk died on `column.hpp:235 CHECK failed: raw != nullptr` after 17 minutes,
**losing everything** — zero partitions written, where a failed fit costs one surface.
Every date in that chunk succeeds run individually, so it is not a bad board; the
distinguishing factor was memory. Recorded with reproduction conditions, not root-caused
to a line.

### 5. 🟠 A full-month, full-universe build does not fit in 16 GB

The loader holds every date's decoded tables until the parallel pass. Chunk it. Peak memory
tracks symbol width and fit workers more than date count, so `--fit-workers` is the better
lever than narrowing the range.

### 6. 🟠 Pre-existing data loss: a disabled symbol is dropped on rewrite

A present-but-**disabled** symbol is silently deleted when its date is rewritten: the
would-drop guard is `present < part->count()`, and the disabled cell was already counted
into `present`, so the guard that exists precisely to stop this cannot fire. No existing
test covers it (`HonorsDisabledSymbol` runs against a fresh DB). Found during FIX-D and
deliberately not fixed there — burying it inside a performance change would have made it
invisible in review.

### 7. Smaller, recorded

Per-row guard not gated on `!filter.empty()` (whole-file blast radius on unfiltered loads);
`listed_dispersion_pipeline.cpp:87-89` swallows a failed panel with no counter;
`verify --symbols` reports no dropped-disabled names; the C++/Python `underlying` column
convention split; T7's python binding remains unverified; `cells_to_fit == 0` is
unachievable by design (see below).

---

## A plan gate that cannot be met, and why it is not being redefined

The plan requires "immediate re-run → `cells_to_fit == 0`". **That is unachievable by
design.** Failed fits deliberately retry forever so a transient failure stays recoverable;
there is no persisted known-failed state, and the source confirms no channel by which a
prior failure could suppress a retry. The only way to satisfy the gate as written would be
to persist known-failed state, which the design explicitly rejects.

Measured at production width: 3 useful retries dragged **147** healthy re-fits — a **49×
amplification whose multiplier is the universe width**. The 3 retries are correct; the 147
were the defect. FIX-D targets the 147 (`cells_refit` 2 → 0 with `cells_carried == 2` in
test), and the honest invariant is **`cells_refit == 0`**.

---

## Test state

- Focused surface-db suites: green throughout, at each stage.
- Full `atx-vol` suite at last complete run: **2102 / 2105**.
- The 3 failures are pre-existing and independently corroborated: two
  `SurfaceV2Qualification.RiskBuild…/{Latency,Balanced}` cases (expect `max_borrow_pairs`
  6/12, source sets 5/5/12) and `PreparedPortfolio.GroupedPrice…`. All three are listed as
  RED at `main@7fca341` in `atx-vol/sprints/2026-07-16-…md:148`, nine days before this
  branch. **Caveat:** `PreparedPortfolio` was green at a later commit — it is a
  host-sensitive golden fingerprint that has flipped before. "Pre-existing" here means "not
  caused by this branch", **not** "stably red".
- Also known: an `OpraPanel` temp-dir race under `ctest -j16` that passes serially
  (`gtest_discover_tests` gives one process per test against a fixed shared path).

---

## Working tree and process notes

- **Uncommitted:** 7 files of unverified FIX-D C1 work (see Open item 1). Nothing else in
  `atx-vol/` or `docs/` is dirty.
- **Not merged.** Left for the operator, per instruction.
- Session hazards worth carrying forward: the **`PowerShell` tool's working directory is
  `C:\atx`** — the main checkout on a *different* branch — so relative paths there silently
  run stale binaries from other work. The Bash cwd also resets there intermittently. Use
  explicit `cd` or absolute paths in every call. Separately, **piping a command whose exit
  code matters reads the pipe's status, not the tool's**; this made a correctly-failing
  `verify` look silently green once, and hid a migrate traceback earlier in the sprint.
