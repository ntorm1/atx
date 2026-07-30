# SP100 multi-year surface-database run — 2026-07-26 .. 2026-07-29

Plan: `docs/superpowers/plans/2026-07-26-sp100-surface-db.md`. Executed from the
`C:\atx-wt\pool-4` worktree on branch `feat/sp100-surface-db`. Per-task briefs,
reports and the sprint ledger live in
`.superpowers/sdd/2026-07-26-sp100-surface-db/` (`progress.md` is the index).

**Budget: $100 authorized. Realized: $0.1088 — all of it the Pilot B 2022
validation pull. The entire 244-session production hive cost $0.0000.** Every
pull was gated on a free `metadata.get_cost` preflight and executed only on an
exact `$0.0000` quote; see [Spend ledger](#spend-ledger).

## What landed

| Artifact | Shape |
|---|---|
| `C:/atx-data/opra-hive` | 244 sessions, **2025-08-01 .. 2026-07-24** contiguous, 12 months, **47,856,441 rows**, 2.6 GB, one `date=*/data.parquet` per session |
| `C:/atx-data/surface-db/sp100-2025` | **104** partitions, 2025-08-01 .. 2025-12-31, **10,283** surfaces, 102 symbols, 72,956,608 B (69.58 MiB) |
| `C:/atx-data/surface-db/sp100-2026` | **140** partitions, 2026-01-02 .. 2026-07-24, **13,922** surfaces, 102 symbols, 87,907,520 B (83.84 MiB) |
| `atx-vol/data/universe/sp100_2026-07.csv` | 102 rows (SPY index leg + 101 constituents), TAB-separated |
| `atx-vol/data/rates/us_3m_monthly.csv` | 55 monthly 3M-rate rows, operator-refinable |
| `atx-vol/tools/run_surface_db_backfill.py` | chunked per-year backfill orchestrator (pull → build → verify) |
| `atx-vol/tools/pull_opra_hive.py` | ET-anchored snapshots, minute-aware resume, absent-symbol sidecars |
| `C:/atx-data/surface-db/pilot-a-2026-rel`, `pilot-b-2022-rel` | Release-binary regression baselines (3 from-scratch builds each, byte-identical) |

**244 DB partitions == 244 hive sessions.** No hive session lacks a partition and
no partition exists without a hive session, in either direction, for either year.

---

## Stage log

### Stage 1 — binaries and baseline gates (Task 1, 1b)

Configured with `-DATX_BUILD_EXAMPLES=ON` and built `atx-vol-surface-db-build`,
`atx-vol-surface-db`, `atx-vol-tests`. C++ baseline over the sprint's regex
(`SurfaceDb|BuildSurfaceDb|GenerateSymbolConfigs|SurfaceArchive|OpraHive|SyntheticHive`):
**303 selected, 302 run, 301 passed, 1 failed** — `SurfaceDbAdmin.VerifyDbFlagsNonFiniteAtmProbe`,
pre-existing. Python baseline (pull tool + migrate tool): **33 passed**.

The failing test *was* the backfill's acceptance gate, so Task 1b repaired it
rather than deferring: `cb7fe2e` (a pre-sprint commit) had tightened
`PricedSurfaceView` to reject a non-positive spot up front, which invalidated the
test's premise that a zero-spot record still maps and only ATM evaluation can
catch it. Commits `407bf55` (retarget the ATM probe at the smile, not the
forward) and `a62b1b7` fixed it, with forward-branch coverage added.

Two environment findings that shaped every later stage:

* The nested `powershell scripts\atx-build.ps1 ...` form re-splits the command
  line, so a `|`-containing ctest regex breaks out into a real PowerShell
  pipeline. Invoke the script directly from an ambient PowerShell session.
* A machine-wide editable install of `atxvol` pointing at the live `C:\atx`
  checkout trips `conftest.py`'s contamination guard in any worktree. The sprint
  therefore runs Python through
  `.superpowers/sdd/2026-07-26-sp100-surface-db/pytest_local.py`, which repairs
  `sys.path`/`sys.meta_path` for the worktree and never imports `atxvol`. The
  standing rule behind that: **`atxvol` and `pyarrow` cannot share a process on
  Windows** (Arrow DLL collision, documented in `atx-vol/python/README.md`).

### Stage 2 — universe (Task 2)

`atx-vol/data/universe/sp100_2026-07.csv` (`39b5843`): SPY at weight 100.0 plus
101 constituents on strictly descending weights, TAB-separated to match the
existing `spy_top50_*.csv` convention. Provenance note `sp100_2026-07.md` records
the survivorship bias explicitly — it is a 2026-07 membership snapshot applied to
older sessions, so early-window coverage gaps are by design. 4 tests.

### Stage 3 — pull tool: ET-anchored snapshots (Task 3)

`9bd883e` .. `dbc7f3c`. Three capabilities, all on `pull_opra_hive.py`:

1. **`--snap-et HH:MM`** (mutually exclusive with the legacy `--snap-utc`).
   `snapshot_minute_utc(date, "15:55")` resolves the real IANA offset per date, so
   one invocation spanning a DST transition stamps 19:55Z on EDT sessions and
   20:55Z on EST sessions. The DBN cache filename carries the resolved UTC minute
   (`2022-01-03_2055_<sha12>.dbn.zst`); the run manifest keeps the stable ET label
   (`manifest_hive_..._1555.csv`).
2. **Minute-aware resume.** `plan_missing` reads each on-disk date file's stamped
   `ts` from parquet footer statistics (never a full read) and, on disagreement
   with the expected minute, re-plans the date with *every* requested symbol and
   logs `MINUTE-MISMATCH <date> have=<HH:MM> want=<HH:MM> — repull` on stderr. A
   flagged date is written as a **full replacement**, not a force-merge — review
   round 1 caught that the merge path preserved stale wrong-minute row groups for
   any symbol the fresh pull did not return, which would have left two distinct
   `ts` values in one file and broken the one-constant-`ts`-per-date invariant the
   C++ loader chain depends on.
3. **Absent-symbol sidecars.** `<hive>/_absent/<date>.json` records
   `{minute_utc, symbols, asof}` for requested symbols that returned zero rows,
   so a provider-absent name is not re-requested (and re-billed) on every resume.
   Subtraction requires an exact `minute_utc` match, so a sidecar from the other
   DST era is ignored rather than trusted. A whole-session zero-row response is
   **not** latched (early-close/holiday signature); it prints `EMPTY-SESSION` and
   re-checks next resume.

Step 6 of that task audited the C++ side for hard-coded minutes and found the
constraint that governs the whole build stage: `OpraHiveSpec` exposes exactly
**one** `snapshot_suffix` per load call, applied uniformly across `[date_lo,
date_hi]`, and that string (not the file-read `snapshot_ts_ns`) drives the
year-fraction / time-to-expiry math. A single load spanning DST is therefore an
hour wrong on one side. The workaround is code-free — chunk the load per DST era
— and is what the orchestrator does.

### Stage 4 — rates table + chunked orchestrator (Task 4)

`b704ec1` (C++ `--snapshot-suffix` flag, validated through a header seam
`tools/surface_db_build_cli.hpp::is_valid_snapshot_suffix`, usage-error exit 2 on
a malformed value) and `ddced32` .. `336f48d` (`us_3m_monthly.csv` +
`run_surface_db_backfill.py`, 789 lines).

Every decision in the orchestrator is a pure importable function — rate lookup,
DST-aware chunking, verify thresholds, spend ledger, command construction,
bisect-and-retry — with only `run_subprocess` and the three `phase_*` drivers
touching the filesystem. **60 tests** after review round 1, which fixed five
findings, the critical one being that `phase_pull` handed raw month bounds to the
pull tool and so re-requested early-close dates forever (the pull tool
deliberately does not latch a zero-row session). `pull_windows_for_month` now
cuts a new window at every early close; verified over all 55 months of the rates
table as clean of early-close windows.

`chunk_sessions` groups by `(month, snapshot-minute)`, which is what makes both
`--r` (resolved from the chunk's first session's calendar month) and
`--snapshot-suffix` valid for the whole chunk — and is why no chunk can straddle
a DST boundary.

### Stage 5 — Pilot A: 2026-07, EDT, r = 0.043 (Task 5)

3 symbols × 5 sessions into `C:/atx-data/opra-hive-pilot-a` /
`C:/atx-data/surface-db/pilot-a-2026`. Preflight `$0.0000`; real pull 133 s,
15/15 cells, 106,780 rows, `ts` constant `19:55Z` on all five files. Build via
the orchestrator split 5 sessions into 4+1 under the default `--chunk-sessions 4`
and carried `--r 0.043000 --snapshot-suffix T19:55:00Z` on both chunks; 15/15
cells ok, 0 failed. Verify exit 0.

**Cross-check vs the pre-existing production DB was exact.** Querying
`prod-2026-07` and `pilot-a-2026` at `key=2026-07-10`, tenor 0.0833:

| symbol | pilot IV | prod IV | Δ | pilot forward | prod forward | Δ |
|---|---|---|---|---|---|---|
| SPY | 0.12077414829583467 | 0.12077414829583467 | **0** | 757.70201778430931 | 757.70201778430931 | **0** |
| NVDA | 0.37307511998560827 | 0.37307511998560827 | **0** | 210.94689672256254 | 210.94689672256254 | **0** |
| MSFT | 0.43432442028686213 | 0.43432442028686213 | **0** | 386.579962633224 | 386.579962633224 | **0** |

`total_variance`, `uid` and `n_slices` matched bit-for-bit as well. Calibration:
5.07 s/cell, peak working set 178.51 MB at 3 symbols in flight.

### Stage 6 — Pilot B: 2022-01, EST, r = 0.0015 (Task 6)

Pilot B is the only stage in this sprint that spent money, and it is where the
sprint's central planning assumption died.

The plan asserted a flat-rate account (`metadata.get_cost` returns $0, so `--cap`
never binds), verified against a fixture whose every sample is dated 2026-07-17.
Pilot B's 2022 preflight came back **$0.1088**, not $0.0000 — the task's own stop
condition — so it reported BLOCKED rather than waving through a trivial dollar
amount. Free `--dry-run` bracketing then established that the account's
entitlement is a **rolling recent window**, not a blanket archive grant:

| Probe window (3 symbols) | unit $/sym-day | free? |
|---|---|---|
| 2022-01, 2023-01, 2024-01, 2025-01, 2025-07 | ~0.0058 – 0.0092 | **no** |
| 2025-12, 2026-01, 2026-07 | 0.000000 | yes |

Extrapolated to 102 symbols across 2022-01 .. mid-2025 that is several hundred
dollars — an order of magnitude over the cap. The user authorized $0.1088 for
Pilot B specifically (ceiling $1.00, `--cap 1`) and later reshaped Task 7 to
free-window-only.

The pull returned 15/15 cells, 73,624 rows, `REALIZED ESTIMATE $0.1088`. pyarrow
verification: every date file carries exactly one distinct `ts` and it is
`<date> 20:55:00` — **the empirical EST-anchor confirmation Pilot A could not
provide.** Build carried `--r 0.001500 --snapshot-suffix T20:55:00Z` on both
chunks (contrast Pilot A's `0.043000` / `T19:55:00Z`, proving the rate lookup is
date-driven), 15/15 cells, verify exit 0, 15/15 ATM IVs inside their bands.

Two independent realism signals beyond the bands: SPY 30d ATM IV rises
0.1195 → 0.1555 across Mon–Fri with the jump landing on **2022-01-05**, the
hawkish-FOMC-minutes session, and MSFT/NVDA move the same way on the same day;
and the forwards track the week's actual closes to well under $1 through a real
~2.4% drawdown. The brief's static forward bands were narrower than that week's
range, which is why SPY/NVDA read a few dollars outside them — adjudicated PASS,
the bands were the estimate that was wrong.

#### The negative control — and the hazard it exposed

Not required by the brief: the same 20:55Z hive was rebuilt **without**
`--snapshot-suffix`, i.e. falling back to the default `T19:55:00Z`, onto a
scratch root.

```
EXIT=0   n_load_errors 0   n_dates_loaded 5   dates_written 5
coverage.cells_ok 14   coverage.cells_failed 1
failed_cell 2022-01-05 SPY code=Unavailable detail=risk surface rejected: model=convex-dense
  mask=2064 butterfly=2 butterfly_slack=0.000148 ...
```

| cell (tenor 0.0833) | correct `T20:55:00Z` | wrong `T19:55:00Z` | rel. Δ |
|---|---|---|---|
| 2022-01-05 SPY K=470 | iv 0.15287756912614786 | **cell absent (fit failed)** | — |
| 2022-01-07 SPY K=465 | iv 0.15548672717479101, n_slices 32 | 0.15542699281483635, n_slices **31** | 3.8e-4 |
| 2022-01-07 NVDA K=275 | iv 0.48733840890522567 | 0.48699592741910186 | 7.0e-4 |
| 2022-01-07 MSFT K=315 | iv 0.29096014761045796 | 0.29071660578478353 | 8.4e-4 |

**The flag is load-bearing, and its failure mode is silent:** exit 0, zero load
errors, all dates written, and the damage is a dropped slice plus ~1e-3 IV drift.
A DST-grouping bug in the production build would not have been caught by exit
codes or load-error counters. That is why the Task 8 suffix audit exists as a
hard gate, and why the operator doc now names per-chunk log verification as
mandatory.

Calibration finding carried forward: Pilot B ran **7.46 s/cell against Pilot A's
5.07** despite 31% fewer rows — fewer rows but more fitter work per cell in the
low-rate regime. Both numbers were later invalidated as *absolute* figures (see
the perf interlude); the 1.47x *ratio* between the two regimes was not.

### Stage 7 — production pull, free window only (Task 7 + addendum)

Policy after the Pilot B finding, decided by the user: pull newest-first,
free-window dates only, stop the moment a preflight quotes more than $0.

`metadata.get_cost` was run over the **full 102-symbol universe** month by month
descending from 2026-07. Every month back through **2025-08 quoted $0.0000**. The
first metered month is **2025-07**, at `$5.6326 = $0.00251006/sym-day × 2244
cells`. Descending iteration stopped there.

Free date-granular probes then pinned the cliff *inside* 2025-07 (2025-07-23
metered, 2025-07-24..25 and 07-28..31 free), which identifies the entitlement as
a rolling ~12-month window that **drifts forward one day per day**. The
controller authorized a narrower exception for the 2025-07-24..31 tail
conditional on a fresh exact-$0.0000 quote. It was re-quoted on both days the
task ran and came back **$0.5213** both times — 2025-07-24 alone had already
fallen out of the window at $0.260667. **The tail was not pulled.** The hive
therefore begins at 2025-08-01, and that is a policy outcome, not a gap.

Per-month result, all preflight-gated at exactly $0.0000 and all realized at
$0.0000:

| Month | sessions on disk / expected | rows |
|---|---|---|
| 2025-08 | 21 / 21 | 3,796,849 |
| 2025-09 | 21 / 21 | 3,845,833 |
| 2025-10 | 23 / 23 | 4,273,009 |
| 2025-11 | 18 / 18 | 3,423,562 |
| 2025-12 | 21 / 21 | 3,902,610 |
| 2026-01 | 20 / 20 | 3,718,441 |
| 2026-02 | 19 / 19 | 3,711,623 |
| 2026-03 | 22 / 22 | 4,547,142 |
| 2026-04 | 21 / 21 | 4,325,163 |
| 2026-05 | 20 / 20 | 4,227,010 |
| 2026-06 | 21 / 21 | 4,483,829 |
| 2026-07 (to 07-24) | 17 / 17 | 3,601,370 |
| **total** | **244 / 244** | **47,856,441** |

"Sessions expected" is XNYS sessions minus early closes: **2025-11-28** and
**2025-12-24** close at 13:00 ET, so a 15:55 ET snapshot cannot exist for them.
Both are absent from the hive and the verifier **asserts** their absence rather
than tolerating it.

Three operational findings from this stage:

* **The 78 "partial" dates left by an earlier halt were two different bugs.**
  2026-01, 2026-02 and the EST part of 2026-03 were stale files from an older
  fixed-UTC `--snap-utc 19:55` pull — an hour off the policy instant on EST
  dates. The new minute-aware resume caught every one
  (`MINUTE-MISMATCH 2026-02-02 have=19:55 want=20:55 — repull`) and replaced
  them. The remainder were genuine 51-of-102 symbol shortfalls from an earlier
  small-universe hive and were topped up per symbol by footer-driven subtraction.
  No date directory was ever deleted; every repair went through the tool's own
  resume path.
* **A 102-symbol single-minute `get_range` reliably 504s** at the gateway. This,
  not concurrency, was the throughput blocker: with 9 processes in flight,
  52-symbol requests returned 0 failed sessions while 102-symbol requests in the
  same window failed 3–8 each. Fixed operationally with no code change — the
  universe was split into two disjoint halves (SPY in both as the degrade
  anchor) pulled **sequentially** per window, since `merge_date_file` writes the
  union and two concurrent writers on one `data.parquet` would race.
* **Orphaned pull processes survive their supervisors.** A halt left up to 14
  detached `pull_opra_hive.py` processes that the environment's permission
  classifier would not let us reap. Transient double-writer races on a date file
  are self-healing by design (atomic tmp+rename, same minute, footer-driven
  resume) and none survived final verification — but "the supervisor exited" does
  not mean "the pull stopped".

Full-hive verification (footers only, pyarrow in its own process): every date
file readable, exact 8-column schema, a **single** distinct `ts` equal to the
expected 15:55 ET instant, complete underlying coverage, early closes absent,
zero unexpected dates. `VERDICT: PASS`.

### Perf interlude — Phases 1, 2a, 2b

Task 8 was held twice while this ran, because the pilots' measured rates implied
a ~30 CPU-hour production build and an intermittent crash made unattended runs
untrustworthy.

**Phase 1 — root cause: the pilots were benchmarked on a fully unoptimized
binary.** `scripts\atx-build.ps1`'s `build` and `ctest` verbs hard-coded
`build\` and ignored `-Preset`, so `configure -Preset rel` really did configure
`build-rel\` and the follow-up `build <target>` then rebuilt `build\` and handed
back a Debug binary — with no error, because the target exists in both trees.
Through the sanctioned wrapper there was **no way to produce a Release binary**.
The `d`-suffixed DLLs in `build/bin` are the tell: the pilots ran Debug
`atx-vol` linked against Debug Arrow and Debug Parquet.

`9457562` fixes it by resolving the preset's `binaryDir` out of
`CMakePresets.json` and walking the `inherits` chain, so adding a preset cannot
silently desync the script. **The clean A/B — identical source, identical input,
back to back — is 102.4 s → 6.7 s = 15.3x from the build flags alone.** That, not
algorithmic work, is what closed most of the gap; `9fcfebe` (batched
`upsert_symbols`, O(N²)→O(N) manifest bytes) and `aa3e1a4` (`fit_workers=0`
resolving to the auto budget) contribute only at large N and are invisible on a
3-symbol pilot.

Phase timing settled where the seconds go: `populate` **97%**, `config` 0.7%,
`load` **0.6%**, exe startup 67 ms. The brief's suspects "loader I/O" and
"per-chunk process overhead" are both refuted by measurement.

Phase 1's correctness gate is **PASS with documented tolerance**, and the
tolerance matters — see [Bistable slices](#1-bistable-slices-open).

**Phase 2a — the intermittent `std::bad_alloc` was never an OOM.**
`panel_from_table` did `rows.reserve(n_rows)` where `n_rows` is the **whole date
file**, once **per symbol**, and then retained the over-reserved buffer. Commit
charge grew at 45.6 MB/symbol — **4,772 MB at 104 symbols** — while the working
set stayed under 170 MB, because `reserve` commits pages and touches none. The
process was hitting the system commit limit, which is why the threshold drifted
with host memory pressure and why RSS at death looked innocent.

`533fd2d` scans and indexes each hive date **once** instead of once per symbol
(P-01), sizing every buffer for the symbol's own rows:

| symbols | 10 | 25 | 50 | 104 |
|---|---|---|---|---|
| `load_s` before → after | 0.425 → 0.243 | 0.822 → 0.247 | 1.687 → 0.301 | **4.278 → 0.491** (8.7x) |
| peak commit before → after | 489.7 → 43.1 MB | 1,173.4 → 53.6 MB | 2,312.2 → 71.8 MB | **4,772.1 → 124.6 MB** |

Output neutrality was proven three ways: a field-by-field bitwise panel-equality
unit test against the pre-P-01 path, a cross-layout comparison against the v1
per-symbol file loader, and **SHA-256 equality of every written partition**
pre/post at 10/25/50/104 symbols and on all 10 pilot dates. `db98be5` separately
contains fit-worker exceptions as failed cells instead of a dead process.

`load_s` now grows 2.0x from 10 to 104 symbols, against 10.1x before. Peak
commit is ~130 MB regardless of symbol count, which cleared the unattended-run
blocker.

**Phase 2b — the brief's premise was wrong, and the measurement says so.** Phase
1 §8.1 read `sl_al_premium_evals : sl_al_boundary_solves ≈ 34:1` off the solve
ledger as "the premium quadrature is where the time goes". Those counters are not
commensurate: the first is per quadrature **node**, the second per whole boundary
**solve**, which is itself 448 node evaluations under the `populate` rung. A new
env-gated cycle probe (`c43538a`) measured CPU time instead of events:

| primitive | share of `populate` fit |
|---|---|
| fast-plane Andersen-Lake boundary solve | **60.99%** |
| ACCURATE audit / certification plane | 23.31% |
| **early-exercise premium quadrature** | **6.70%** |
| everything else (eSSVI, curve selection, arb checks, serialize) | 8.98% |

So the proposed premium surrogate is capped at **1.07x** and was deliberately not
built. The shipped answer (`de4ec24`) is `FitPreset::Bulk` / `--preset bulk`,
**default OFF**: `populate`'s tier in every field that sets surface quality, with
the de-Am *and* carry AL rungs moved onto the ladder's `ql_fast` scheme
(`{7,8,2,·,n_quad_price=32}`, sweep work per solve 448 → 112). Measured **1.47x**
on two dates 11 months apart (12.20 → 8.31 s and 21.38 → 14.58 s per 104-symbol
date), with alternating rep-by-rep interleaving because host drift over a
6-minute loop is larger than the effect.

Finding the carry rung is what made it work: solving the two sweep counts gave
**623,627 carry solves against 291,263 de-Am solves — 66% of the fast-plane
boundary solves are the PCP borrow fixed point**, and `carry_al_opts` is re-pinned
unconditionally in `apply_risk_policy`, so lowering only the de-Am rung caps the
win at ~1.1x.

Knob OFF is **bitwise identical** to the Phase-2a `*-rel` baselines (all 10
partition files, every SHA-256 matching). Knob ON is out of Phase 1 §7.4's
tolerance band on Pilot A and is documented as such — see
[the `bulk` tier](#4-the-bulk-tier-is-default-off-for-a-reason-open).

#### Rate history, honestly attributed

| Measurement | Value | Cause of the change |
|---|---|---|
| Pilot B, original (Debug, orchestrator) | **7.60 s/cell** | — |
| Pilot A, original (Debug, orchestrator) | 5.07 s/cell | — |
| Build-flag A/B, identical source + input | 102.4 s → **6.7 s (15.3x)** | `9457562` — Release actually reachable |
| Pilot B / Pilot A, Release | 0.30 / 0.34 s/cell | same |
| 104-symbol date, `populate`, post-Phase-1 | 19.2 s/date | same |
| 104-symbol date, `populate`, post-P-01 | **14.0 s/date** | `533fd2d` loader |
| 104-symbol date, `hft`, post-P-01 | **3.6 s/date** | `533fd2d` (loader is ~half of an `hft` build) |
| 104-symbol date, `bulk` (opt-in) | **8.31 s/date** | `de4ec24` cheaper AL rung, 1.47x |

The `~7.6 s/cell → 14.0 s/date` jump is **the build-flags fix**, not algorithmic
work. The brief's original 0.01 s/cell target was never reachable by removing
overhead: it was calibrated on ~2,000 quote rows per underlying, which holds on
average (208k rows / 104 underlyings ≈ 2.0k) but not for the index leg — SPY
carries 9,528 rows in Pilot B and 13,690 in Pilot A, 5–7x the assumption, and it
is also the name given the dense index recipe.

(The perf lane labels a full production date "104 symbols"; the tracked universe
is 102 rows including the SPY index leg, and a complete hive date carries 102–105
row groups. See [Coverage](#coverage--the-only-correct-invariant).)

### Stage 8 — production builds, per-year roots (Task 8)

Both years were built with the **Release** binaries at `533fd2d`
(`build-rel\bin\atx-vol-surface-db-build.exe`, `...-surface-db.exe`), preset
`populate`, `--fit-workers 0`, across resumed runs and two parent-process deaths.
No rebuild happened mid-run — a binary swap would have re-rolled the bistable
cells (below) and invalidated cross-chunk comparability.

The canonical build-chunk invocation, verbatim from `t8-2025/orchestrator.log`
line 1:

```
'build-rel\bin\atx-vol-surface-db-build.exe' --db C:/atx-data/surface-db/sp100-2025
  --hive 'C:\atx-data\opra-hive' --from 2025-08-01 --to 2025-08-06
  --symbols SPY,NVDA,MSFT,AAPL,...,CVS --index SPY --preset populate
  --r 0.043000 --fit-workers 0 --snapshot-suffix T19:55:00Z
  --report 'C:\atx-data\logs\sp100\t8-2025\build_2025_2025-08-01_2025-08-06.csv'
```

driven by:

```
$ python atx-vol/tools/run_surface_db_backfill.py \
    --universe atx-vol/data/universe/sp100_2026-07.csv \
    --hive 'C:\atx-data\opra-hive' \
    --db-prefix C:/atx-data/surface-db/sp100 \
    --from 2026-07-01 --to 2026-07-24 --phase build \
    --build-exe build-rel/bin/atx-vol-surface-db-build.exe \
    --admin-exe build-rel/bin/atx-vol-surface-db.exe \
    --log-dir 'C:\atx-data\logs\sp100\t8-2026' \
    --index SPY --fit-workers 0 --chunk-sessions 6
```

The final 17 July-2026 sessions were built in 3 chunks (dry-run first), all exit
0, 342 s wall. Note the earlier 123 days of 2026 were built at the default
`--chunk-sessions 4`, so that year carries two chunk sizes. That was verified
fit-neutral before use — `--r` resolves from the chunk's first session's calendar
*month* and `chunk_sessions` never spans a month or a DST minute, so `r` is
identical either way; `generate_symbol_configs` is idempotent and reported
`n_skipped_existing=102`; and `populate_universe_streaming` groups boards
`by_date` and rewrites each date's partition independently, so a date's fit
depends only on that date's boards. Empirically the 6-session slice failed
*less* (1.75% vs 2.24%).

#### Six acceptance gates, all PASS

| # | Gate | Verdict |
|---|---|---|
| 1 | every chunk of both years exits 0 | **PASS** |
| 2 | snapshot-suffix log verification (hard gate) | **PASS** |
| 3 | `verify` exit 0 per root, 0.7x floor, absent-latched coverage | **PASS** |
| 4 | cell accounting with failure reasons | **PASS** (rate > 2%, cause stated) |
| 5 | partition count == session count per year | **PASS** |
| 6 | spot-check queries, 3 dates × 3 symbols per year | **PASS** |

**Gate 1.** The full `orchestrator.log` corpus for both years, not just the
final chunks: **134 `cmd=` lines** (2025: 61, 2026: 73) = 115 real build-chunk
invocations + 3 `[DRY-RUN]` + 16 admin. **Zero non-zero exit codes anywhere.** No
bisect/retry ladder ever triggered, no `permanently-failed session(s)` line, no
exit 3 (produced-nothing) or exit 5 (coverage regression). Both years show a
second resumed pass re-walking earlier chunks in 2–15 s each — cell-aware resume
carries already-complete dates rather than re-fitting them.

**Gate 2 — the one the Pilot B negative control demanded.** For every `cmd=`
line whose `--db` is an `sp100-<year>` root, the auditor parses
`--from`/`--to`/`--snapshot-suffix`, enumerates the real XNYS sessions in the
window, and requires the logged suffix to equal the expected minute for **every**
session, against two independent oracles: **A** = `snapshot_minute_utc()`
imported from `pull_opra_hive.py` (real IANA tz data), **B** = a deliberately
literal hard-coded restatement of the rule, so that if A were itself wrong,
A-vs-logged would agree and hide it.

```
cmd= lines scanned                       : 134
suffix-bearing sp100 build lines checked : 118   (of which [DRY-RUN]: 3)
empty-session windows                    : 0
oracle A vs oracle B disagreements       : 0
chunks straddling the EST/EDT boundary   : 0
SUFFIX MISMATCHES                        : 0
GATE 2 (snapshot-suffix log verification): PASS
```

The boundary is independently correct: DST ended 2025-11-02 so the first EST
session is Mon **2025-11-03**; DST resumed 2026-03-08 so the last EST session is
Fri **2026-03-06** — exactly where the 2025 log flips at line 19 and the 2026 log
at line 13. A third, empirical cross-check: the pull side's own
`_absent/<date>.json` records the `minute_utc` actually used to pull each date,
and it agrees (`2025-11-24 -> "20:55"`, `2026-01-02 -> "20:55"`,
`2025-10-30 -> "19:55"`). Pull-side and build-side minutes match.

**Gate 3.**

```
verify_2025:  partitions 104  partitions_in_db 104  symbols 102
              cells_checked 10608  cells_ok 10283  cells_absent 325
              cells_unmappable 0  cells_non_finite 0  cells_checksum 0
              partitions_index_mismatch 0  symbols_disabled 0
              failures_reported 0  failures_elided 0
              min_cells 7425  max_absent 3183   verdict ok

verify_2026:  partitions 140  partitions_in_db 140  symbols 102
              cells_checked 14280  cells_ok 13922  cells_absent 358
              cells_unmappable 0  cells_non_finite 0  cells_checksum 0
              partitions_index_mismatch 0  symbols_disabled 0
              failures_reported 0  failures_elided 0
              min_cells 9996  max_absent 4284   verdict ok
```

Thresholds are exactly `verify_thresholds(102, n_hive_sessions)`:
`floor(0.7 × 102 × 104) = 7425` and `floor(0.7 × 102 × 140) = 9996`. Run through
the orchestrator's own `--phase verify` with full-year bounds so the thresholds
size off the whole year, not a resumed sub-range. `cells_unmappable`,
`cells_non_finite`, `cells_checksum`, `partitions_index_mismatch` and
`failures_reported` are **all zero for both roots** — every stored surface maps
zero-copy and ATM-evaluates finite. That is the strongest statement `verify`
makes; it is not a statement that the numbers are right or the coverage complete.

**Gate 5.**

| | XNYS sessions | early closes (absent by design) | hive sessions | **DB partitions** |
|---|---|---|---|---|
| 2025 (08-01..12-31) | 106 | 2 — `2025-11-28`, `2025-12-24` | 104 | **104** |
| 2026 (01-02..07-24) | 140 | 0 | 140 | **140** |

`partitions_missing 0` for both; set differences empty in both directions.
2026-07-03 is a full holiday for the Saturday July 4, not a short session.

**Gate 6.** 3 dates per year × {SPY + 2 others}, each probed at `--strike 100` to
read `forward`, then evaluated at the ATM strike across `T = 0.0833, 0.25, 0.5,
1.0`. **18 cells × 4 tenors = 72 evaluations, 0 assertion failures.** The
monotonicity asserted is **total variance `w(T) = iv²T` non-decreasing in T** — a
decrease *is* calendar arbitrage — not raw IV term structure, which is not
required to be monotone. It held on every ladder including at T = 1.0, where the
sparse-LEAPS bistability lives.

The strongest signal here is a seam that nothing forced to agree: **SPY's forward
is 685.0119 on 2025-12-31 (last session of `sp100-2025`) and 684.8601 on
2026-01-02 (first session of `sp100-2026`) — a 0.02% step across two
independently built databases.** AAPL 273.44 → 271.63 and MSFT 485.73 → 474.12
likewise. Forwards track real levels throughout (SPY 624.65 → 666.14 → 685.01 →
702.23 → 741.15); ATM IVs sit at SPY 12.3–16.7%, AAPL 22.3–29.5%, MSFT
18.9–41.6%, with 16–33 slices per surface.

---

## Spend ledger

| Stage | Preflight | Realized | Cumulative |
|---|---|---|---|
| Pilot A (2026-07, 15 cells) | $0.0000 | **$0.0000** | $0.0000 |
| **Pilot B (2022-01, 15 cells)** | $0.1088 | **$0.1088** | **$0.1088** |
| Production hive, 12 months, 244 sessions | $0.0000 each month | **$0.0000** | $0.1088 |
| 2025-07 boundary month (not pulled) | $5.6326 | — | $0.1088 |
| 2025-07 free tail, re-quoted twice (not pulled) | $0.5213 | — | $0.1088 |
| Builds, verifies, gates, perf work (all local) | — | $0.0000 | **$0.1088** |

**$0.1088 of $100 authorized.** The $100 was never drawn down beyond the single
Pilot B pull. Preflights are free (`metadata.get_cost`, no egress) and every one
of them is in the logs *before* its paid call; the audit over every log this
sprint produced (113 files, 115 cost lines) finds exactly two non-zero cost
lines, both from `--dry-run` preflights that never pulled.

The mechanism that made $0.0000 possible: the account's flat-rate entitlement is
a **rolling ~12-month window that drifts forward one day per day**. That is why
the span starts 2025-08-01, why 2025-07 was skipped, and why the free tail
authorized on 2026-07-26 was already metered by 2026-07-28. **It also means the
hive's own early dates decay:** 2025-08-01 preflighted free on 2026-07-28 at
~11.9 months old and meters within roughly a week. Any future repair or re-pull
of early 2025-08 must re-preflight, never assume.

---

## Snapshot policy — the invariant the whole sprint turns on

**Policy: 15:55 America/New_York.** That is `19:55Z` under EDT and `20:55Z` under
EST. Measured from the parquet footers of all 244 files:

| Snapshot minute | Sessions | Range |
|---|---|---|
| `19:55Z` (EDT) | **161** | 2025-08-01 .. 2025-10-31, then 2026-03-09 .. 2026-07-24 |
| `20:55Z` (EST) | **83** | 2025-11-03 .. 2026-03-06 (one unbroken run) |

The pull tool resolves this per date from `--snap-et 15:55`. The **C++ loader
does not**: `OpraHiveSpec` takes exactly one `--snapshot-suffix` per invocation
and applies it across the whole `[--from, --to]` range, and that string drives
the time-to-expiry math. So a build chunk may never straddle
2025-10-31/2025-11-03 or 2026-03-06/2026-03-09. `chunk_sessions` enforces this by
grouping on `(month, snapshot-minute)`.

**A wrong suffix fails silently** — exit 0, `n_load_errors 0`, all dates written,
and the damage is a dropped slice plus ~1e-3 relative IV drift (Stage 6's
negative control). Exit codes and load-error counters cannot catch a DST-grouping
bug. **Per-chunk log verification against `snapshot_minute_utc()` is therefore
mandatory, not advisory**, and there is still no hard assertion inside the
orchestrator — see [open items](#6-no-hard-suffix-assertion-in-the-orchestrator-open).

## Coverage — the only correct invariant

**`underlyings_on_disk ∪ absent_latched == universe`, the two sets disjoint.**
Not a row-group count, not a symbol-count threshold.

`2025-11-24` is the case that proves it. The date carries **95** of 102
underlyings — below any naive `row_groups >= 100` threshold — and looks like a
truncated write. It is not. A targeted `--force` re-pull of exactly the seven
missing names for exactly that date returned `requested=7 written=0 no_options=7
failed=0 | rows: returned=0`: the provider genuinely has no quotes for them in
that snapshot minute. The seven are `BLK, BMY, BRK.B, BX, C, CAT, CL` —
**alphabetically contiguous**, because OPRA distributes quotes across multicast
lines partitioned by underlying symbol range, so one line missing for one minute
produces exactly this block. 95 on disk + 7 latched = 102: the date is complete,
and its 90 stored surfaces are not a coverage hole.

Across the hive: 90 sidecars (43 empty, 43 single-symbol, 3 two-symbol, 1
seven-symbol), only 12 distinct names ever latched (`BK, BLK, BMY, BRK.B, BX, C,
CAT, CL, CMCSA, FDX, HON, SPGI`). The observed row-group band over all 244 files
is `min=95, p05=102, p50=102, max=105` — parquet may split a large underlying
across more than one row group, so 102–105 is the healthy band for a complete
date. Gate 3 asserted the invariant on all **90** latch-bearing dates (2 in 2025,
88 in 2026) with **0 violations**.

The hive also holds three underlyings outside the universe (`AMAT`, `LRCX`,
`MU`), left over from an earlier pull. The build correctly ignores them because
`--symbols` is always explicit.

---

## Cell accounting

Derived **per date from the DB and the hive**, not from `year_summary_*.csv` —
see the caveat below. `cells_ok` is the symbol set in each date's archive
directory intersected with the universe; `hive_present` is the universe
intersected with the distinct `underlying` values in that date's hive parquet
(read in a separate process, so `pyarrow` and `atxvol` never share an
interpreter); `provider_absent = 102 − hive_present`; `cells_failed =
hive_present − cells_ok`. Reasons come from the deduplicated
`(date, symbol, code, detail)` rows of the latest per-window build report CSV
covering each date (29 non-overlapping windows for 2025, 37 for 2026).

| | 2025 | 2026 |
|---|---|---|
| denominator (102 × sessions) | 10,608 | 14,280 |
| `cells_ok` (surfaces stored) | **10,283** | **13,922** |
| `cells_failed` (loaded, not stored) | **317** | **310** |
| provider-absent (never pulled) | 8 | 48 |
| **failure rate vs loaded cells** | **2.99%** | **2.18%** |

Three independent cross-checks tie out exactly:

| check | 2025 | 2026 |
|---|---|---|
| `ok + failed + provider_absent == 102 × n_sessions` | 10283 + 317 + 8 = **10608** | 13922 + 310 + 48 = **14280** |
| `cells_ok == info.surfaces == verify.cells_ok` | **10283** | **13922** |
| `provider_absent + cells_failed == verify.cells_absent` | 8 + 317 = **325** | 48 + 310 = **358** |

### Why the rate is above 2% — the borrow/dividend carry solve

| reason (deduplicated `(date, symbol)` rejections) | 2025 | 2026 |
|---|---|---|
| `carry=failed` — borrow/dividend carry solve | **122** | **151** |
| butterfly + calendar arbitrage | 117 | 57 |
| `inversion=failed` — IV inversion | 37 | 51 |
| calendar arbitrage only | 5 | 18 |
| butterfly arbitrage only | 16 | 7 |
| non-finite / other | 19 | 26 |

Codes: 2025 `Unavailable` 307 / `NotFound` 9; 2026 `Unavailable` 298 /
`NotFound` 12.

The failure mass is concentrated, not broad-based:

| | worst symbols (failed/attempted) | top-5 share | rate excl. top-5 | excl. top-10 |
|---|---|---|---|---|
| 2025 | AMT 48/104 (46%), CL 20/103, SYK 16/104, BK 11/104, DUK 10/104 | 33% | **2.10%** | **1.84%** |
| 2026 | AMT 47/140 (34%), CMCSA 27/139, SYK 25/140, SPY 18/140, KHC 14/140 | 42% | **1.32%** | **1.08%** |

**Stated cause: the borrow/dividend carry solve** — 39% of 2025 and 49% of 2026
failures — and the chronically failing names are precisely the high-dividend-yield
/ REIT-like underlyings where that solve is hardest: AMT (a REIT, the single
worst symbol in *both* years, ~15% of each year's failures), CL, DUK, KHC, SYK,
CMCSA, MDT, BMY, T, PM, JNJ, SO, MO. Excluding five chronic names puts both years
at or below 2%. Where a rejection is an arbitrage violation the slack is typically
1e-4 to 3e-3 — marginal tolerance misses, not gross misfits. This is a
**symbol-level fitter characteristic on a known-hard family, stable across two
independently built years, not a regression.**

`SPY` gets its own line because it is the pinned index leg every other symbol's
forward machinery leans on: 6 failures in 2025 (5.8%) rising to 18 in 2026
(12.9%). All 18 are `model=convex-dense` (the pinned dense index recipe), 15 of
them `carry=failed`, with `butterfly_slack` of 0 to 3.6e-3. Same signature both
years — the count rises but the mode does not change.

### Dispositions

* **317 vs 316.** The DB-derived `cells_failed` for 2025 is 317 but only 316
  rejection rows exist. The extra cell is **(2025-11-24, BKNG)** and it is a
  *loader* rejection, so it correctly produced no fit-rejection row: that date is
  a degraded provider snapshot (162,740 rows / 95 underlyings against ~190,000 /
  102 on both neighbours; BKNG at 1,841 rows against ~6,900; BK at 42 against
  ~500), and BKNG was too thin for the loader to form a usable panel. BK *did*
  reach the fitter and was rejected with `NotFound: fit_curve_surface: no expiry
  produced a usable slice`. It is the only loader-side `n_load_errors` hit in
  either year.
* **`year_summary_*.csv` is not authoritative.** `cells_failed=316` in
  `year_summary_2025.csv` is real, but `cells_to_fit=1995` is a resume accounting
  artifact (it is the *second* pass only, whose 29 chunks refit 1,995 cells and
  carried 8,400), and `config.n_symbols=2958` is `102 × 29 chunks` — the
  non-additive-counter defect parked in Task 4. Worse,
  **`year_summary_2026.csv` was silently overwritten** by the final 17-session
  run and now describes only those sessions (`dates_total 17`, `cells_ok 1686`,
  `cells_failed 30`). Read the per-chunk CSVs or the DB. The *loader* counters do
  corroborate: `n_coverage_holes=8` equals the independent provider-absent count,
  and `n_load_errors=1` is the BKNG cell.
* **The July-2026 slice introduced no regression.** 17 sessions / 1,716 loaded /
  1,686 ok / 30 failed = **1.75%**, against Jan–Jun's 12,516 / 12,236 / 280 =
  **2.24%**. Per-date surface counts of 98–101 are indistinguishable from the rest
  of the year, and two of the three new SPY failures carry the identical
  signature to the pre-existing ones.

---

## Timing, throughput, memory

| | 2025 | 2026 |
|---|---|---|
| real build invocations | 53 | 62 |
| `sum(duration_s)` | 4,214.8 s (70.2 min) | 82,691.0 s — see the anomaly below |
| excluding the anomalous entry | — | 61 invocations, 4,766.9 s (79.4 min) |
| amortized | includes one full idempotent re-scan pass; max single chunk 196.8 s | ~34 s/session |
| final slice (17 July sessions, 3 chunks) | — | 342 s wall / 320.1 s CLI = 18.8 s/session |
| partition bytes + manifest | 72,916,992 + 39,616 | 87,863,296 + 44,224 |
| per session-partition | ~0.67 MiB | ~0.60 MiB |
| per stored surface | ~7.1 KiB | ~7.1 KiB |

**The 21.6-hour chunk is a measurement artifact, not a performance signal.**
`build_2026_2026-06-17_2026-06-23` logged `duration_s=77924.172` yet `exit=0`:
the log jumps from `2026-07-29T00:45:43` to `2026-07-29T22:24:27`, which is
exactly when the previous implementer's parent process died and the host was
suspended. That chunk's data is healthy (`cells_loaded 404, cells_ok 400,
cells_failed 4, dates_written 4, n_load_errors 0`; partitions at 101/99/99/101
surfaces), and the very next chunk took 76 s. Absolute walls throughout this
sprint carry a **±40% host-contention caveat** — the box is an i7-1260P
(12C/16T), 15.7 GB, and it ran concurrent agents for most of the sprint. Ratios
measured back-to-back are the trustworthy quantity.

Memory: peak commit is **~130 MB per build process regardless of symbol count**
after `533fd2d` (`populate` 121.7 MB / `bulk` 120.8 MB at 104 symbols,
independently sampled in Phase 2b). Before it, a 104-symbol date committed
4,772 MB. The pilots' 178.5 MB (Pilot A) and 137.6 MB (Pilot B) peak working sets
at 3 symbols are Debug-era figures and are superseded.

`--fit-workers 0` resolves to the auto worker budget; the cycle probe measured
**7.7 of 8 workers busy** on this host, so the fit is not scheduler-starved at
the default budget.

---

## Pilot cross-checks

| Fixture | Regime | Result |
|---|---|---|
| `pilot-a-2026` (2026-07-06..10, EDT, r = 0.043) | recent, dense chains | 15/15 cells; IV bands pass; **delta 0, bit-for-bit** vs `prod-2026-07` on IV, forward, `total_variance`, `uid`, `n_slices` |
| `pilot-b-2022` (2022-01-03..07, EST, r = 0.0015) | history + DST + low rate | 15/15 cells; `ts = 20:55:00Z` on all 5 files; build carried `--r 0.001500` + `--snapshot-suffix T20:55:00Z`; 15/15 ATM IVs in band; forwards within <$1 of actual closes through a real 2.4% drawdown; `verify` exit 0 |
| `pilot-a-2026-rel`, `pilot-b-2022-rel` | Release re-baselines | `verify` exit 0, 15/15 cells, **3 independent from-scratch builds each, all partition files byte-identical**; Phase 2b's knob-OFF build reproduced all 10 partitions bit-for-bit (Pilot B `2022-01-03` = `870087ff464e106a…`, matching Phase 1) |

Pilot A and Pilot B together cover both sides of the DST branch with real data,
which is what makes the 244-session split (161 EDT / 83 EST) trustworthy. The
`*-rel` roots are the correctness baselines going forward; the Debug roots
`pilot-a-2026` / `pilot-b-2022` were never touched and are only reproducible by
the exact Debug binary that made them.

---

## Test gates at `de4ec24`

Run as the sprint's final gate, from `C:\atx-wt\pool-4`, tree clean.

**C++, `dev` preset** — the sprint's baseline regex, recorded in Task 1:

```
.\scripts\atx-build.ps1 build atx-vol-tests atx-vol-surface-db-build atx-vol-surface-db --parallel 6
  -> 236/236 build steps, exit 0
.\scripts\atx-build.ps1 -Ctest -R "SurfaceDb|BuildSurfaceDb|GenerateSymbolConfigs|SurfaceArchive|OpraHive|SyntheticHive"
  -> 100% tests passed, 0 tests failed out of 312
     (313 selected, 1 not run: SurfaceDbPopulate.ScalingCurveDiagnostic, Disabled by design)
     Total Test time (real) = 99.96 sec
```

Task 1's baseline over the same regex was 301/302 with one failure
(`SurfaceDbAdmin.VerifyDbFlagsNonFiniteAtmProbe`); Task 1b repaired it and it
passes here. The selection grew 303 → 313 as the sprint added tests.

**Python, sprint lane:**

```
python .superpowers/sdd/2026-07-26-sp100-surface-db/pytest_local.py \
  atx-vol/python/tests/test_pull_opra_hive.py \
  atx-vol/python/tests/test_run_surface_db_backfill.py \
  atx-vol/python/tests/test_migrate_opra_hive.py \
  atx-vol/python/tests/test_sp100_universe.py -q
  -> 109 passed in 4.28s
```

(27 pull-tool + 60 orchestrator + 18 migrate + 4 universe.)
`test_surface_db_build.py` is not collectable in this worktree — it `import
atxvol`, whose native extension is not built here. Worktree-local limitation, not
a regression; the C++ side of that surface is covered by the gtest suites above.

**C++, `rel` preset** — gated separately, because `rel` is the shipping
configuration. Phase 2b's wider filter, re-run at `de4ec24`:

```
.\scripts\atx-build.ps1 -Preset rel build atx-vol-tests --parallel 6
.\scripts\atx-build.ps1 -Preset rel -Ctest -R "SurfaceDb|OpraHive|OpraBatch|BuildSurfaceDb|American|
  AlBulkRung|FitPresetBulk|NegRateDomainMap|BoundaryHoist|Calib|VolaSession|CurveSelector"
  -> 99% tests passed, 3 tests failed out of 466   (140.73 s)

The following tests FAILED:
  466 - SviMmCalib.FixedFixtureFit_IsBitIdentical (Failed)
  1874 - SurfaceDbPopulate.CarryOverIsByteIdenticalAcrossWorkerCounts (Failed)
  2473 - BoundaryHoist.PriceBitIdenticalToPrechange (Failed)
```

**Exactly the three failures Phase 2b reported, independently reproduced here** —
all three pre-existing at `533fd2d`, all three passing under `dev`. Phase 2b saw
2–3 because the third is intermittent; this run caught all three. Detail and root
cause in [item 2](#2-rel-is-the-shipping-configuration-and-it-is-not-green-open).
**`rel` is not green, and this sprint did not make it so.**

## Limitations and open items

Nothing below is a gate failure. All of it is real, and some of it is load-bearing
for whoever works here next.

### 1. Bistable slices (open)

**A 1e-9 relative change to `--r` moves ~2% of fitted IVs by >1%.** This was
established as a controlled experiment, not inferred: take the *unchanged* Debug
binary, nudge `--r 0.0015 → 0.0015000000015`, and rebuild Pilot B.

| | Debug vs Release (build flags) | Debug vs Debug (1e-9 rate nudge) |
|---|---|---|
| max rel dev `iv` | 2.177e-01 | **2.177e-01** |
| worst cell | 2022-01-03/SPY K=573.13 T=1.00 | **the same cell** |
| alternate value | 0.178244798**49469935** | 0.178244797**8982718** |
| cells > 1e-6 | 10.48% | **10.71%** |

The same cells flip, to the same alternate values, in the same proportion. Some
slice fits sit between two local optima and the last bits of the input decide
which one is reached; divergence is worst at T ≈ 1.00, the sparse long-dated
LEAPS region. That made Phase 1's correctness gate a **PASS with documented
tolerance** rather than a clean pass — but a pipeline this sensitive is fragile
independently of this sprint. **Any compiler upgrade, Eigen bump, ISA change or
preset change will re-roll those cells.** Slice-fit conditioning (the T ≈ 1.0
sparse-expiry region first) is unstarted work. The Release re-baselines exist
precisely because of it.

### 2. `rel` is the shipping configuration and it is not green (open)

Phase 2b deliberately widened the ctest filter beyond anything this sprint had
used (`SurfaceDb|OpraHive|OpraBatch|BuildSurfaceDb|American|AlBulkRung|FitPresetBulk|NegRateDomainMap|BoundaryHoist|Calib|VolaSession|CurveSelector`,
466 tests) and found **3 failures under the `rel` preset that pass under `dev`**.
This task re-ran that filter at `de4ec24` and reproduced all three
(`99% tests passed, 3 tests failed out of 466`). All three were proven
pre-existing at `533fd2d`, not assumed:

* `SviMmCalib.FixedFixtureFit_IsBitIdentical` — pure `svi_mm_fit_slice` on a
  synthetic fixture, no Phase-2b code on its call graph at all; fails with
  identical values at `533fd2d` in `rel`, passes in `dev`.
* `BoundaryHoist.PriceBitIdenticalToPrechange` — fails by **exactly 1 ULP** on
  the `accurate` pin; verified twice (with all probe scopes neutralised, and at
  `533fd2d`).
* `SurfaceDbPopulate.CarryOverIsByteIdenticalAcrossWorkerCounts` — intermittent;
  7/8 fail at `533fd2d` in `rel`, 0/6 in `dev`.

Root cause of the first two is named in the tree: `tests/support/isa_golden_tol.hpp`
gives golden pins a band that is **zero (byte-exact) unless `__FMA__` is
defined**, on the premise that such values stay byte-exact on the SSE2 reference
ISA. `rel` defines no `__FMA__` yet its `/O2` codegen still moves the last bit —
which is item 1 again. **The band needs to widen on optimization level, not only
on ISA.** The third is a genuine worker-count-determinism failure in Release and
asserts a property the populate path actually claims; it is the one worth
chasing.

This sprint has only ever gated on `dev`, so the gap was invisible until the
filter widened. It should not be inherited silently.

### 3. `year_summary_<year>.csv` has two distinct defects (open)

It sums non-additive `config.*` counters across chunks (parked since Task 6), and
**a sub-range resume silently overwrites the file with only the resumed
sub-range**, destroying the prior aggregate in place. `year_summary_2026.csv`
currently describes 17 sessions of a 140-session year. Related log-hygiene item:
`--dry-run` appends `[DRY-RUN]` lines into the production `orchestrator.log`, so
a plan and an execution share one evidence file.

### 4. The `bulk` tier is default-OFF for a reason (open)

Knob OFF is bitwise identical to the baselines. Knob ON is **out of Phase 1
§7.4's tolerance band on Pilot A**: max relative `iv` deviation 6.60e-2 against a
1.37e-2 band, 1.36% of cells > 1e-2 against 0.27%. Attributed, not waved:
everything above 1e-2 sits at **T ≤ 0.15**, the worst cell being a 7-day option
23% out of the money where vega is ~0 and the rung's own 1.5e-3 price error maps
to a −0.037 absolute IV move. It is inside the economic price gate everywhere
(1.49e-3 max abs error against `min(0.5·tick, 0.1·vega·1e-4)` = 5.0e-3 on a $100
strike) and `forward` is essentially unmoved (max 9.1e-4 relative). It is **not**
bistability — 0 cells are bitwise identical and the deviation is monotone in
tenor and moneyness. **Do not promote `bulk` to the production default on the
strength of the 1.47x alone**; τ-gating the rung (the data says the damage is
where `σ√T` is small) or re-specifying the tolerance in economic rather than
relative-IV terms are the named follow-ups, neither done.

Adjacent: `DeAmOptions::serve_al_opts` is a workaround, not a fix.
`n_quad_price` is unpersisted in all three `AlOpts` record formats, so a baked
`{7,8,·,32}` rung would read back as `{7,8,·,0}` and every query against a
`bulk`-built surface would re-price off an 8-node premium quadrature nobody asked
for — invisible in every gate. The durable fix is to persist the field.

### 5. `--max-absent` is ~10x too loose (open)

The verify thresholds used here derive from the 0.7x floor: 3183 / 4284. The
**true** absent counts are 325 / 358, and this report baselines them. Tighten the
flag toward the real figures so a run that destroys stored surfaces flips the
verdict to exit 4 instead of needing a human to notice — an absent cell is
byte-identical whether it never fitted or was wiped by a whole-file partition
rewrite, and `cells_absent` moves no verdict on its own.

### 6. No hard suffix assertion in the orchestrator (open)

Gate 2 is an *audit over the logs after the fact*. The orchestrator still does
not assert that each chunk's `--snapshot-suffix` equals `snapshot_minute_utc()`
for every session in the chunk, and the failure mode remains silent. The
candidate hardening was parked for the whole-branch review.

### 7. Data and scope caveats

* **The universe is a 2026-07 membership snapshot** applied to older sessions.
  Survivorship bias is by design and documented in `sp100_2026-07.md`; a
  point-in-time membership file is the refinement path.
* **The span is 2025-08-01 .. 2026-07-24, not 2022–2026.** That is the free
  entitlement window, not a technical limit. Extending backwards costs roughly
  $5–6 per full month of 102 symbols at ~$0.0025/sym-day and needs its own
  authorization.
* `QueryAccelerator::build`'s unbounded `std::async` fan-out (up to 16 threads
  per call) was checked and **excluded** as the `bad_alloc` cause — it is not on
  the surface-DB build call graph — but it is still uncapped on the backtest arm.
* Discovery-mode phase A still reads every date's table in full before the
  parallel pass. The production path never takes it (`run_surface_db_backfill.py`
  always passes `--symbols`).
* `manifest.atxdb` is **not** byte-reproducible: two fresh builds of the same
  pilot with the same binary give identical partitions and different manifests
  (embedded wall-clock timestamps plus the manifest checksum). No future gate
  should assert it.
* The universe file is **TAB**-separated despite the `.csv` extension, which is
  easy to misparse.

---

## Reproduction

Release binaries (the `9457562` fix is what makes this actually build `build-rel\`):

```powershell
powershell scripts\atx-build.ps1 configure -Preset rel `
  -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-4/deps/rel -DATX_BUILD_EXAMPLES=ON
powershell scripts\atx-build.ps1 -Preset rel build `
  atx-vol-surface-db-build atx-vol-surface-db --parallel 6
```

`-j` cannot be used to cap build parallelism — PowerShell binds it to the
script's own `-Jobs`. Use `--parallel N`.

Pull one month (preflight first, and only pull on an exact `$0.0000` quote):

```bash
python -u atx-vol/tools/pull_opra_hive.py \
  --universe atx-vol/data/universe/sp100_2026-07.csv \
  --start 2026-06-01 --end 2026-06-30 \
  --out C:/atx-data/opra-hive \
  --snap-et 15:55 --cap 1 --index-symbol SPY \
  --env-file C:/atx/.env --dry-run      # drop --dry-run only after reading $0.0000
```

Build and verify a year:

```bash
python atx-vol/tools/run_surface_db_backfill.py \
  --universe atx-vol/data/universe/sp100_2026-07.csv \
  --hive C:/atx-data/opra-hive --db-prefix C:/atx-data/surface-db/sp100 \
  --from 2026-01-02 --to 2026-07-24 --phase build \
  --build-exe build-rel/bin/atx-vol-surface-db-build.exe \
  --admin-exe build-rel/bin/atx-vol-surface-db.exe \
  --log-dir C:/atx-data/logs/sp100/t8-2026 --index SPY --fit-workers 0
# ... then --phase verify with the SAME --from/--to so thresholds size off the year
```

Test gates:

```powershell
.\scripts\atx-build.ps1 build atx-vol-tests atx-vol-surface-db-build atx-vol-surface-db --parallel 6
.\scripts\atx-build.ps1 -Ctest -R "SurfaceDb|BuildSurfaceDb|GenerateSymbolConfigs|SurfaceArchive|OpraHive|SyntheticHive"
```
```bash
python .superpowers/sdd/2026-07-26-sp100-surface-db/pytest_local.py \
  atx-vol/python/tests/test_pull_opra_hive.py \
  atx-vol/python/tests/test_run_surface_db_backfill.py \
  atx-vol/python/tests/test_migrate_opra_hive.py \
  atx-vol/python/tests/test_sp100_universe.py -q
```

Never invoke `pytest` directly on this box (see Stage 1), and never import
`pyarrow` and `atxvol` in one process.

## Evidence inventory

Kept out of the repo so the worktree stays clean:

* `C:\atx-data\logs\sp100\t8-2025\`, `t8-2026\` — per-chunk `orchestrator.log`
  (`tag= exit= duration_s= cmd=`), per-chunk `build_*.csv` reports, `pass1/`
  subdirectories holding the first pass's report CSVs.
* `C:\atx-data\logs\sp100\t8-gates\` — `audit_suffix.py`, `cell_accounting.py`,
  `spotcheck.py`, `hive_symbols.py` and their outputs `suffix_audit_final.txt`,
  `cell_accounting.txt`, `spotcheck.txt`, `hive_symbols.json`.
* `C:\atx-data\logs\sp100\` — the pilot-era `orchestrator.log`, `build_2022_*`,
  `build_2026_*`, `year_summary_*`, `verify_*`, `info_*`, `query_*` tee files.
* `C:\atx-data\logs\sp100-prod\` — the production pull logs and cost lines.
* `.superpowers/sdd/2026-07-26-sp100-surface-db/` — every task brief, addendum
  and report, plus `progress.md` (the ledger) and `pytest_local.py` (the Python
  lane).
