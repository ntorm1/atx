# Backtest Framework Waves C/D/E — SDD Progress

Controller: Claude (session b8ae4870). Repo root C:\atx, branch main, in place.
Base commit at Wave C start: 587ee97 (Wave B closed).

Plans:
  docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-c.md
  docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-d.md
  docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-e.md
Each carries a "Controller decisions (pre-dispatch)" block that overrides its body.
Design spec + errata: docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md
Wave B ledger (committed): .superpowers/sdd/backtest-wave-b/progress.md

User decisions for this stretch:
  - Wave C = SPINE-ONLY. T7 (mag7 RunArchive write) DROPPED — its reader is Python,
    which is out of scope, so the write would be dual-write (forbidden by design §2
    and by the sprint's hard-cutover decision).
  - Sprint close = per-wave gate + ONE fresh whole-sprint review over A-E at the end.
  - ALL Python work is dropped. No pytest task in any wave.

## Planning outcomes (3 parallel Opus planners, read-only)

WAVE C — 7 tasks after the T7 drop. L11's headline measured FALSE: stage-by-stage
  across the 5 named drivers, only stages 5+6+7 (timed engine call -> tearsheet fold
  -> stats capture) are common to all five, and 4 of those 6 nodes are ALREADY
  library calls. Args/split/join/fmt_num 2/5; RunConfig overlay 2/5; clock source
  0/5; strategy construct 0/5; output shape 0/5 (four distinct shapes);
  EngineRunStats 1/5. So Wave C extracts ONE function (RunOutcome + two run_timed
  overloads), not a BacktestJob. Three design-§4.3 errors found and recorded as
  errata in the design spec (no third "tradeable manual evaluator" engine slot; the
  "six drivers" sixth is Wave B's thin surface-backtest command; only the RETURN
  type was right). The synthetic-corpus helper trio L11 folds into the spine appears
  in 28 files — real duplication, wrong home, separate hygiene sweep.

WAVE D — 7 tasks. StepObserver signature settled:
    struct StepEvent { std::size_t step_index; const SnapshotRef &ref;
                       const MarketSnapshot &snapshot; const IStrategy &strategy; };
    using StepObserver = std::function<Status(const StepEvent &)>;
  Fired after both on_step sites (backtest.cpp:1862, :1999) and BEFORE
  validate_strategy_transition — the shadow loop read strategy state with nothing in
  between, so that is the only definitionally-equivalent point. All four members are
  read in-tree (Wave B had to delete four dead fields; not repeating that).
  Blast radius verified empirically: 46 files include backtest.hpp; zero positional
  aggregate-init sites, zero sizeof/offsetof asserts, nothing hashes RunConfig — so a
  defaulted tail field is source-compatible, but T1 still mandates a FULL build.
  Anti-vacuity: cold-route rows=0 cannot falsify, so the proof rides on
  --execution configured (rows > 0), with a RED probe on the comparator and a
  Vacuity Ledger entry per task.

WAVE E — 9 tasks. KEPT P5, P2, P3, narrowed P1. DROPPED P4/P6/P7 with evidence
  (P4: parallelism already exists, n_threads=0=auto, and a range batch would hold
  ~6.9k OpraPanels resident. P6: surface_fingerprint 15611810793130839 is a byte
  golden in the committed trade_schedule.tsv, so the swap is a re-baseline not a perf
  pass. P7: precondition provably false — corpus built from all_symbols over the same
  universe file, so an archive cannot exceed the universe).
  P5 found LARGER than documented: the divergence replay loop is all 135 sessions,
  not ~60, so the double-deserialize is 2.2x the review's assumption.
  P3 found UNDERSTATED: the 9-field vector is ~6 allocs/row, not 1, plus L3's
  throwaway 696 MB re-serialization.
  Biggest correctness risk: P2 narrows three whole-panel fail-closed gates to the
  ~102 consumed leg keys, and its outputs (reconciliation, contract_marks) were the
  only artifacts with NO golden — Task 1 pins them first and BLOCKS P2.

## Task ledger
(dispatch begins below)

## Task ledger — Wave C

T1 (backtest_driver spine): implementer DONE (commit e996f2c; 526 insertions, exactly
  the 5 planned paths). RED observed as a build error —
  `backtest_driver_test.cpp(42,10): fatal error: 'atx/vol/backtest_driver.hpp' file
  not found` -> `ninja: build stopped`. GREEN 5/5 on
  `--gtest_filter=BacktestDriver.*:RunTimed*` (12 rows x 25 F64 cols; dispersion
  5 rows, 2 signals; n_steps=12 wall_ms=49.9 loads=12 hits=11). TearSheet.*/RunReport.*
  still 12/12.
  GATE PROVEN LIVE (implementer went beyond the task): a temporary probe in run_timed
  (`result.cash.back() += 1.0`, `stats.n_steps = size()+1`) failed 3 of 5 tests —
  `cash row 11` bit mismatch and `n_steps 13 vs 12`. Reverted, rebuilt, PROBE absent.
  So the bit-identity assertions can genuinely fail — the property Wave B's M1 test
  lacked. Bit-identity is driven off backtest_series_columns() by name (all 25) plus
  date/ts_ns/step_pnl_total/signals; TearSheet equality covers all 27 fields under
  static_assert(sizeof(TearSheet) == 27*sizeof(double)).
  Deviation (accepted): the test was registered in tests/CMakeLists.txt at Step 1
  rather than Step 3 — a build-error RED requires the file to be in the build. Correct
  call; the plan's step order was wrong.
T2 (pre-migration byte goldens): implementer DONE. See "## T2 goldens" below for the
  self-contained, scratchpad-independent derivation of all ten hashes. Summary: 10/10
  artifacts gated (7 whole-file, 3 filtered); 0 ungated. Suite unchanged
  (2019 ran / 1973 passed / 43 skipped / 3 failed = the three documented reds) with
  6 -> 7 DISABLED tests.

  Minor roll-up: (1) the plan's premise that `expect_result_bit_identical` covers only
  10 columns is WRONG for tearsheet_test.cpp:199-246, which already covers all 25 +
  step_pnl_total + signals; the narrow copies live in the other driver test files. My
  plan text overstated the gap. (2) Commit-trailer inconsistency: subagent commits in
  this sprint use `Claude Opus 4.8` per the sprint convention, controller commits use
  `Claude Opus 5 (1M context)`. Deliberate (different authors), noted so a reader does
  not read it as sloppiness.

## T2 goldens

Captured at HEAD `e996f2c` (Wave C T1) + T2's test-file change, before ANY driver edit.
This section is SELF-CONTAINED on purpose: `$FX` is session-scoped scratchpad and may be
cleaned, so everything needed to re-derive every hash is here. Nothing under
`C:\atx-data` was read or written.

### Environment

    $FX  = C:\Users\natha\AppData\Local\Temp\claude\c--atx\b8ae4870-03de-493c-ad84-2006e8f7409e\scratchpad\wave-c
    $env:PATH = "C:\atx\build-rel\bin;$env:PATH"     # parquet.dll
    $ErrorActionPreference = 'Continue'
    Build (ONE invocation, Release preset):
      cmake --build C:\atx\build-rel --target atxvol_dispersion_backtest atxvol_strategy_examples \
            spy_strangle_backtest mag7_dispersion_backtest spy_dispersion_pnl

### The mag7 fixture db (needed by 2 of the 5 drivers)

Built ONCE via the new fixture emitter and reused unchanged for the whole wave:

    $env:ATX_MAG7_FIXTURE_DB = "$FX\mag7_db"
    cd C:\atx\build-rel
    .\bin\atx-vol-tests.exe --gtest_also_run_disabled_tests \
      --gtest_filter=Mag7DispersionBacktest.DISABLED_PersistFixtureDbForDriverGoldens
    # -> "persisted fixture db: ...\mag7_db (12 partitions, surface_count=8 each)"

Content is deterministic (base_ts 1'700'000'000'000'000'000; 12 daily partitions
2026-03-01..2026-03-12; 8 symbols = AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA + SPY, uids 1..8;
fixed kBaseSpot/kVolBump; the emitter reuses `build_fixture_db_at`, the same body the five
existing Mag7DispersionBacktest tests use). Per-partition `file_size` and `created_ts_ns`
are NOT reproducible across rebuilds (`created_ts_ns = wall_clock_ns()`,
`src/surface_db.cpp:1114-1115`), so **rebuilding the db invalidates `db_stats.csv` only** —
the three economics artifacts (`series.csv`, `strategy_metrics.csv`, `engine_metrics.csv`
filtered) depend on db CONTENT only and survive a rebuild.

### Hashing method (must be reproduced exactly or the hex will not match)

    whole-file : (Get-FileHash -Algorithm SHA256 <file>).Hash.Substring(0,16)
    filtered   : $lines = Get-Content <file> | Where-Object { $_ -notmatch '<regex>' }
                 $joined = $lines -join "`n"
                 $ms = [IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes($joined))
                 (Get-FileHash -Algorithm SHA256 -InputStream $ms).Hash.Substring(0,16)

The filtered form drops the file's trailing newline (Get-Content semantics) and normalises
to `\n` joins. Any other method yields a different hex. (Helper scripts used:
`$FX\hash.ps1`, `$FX\capture.ps1`, `$FX\report.ps1` — scratchpad, re-creatable from the
recipe above.)

### The ten goldens

| # | Driver (target) | Invocation | Artifact | Class | Filter regex (verbatim) | sha256[0..16) |
|---|---|---|---|---|---|---|
| 1 | `atxvol_dispersion_backtest` | *(no args)* | `%TEMP%\atx-dispersion-backtest\dispersion.tsv` | whole-file | *(none)* | `87DA84887A2793AE` |
| 2 | `atxvol_strategy_examples` | *(no args)* | `%TEMP%\atx-strategy-examples\exampleA\example_a.tsv` | whole-file | *(none)* | `59A8C0174510C8D8` |
| 3 | `atxvol_strategy_examples` | *(no args)* | `%TEMP%\atx-strategy-examples\exampleB\example_b.tsv` | whole-file | *(none)* | `5647023F4B98FEC8` |
| 4 | `spy_strangle_backtest` | *(no args -> seeded synthetic corpus)* | `%TEMP%\atx-spy-strangle-backtest\spy_short_strangle.tsv` | whole-file | *(none)* | `57A351D477E84F10` |
| 5 | `spy_strangle_backtest` | *(no args)* | `%TEMP%\atx-spy-strangle-backtest\spy_short_strangle.csv` | filtered | `^# (wall_clock_ms\|steps_per_s)=` | `1B632185037D31B5` |
| 6 | `mag7_dispersion_backtest` | `--db $FX\mag7_db --out <OUT> --threads 1` | `<OUT>\series.csv` | whole-file | *(none)* | `128DBD4E99118D36` |
| 7 | `mag7_dispersion_backtest` | *(as #6)* | `<OUT>\strategy_metrics.csv` | whole-file | *(none)* | `D49500348A9E5B3C` |
| 8 | `mag7_dispersion_backtest` | *(as #6)* | `<OUT>\engine_metrics.csv` | filtered | `^(wall_clock_ms\|steps_per_s),` | `7A56EA26F3EC5395` |
| 9 | `mag7_dispersion_backtest` | *(as #6)* | `<OUT>\db_stats.csv` | whole-file, SAME-DB ONLY | *(none)* | `6916983A49E258C5` |
| 10 | `spy_dispersion_pnl` | `--db $FX\mag7_db --names AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA --index SPY --min-names 4 --out <OUT> --threads 1` | `<OUT>\pnl_track.tsv` | filtered | `^# (wall_clock_ms\|steps_per_s)=` | `CC90B900A7116CC3` |

`<OUT>` is arbitrary (the drivers create it); the artifact bytes do not embed it. All five
drivers exited 0. mag7's out dir contains EXACTLY the four files above —
`populate_stats.csv` is absent (a synthetic db has none) and that absence is part of the
golden. `spy_dispersion_pnl` writes exactly `pnl_track.tsv`.

### Determinism proof

Three independent full passes:
  * r1, r2 — back-to-back.
  * r3 — after deleting all 216 `%TEMP%\atx-*` dirs AND `$FX\pre`, so no leftover state.

All ten hexes above are identical in r1, r2 and r3.

What drifted, exactly — the ONLY non-determinism found anywhere in the ten artifacts is
`wall_clock_ms` and its derived `steps_per_s`, in three artifacts:

| Artifact | Line | r1 | r2 | r3 |
|---|---|---|---|---|
| `spy_short_strangle.csv` | 27 `# wall_clock_ms=` | 507.5203 | 462.5062 | 1064.7341 |
| `spy_short_strangle.csv` | 28 `# steps_per_s=` | 254.177025 | 278.9151799 | 121.15701 |
| `mag7\engine_metrics.csv` | 20 `wall_clock_ms,` | 700.3143 | 769.7743 | 1241.6575 |
| `mag7\engine_metrics.csv` | 21 `steps_per_s,` | 17.13516345 | 15.58898498 | 9.664500879 |
| `pnl\pnl_track.tsv` | 40 `# wall_clock_ms=` | 870.544 | 877.6919 | 1432.3347 |
| `pnl\pnl_track.tsv` | 41 `# steps_per_s=` | 13.78448418 | 13.6722237 | 8.377930103 |

Those three files are 166 / 25 / 54 lines and differ across runs on EXACTLY those two lines
each, both matching the file's own filter (verified index-by-index in PowerShell; line
counts equal, no insertions). `db_stats.csv` (#9) was byte-stable across all three runs
because the db was not rebuilt.

Controller decision 4 (`spy_strangle_backtest` telemetry) resolved BETTER than the plan
assumed: the filter is `^# (wall_clock_ms|steps_per_s)=` and NOTHING ELSE. The plan
pre-emptively wanted `snapshot_preload_ms` plus "whatever the `# pricing_*` sampled
telemetry shows drifting" filtered too. Measured, they do not drift on the no-args golden
path — they are structurally constant there:

    # cache_fast_build_loads=0
    # cache_reuse_only_fast_hits=0
    # cache_reuse_only_cold_resolutions=0
    # pricing_telemetry_sample_period=64
    # pricing_cache_attempts_est=0
    # pricing_cache_hit_rate=0
    # pricing_cold_fallback_rate=0
    # snapshot_preload_ms=0
    # adaptive_confirm=false

so all of them stay INSIDE the gate. Consequences worth knowing:
  * The `# cache_*` lines being gated at 0 is direct pre-evidence for T4's trap 2 — the
    `SnapshotCacheStats{}` substitution is byte-safe on the golden path.
  * `# snapshot_preload_ms=0` being gated means T4's preload timer must stay OUTSIDE
    `run_timed` and must remain un-entered on the no-args path.
  * No driver was changed and no `--no-telemetry` flag was added. Nothing under
    `atx-vol/examples/` was touched by T2.

### Reference bytes

An unfiltered copy of all ten artifacts, plus each driver's stdout (ungated), is at
`$FX\golden-ref\`. T3-T6 use it for the "unfiltered file differs on exactly the filtered
lines" audit. Scratchpad, so expendable: if gone, rebuild the five drivers at `e996f2c`
and re-run the invocations above.

### Not gated, and what that leaves unprotected

  * **stdout/stderr of all five drivers.** Every one prints a wall-clock number, so it was
    never stable. Captured to `$FX\golden-ref\stdout_*.txt` as context only. Unprotected: a
    migration could reword or reorder console output undetected — T3/T5 constrain this by
    review, not by hash.
  * **`populate_stats.csv`** — absent by construction (synthetic db). Its ABSENCE is gated.
  * **`db_stats.csv` (#9)** is captured and stable, but per controller decision 3 it is NOT
    a wave gate: the db lives in session scratchpad, so the hash dies with the directory.
    mag7's real gate is #6, #7, #8. Unprotected: `write_surface_db_stats_csv`'s output
    shape. T6 touches no code on that path (the `:267` emitter call is untouched), so the
    exposure is small — but if `$FX\mag7_db` still exists when T6 runs, T6 SHOULD check #9
    as a free bonus assertion.
  * **`wall_clock_ms` / `steps_per_s` in #5, #8, #10.** Unprotected: the VALUE of the
    engine-timing telemetry. This is design §6/I7's accepted residual and is exactly why
    T1's contract pins the timed interval to the engine call only — a widened bracket would
    move these numbers and no hash would notice. Reviewers must read the bracket rather
    than trust the hash.

### Fixture emitter (the only code change in T2)

`atx-vol/tests/mag7_dispersion_backtest_test.cpp`:
  * split `build_fixture_db(tag)` into `build_fixture_db_at(root)` + a wrapper passing
    `test_root(tag)` — behaviour-preserving for the five existing tests;
  * added `fixture_db_env()` reading `ATX_MAG7_FIXTURE_DB` via `_dupenv_s` (plain
    `std::getenv` trips `/WX -Wdeprecated-declarations`; pattern copied from
    `spy_fit_corpus_test.cpp:37-51`);
  * added `TEST(Mag7DispersionBacktest, DISABLED_PersistFixtureDbForDriverGoldens)`.

Plan correction: the plan said to assert "12 partitions and 8 symbols". `db->symbols()` is
EMPTY for this fixture — `SurfaceDb::write_partition` only refreshes provenance on symbols
already present in the manifest symbol table and never adds any
(`src/surface_db.cpp:1122-1135`), so a db built purely by `write_partition` has no manifest
symbol table. Not a defect, and not something T2 changed. The test now asserts 12
partitions, `db->symbols().empty()` (with the reason inline), `DbPartitionInfo::surface_count
== 8` on every partition (this is what actually witnesses the 8 symbols), and that
`Clock::from_surface_db` succeeds.

### Suite counts (Step 4)

Before T2's change: 2019 ran / 1973 passed / 43 skipped / 3 failed / **6** disabled.
After  T2's change: 2019 ran / 1973 passed / 43 skipped / 3 failed / **7** disabled.
The 3 failures are exactly the three documented pre-existing reds
(`BoundaryHoist.PriceBitIdenticalToPrechange`,
`AllQualityModes/SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/Latency`
and `/Balanced`). Both runs from `C:\atx\build-rel` as CWD.

### Tooling hazard discovered — READ THIS BEFORE VERIFYING ANY LATER GATE

In this environment the Bash tool's `grep` and `diff` are proxied and returned WRONG answers
during T2: `diff` reported "[ok] Files are identical" for two files whose sha256 differed,
and a piped `grep -c '^[<>]'` reported 0 matches on a stream containing 2. Every byte/line
comparison in T3-T8 must be done in PowerShell (`Get-FileHash`, `Get-Content` + an index
loop, or `Compare-Object`). A task that "verified" its gate through Bash `diff` has verified
nothing.
