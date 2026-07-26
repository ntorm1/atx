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

### Controller notes after T2 (sprint-wide, propagate to every later brief)

1. **TOOLING HAZARD — never use `diff` or `grep` via the Bash tool for byte
   comparison.** T2 found `diff` reporting two files IDENTICAL when their sha256
   differed (the commands are proxied/rewritten). All byte comparisons for the rest
   of this sprint must use PowerShell (`Get-FileHash`, `Compare-Object`) or
   `sha256sum`, and must PRINT both the computed and expected value so the
   comparison is visible rather than asserted. This warning is now carried in every
   remaining task brief.
2. **T2's filters are narrower than the plan proposed** — only `wall_clock_ms` and
   the derived `steps_per_s` drift, anywhere. `# snapshot_preload_ms` and all four
   `# pricing_*` lines are stable (0/0/0/0, sample_period=64) and therefore stay
   INSIDE the gate. That is free pre-evidence for Wave C's later trap-2 concern.
   Controller decision 4 anticipated a wider filter; the narrower one is better and
   stands. Do NOT widen a filter to make a hash match — a hash that only matches
   under a wider filter means the migration changed something real.
3. **Plan errors found by T2** (corrected in place): (a) "assert 12 partitions and 8
   symbols" is wrong — `db->symbols()` is EMPTY, because `write_partition` only
   refreshes provenance on already-registered symbols and never adds any
   (`src/surface_db.cpp:1122-1135`); the test asserts `surface_count == 8` across all
   12 partitions instead. (b) mag7 emits FOUR files, not five —
   `populate_stats.csv` is absent; the T6/T7 text saying "five CSVs" is wrong (moot
   for T7, which the controller dropped). (c) `std::getenv` trips `/WX`; `_dupenv_s`
   used instead.
4. **Repo hygiene:** two stray files in the repo root with mangled names
   (`C:Usersnatha...t7_build_all.log` / `...t7_build_tests.log`) were ninja output
   from a Wave B T7 subagent whose `>` redirect lost its path separators. Inspected
   (plain build logs, no data) and deleted by the controller.

T3 (migrate the two zero-arg synthetic drivers): implementer DONE (commit 18dc1ef,
  3 files, explicit paths). BYTE GATE 3/3 UNMOVED (observed == T2 golden):
  dispersion.tsv 87DA84887A2793AE, example_a.tsv 59A8C0174510C8D8,
  example_b.tsv 5647023F4B98FEC8. Also line-by-line audited in PowerShell against
  $FX\golden-ref — 0 differing lines each; no filter (all whole-file class).
  strategy_examples stdout byte-identical (0/6 lines differ) per decision 5;
  dispersion_backtest stdout differs only in line 1's engine_ms field.
  Suite 2020/1974/43/3/7 from build-rel CWD = T2 baseline +1 (T1's test); the 3
  failures are exactly the documented reds.
  LINE DELTAS — HONEST, AND THE POINT: dispersion_backtest.cpp 209 -> 208 (-1);
  strategy_examples.cpp 246 -> 246 (ZERO; it had no timer, so migration removes only
  the tearsheet fold at 2 sites, offset by +1 include and +1 comment). This is the
  L11-is-false finding showing up concretely rather than in a table: for drivers
  without a timer there is essentially nothing to extract. The remaining three
  (spy_strangle 635, spy_dispersion_pnl 554, mag7 306) are where the shared
  timed-call/tearsheet/stats triple actually lives — mag7 is the 1/5 that hand-builds
  EngineRunStats. If T4-T6 also yield ~0, Wave C is migration risk without benefit
  and the controller should say so in the wave verdict rather than bank a win.
  Plan contradiction (correctly handled): T3 Step 1's
  RunTimedDispersion_SignalsSurviveTheSeam as specified is ALREADY implemented
  verbatim by T1's test 5 (expect_result_bit_identical compares signals name-for-name
  and value-for-value and asserts non-empty). Writing it as specified would have been
  a pure duplicate — the anti-pattern T4 Step 1 itself rejects. Implementer kept the
  name and rescoped it to what T1 does NOT cover: the emitted TSV BYTES from both
  routes, with a header-contains-each-signal precheck so two files that both lost a
  signal cannot pass. Passed first run — disclosed as a REGRESSION LOCK, not a RED
  (T1's probe already proved this comparison can fail).
  Minor roll-up: (1) dispersion_backtest.cpp header comment still says it runs via
  run_backtest — stale since before Wave C; (2) strategy_examples.cpp include comments
  now over-claim, though both headers are still needed; (3) NEW TOOLING HAZARD — the
  Grep TOOL (not just Bash grep) rendered `//` as `\` in tearsheet.hpp:94, so its
  content output is unreliable too; use Read when exact characters matter.

T4 (migrate spy_strangle_backtest): implementer DONE (commit 2858cab, 1 file,
  +21/-15). BYTE GATE 2/2 UNMOVED, from a wiped %TEMP% and reproduced on a second
  clean run: spy_short_strangle.tsv 57A351D477E84F10 == golden;
  spy_short_strangle.csv (filtered) 1B632185037D31B5 == golden. Unfiltered CSV
  audited index-by-index in PowerShell vs T2's reference: 166 lines both sides,
  EXACTLY TWO differing (L27 # wall_clock_ms 507.5203 -> 488.8151, L28 # steps_per_s),
  both in-filter. stdout 35 lines, differing only on the three wall-clock lines.
  Suite 2020/1974/43/3/7 = baseline; SpyStrangleBacktest.*:BacktestDriver.*:RunTimed*
  10/10.
  LINE DELTA +6 (605 -> 611): executable code shrank 6 lines, 8 comment lines added
  recording the trap findings. Net positive. Reported as-is, not massaged.
  GATE LIVENESS PROVEN by NEGATIVE control (no rebuild): 5 one-byte perturbations of
  the golden CSV each moved the filtered hash (cache_fast_build_loads,
  snapshot_preload_ms, pricing_cache_hit_rate, total_return, last data row), while the
  two wall_clock_ms/steps_per_s controls correctly did NOT move it.
  PLAN ERROR FOUND (3rd of the wave): Task 4 Step 1 calls its probe "the RED for this
  task", but the probe's success criterion is that the hash STILL MATCHES — a positive
  control, which cannot demonstrate the gate can fail. The implementer ran it as the
  positive control it is (hashes matched; # cache_* are genuinely 0, reverted) and
  added the negative control above. T6's probe (corrupt n_steps) is the genuine
  article. My plan text conflated the two.
  Trap 1 verified: src/tearsheet.cpp has zero `counters` occurrences and no
  counters.hpp include — tearsheet() is pure arithmetic, so moving the fold cannot
  perturb sampled telemetry.
  Tests: none written. Task 4 specifies none, and T1's RunTimed_NullCacheYieldsZeroedStats
  already covers this driver's null-cache path — correctly skipped rather than duplicated.
  Minor roll-up: adding the backtest_driver.hpp include realigns 4 neighbouring
  trailing comments under clang-format and drops panel.hpp from the alignment group —
  4 diff lines of pure cosmetic churn. T5/T6 add the same include; run
  `clang-format -n --Werror` on the file.

### Running line-delta tally (the Wave C value question)
  dispersion_backtest  209 -> 208   (-1)
  strategy_examples    246 -> 246   ( 0)
  spy_strangle         605 -> 611   (+6, exec -6 / comments +8)
  Three drivers in, the spine extraction is a WASH on code size. Remaining: T5
  spy_dispersion_pnl (554) and T6 mag7 (306) — mag7 is the ONLY driver of the five
  that hand-builds EngineRunStats, so it is the one genuine dedup in the wave and the
  best case. The controller must give an honest Wave C verdict at the gate: if the
  wave's real deliverables are (a) the measurement that disproved L11 and (b) 10
  driver byte-goldens that never existed before, then say that, rather than banking a
  code-reduction win that did not happen.

---

## SESSION RESUMPTION — 2026-07-25 (controller)

This ledger covers THREE plan files (the heading says so): wave-c, wave-d, wave-e
under `docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-{c,d,e}.md`.
Briefs/reports/review-packages for this session live in the per-plan workspaces
`.superpowers/sdd/2026-07-24-atx-vol-backtest-framework-wave-{c,d,e}/`; this file
stays the single ledger.

**BRANCH CHANGE (user instruction, overrides all three plans' "work directly on
local main" constraint):** all remaining work lands on
`sprint/atx-vol-backtest-waves-cde`, branched from `main` @ `2858cab`. Nothing is
merged into local `main` this session. The four already-landed Wave C commits
(`e996f2c` T1, `b3c10d3` T2, `18dc1ef` T3, `2858cab` T4) are on `main` from the
prior session and stay there; the branch continues from them.

Sprint-wide constraints handed to every subagent as a file:
`.superpowers/sdd/2026-07-24-atx-vol-backtest-framework-wave-c/SPRINT-CONSTRAINTS.md`

### Tooling-hazard note CORRECTED (supersedes the T2 wording above)

The T2 section above says Bash `diff` "reported [ok] Files are identical for two
files whose sha256 differed". Measured precisely this session on a purpose-built
3-file fixture: the proxied `diff` prints the CORRECT textual difference
(`+1 added, -1 removed` plus the changed line) but returns **exit code 0 on
differing files**, whereas `rtk proxy diff` correctly returns 1. So the tool's
OUTPUT is trustworthy; its EXIT STATUS is not. Anything branching on `$?`,
`&&`, `||`, or `if diff a b; then` silently concludes "identical". The operative
rule is unchanged and still binding — establish and verify every byte comparison
in PowerShell (`Get-FileHash`, `Compare-Object`) and print both values — but the
claim "returns wrong answers" was stronger than the evidence supports and is
narrowed here to the exit code.

`grep` was ALSO re-tested this session and is **clean**: hooked `grep -c` returned
the correct count with exit 0 on a match and exit 1 on no match, matching
`rtk proxy grep` exactly, including through a pipe (`rtk proxy diff a b | grep -c
'^[<>]'` -> 2, exit 0). So T2's "a piped `grep -c` reported 0 matches on a stream
containing 2" **does not reproduce** and is withdrawn as a standing hazard. The
narrowed, evidence-backed hazard is exactly one thing: **the proxied `diff`
returns exit 0 on differing files.**

### Controller decisions — Wave D open questions (2026-07-25)

The plan's own "Controller decisions" block (accepted as written) already binds
questions 1, 2, 4 and 6. Remaining:

- **Q3 (T4 Step 2 fallback), decided BEFORE T5 as the plan requires.** If
  `--execution configured` cannot complete on the parity corpus, or completes with
  divergence rows N == 0, fall back to the **perturbed-`model_mark` copied-schedule
  corpus** — not to "T2's gtest plus T3's RED probe suffice". A gtest that hand-builds
  a `StepEvent` cannot prove the engine fires the hook where the shadow looped, and
  that is the entire claim T5's deletion rests on. If BOTH the configured route and
  the perturbed corpus fail to produce a nonzero-row bit-comparison, **T5 is not
  dispatched and the shadow loop stays** — recorded as a wave shortfall, not deleted
  on faith. Deleting a shadow whose replacement was never proven on a nonzero corpus
  is exactly Wave B's defect class.
- **Q5 (Wave C ordering) — resolved by fact, not by choice.** Wave C landed FIRST
  this sprint (T1-T5 committed). So the plan's "confirm Wave D lands first" is moot.
  `run_timed` takes the `RunConfig` the driver already built and forwards it to the
  engine unchanged, so `step_observer` rides through the seam with no Wave C edit
  required. T1 must nonetheless VERIFY that by reading
  `atx-vol/include/atx/vol/backtest_driver.hpp` and say so, rather than assuming it.

### Controller decisions — Wave E open questions (2026-07-25)

The plan's own decisions block binds the triage, Task 1-blocks-P2, P5-first, P1's
scope/gating/key, the mandatory stale-input test, and controller-owned measurement.
Answering the five open questions it left:

- **Q1 (T2/P5 memory bound).** Land the routing through the shared cache; keep the
  plan's abort threshold (fixture peak working set > 3x baseline => do not land the
  clock-sized capacity). BUT the fixture is 3 dates, so its memory number **cannot
  falsify the 135-board production risk** — an honest report must say that. Therefore
  T2 additionally REQUIRES a measured per-board resident cost on the fixture and an
  explicit extrapolation to 135 boards, reported in MB. Do NOT add a capacity knob:
  a bounded cache evicts the boards before the priced run reads them, which deletes
  the entire win P5 exists to capture. If the extrapolation is alarming, report the
  number and let the wave gate decide — do not silently cap it.
- **Q2 (T3/P2 narrowed gates).** Trade ACCEPTED: panel-wide *definition* validation
  narrows to the consumed leg keys. Two tests are mandatory and the task is not done
  without both: (a) a bad definition on a **consumed** key still aborts fail-closed;
  (b) a bad definition on a **non-consumed** key no longer aborts — that is the
  behaviour change, and it must be pinned deliberately rather than discovered later.
  Document the narrowing at the call site in code, not only in the report.
- **Q3 (T6 fold set).** `trade_schedule.tsv` **IS** in the identity fold. The hole
  being closed is precisely "a `backtest` section unioned with sections computed from
  a different schedule", and excluding the schedule leaves that hole open. The
  self-dependency (build-schedule writes the file, then folds it) is fine because the
  fold happens at archive-write time, after the write; T6 must confirm that ordering
  from the code and show it.
- **Q4 (T7 GO/NO-GO threshold).** 15% of combined `build-schedule` + `run-backtest`
  fixture wall time stands. Do NOT wait for a controller-measured 696 MB production
  number: that measurement is a `C:\atx-data` run this session is not spending, and
  the fixture ratio is the decision input the plan was built around. A NO-GO that
  stops the wave at Task 6 is a real, acceptable outcome — report it and stop.
- **Q5 (T8 default).** Cache **DISABLED by default**. Confirmed. No existing
  invocation may change behaviour, and the parity gate must keep proving the
  uncached path.

### Wave C T5 (migrate spy_dispersion_pnl) — COMPLETE

T5: complete (commits 2858cab..a9e62e4, review clean after 1 fix round)
  Implementer inherited +30/-12 (driver) and +95 (test) of UNCOMMITTED, unverified work
  from the prior session's killed implementer; audited it hunk by hunk rather than
  trusting it, kept every driver hunk, corrected one stale comment.
  BYTE GATE UNMOVED: pnl_track.tsv filtered computed CC90B900A7116CC3 == expected
  CC90B900A7116CC3, three fresh --out dirs; unfiltered audited index-by-index vs T2's
  reference — 54 lines both sides, EXACTLY TWO differing (L40 # wall_clock_ms, L41
  # steps_per_s), both in-filter. The REVIEWER independently re-ran the binary and
  reproduced the hash, the 2-of-54 audit, and every perturbation hash.
  Suite 2021/1975/43/3/7 from build-rel CWD = baseline +1 ran/+1 passed; the 3 failures
  are exactly the documented reds. Targeted filter 15/15.
  LINE DELTA +6 (554 -> 560): exec -6, comments +12.
  PLAN ERRORS FOUND (4th and 5th of the wave), both confirmed correct by the reviewer:
  (a) the brief's claim that a 1-ULP tearsheet difference shows up in pnl_track.tsv is
  FALSE — nextafter(387.1141627) renders byte-identically under %.10g, and
  append_backtest_series_tsv writes no tearsheet value at all, so the %.17g body cannot
  witness a sheet-only ULP move either. ULP protection comes from T1's test 2, not the
  byte golden. (b) The brief's Step-1 test as specified is a STRICT DUPLICATE of T1's
  test 2 (%.10g of bit-equal doubles is equal by construction).
  FIX ROUND 1/5 (3 addressed, 0 open; commits 35a55cf..a9e62e4): the reviewer caught that
  the implementer's *replacement* test was itself strictly implied by tests 1+2, and that
  its comment falsely claimed to "run the driver's own composition" when both helpers are
  COPIES of driver code. Fixed by (i) rewriting the comment to split the two assertions by
  strength and adding an explicit "WHAT THIS TEST DOES NOT COVER" paragraph carrying the
  reviewer's own falsifying scenario, and (ii) adding a genuinely not-implied assertion —
  a prelude FRAMING pin against a literal (# k=v shape, meta INSERTION order vs sorted,
  tab->space sanitization, prelude/series-header boundary, %.10g rendering), all five
  invisible to tests 1+2 because neither calls the TSV writer. RED observed. Renamed
  RunTimed_SheetFieldsAreBitEqualUnderFmtNum -> RunTimed_PnlTrackBytesIdenticalBothRoutes.

T5: minor (deferred): backtest_driver.hpp:16's per-driver line-number list
  ("mag7:201-203, spy_dispersion_pnl:459-461, spy_strangle_backtest:457-459,
  dispersion_backtest:155-159") is stale — after T3/T4/T5 three of those four brackets no
  longer exist. Carried into T6's dispatch, which is the last driver migration and can
  therefore finalize the wording.
T5: minor (deferred): pre-existing clang-format violations in both files (18 driver lines,
  8 test lines). Verified ZERO fall on any line T5 added. No action.
T5: minor (deferred) — REAL GAP, for the wave gate / final review to triage: NO COMMITTED
  test can see a driver-side change to spy_dispersion_pnl's 41-key Meta set or its %.10g
  precision. Change `%.10g` to `%.9g`, or delete {"sharpe", fmt_num(ts.sharpe)}, and the
  whole suite still passes; the only thing that catches it is T2's filtered hash, which
  lives in session scratchpad and is NOT committed. Documented in the test itself rather
  than papered over. Closing it properly needs a committed driver-produced pnl_track.tsv
  golden fixture (the implementer's recommendation, and mine) — a separate task, not a T5
  amendment. NOTE this generalizes: the same is true of all five migrated drivers.

### Wave C T6 (migrate mag7_dispersion_backtest) — COMPLETE

T6: complete (commit 36f9707, review clean, ZERO fix rounds — spec compliance PASS and
  task quality APPROVED on the first pass, the only task in the wave to manage it)
  Two files: atx-vol/examples/mag7_dispersion_backtest.cpp and (comment-only)
  atx-vol/include/atx/vol/backtest_driver.hpp.
  BYTE GATE 4/4 MATCH, and the REVIEWER independently re-ran the binary into its own
  fresh --out and reproduced all four: series.csv 128DBD4E99118D36,
  strategy_metrics.csv D49500348A9E5B3C, db_stats.csv 6916983A49E258C5,
  engine_metrics.csv (filtered) 7A56EA26F3EC5395. Unfiltered engine_metrics.csv audited
  index-by-index: 25 lines both sides, EXACTLY TWO differing (L20 wall_clock_ms
  700.3143 -> 856.4605, L21 steps_per_s 17.13516345 -> 14.01115405), both in-filter on
  both sides. n_steps,12 / cache_loads,12 / cache_hits,11 / cache_prefetches,11 all
  byte-identical INSIDE the gate. 18-key MetaKv set and order identical in all four
  files and identical to mag7_dispersion_report_test.py:31-50's SHARED_META.
  populate_stats.csv still absent. Suite 2021/1975/43/3/7 = T5 baseline (T6 added no test).
  Targeted 18/18 (5 Mag7 + 6 RunReport + 7 BacktestDriver), re-run by the reviewer.
  THE WAVE'S ONE GENUINE RED, on both halves: the probe (stats.n_steps = r.size()+1 in
  src/backtest_driver.cpp) moved engine_metrics.csv's filtered hash to 344EA73FC3952AB9
  while the other three stayed matching, AND failed
  RunTimed_StatsCaptureStepsAndCache at backtest_driver_test.cpp:396 (13 vs 12).
  The reviewer independently DERIVED 344EA73FC3952AB9 from T2's golden bytes with only
  n_steps,12 -> n_steps,13 changed — the claimed RED hash is arithmetically confirmed,
  not taken on trust. Probe residue zero (git diff on src/backtest_driver.cpp empty).
  The reviewer also confirmed the shipped binary really goes through the seam by reading
  the OBJECT FILE's symbol table: mag7_dispersion_backtest.cpp.obj carries an external
  ref to ?run_timed@vol@atx@@ and ZERO refs to run_backtest/tearsheet.
  PLAN ERROR (6th of the wave), judged CORRECT by the reviewer: Task 6 Step 1's probe
  ORDER IS NOT RUNNABLE AS WRITTEN. Pre-migration the driver did not include
  backtest_driver.hpp and assembled EngineRunStats locally, so a probe inside the library
  had NO PATH to engine_metrics.csv — run in the brief's order the artifact half of the
  RED would have been a FALSE GREEN that looked like a pass. The implementer applied
  probe+migration together and reverted only the probe: correct, and strictly stronger.
  General rule extracted, carry to Waves D/E: before probing a library to prove a
  driver's artifact gate, confirm the driver already consumes that library.
  Also confirmed: "five CSVs" is wrong throughout the plan AND in the driver's own header
  comment (four + a conditional copy); corrected.
  LINE DELTA +1 (306 -> 307); code-only -5 (15 exec lines out, 10 in — the hand-built
  EngineRunStats genuinely disappears), +6 comment lines recording why the interval and
  the cache branch are byte-safe. Reviewer reconciled the arithmetic line by line and
  confirmed nothing unrelated was compressed and no comment was deleted to buy a number.

T6: minor (deferred): mag7_dispersion_backtest.cpp:206's new comment cites ":189" for the
  shared-cache assignment; at HEAD that line is `RunConfig rc;` and the assignment is
  :190. Born stale in the very commit that fixes two OTHER rotted citations. Fix is to
  drop the number. FIVE file:line citations in contract comments have now rotted across
  three commits in this wave — RECOMMEND A STANDING RULE: no line numbers in contract
  comments, name the symbol instead. Carried to the final review as a wave-wide item.
T6: minor (deferred): report §4's cache-equality inference is weaker than stated (the
  conclusion still holds). engine_metrics() emits only 3 of SnapshotCacheStats's 8
  fields, so byte-identical cache_loads/hits/prefetches cannot by itself prove
  value-identity — the other 5 fields are invisible to every mag7 artifact. What DOES
  close it: backtest_driver_test.cpp:400-402 compares outcome->stats.cache against a
  post-run_timed cfg.snapshot_cache->stats() across all 8 fields, and run_timed takes
  const RunConfig& so cfg.snapshot_cache IS rc.snapshot_cache (same shared_ptr).
  UNRECORDED SEMANTIC CHANGE, noted here because the report omitted it: the cache read
  MOVED EARLIER, from after the tearsheet fold to before it. Inert (tearsheet is pure
  arithmetic over a const BacktestResult&) but it is a reordering and belongs in the record.
T6: minor (deferred): the commit touches backtest_driver.hpp, which is not in the brief's
  Files list. Comment-only, code byte-identical, and the :16 fix was named by the
  controller's dispatch; the :24 spy_strangle_backtest.cpp:468-469 fix was volunteered
  and is factually correct (T4 removed that ternary). Recorded so the wave's file-scope
  ledger says two files, not one.
T6: minor (deferred): commit trailer is `Claude Opus 5 (1M context)` where
  SPRINT-CONSTRAINTS says `Claude Opus 4.8 (1M context)`. Deliberate — the implementer IS
  Opus 5 and treats the trailer as an attribution claim. CONTROLLER RULING: correct call,
  accepted, no amend. The trailer records who wrote the code; cosmetic consistency with
  earlier commits is worth less than a true attribution. The constraints file's fixed
  string was carried over from Waves A/B and is now wrong for any non-4.8 implementer.
T6: minor (deferred), residual and wave-wide: none of the 7 BacktestDriver tests drives
  run_timed's Err route, so backtest_driver.hpp:29-30's "error propagates VERBATIM"
  contract is pinned by nothing executable. Provably true from ATX_TRY_IMPL (which moves
  the Error with no rewrap) and unreachable by any byte gate, so no action — but the hole
  is identical for all five migrated drivers.

### Controller resolution of T6's two "cannot verify" items

Both resolved in favour of the implementer; neither is a gap:
1. "gtest half of the RED, cannot verify by execution" — the reviewer was correctly
   forbidden from building. backtest_driver_test.cpp:396 is
   EXPECT_EQ(outcome->stats.n_steps, static_cast<uint64_t>(outcome->result.size())), so
   stats.n_steps = r.size()+1 fails it NECESSARILY, and the quoted 13-vs-12 is exactly
   what that assertion prints. Structural certainty, not corroboration.
2. "Wave C's five-driver tally, cannot verify from this diff" — the controller holds that
   context. Confirmed from the ledger's own per-task entries: -1, 0, +6, +6, +1.

### Wave C T8 — CONTROLLER INTEGRATION GATE: **PASS**

Verification delegated to a fresh Opus agent (read-only on source; builds and binaries
only), full evidence in
`.superpowers/sdd/2026-07-24-atx-vol-backtest-framework-wave-c/task-8-gate-report.md`.
Rulings below are the controller's.

- **Step 2, suite:** 2021 ran / 1975 passed / 43 skipped / 3 failed / 7 disabled from
  `C:\atx\build-rel` CWD. The three failures are exactly the documented pre-existing reds.
  No fourth failure. PASS.
- **Step 3, the ten goldens:** **10/10 MATCH**, reproduced on two independent clean-dir
  passes. All three filtered artifacts audited unfiltered index-by-index against
  `$FX\golden-ref`: equal line counts on both sides (166/166, 25/25, 54/54) and exactly
  two differing lines each, every one of them `wall_clock_ms` / `steps_per_s` and
  in-filter on BOTH sides. No filter was widened anywhere in the wave. PASS.
- **Step 4, freeze:** `kRaMinor == 0`; `RunArchiveSchema.GoldenHashPinned` passed in-suite
  (so `ra_schema_hash() == 0xdcce47781ac8390d` is asserted by the suite, not just read);
  committed fixture sha256 `71ea9632...29f7424` unchanged; the wave diff (16 files) contains
  no `run_archive_schema.hpp`, no `_schema.py`, no `*.atxrun`, **no `*.py` at all**, and no
  `run_report.{cpp,hpp}` / `run_report_test.cpp` whatsoever. PASS.
- **Step 5, Wave B route:** `examples/spy_dispersion_backtest.cpp` absent from
  `git diff --name-only 587ee97..HEAD`; `atxvol_spy_dispersion_backtest` builds. The
  135-session parity run is **explicitly not required** this wave — Wave C touched no
  economics seam — and was not run. PASS, stated rather than silently skipped.
- **Extra check the controller added (not in the plan), and the strongest evidence in the
  wave: 5/5 PASS at the OBJECT-SYMBOL level.** Every one of the five migrated drivers'
  `.obj` files imports `?run_timed@vol@atx@@` and imports NONE of
  `run_backtest` / `run_dispersion_backtest` / `tearsheet`; `src/backtest_driver.cpp.obj`
  defines both overloads and holds exactly those three imports. A closed loop — the
  shipped binaries genuinely route through the seam, not merely the source.

#### Step 1, full build: the literal step FAILS, and it is not Wave C's fault

`cmake --build C:\atx\build-rel` (default target) exits **1** with two `/WX` errors:

    atx-vol/examples/universe_autofit.cpp:82        FitPreset::Populate not handled in switch
    atx-engine/tests/core/phase4_integration_test.cpp:335   unused parameters
                                                            (ATX_ASSERT compiles out under NDEBUG)

Both are **pre-existing and outside this wave**: neither file appears in
`git diff 587ee97..HEAD`; `universe_autofit.cpp` was last edited 2026-07-12 and the
`FitPreset::Populate` enumerator it fails to handle was added 2026-07-18 (both ancestors
of the wave base); the engine test was last edited 2026-06-17. Neither target had **ever**
been compiled in this build dir, which is why no prior task in Waves A/B/C hit it — every
one of them built specific targets, never the default. A follow-up `-k 0` build enumerated
everything and confirmed **every gate-relevant target links**, with zero first-party
warnings (only 7 third-party spdlog `/MP` unused-argument lines).

**CONTROLLER RULING.** Wave C's gate PASSES on the merits: no target it touches fails, and
the two errors are demonstrably older than the wave. But **"Wave C closes with a clean full
build" is a claim that cannot be made**, and saying otherwise would be exactly the kind of
banked-win-that-did-not-happen this wave has been careful to avoid. Furthermore these two
errors are **load-bearing for what comes next**: Wave D T1 REQUIRES a full
`cmake --build C:\atx\build-rel` (all targets) because `sizeof(RunConfig)` grows and 46
files include `backtest.hpp`. So they are fixed in a separate, clearly-labelled hygiene
commit before Wave D T1 is dispatched — not folded into a Wave C commit, and not deferred.

#### Three further findings from the gate, recorded

1. `atx-engine-core-tests` is the only one of 15 engine test groups with **no binary** —
   its tests run nowhere. Pre-existing, far outside this sprint's scope. Recorded.
2. The Python extension is **not** in the default CMake target (no `.pyd` anywhere in
   `build-rel`); it builds through its own `pyproject.toml`. Flagged so no later wave
   assumes the default build covers it.
3. **The deferred "no committed golden" gap is BROADER than T5 and T6 recorded.**
   `mag7/engine_metrics.csv` embeds `# db_root=<absolute $FX path>`, so **four** of the ten
   goldens die with the scratchpad, not one. Combined with T5's `pnl_track.tsv` finding:
   when `$FX` is cleaned, the byte contract for all five migrated drivers is pinned by
   nothing in the repo. This is the single largest residual risk of Wave C and is handed to
   the final whole-sprint review as a named item.
4. **Process note worth propagating.** The gate agent's own audit script hit a real
   near-miss: PowerShell variables are case-insensitive, so a loop-local `$ref` clobbered
   its `$REF` golden-ref root, and two filtered audits printed
   `ref lines = 0 / COUNT DIFFERS -> TRUNCATION` — indistinguishable from a genuine gate
   failure. It was caught precisely because the protocol requires printing BOTH values and
   the line counts; a boolean-only verdict would have produced a false GATE FAIL. Keep that
   requirement in Waves D and E.

### WAVE C VERDICT — honest, as the plan's own T3 entry demanded

**What Wave C was chartered to do:** close L11 by extracting a strategy-agnostic 9-stage
spine "copy-pasted across 5 of 6 drivers", per design spec 4.3's `BacktestJob`.

**What Wave C actually delivered:**

1. **The measurement that disproved L11.** Stage by stage against the code: clock source
   0/5 identical, strategy construct 0/5, output shape 0/5 (four genuinely distinct
   shapes), `EngineRunStats` 1/5, `Args`/`fmt_num`/`split`/`join` 2/5, RunConfig overlay
   2/5. Only stages 5+6+7 are 5/5 — and four of that triple's six nodes were already
   library calls. So the extractable surface was one small function, not a god-struct.
2. **Five drivers migrated onto `run_timed`, with the migration PROVEN at the object-symbol
   level** and every emitted artifact byte-identical.
3. **Ten driver byte-goldens that did not previously exist.** Before T2, not one of the five
   drivers had an output-byte regression anchor: every existing "gate test" re-implemented
   the driver flow in the test and asserted properties, and none read a file the driver
   binary wrote.
4. **Six plan errors found and reported by implementers rather than worked around** — the
   1-ULP claim, the duplicate Step-1 tests (three separate briefs), T4's positive control
   mislabelled as a RED, T6's un-runnable probe order, and "five CSVs" (which was also
   wrong in mag7's own header comment).

**Line deltas, the whole truth:** -1, 0, +6, +6, +1 = **+12 across five drivers.**
Code-only, mag7 is -5 (the hand-built `EngineRunStats` genuinely disappears); the rest is
comments recording measured findings. **Wave C is a wash on code size, and that is the
finding, not a shortfall.** The wave removed the duplication that actually existed, which
was small, and the measurement proving the rest was never there is worth more than the
lines would have been. Design spec 4.3's `BacktestJob` remains correctly **rejected on
evidence**, with three of its factual claims recorded as errata.

**What Wave C is NOT:** a code-reduction win. Any later reader looking for one will not
find it, and should read the L11-reality-check section rather than re-litigating the
abstraction.

---

## Pre-Wave-D hygiene commit — `017fc5c`

Not part of any wave's plan. Dispatched because Wave C's gate found that
`cmake --build C:\atx\build-rel` (the DEFAULT target) exits 1 on two `/WX` errors, and
**Wave D T1 mandates a full build** (`sizeof(RunConfig)` grows and 46 files include
`backtest.hpp`), so the breakage was load-bearing rather than cosmetic.

Two files, both pre-existing breakage, neither in `git diff 587ee97..HEAD`:

1. `atx-vol/examples/universe_autofit.cpp` — `preset_name`'s switch over `FitPreset` was
   non-exhaustive: `FitPreset::Populate = 4` (`session.hpp:244`) was unhandled. Added the
   case. **Also** added `parse_preset`'s `"populate"` arm, so the new printer branch is
   reachable from the CLI rather than dead — a deliberate 1-line behaviour addition
   (`--preset populate` previously fell through to `Fast`), recorded rather than slipped in.
2. `atx-engine/tests/core/phase4_integration_test.cpp` — **this one was a real bug, not a
   warning.** `apply_guard` asserted with `ATX_ASSERT`, which `macro.hpp:126-132` defines as
   `((void)0)` under `NDEBUG`. In Release the body vanished — which is *why* both parameters
   read as unused, but the real consequence is that
   `EXPECT_DEATH(apply_guard(model.value(), 0U), ".*")` at `:513` **could not pass in
   Release**: the guard no longer aborted. The test was vacuous under NDEBUG, and had
   escaped notice only because this target had never been built here. Fixed by switching to
   the unconditional `ATX_CHECK` — which makes the parameters genuinely used (warning gone
   as a side effect) and makes the EXPECT_DEATH real. **Explicitly NOT fixed with
   `[[maybe_unused]]`**, which would have silenced the compiler and left the test vacuous.
   Comments naming the old macro were corrected, with the NDEBUG reason recorded in-place.

Verification: full default-target build exits **0**, zero first-party warnings (only the 7
expected third-party spdlog `/MP` lines). `atx-vol-tests` 2021/1975/43/3/7 from
`build-rel` CWD, failures exactly the three documented reds — so the `universe_autofit`
edit broke nothing.

**Byproduct worth recording:** `atx-engine-core-tests.exe` now exists and ran for the first
time ever in this build dir — **326 ran / 314 passed / 12 failed**, all pre-existing
death-test-style failures (Market / Portfolio / RollingPanel / SimClock). Reported as
information, NOT chased: they predate this sprint entirely and are outside its charter. The
one test that fix 2 targets,
`Phase4IntegrationDeathTest.Firewall_ApplyInsideFitWindow_CallerGuardAborts`, **passes**,
and its log confirms the abort now fires through `ATX_CHECK` — direct evidence the
EXPECT_DEATH is non-vacuous in Release, which is the point of the fix.

This also closes Wave C's gate caveat: from `017fc5c` onward, "the full build is green" IS
a claim that can be made, and any new warning in Waves D/E is that wave's own.

---

## Task ledger — Wave D

Plan: `docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-d.md`
Workspace: `.superpowers/sdd/2026-07-24-atx-vol-backtest-framework-wave-d/`
Base: `017fc5c` (the pre-Wave-D hygiene commit, itself on Wave C's close `cb875dc`).

### Wave D T1 (StepObserver substrate) — COMPLETE

T1: complete (commit 42c60d8, review clean, ZERO fix rounds — spec PASS, quality APPROVED)
  3 files, +326/-0, zero deletions: backtest.hpp, src/backtest.cpp, tests/strategy_test.cpp.
  `step_observer` is the LAST member of RunConfig, appended after `settlement_mark_memo`
  with a default member initializer and the third "Appended for positional aggregate source
  compatibility" comment. Exactly one field. No existing member moved, renamed or retyped.
  FULL default-target build exit 0, zero first-party warnings. Suite 2026/1980/43/3/7 from
  build-rel CWD = baseline +5/+5 exactly, no fourth failure. The REVIEWER independently
  re-ran both the 5-test filter (5/5) and the full binary and reproduced the counts.
  BLAST-RADIUS CLAIMS SPOT-CHECKED BY THE REVIEWER — all four hold: zero positional
  aggregate-init sites (all 9 `RunConfig{` hits are `RunConfig{}` value-init), zero
  sizeof/offsetof asserts, zero byte-serialization of any cfg/config in atx-vol, and
  run_identity_hash folds run_spec.tsv (+ optional universe_schedule.tsv) FILE BYTES only.
  The reviewer added the check that actually closes the silent-drop question and that the
  implementer did not make: **no function anywhere in atx-vol/atx-engine takes RunConfig BY
  VALUE**, and the complete set of functions accepting one is the two run_backtest
  overloads, run_timed, the shadow, and one bench reporter. There is no fourth engine path
  that could ignore the observer.
  run_timed FORWARDS CLEANLY, both slots verified from source: slot A is
  `run_backtest(clock, strat, cfg)` on the same object by `const RunConfig &`; slot B goes
  through `run_dispersion_backtest`, whose entire body is
  `return run_backtest(clock, strategy, config.run);`. No Wave C edit needed, no drop path.
  FIRING SITES JUDGED CORRECT by the reviewer, and this is the part that matters: only two
  `on_step` call sites exist in backtest.cpp and each is immediately followed by a fire, so
  fire count == clock.size(), exactly the shadow's iteration count. There is NO
  step-loop-body-scope `continue` (the only `continue`s are inside the inner hedge-ledger
  and expiry loops), and every other exit between the top of an iteration and the fire is a
  hard `return Err` that aborts the whole run — so no path silently skips a step the shadow
  would have counted. That was the defect class that would have made T3/T4's equivalence
  proof pass while the real outputs differed.
  NO STALE VIEW: `base = std::move(shifted)` precedes `on_step`, so `*base` already
  corresponds to `refs[i]` at fire time; `refs` is a const span, MarketSnapshot is const,
  and nothing between the fire and the end of the iteration touches `strat`.
  B0 fixed-book overload FAILS CLOSED with InvalidArgument, placed after
  validate_run_config / validate_run_query_route but BEFORE validate_lot_economics and the
  `refs.empty()` check — so the rejection cannot be masked by a bad book or clock.
  NO DEAD MEMBERS: all four StepEvent members are read by
  StepObserverFiresOncePerStepInOrder in this very commit, and task-2-brief names a second
  reader for three of them.

#### The review's most important finding: bit-identity is STRUCTURALLY IMPLIED, not hoped

The implementer raised a forward-looking concern that the shadow replay steps a book mutated
only by `on_step` (no transition validation, execute, hedge, settlement or expiry erase) and
loads via uncached `MarketSnapshot::load`, whereas the observer rides the real engine book
and the SnapshotCache — so bit-identity is "expected but must be proven". **The reviewer
established a stronger result: `ListedDispersionStrategy::on_step` NEVER READS `book`.** It
only writes it (`book.lots = std::move(replacement)`), and the divergence rows are computed
in the pre-mutation seeds loop from `base` + the frozen schedule + `policy_` alone.
Therefore every book difference in the implementer's concern is **provably irrelevant** to
`last_mark_divergences_`. On the snapshot half: `run_projected_backtest_command` builds ONE
RunConfig shared by both routes, so tier and price are identical; `query_cache_build_policy`
is never overridden so it stays `Eager` (no ReuseOnly cold-key downgrade); and
subset-deserialize is wired only for the FIXED-BOOK overload's private cache, so the
strategy overload's cache reaches the same whole-board `MarketSnapshot::load(path, tier)`
the shadow calls. **T1 has ENABLED the T3/T4 proof, not foreclosed it.** The residual risk
is narrower than reported: only a future flag turning on ReuseOnly, or a strategy-side uid
subset, would break the argument. Carry this argument into T3 and T4 — it is what makes the
comparison a proof rather than a coincidence.

#### Plan errors found (Wave D's first three), all judged correct

1. The Python module is **not** in the `build-rel` default target — no pybind target, no
   `.pyd`, no pybind entry in CMakeCache. So a full build gives NO binding coverage. The
   reviewer strengthened this: at HEAD there is **no tracked RunConfig binding at all** —
   the 11-entry `def_readwrite` list ending at `settlement_mark_memo` lives in the
   UNTRACKED `python/src/bindings/backtest.cpp`. "Binding untouched" therefore holds
   trivially, for a stronger reason than the plan gave.
2. `backtest.hpp` has **45** includers, not 46.
3. `RunConfig` reference count: the plan's "~120 sites" is a large understatement.
   Implementer measured 206 across 40 files; reviewer measures 219 lines / 220 occurrences
   across 40 files. Direction correct, exact figure not reproducible — see the minor below.

T1: minor (deferred): `tests/strategy_test.cpp:1656`'s stride gate asserts
  `EXPECT_LT(res->size(), corpus.day_off.size())` where the recorded row set at stride 3 is
  deterministically {0, 3, 6}. `<` passes for ANY row count in 0..6, including a future
  regression recording only row 0. Fix is `EXPECT_EQ(res->size(), 3u)`. The BRIEF specified
  `<`, so this is an inherited brief weakness, not a deviation — but in a wave whose whole
  premise is falsifiability it is worth one line. CARRIED TO THE WAVE D GATE (T7).
T1: minor (deferred): report §6c row 3 overstates its own disclosure — it claims no
  mutation could reach `EXPECT_EQ(fired, 3u)`, but a deferred-abort mutation (capture the
  observer's Error, continue the loop, return it after) leaves `res` in Err so
  `ASSERT_FALSE(res.has_value())` still passes while `fired == 7`, isolating exactly that
  assertion. The recorded CONCLUSION ("regression lock, not a demonstrated RED") is the
  conservative and correct one, so the accounting is honest in direction; only the
  impossibility claim is wrong.
T1: minor (deferred): two report overstatements. (a) "206 references across 40 files" does
  not reproduce (219/220 across 40). (b) "the backtest.hpp ABI note that already declares
  RunConfig layout-fragile stays accurate" — **there is no such note.** The only ABI comment
  in the header is about the IStrategy vtable, and nothing in the header claims RunConfig is
  trivially copyable, so adding a std::function falsified nothing. The conclusion holds; the
  cited support does not exist.
T1: minor (deferred) — **FORWARD NOTE, MUST REACH T3.**
  `collect_mark_divergence_replay` takes `const RunConfig &config` and steps a strategy once
  per clock date WITHOUT consulting `config.step_observer`. T3's brief sets
  `config.step_observer` on that same shared config while keeping the shadow alive, so
  during T3 the shared config carries an observer the shadow silently ignores. Harmless
  there (shadow writes `arena`, observer writes `observed`, no double count) and T1 could
  not have guarded it because the shadow is not an engine entry point — but T3's report must
  say so explicitly, so the "no silently ignored observer" invariant is not assumed total.

#### Controller resolution of T1's two "cannot verify" items

1. "Full build exit 0 / zero first-party warnings" — reviewer was correctly forbidden from
   building. Accepted: `build-rel/bin/atx-vol-tests.exe` (09:11:17) postdates all three
   sources (latest 09:09:51) and passes the full suite, and the immediately preceding
   hygiene commit `017fc5c` had already established a green full build with the 7 known
   third-party lines as the only warnings. Any first-party warning would have been new.
2. "The compile-time RED and the four behavioural mutations M1-M4" — unreproducible after
   revert by construction. Accepted: the 12 quoted diagnostics are internally consistent
   (`-Werror,-Wunused-parameter` matches tree policy, line/column pairs land on the five
   `cfg.step_observer =` assignments), zero mutation scaffolding survives in the tree
   (`MUTATION` / `m3_prev_base` → 0 matches), and the reviewer independently confirmed the
   vacuity properties of all five gates from the source.

### Wave D T2 (mark-divergence collector) — implementation + review

T2: implementer DONE_WITH_CONCERNS (commit c464051, 3 files, +473/-0):
  listed_dispersion_pipeline.{hpp,cpp} + tests/listed_dispersion_pipeline_test.cpp.
  Suite 2033/1987/43/3/7 from build-rel CWD = +7/+7 over T1's 2026/1980 baseline, failures
  exactly the three documented reds. Full default-target build exit 0, zero first-party
  warnings. Reviewer independently re-ran BOTH the targeted filter (ListedDispersionPipeline.*
  18/18 = 11 pre-existing + 7 new) and the full suite, and reproduced the counts by
  `--gtest_list_tests` arithmetic (2040 listed - 7 disabled = 2033 runnable).
  SPEC ✅ with two disclosed, justified deviations (the bps literals, and tests-after-impl
  with the compile RED reconstructed — cited diagnostic line numbers match the landed file).

  THE THREE THINGS THAT MATTER MOST, all verified by the reviewer from source rather than
  taken from the report:
  1. **The downcast is fail-SAFE, not fail-silent.** `Err(InvalidArgument, "...strategy is
     not a ListedDispersionStrategy")` propagates through `ATX_TRY_VOID` at BOTH engine fire
     sites, aborting mid-step before transition validation; and the fixed-book overload
     already rejects `step_observer` outright. There is no path where the collector returns
     Ok() having observed nothing it should have observed.
  2. **Book independence HOLDS**, read set enumerated line by line: the collector reads
     exactly `last_mark_divergences()`, `next_roll_index()`, `schedule.rolls[...]`,
     `event.ref.date`, `event.snapshot.ts_ns()`. `ListedDispersionStrategy::on_step` touches
     `book` exactly once — `book.lots = std::move(replacement)` — AFTER the seeds/divergence
     loop, and `StepEvent` carries no book at all, so the dependence is structurally
     impossible to add by accident. **T4's proof is not foreclosed.**
  3. **Neither fail-closed guard can fire on legitimate production input.** A non-empty
     divergence record implies on_step passed its valuation-ts check, so
     `base.ts_ns() == roll.valuation_ts_ns`; and implies `++next_roll_` ran, so
     `1 <= next_roll_ <= rolls.size()`. Both unconditionally satisfied. No row changes.

  ANTI-VACUITY: no test is satisfiable by a collector emitting zero rows (every capture path
  asserts `rows.size() == 1` before reading fields; every rejection path requires an Err,
  which a no-op collector cannot produce), and none is satisfiable by an engine that never
  fires. The bps expectations are hand-derived dyadic literals, not read back from the
  function. The one `f(x)==f(x)`-shaped assertion re-derives the formula inline instead of
  calling `listed_mark_divergence_bps`, so it does catch a numerator/denominator swap.

  THE IMPLEMENTER ADDED A 7TH TEST THE BRIEF DID NOT SPECIFY, and it was right to:
  `MarkDivergenceObserverRidesTheEngineStepHook` drives the real `run_backtest` strategy
  overload. All five briefed tests hand-build the StepEvent, so **as specified this task
  would have shipped with zero evidence the collector is reachable from the engine** — the
  precise Wave B failure mode. Mutation M-A (a fail-silent collector) proves it is the only
  gate catching zero rows at engine scale.

  ENVIRONMENT HAZARD, judged PURELY ENVIRONMENTAL by the reviewer: the implementer's first
  full build failed on three unrelated targets and killed the PowerShell host with
  `llvm-objdump ... OutOfMemoryException`; an immediate retry with no source change was
  exit 0. Only three TUs include the touched header, and none of the three failing targets
  (`spy_carry_diag`, `spy_dec_curve`, `spy_fit_matrix_bench`) contains any `listed_dispersion`
  reference — an additive header include plus two new symbols cannot produce a link-stage OOM.
  **CONTROLLER NOTE: the box is shared with other sessions' worktrees. This sprint's
  implementers are dispatched strictly one at a time, so the contention is external. It
  matters for WAVE E, whose measurement protocol requires a quiet box — Wave E tasks must
  confirm quiet before measuring and discard any pair of numbers straddling foreign load.**

  PLAN ERROR (Wave D's 4th), CONFIRMED: the brief's four pinned bps literals cannot hold
  under the `EXPECT_EQ` the brief also demands — `(2.0, 2.02)`, `(2.0, 1.98)` and
  `(-1.0, -1.01)` all yield `100.00000000000009`. The implementer preserved all four CASES
  with dyadic inputs (2500.0 / 0.0), which keeps `EXPECT_EQ` and weakens no coverage.
  (The implementer's *supporting* reasoning was itself wrong twice — see the minor below.)
  PLAN ERROR WITHDRAWN: the report's plan-error #2 is a misreading. The brief cited
  `:714-747` as the inner block to lift, not as the function's extent, and both the
  NotFound message and the three arithmetic lines are exact.

T2: fix round 1/5 dispatched — 1 Important, 3 Minors:
  IMPORTANT: nothing gates row MULTIPLICITY or ACCUMULATION, which are exactly the two
  properties the shadow's arena has and exactly what T4 bit-compares. Every fixture perturbs
  one leg on one step and the engine test runs a ONE-DATE clock, so three mutations survive
  all seven tests: `out.clear()` before push (invisible because every test uses a private
  vector expecting one row), `break` after the first push, and hoisting the push out of the
  loop. Fix: perturb two legs (the roll has six), assert rows.size()==2 with distinct legs in
  divergence order; and give the engine test a two-date/two-roll clock so accumulation across
  steps is proven — which also closes the fact that NO collector-level test currently
  exercises T1's per-step firing site, only the inception site.
  Minor 1: `test:1129` uses EXPECT_DOUBLE_EQ for `live_mark` where every other mark
  assertion in the file (including `schedule_mark` three lines above) is EXPECT_EQ and
  passes. Undisclosed. Likely cause: engine route has `price.analytic_greeks == true` while
  the unit tests' 4-arg on_step uses `PriceOptions{}` with it false. **Either tighten to
  EXPECT_EQ or document why — because if the two routes are NOT bit-identical on `live_mark`,
  that is a finding the controller needs BEFORE T4, not after.**
  Minor 2: the header comment's "one schedule object shared by strategy and observer" is
  factually wrong — `ListedDispersionStrategy::create` takes the schedule BY VALUE and stores
  a copy, so they are never the same object. Conclusion survives via value-equality; restate
  the invariant as value-equality, since that is what a caller can actually violate.
  Minor 3: correct the report's false lemma — 100 bps is a relative diff of 1e-2 not 1e-4,
  and exactly 100.0 IS reachable (`bps(100.0, 101.0) == 100.0` exactly).

#### Two forward notes the controller carries (NOT for T2 to fix)

- **For T5's brief:** the shadow ends with
  `Err(Unavailable, "divergence replay did not consume every scheduled roll")`, whose comment
  is the very claim the fail-safety argument rests on ("an empty section must mean every roll
  fired"). A per-step observer structurally cannot carry it. It is **not lost** — the priced
  run already re-checks the same strategy — but T5 must keep that check when it deletes the
  shadow and accept that the message text changes.
- **For T4's brief, verbatim:** book independence is necessary but NOT sufficient for the
  equivalence T4 asserts. `live_mark` comes from the loaded surface, and the two routes load
  differently — the shadow uses `MarketSnapshot::load(path, tier)` while the engine uses
  `SnapshotCache::load(path, tier, build_policy)`. Under the CLI's `Eager` default these
  should agree, and on `--execution cold` the tier is `LegacyCompatible` on both sides — but
  **that is the half of the equivalence T4 must actually MEASURE**, and it must measure it on
  a route producing a nonzero row count. Also use a corpus with MORE THAN ONE ROLL, and
  report the row count compared.

T2: fix round 1/5 (4 addressed, 0 open; commits c464051..c438a64)
T2: complete (commits 42c60d8..c438a64, review clean after 1 fix round)
  The Important was CORRECT and the fix confirmed it: all three named mutations DID survive
  round 1. New gates:
  - `MarkDivergenceObserverAppendsOneRowPerDivergedLegInOrder` perturbs legs[2] (N0 CALL,
    +0.01) and legs[5] (N1 PUT, +0.02) — deliberately NOT an adjacent pair, so the two rows
    differ in symbol, side, strike, diff AND bps and neither can substitute for the other.
    Order asserted element-wise against the STRATEGY's own divergence vector (a different
    object), so it is not a restatement of the collector's loop.
  - Engine test rebuilt on a TWO-DATE / TWO-ROLL clock: 1 diverged leg on roll 0, 2 on
    roll 1 -> 3 accumulated rows, a total no per-call clear (2), break (2) or hoisted
    push (2) can produce. This also closed the round-1 hole that NO collector-level test
    exercised T1's per-step firing site — only the inception site.
  MUTATION EVIDENCE, each isolating a different gate: M-F clear-then-push -> 2 FAILED
  (1 vs 2, 1 vs 3); M-G break -> 2 FAILED (1 vs 2, 2 vs 3); M-H per-call clear -> **1
  FAILED, the engine test ALONE** (2 vs 3) — direct proof that the two-roll clock, not the
  multiplicity test, is what closes accumulation. All reverted.
  DISCLOSED PLAINLY: both new gates passed on first run. The behaviour was already correct,
  merely ungated — no RED claimed. Correct framing, confirmed by the re-reviewer.
  Minor 1 tightened to EXPECT_EQ AND IT PASSES — the engine route's disk-round-tripped
  archive mark under analytic_greeks=true equals the schedule's in-memory authored
  model_mark BIT-FOR-BIT, and the unit tests' PriceOptions{} route equals the same value, so
  the two agree transitively. Minor 2's invariant restated as value-equality in both header
  and .cpp, with a note that `out` accumulates and is never cleared. Minor 3's false lemma
  withdrawn in place and plan-error #2 withdrawn as a misreading — itemised in report §9.5
  so the corrections are auditable rather than silent.
  Suite 2034/1988/43/3/7, reproduced by the re-reviewer; ListedDispersionPipeline.* 19/19;
  full default-target build exit 0, zero first-party warnings. Guard logic, the downcast
  fail-safe path and book-independence are BYTE-IDENTICAL to round 1 (the .hpp/.cpp changes
  are comment-only), so nothing was weakened to make the new gates pass.
T2: minor (deferred): the third round-1 mutation ("hoist the push out of the loop") has no
  explicit post-fix negative-control run — logically subsumed by ASSERT_EQ(rows.size(), 2u)
  (any code producing fewer than 2 rows on a 2-divergence call fails it) but not empirically
  exercised as its own mutation.
T2: minor (deferred), self-disclosed: the two-roll clock proves accumulation across TWO
  steps, not arbitrary N.

### Wave D T3 (dual-run equivalence comparator, shadow RETAINED) — implementation + review

T3: implementer DONE_WITH_CONCERNS (commit f60ce3c, 1 file, +185/-0, ZERO deletions —
  the shadow is retained by construction and the emitted artifact still comes from the
  arena). Full suite 2034/1988/43/3/7 = ZERO delta (the example is a standalone
  add_executable, not linked into atx-vol-tests, so a gtest delta is structurally
  impossible — the reviewer confirmed this rather than accepting the number). Targeted
  filter 118/118. Full build exit 0, zero first-party warnings.
  FIXTURE RUN: 3 sessions / 2 rolls, `--execution configured` -> **observer=36 shadow=36
  rows MATCH**, exit 0, final_nav=-4779.718393. Cold -> 0 rows MATCH. `--no-divergence`
  unchanged. **The REVIEWER independently reproduced every one of those numbers**, plus the
  19/0/17 row split by date and 36 distinct raw_symbol values.
  FOUR RED PROBES, coverage judged ADEQUATE by the reviewer and covering the modes that
  matter: row-COUNT (checked first, before any value is read), **row-ORDER** (positional
  walk, no sort/key-join — the reviewer singled this one out as the probe the brief did NOT
  ask for and the one that matters most, since key-joining is the "obvious improvement" that
  would silently destroy the discriminator), value, and vacuity. Residue impossible: the
  diff is +185/-0, so no probe line can survive in any form.
  THREAT MODEL SEARCHED BY THE REVIEWER: no path exists where the comparator reports
  identical while the sources differ — row count checked first, all ten registry columns
  compared, positional walk, no tolerance / sort / key join / field exemption, arena
  structural guards make every per-row read in-bounds and every dict decode valid. **The
  only hole is n == 0**, which is exactly what the vacuity guard exists for.
  Value-equality preserved: the observer's schedule is passed as an lvalue to `create`
  (by value -> copy) with NO std::move, so it stays value-equal; validate takes const& and
  does not reorder.
  The "shadow ignores step_observer" fact is made STRUCTURAL rather than argued — the
  observer is installed AFTER `collect_mark_divergence_replay` returns.

  PLAN ERROR (Wave D's 5th), CONFIRMED: the brief's pinned fixture is GONE — both
  `C:\atx\scratchpad` and `C:\atx\.scratchpad` are absent. The implementer rebuilt an
  equivalent from `C:\atx-data` READ-ONLY (archives copied out, nothing written there) at
  3 sessions / **2 rolls** rather than the brief's 1. The reviewer judged the rebuild
  **adequate and strictly better**: it exercises both T1 firing sites (inception i=0 -> 19
  rows; per-step i=2 -> 17 rows), an accumulation boundary (a per-call out.clear() would
  have yielded 17, not 36), and one quiet non-roll step (i=1 -> 0). **The brief's
  `dates=3 rolls=1` economics acceptance line is RETIRED, not open.**

  MERGE-WRITE WARNING FOR T4, CONFIRMED BY THE REVIEWER FROM `run_archive.cpp`: on an
  existing archive whose `run_identity_hash` matches, sections carry forward only if their
  name is NOT in the incoming write set — **"on a name collision the NEW section wins."**
  `run-projected-backtest` writes projected_cold, mark_divergence, meta and diagnostics, so
  running it on `parity-full` would REPLACE ALL FOUR and destroy the pinned cold
  `mark_divergence rows=0`. The copy remedy is sound: `run_identity_hash` folds only
  run_spec.tsv + universe_schedule.tsv, and the manifest's archive paths are ABSOLUTE, so a
  metadata-only copy runs correctly and keeps the same identity. **T4 RUNS ON A COPY.**

T3: fix round 1/5 dispatched — 2 Importants, no Criticals.
  **IMPORTANT 1 IS THE CONTROLLER'S FAULT, recorded as such.** My dispatch told the
  implementer the comparator "must assert the compared row count is > 0 on that route and
  fail the gate as vacuous if it is 0". I did not scope it; the implementer built exactly
  that; it is mis-scoped. The reviewer proved it EMPIRICALLY rather than arguing it:
  - Zero rows on `--execution configured` is LEGITIMATELY REACHABLE. A row exists iff
    `seed.greeks().price != leg.model_mark` (exact !=), and `QueryPricingRoute::ColdFallback`
    is documented as what a fast-configured surface reports outside its certified correction
    box, with `priced_surface_test.cpp` pinning that fallback as EXACTLY EQUAL to the cold
    value. So a configured run whose legs all fall outside the box reproduces the frozen cold
    marks bit-for-bit and emits zero rows — as does any schedule authored under the fast tier.
  - REPRODUCED: the reviewer copied the fixture, rewrote trade_schedule.tsv so each diverging
    leg's model_mark equals the configured route's own live_mark (a well-formed schedule that
    passes validation), and got `Unavailable: ... 0 divergence rows`, EXIT=1.
  - **WORSE THAN AN EXIT CODE: the run's entire output is destroyed.** The `return Err`
    precedes `write_run_archive`, and the reviewer confirmed the run dir afterwards contains
    NO run.atxrun at all — projected_cold, meta and diagnostics lost. Before the change that
    run wrote all four sections and exited 0.
  - **The conditioning is keyed on the wrong axis.** On the same patched fixture, `cold`
    produced 36 rows and PASSED while `configured` produced 0 and FAILED. Neither
    "cold => 0" nor "configured => >0" is a property of the routes; both are properties of
    the schedule's provenance. So the guard PERMITS the one genuinely vacuous case
    (cold + 0 rows) and KILLS a run that is merely uninteresting — on the DEFAULT route.
  CONTROLLER RULING, both halves ordered: (1) make the guard OPT-IN and route-independent
  via `--require-divergence-rows` — T4's proof run passes it, nothing else gains a failure
  mode, and it also lets the cold route be checked for non-vacuity on a corpus that does
  produce rows; (2) relocate the `return Err` to AFTER `write_run_archive` regardless, so a
  rejected proof still leaves the artifact behind. The exit code is recoverable; the archive
  is not.
  IMPORTANT 2: the MATCH line goes to stdout while the "0 rows — plumbing check" caveat goes
  to stderr, so a stdout-only log shows a bare unqualified
  `mark divergence equivalence: observer=0 shadow=0 rows MATCH`. That stdout line is what T4
  greps and what a ledger transcript would carry — i.e. exactly the artifact **T5's deletion
  decision rests on**. Fix: put the qualifier on the same stdout line as a suffix; any
  `mark divergence equivalence:` / `rows MATCH` grep still matches.

T3: minor (accepted, no change): the bit-compare rationale claims discriminations these
  fields cannot exercise — -0.0 is unreachable (a row exists only when live != schedule),
  abs_diff_bps_of_mark returns literal 0.0 on both sides for a zero denominator, and the
  schedule parser rejects non-finite model_mark. The deviation is SAFE and STANDS:
  bit-compare is never weaker than == on any value these fields can hold.
T3: minor (accepted, report-only): "a difference surfaces three times over" is inflated —
  `diff` and `abs_diff_bps_of_mark` are computed from live_mark/schedule_mark by identical
  arithmetic on both sides, so they are not independent detectors.
T3: minor (accepted, report-only): "the two load paths are genuinely independent" is true as
  to shared state, but both reduce to *deserialize archive -> with_query_pricing(tier)*, a
  deterministic pure function of the same bytes — so bit agreement was EXPECTED, not a
  coincidence surviving a hard test. The comparator still covers the axis (a ReuseOnly /
  referenced_uids / tier difference would land on live_mark and on the row set and be
  reported), which is what makes it the right instrument for FUTURE divergence — and that,
  not the current agreement, is what T5's deletion actually rests on.

### STOP POINT — 2026-07-25, user instruction "stop here"

T3: fix round 1/5 — code fix COMMITTED as 5c227e8, but the round is NOT closed:
  - the implementer's fix report was never appended to task-3-report.md (it was stopped
    mid-sentence doing exactly that), so the fix's own test evidence is off the record;
  - **the scoped re-review never ran.** 5c227e8 is the ONLY commit on this branch that no
    reviewer has seen. Re-review range f60ce3c..5c227e8 before anything else.
  Resume by re-running the fix round's verification (fixture matrix across configured, cold,
  --no-divergence, and --require-divergence-rows both set and unset — including the
  flag-set-with-zero-rows case, which must fail AFTER writing the archive, with the archive
  demonstrably present afterwards), then the scoped re-review, then T4.
  Working tree is clean for every file this sprint touched. The pre-existing unrelated
  uncommitted work was never staged at any point.
  Status doc: docs/superpowers/2026-07-25-atx-vol-backtest-sprint-status.md

### Wave D T4 (135-session observer/shadow equivalence proof) — CONTROLLER, GREEN

T4: **GREEN. The T5 deletion is AUTHORIZED.** Run on COPIES ONLY; `parity-full` was
  never written — its `run.atxrun` hashed `D88BFEE04D3EF300` before the first copy and
  `D88BFEE04D3EF300` after every run in this task.

  **PLAN ERROR (Wave D's 6th), CONFIRMED AGAINST THE AUTHORITY.** The brief's Step 1 and
  Step 2 invocations omit `--schedule projected_schedule.tsv`. As written they cannot
  meet Step 1's own acceptance line. Measured, not argued: the brief's literal command
  (`run-projected-backtest --run <dir> --execution cold`, default schedule) produced
  `final_nav=125026.0592`, NOT the required `123243.1172` — and its `projected_cold`
  dump hashed **`a05470c7a6f6572f`, byte-identical to the sprint's `backtest` golden
  `a05470c7`**. Replaying `trade_schedule.tsv` cold reproduces the plain listed backtest
  exactly, which is the correct behaviour and the proof of the diagnosis. The contract is
  stated in code at `spy_dispersion_backtest.cpp:677` — *"projected_schedule.tsv stays a
  text INPUT: run-projected-backtest reads it back via --schedule"* — and the authority is
  `parity_full_run.ps1:30-33`, whose Step 4 passes `--schedule <run>/projected_schedule.tsv`.
  The briefs paraphrased that step and dropped the flag. **The same omission is in
  `task-7-brief.md` Step 3 and Wave E `task-9-brief.md` Step 3** — both corrected here by
  reference rather than left to be rediscovered at the gate.
  Side benefit: the accidental run re-verified the `a05470c7` `backtest` golden on 135
  sessions, from a completely different subcommand than the one that pinned it.

  **BASELINE (parity-full, read-only, before anything ran).** Captured with bash
  redirection, hashed in PowerShell:
    projected_cold   48627 B  136 lines  cbabca44e411d4d9   == the Wave A/B pin cbabca44
    mark_divergence     98 B    1 line   c9a04d1bcf0e3c07   header-only, rows=0
    meta               751 B   24 lines  2946e79b31e701a0
    diagnostics        368 B    7 lines  02ca2f17c0a069ff

  **TOOLING HAZARD, NEW AND MEASURED — PowerShell `>` CORRUPTS A BYTE GATE.** The first
  baseline capture used PowerShell `>` to redirect the exe's stdout and hashed
  `E0C2ABB1AA1E49DB` for `projected_cold` — a clean FAIL against the `cbabca44` pin. The
  bytes were never wrong; PS 5.1 decodes native stdout to text and re-encodes it with a
  UTF-8 BOM and CRLF. Re-captured through bash redirection the same dump hashed
  `cbabca44e411d4d9`. **Rule, now binding for the rest of the sprint: capture bytes with
  bash `>`, hash with PowerShell `Get-FileHash`. Never redirect a native exe with
  PowerShell `>` into anything a hash will be taken of.** This sits alongside the existing
  Bash-`diff`-exit-code hazard; both are ways to manufacture a false gate verdict.

  **STEP 1 — cold, canonical (`--schedule projected_schedule.tsv --execution cold`), on
  the copy `t4-cold`. PASS, 4/4 acceptance criteria:**
    exit 0
    `mark divergence equivalence: observer=0 shadow=0 rows MATCH (VACUOUS: 0 rows
     compared, a plumbing check and NOT the observer/shadow equivalence proof)`
    `projected backtest complete [cold]: dates=135 rolls=7 final_nav=123243.1172`  EXACT
    projected_cold  48627 B  136 lines  **cbabca44e411d4d9** == pin, and
      `Compare-Object` vs the untouched baseline = **0 differing lines**
    mark_divergence 98 B 1 line **c9a04d1bcf0e3c07** == baseline, header-only
    wall 1091.602 ms / 135 sessions = 8.086 ms/session
  **`MD-COLD` = `c9a04d1bcf0e3c07138e3ba4752c6c7ca762e68dffc5af9f607000cd2fcd6085`.**
  Note the T3 fix is doing exactly its job here: this MATCH is labelled VACUOUS on the
  stdout line itself. Before `5c227e8` this transcript would have read as a bare
  unqualified MATCH — i.e. Important 2 was a real hazard and this is the run that would
  have carried it into the deletion decision.

  **STEP 2 — THE PRIMARY NON-VACUOUS PROOF. configured, canonical, WITH
  `--require-divergence-rows`, on the copy `t4-cfg`. PASS:**
    exit 0 (the opt-in proof gate was satisfied, not merely absent)
    `mark divergence equivalence: observer=137 shadow=137 rows MATCH`   **N = 137 > 0**
    `projected backtest complete [configured]: dates=135 rolls=7 final_nav=132776.9818`
    mark_divergence 22182 B 138 lines (137 rows + header) **9e958a90ae15ac74**
    projected_cold  48572 B 136 lines  467d4b2a29437da8   (diagnostic route, not a golden)
    wall 45947.434 ms / 135 sessions = 340.351 ms/session
  **`MD-CFG` = `9e958a90ae15ac74…`, N = 137.**

  **THE STATEMENT THE BRIEF REQUIRES:** *observer-derived divergence == shadow-derived
  divergence, bit-exact, on N = 137 real rows drawn from a 135-session / 7-roll
  production corpus — the T5 deletion is authorized.*

  What that sentence does and does not cover, stated so T5's reviewer does not have to
  rediscover it: the comparison is bit-exact over all ten registry columns, positional
  (no sort, no key join, no tolerance), row-count checked before any value is read. It
  covers the load-path axis that T2's review flagged as the genuinely unmeasured half —
  the shadow loads each session through `MarketSnapshot::load(path, tier)` while the
  engine loads through `SnapshotCache::load(path, tier, build_policy)` — because
  `live_mark` is one of the ten compared columns and a tier or cache-build-policy
  difference would land on it and on the row set. On the configured route, where 137 rows
  exist and every one of them is a live-vs-frozen mark difference, that axis is now
  measured rather than assumed. It does NOT prove anything about routes or corpora not
  run here.

  **DETERMINISM — PROVEN, not asserted.** A third pass ran the identical configured
  invocation on a SECOND fresh copy (`t4-cfg2`) made from `parity-full` after Step 2 had
  already finished, so it shares no state with Step 2's dir:
    `observer=137 shadow=137 rows MATCH`, `final_nav=132776.9818` (identical)
    mark_divergence 9e958a90ae15ac74 == 9e958a90ae15ac74, Compare-Object = 0 diff lines
    projected_cold  467d4b2a29437da8 == 467d4b2a29437da8, Compare-Object = 0 diff lines

  **NON-VACUITY OF THE 137 ROWS — enumerated, because "N > 0" alone is a weak claim.**
  The section is not padding and not a repeated constant:
    137 rows / **137 distinct `abs_diff_bps_of_mark` values** (no duplicates at all)
    137 distinct `raw_symbol` values (every row a different contract)
    **7 distinct dates == the corpus's 7 rolls** — so the observer accumulated across
      every roll, which is precisely the property T2's fix round had to add a gate for
      (`out.clear()`, `break`-after-first and hoisted-push all survived T2's original
      seven tests); here it is demonstrated at production scale
    11 distinct underlyings: AAPL, AMZN, AVGO, GOOGL, JPM, LLY, META, MSFT, NVDA, SPY, XOM
    bps spread 2.3076e-12 .. 795.7988 — five orders of magnitude below one bp up to eight
      hundred, i.e. the comparison is exercised across the full dynamic range of the
      metric, including values where a tolerance-based comparator would have hidden a
      real difference. Both `Call` and `Put` sides present.

  **ESCALATION PATH NOT TAKEN.** The pre-decided fallback (perturbed-`model_mark` copied
  schedule) was not needed: the configured route completed on the real corpus and yielded
  N = 137 > 0. Recording that it was available and unused, so the absence of an
  escalation note is not read as an oversight.

  **ARTIFACTS RETAINED FOR T7 STEP 4:** `C:\atx-data\spy-dispersion\runs\t4-cold`,
  `t4-cfg`, `t4-cfg2` (metadata-only copies, ~12 MB each — the two 696 MB
  `definitions*.tsv` are deliberately excluded because `run_projected_backtest_command`
  reads only `run_spec.tsv`, `surface_manifest.tsv` and the schedule; verified by reading
  the function, and by these runs completing). The manifest's `archive_path` column is
  ABSOLUTE (`C:/atx-data/spy-dispersion/runs/bt-sota-full/archives/...`), which is what
  makes a metadata-only copy run correctly and keep the same `run_identity_hash`.

### Wave D T3 fix round 1 (5c227e8) — SCOPED RE-REVIEW, CLEAN

T3 fix 1: **Spec ✅ / Code quality APPROVED. Zero Critical, zero Important. 5 Minors.**
  The fix round is CLOSED and `5c227e8` is no longer the branch's one unreviewed commit.
  Review file: `.superpowers/sdd/2026-07-24-atx-vol-backtest-framework-wave-d/task-3-rereview.md`.

  **THE DECISIVE TEST, run rather than argued.** Important 1's second half was "relocate
  the `return Err` to AFTER `write_run_archive` so a rejected proof still leaves the
  artifact behind." The reviewer built a zero-row fixture, set the flag, DELETED the
  existing `run.atxrun` first so a survivor could not be mistaken for a leftover, and got:
    `EXIT=1, archive_before=False, archive_after=True, size=7248`
  and then dumped all four sections cleanly OUT of that post-failure archive
  (`projected_cold` 4 lines, `mark_divergence` 1, `meta` 24, `diagnostics` 7). Before the
  fix the same scenario left NO `run.atxrun` at all.

  **BOTH OLD DEFECTS INVERTED, measured on the same fixture:**
    configured + 0 rows + no flag -> **exit 0** (was: exit 1 AND a destroyed archive)
    cold       + 0 rows + flag    -> **exit 1** (was: PASSED — this is the genuinely
                                     vacuous case the old route-conditioned guard let
                                     through, and it is the more interesting half: the
                                     old guard was not merely too strict, it was
                                     simultaneously too strict on one route and too
                                     lax on the other)
  Route-independence confirmed STRUCTURALLY, not by inspection of intent: `cold` is read
  at exactly one site (`:948`, config selection) and appears nowhere on the guard path;
  the predicate is `require_divergence_rows && (n_rows == 0)`. Flag parse verified clean
  in first/middle/last argv position; typos and unknown flags exit 2 with usage.

  Evidence: suite 2034/1988/43/3/7 (ZERO delta, the 3 documented reds only); targeted
  filter 118/118; 8-case flag matrix + 5 parse cases with stdout and stderr captured to
  separate files; full build exit 0, no first-party warnings, the added line
  clang-format-clean.

  **ARTIFACT INVARIANCE ACROSS THE FIX — proven against an INDEPENDENT pre-fix capture.**
  The prior reviewer's `md_cfg.tsv`, produced from the `f60ce3c` binary, is byte-identical
  to the re-reviewer's post-fix dump from the `5c227e8` binary:
    both = 39D47B8B64AF852C08CE6983821A95A3ED2FC3BC3D22A1FD4ADCEB7C8D95A91F
  Two different agents, two different builds, one hash. That is a stronger invariance
  claim than a self-comparison could ever be.

  **MINOR 2 IS A CONTROLLER-LEVEL METHODOLOGY FINDING, not a task nit.** T3's quoted gates
  `893A01F1728A4E25` (cold) and `AFD4C06EC0E6878A` (configured) are hashes of a
  **CRLF-normalised** capture. The raw `runarchive dump --tsv` bytes use bare LF and hash
  to `39D47B8B64AF852C…`. A future re-check taken with a raw capture would read as drift
  and fail a gate that is actually green.
  **This is the SAME hazard the controller hit independently during T4 from the opposite
  direction** — a PowerShell `>` capture of `projected_cold` hashed `E0C2ABB1AA1E49DB`, a
  clean-looking FAIL against the `cbabca44` pin, purely from BOM + CRLF re-encoding. Two
  agents, two tasks, same root cause, in one session.
  **RULE, now binding for the rest of the sprint: capture bytes with bash redirection,
  hash with PowerShell `Get-FileHash`, and record which convention a pinned hash was taken
  under.** T5's dispatch carries the raw values, not T3's CRLF ones.

  Minor 1 (`--no-divergence --require-divergence-rows` silently ignores the proof gate):
    accepted, no change. Cannot produce a false positive — with `--no-divergence` no MATCH
    line prints at all, so any downstream grep fails loudly rather than quietly.
  Minor 3 (a MISMATCH still discards the valid `projected_cold`/`meta`/`diagnostics`):
    accepted, deliberate, documented in code at `:1019-1024`. Defensible: merge-write
    would overwrite a good section with one whose provenance the run had just refuted.
    Recorded, not actioned. Moot after T5 deletes the comparator.
  Minor 4 (a naive `rows MATCH` grep still matches the VACUOUS line): by design per the
    controller ruling — the greppability of `mark divergence equivalence:` / `rows MATCH`
    was a requirement, and the qualifier is a suffix. Mitigated by the flag's exit code,
    which is what T4 actually relied on.
  Minor 5 (4 comment lines added by the commit still carry em-dashes): the ASCII claim was
    about printed/greppable string literals and is true as stated. Phrasing note only.

  **PROCESS GAP, RECORDED HONESTLY: the implementer's §10 fix report was never written.**
  `task-3-report.md` carries the pointer header at lines 3-9 but the body still ends at §9
  — the implementer was stopped mid-sentence appending it. **This re-review is therefore
  the sole evidence record for the fix**, which is why it was run before T5 rather than
  waived. It also independently corroborated the flag at production scale by noticing that
  commit `819c442` (T4) had already exercised `--require-divergence-rows` on the
  135-session corpus at exit 0 with observer=137 shadow=137.

  No source modified, nothing committed, `C:\atx-data` never touched.

#### Controller ruling on `--require-divergence-rows` (for T5)

The flag is **deleted by T5 together with the comparator it gates.** Its documented
purpose is that the observer/shadow *comparison* is vacuous; after T5 there is no
comparison, so the flag would gate nothing. This is not churn: it was added because T3's
original guard was actively destructive, T4 used it to make the 135-session proof
fail-closed rather than silently vacuous, and it retires with the machinery it guarded.
Recorded here so the delete reads as a decision rather than drift.

### Wave D T5 (delete the shadow loop — the L10 payoff) — implementation + review

T5: **REVIEW CLEAN ON THE FIRST PASS. Spec ✅ / Code quality APPROVED, zero Critical,
  zero Important.** Two Minors, both carried. Review file: `task-5-review.md`.

  **THE REVIEWER RAN A GATE NOBODY ASKED FOR, AND IT IS THE ONE THAT MATTERS.** Every gate
  in the brief, in the dispatch and in the implementer's report compares **TSV dumps**. A
  TSV dump renders the decoded logical rows — it structurally CANNOT see the dictionary
  encoding, so a changed `dict_intern` insertion order would produce identical TSV and a
  different `run.atxrun`. The controller's dispatch flagged that as an open question and
  asked the reviewer to say whether any gate covered it. Instead of answering, it closed
  it: it parsed the `run.atxrun` container and compared **per-section binary payload
  crc32c + sha256** between the pre-change and post-change binaries.
    mark_divergence BINARY-IDENTICAL  cfg CE2CE861 (4280 B / 36 rows), cold C6C24283,
                                      --schedule 61DF66C4
    projected_cold  BINARY-IDENTICAL  8AB97D2C / 3ED85C3B / 05A6AAF1
    projected_nodiv BINARY-IDENTICAL  3ED85C3B  (== cold projected_cold, as designed)
    only `diagnostics` (wall_ms) and `meta` (run-dir path) differ — both expected.
  **The deletion is now proven byte-identical at the container level, not merely at the
  rendered-text level.** That is a materially stronger claim than the task was asked for.

  The reviewer also re-linked the example itself and re-derived all nine of the
  implementer's hashes from its OWN post-change binary against the implementer's PRE
  captures — i.e. it did not accept a single reported number.
  Suite 2034/1988/43/3/7 (exact baseline, 3 documented reds); filter 118/118; determinism
  reproduced across two independent runs; diagnostics 7 lines with
  `[setup_read, divergence_replay, archive_load, priced_run, write_outputs, total]`
  identical pre/post on all FIVE routes, `divergence_replay` count 3, `archive_load` 0/0;
  the retired flag returns rc=2 in both argv positions.

  BOTH implementer concerns UPHELD by the reviewer:
  - Concern 1 (the controller's dispatch error) — upheld. `run_fixture.ps1:20,:34` pass no
    `--schedule`, so T3's pins are default-invocation. The reviewer additionally verified
    the implementer's claim to have gated BOTH invocations pre/post was true and
    like-for-like (one parameterised capture script, five routes, both phases) by
    re-deriving all nine hashes itself.
  - Concern 2 (its own `MD-3S-COLD` gate is near-vacuous) — upheld and "correctly
    reasoned". Cold `mark_divergence` is 98 bytes of header only, byte-identical to the
    135-session corpus's, and satisfiable by a build with NO observer installed.
    `projected_cold` is the load-bearing cold gate; the reviewer confirmed its binary
    payload identity too.

  T5 review Minor M1: dangling `collect_mark_divergence_replay` provenance comments left
    in `listed_dispersion_pipeline.hpp:282` and `.cpp:451` — outside the brief's one-file
    scope. **Carried to a Wave E comment sweep.**
  T5 review Minor M2: the doc block this commit rewrote still calls `--no-divergence` "the
    bare-backtest wall-time path" while the same commit reduces that saving to 0.0069 ms
    of 24.03 ms. **Carried to Wave E** (same item as implementer concern 4).
  T5 review Minor M3: the arena staging has no automated test — the same exposure the
    shadow had, so not a regression. Carried to the final review.

### Wave D T5 — implementation detail

T5: implementer DONE_WITH_CONCERNS, commit `d955e93`, one file
  (`atx-vol/examples/spy_dispersion_backtest.cpp`), 108 insertions / 301 deletions.
  **LINE DELTA 1459 -> 1266 = -193** (-150 code, -37 comment, -6 blank). Of the 301
  removed lines 174 were code; of the 108 added, 24 — a 7.25:1 code reduction on the
  touched region. `grep -c collect_mark_divergence_replay` 4 -> 0; every deleted symbol
  (`collect_mark_divergence_replay`, `compare_mark_divergence_sources`, `bit_equal`,
  `fmt_g17`, `require_divergence_rows`) greps to ZERO tree-wide (controller re-verified).
  Suite 2034/1988/43/3/7 from build-rel CWD — EXACT baseline, the 3 documented reds only.
  Targeted 118/118. Full build exit 0, zero first-party warnings; clang-format sites
  9 -> 8 (the lost one was inside deleted code).

  **THIS IS THE FIRST REAL CODE-SIZE WIN OF THE SPRINT.** Wave C was chartered to reduce
  duplication and came out +12 lines — recorded honestly at the time as a wash. Wave D
  was chartered to delete a shadow and deleted 193 lines, 150 of them code. Worth stating
  plainly because the two waves are often conflated: the abstraction did not pay, the
  measurement-then-deletion did.

  **THE INVARIANT SURVIVED, and the carry is argued rather than asserted** (controller
  read the landed code at `:826-836` directly). The deleted loop's post-loop gate carried
  the evidence-channel contract — *"an empty mark_divergence section must mean 'every roll
  fired and none diverged', never 'the replay silently skipped rolls'"*. That comment is
  carried verbatim onto the priced run's pre-existing `all_rolls_consumed()` gate, with an
  added paragraph explaining WHY the surviving gate covers the same failure mode: the
  observer only ever appends rows for a roll the priced strategy actually fired, so "every
  roll consumed" is exactly the precondition that makes a zero-row section readable as
  "no divergence" rather than "no coverage". The message text differs ("projected backtest"
  vs "divergence replay") because the subject differs — one run, not a replay. Accepted.

  The arena fill is registry-order, positional and append-only, with the SAME `dict_intern`
  calls and the same `Side::Call ? 0 : 1` mapping the shadow staged with, so
  `build_mark_divergence_section` is untouched and sees a bit-identical arena.
  `--require-divergence-rows` retired in full, with a negative control: it now exits 2 with
  usage in BOTH terminal and non-terminal argv position rather than being silently ignored.

  GATES (both values printed in the report):
    mark_divergence configured pre == post ==
      39D47B8B64AF852C08CE6983821A95A3ED2FC3BC3D22A1FD4ADCEB7C8D95A91F  (37 lines / 36 rows)
      — equals the raw-LF anchor two independent reviewers produced across the T3 fix.
    projected_cold identical on ALL FOUR routes — the gate that actually proves removing
      the shadow did not perturb the priced run.
    projected_nodiv identical, and equal to cold's projected_cold.
    diagnostics byte-stable at 7 lines, same five phase names in order, `divergence_replay`
      count still 3.
    determinism proven, both hashes agreeing.
    `meta` differed by exactly 2 bytes — traced to the implementer's own pre/post DIRECTORY
      NAMES, not to the change; a normalised comparison and a same-directory determinism
      run both hash equal. Cause measured rather than waved away.
  Cold wall time 108/124/146 ms -> 71/92/101 ms, reported as an OBSERVATION not a gate
  (perf is Wave E).

  **CONTROLLER DISPATCH ERROR, MINE, RECORDED AS SUCH.** My T5 dispatch carried correction
  4: "the canonical invocation includes `--schedule projected_schedule.tsv`". That is true
  for the 135-session parity corpus and I had just measured it there (T4) — but I
  generalized it to T3's 3-session fixture without checking how that fixture was pinned.
  The implementer caught it and cited the generator: `<scratchpad>\run_fixture.ps1:20,:34`
  passes NO `--schedule`, so T3's `-6679.892579` / `-4779.718393` and the `39D47B8B…`
  anchor all come from the DEFAULT invocation. With `--schedule` the fixture yields
  `-3944.275714` / `-2080.502348` and a different 5886-byte dump hashing `791FF53E…`.
  **Controller verified `run_fixture.ps1` directly — the implementer is right.** Corrections
  2 and 4 were mutually exclusive on that fixture and I did not notice. The implementer's
  remedy was better than either: it gated BOTH invocations pre/post, so the result is
  invocation-independent. Task 7 is unaffected — `parity_full_run.ps1` correctly passes
  `--schedule` for the 135-session corpus, which is where my T4 finding actually applies.
  Lesson for the remaining tasks: an invocation is a property of the FIXTURE, not of the
  subcommand; cite the generator that pinned the hash, not a different corpus's script.

  **ANTI-VACUITY, VOLUNTEERED BY THE IMPLEMENTER — the right outcome, recorded as a win.**
  It reports that the `MD-3S-COLD` gate is near-vacuous and explains why: a zero-row
  `mark_divergence` dump is the header and nothing else, so it is satisfied by ANY build
  that emits the column names in order — including one with no observer installed at all.
  It confirmed the controller's untested hypothesis in the process: the fixture's raw cold
  hash is `C9A04D1BCF0E3C07…`, byte-identical to the 135-session cold hash, because a
  header-only dump is corpus-independent. Conclusion accepted: **the cold route's
  load-bearing artifact is `projected_cold`, not `mark_divergence`.** Both gates pass;
  only one carries information, and the report says so instead of counting two.
  Its Vacuity Ledger labels gates 3 and 8 as weak rather than tallying them — which is
  exactly the discipline Wave B lacked.

  CARRIED, not actioned:
  - `archive_load` now reads `0/0` in every `run_projected_backtest` diagnostics section.
    Intended and documented in-code (the loads belong to `priced_run` now), but it IS a
    value-column change an operator will see. No in-tree reader asserts nonzero; the
    Python side could not be checked (out of scope this sprint). **For the final review.**
  - `--no-divergence` now skips only 0.398 ms (cold) / 0.046 ms (configured) of callback
    rather than a whole second pass, so its documented "bare-backtest wall-time path"
    framing is now a rounding error. **Wave E decision.**
  - `SPRINT-CONSTRAINTS.md`'s suite baseline was stale (2020/1974); controller corrected it
    to 2034/1988 in place.

### Wave D T6 (L12 — RunSpec.index_symbol, de-SPY all_symbols / universe_at) — impl + review

T6: **Spec ✅ / Code quality APPROVED. Zero Critical, ONE Important (documentation and
  Task-7 scoping — NO source change required), 5 Minors.** Review: `task-6-review.md`.

  **THE REVIEWER DISPROVED THE IMPLEMENTER'S COVERAGE CLAIM, THEN CLOSED THE GAP ITSELF.**
  The report claimed the two library sites are "exercised by 19 passing
  `ListedDispersionPipeline.*` tests". That is **false**: no test calls
  `build_listed_dispersion_schedule` at all. The only reference is
  `listed_dispersion_pipeline_test.cpp:622` taking its ADDRESS
  (`BuildScheduleSymbolIsDeclared`), and the file's own comment at `:613-614` says so. Its
  sole tree-wide caller is `spy_dispersion_backtest.cpp:452` (`build-schedule`) — another
  command the fixture cannot run. **So all SIX threaded sites are compile-covered only,
  not four.** The gap was twice as wide as disclosed.

  Rather than merely reporting that, the reviewer built its **own differential
  executable** — the pre-change (`d955e93`) function bodies linked against the shipped
  `build-rel\lib\atx-vol.lib`, under the real `/WX` flags — and ran
  **20,000 row-sets / 76,630 `universe_at` pairs: ZERO mismatches**, with a negative
  control returning False. It also reconstructed the old bodies to verify the RED:
  `all_symbols` 3≠4, and `universe_at` `"SPY"`≠`"QQQ"` **and** `names.size()` 2≠3 — i.e.
  `UniverseAtHonoursIndexSymbol` genuinely catches BOTH predicted defects, not just the
  mislabelled index.
  **That differential is a stronger claim than any gate the brief specified**: it proves
  the defaulted functions are behaviourally identical to the hardcoded ones over 20k
  randomized inputs, not merely on the one default path a byte golden walks.

  Reviewer's own evidence: suite 2044/1998/43/3/7 (3 documented reds); `DispersionWorkflow.*`
  10/10; `RunArchive*:RunDir.*:ListedDispersion*:Tearsheet*` 82/82 including
  `MatchesCommittedPythonFixture` and `SchemaHashStableAndNonzero`
  (`0xdcce47781ac8390d`, `kRaMinor 0`); on its own fresh COPY of the fixture, configured
  `mark_divergence` == T5's pre-T6 pin `39D47B8B…A91F`, determinism agreed across two
  independent runs, `run_spec.tsv` `2AF1B37C…F75B` unchanged (`Compare-Object` 0 rows),
  `meta` 25 lines with `index_symbol\tSPY` sitting between `core_mode` and
  `projected_execution`.

  All four implementer concerns UPHELD. Concern 1 upheld **but understating the gap**
  (six sites, not four) — with the reviewer's judgement that the evidence base is
  nonetheless adequate, because the 20k differential proves function identity and every
  site threads `"SPY"` today, making **Task 7 a LIVENESS gate rather than an economics
  gate.** Accepted. Concern 2 upheld: the root `CMakeLists.txt` never adds
  `atx-vol/python`, there are zero PYTHON/PYBIND cache entries and no `_core*.pyd`, so the
  controller's full-build justification was simply wrong; the reviewer recompiled both
  binding expressions itself under the real `/WX` flags, exit 0. Concern 3 upheld: it
  re-hashed all 13 captured artifacts, 13/13 match, and confirmed the PRE captures are
  provably pre-change (24-line `meta` vs 25). Concern 4 upheld:
  `RunDir::verify` (`run_archive.cpp:1629-1662`) counts `backtest`/`reconciliation` only,
  `_schema.py:27-30` registers `meta` as 2 columns, and no positional Python `meta`
  consumer exists.

  T6 review Minors, all carried: the meta-side append ORDER is asserted only by comment,
  not by a test; `ReadRunSpecRejectsEmptyIndexSymbol` does not assert the message;
  `write_resolved_spec` can emit an empty value its own parser would reject (unreachable
  in-tree); stale "SPY" prose in the example's error message and methodology map; the
  Python docstring still says "SPY is the index leg".

#### Controller action on the Important — all six sites are now EXECUTED at T7

The brief's T7 Step 3 sequence reaches only three sites. Two additions were made, and
the site map was verified from the code rather than from the brief (which mislabels
`:320` as belonging to `build-schedule` — it is in **`build_corpus_command`**, and the
controller repeated that error in the T6 dispatch before the reviewer corrected it):

| site | carrying command | in brief's T7? |
|---|---|---|
| `spy_dispersion_backtest.cpp:320` `all_symbols` | **`build-corpus`** | no — ADDED |
| `:541` `all_symbols` | `run-backtest` | yes |
| `:915` `universe_at` | **`run-surface-backtest`** | no — ADDED |
| `:992` `universe_at` | `run-projected-var` | yes |
| `listed_dispersion_pipeline.cpp:205` `all_symbols` | `build-schedule` (sole caller `:452`) | yes |
| `:224` `universe_at` | `build-schedule` | yes |

`build-corpus` is run as a **one-day** corpus over the real 11-symbol universe into a
scratch dir (`t7-corpus1d`) — OPRA day files are 50-200 KB, so it is cheap — which also
executes `write_resolved_spec`'s new append, the other new L12 write path. It is NOT run
against any existing corpus, because `build-corpus` is the one command that rewrites
`run_spec.tsv` and would move `run_identity_hash` for that dir.

Independently of execution, the controller **read all six call sites** and confirmed each
threads the correct field. One is subtle and worth recording: the two library sites must
use `quote_source.index_symbol`, not `spec.index_symbol`, because the `spec` in scope
there is a `ListedScheduleSpec` — a different type with no such field. Passing the wrong
object would not compile, which is why that pair is safe by construction.

### Wave D T6 — implementation detail

T6: implementer DONE_WITH_CONCERNS, commit `0a895b8`, 8 files, +270/-15 = net **+255**
  (production code alone +57/-15; the new `dispersion_workflow_test.cpp` is +209 of it).
  A net ADD is correct here and is not a regression against T5's -193: L12 removes three
  string literals and adds a field, two defaulted parameters, a parse arm, a guard, two
  appends and — the bulk of it — **the first direct test file `dispersion_workflow` has
  ever had.** The module previously had zero direct coverage.
  Suite **2044/1998/43/3/7** = +10/+10 over the 2034/1988 baseline, same three documented
  reds. `DispersionWorkflow.*` 10/10; `RunArchive*:RunDir.*:ListedDispersion*:Tearsheet*`
  82/82 including `MatchesCommittedPythonFixture`.

  GATES: `projected_cold` and `mark_divergence` byte-identical on BOTH routes, with the
  configured `mark_divergence` reproducing T5's raw-LF pin `39D47B8B…A91F` exactly;
  `meta` gains **exactly one** row `index_symbol\tSPY` (24 -> 25, `Compare-Object` count
  1, nothing else moved); `run_spec.tsv` unchanged (`2AF1B37C…`) in a dir whose
  `build-corpus` was not re-run — which is what keeps `run_identity_hash` stable for every
  existing corpus and is the difference between a no-op and silently invalidating the
  parity corpus; `ra_schema_hash` and `kRaMinor` untouched.
  `UniverseAtHonoursIndexSymbol` was a **genuine observed RED** catching both predicted
  defects (the old hardcode mislabels the index AND silently drops SPY from `names`).
  The Vacuity Ledger declares four gates weak/near-vacuous rather than counting them.
  No Python edited; the `.def_readwrite` omission recorded as a decision.

  **CONCERN 1 — THE REAL FINDING, AND IT IS A COVERAGE GAP, NOT A DEFECT.** The brief's
  Step 4 byte gate is unrunnable and its numbers are stale — independently confirmed by
  the controller before dispatch and by the implementer during it. The surviving 3-session
  fixture is 3 sessions / **2 rolls** with navs `-6679.892579` / `-4779.718393`, not the
  brief's `rolls=1, -456.5769067` (that fixture was cleaned with a prior sprint's
  scratchpad); and it carries no `definitions.tsv` and no `universe_schedule.tsv`, so
  `build-schedule`, `run-backtest`, `run-surface-backtest` and `run-projected-var` — **the
  four commands carrying all four example-side call sites** — cannot run on it at all.
  **Those four sites are therefore COMPILE-COVERED ONLY, never executed by this task.**
  Behavioural coverage comes from the two LIBRARY sites
  (`listed_dispersion_pipeline.cpp:205,:224`, exercised by 19 passing
  `ListedDispersionPipeline.*`) plus the 10 new unit tests.
  Stated plainly by the implementer rather than left for a reviewer to find, which is the
  behaviour this process exists to produce.
  **CONTROLLER ACTION: Task 7's 135-session run is therefore load-bearing for L12 itself,
  not merely for its own gate.** The brief's T7 Step 3 sequence reaches only three of the
  four sites (`:320` build-schedule, `:541` run-backtest, `:992` run-projected-var), so
  **`run-surface-backtest` has been ADDED to the T7 gate** specifically to execute the
  fourth (`:915`). Recorded here so the addition is a decision, not an undocumented
  deviation from the brief.

  **CONCERN 2 — THE CONTROLLER'S DISPATCH WAS WRONG AGAIN, AND THE IMPLEMENTER'S
  SUBSTITUTE IS BETTER THAN WHAT I ASKED FOR.** My dispatch (and the brief's Step 4)
  justified the mandatory full build as proving the pybind11 bindings still compile
  against the defaulted signature. They do not: `atx-vol/python/` is a **standalone
  scikit-build-core project** and is not in `build-rel` at all (no `PYTHON`/`PYBIND` cache
  entries), so a full `build-rel` build cannot compile a single binding TU and never
  could. The implementer proved the claim properly instead — it compiled a probe TU
  containing the two `dispersion.cpp` call expressions **verbatim**, with the real
  `compile_commands.json` flags, under `/WX`: EXIT=0. That is direct evidence where the
  full build was a non-sequitur. **This is my second dispatch error in two tasks; both
  were caught by implementers and both are recorded as mine.**

  **CONCERN 3 — A FOURTH FALSE-GATE METHODOLOGY DEFECT, SELF-CAUGHT MID-RUN.** The
  implementer's first comparison script defined `function H`, which resolved to the
  built-in `Get-History` alias. All six hashes came back `$null`, so every verdict was a
  vacuous `$null -eq $null` -> `True`: **six gates "passed" having compared nothing.** It
  caught this itself, rewrote with `Get-Sha` / `Get-Lines` helpers that `throw` on a
  missing or empty file, re-ran everything, and added an explicit negative control showing
  the comparator can return `False`. All reported numbers come from the corrected run.
  **This is the fourth distinct way a byte gate has silently lied in this sprint**, after
  Bash `diff`'s exit code, PowerShell `>` BOM/CRLF re-encoding, and unrecorded capture
  convention. All four are now enumerated in `SPRINT-CONSTRAINTS.md` under one rule:
  **a comparison that cannot be shown to fail has not been run — print both values and
  prove the comparator can say False.**

  CONCERN 4, carried: `meta` now has 24 rows where a positional Python reader might assume
  23. Nothing in C++ counts `meta` rows (`RunDir::verify` compares `backtest` vs
  `reconciliation` cardinalities only, verified in code) and `MatchesCommittedPythonFixture`
  is green, but pytest is out of scope this sprint. **For the final review.**

### Wave D T7 — CONTROLLER INTEGRATION GATE

**Step 1 — full build.** `cmake --build C:\atx\build-rel` (all 95 targets: library,
`atx-vol-tests`, all examples, both benches, every `atx-engine-*` test binary,
`atx-impl`). **EXIT 0.** Required rather than a `--target` build because `sizeof(RunConfig)`
changed in T1 and a public `dispersion_workflow` signature changed in T6.
*(Recorded correction: the brief and the T6 dispatch also justified this build as proving
the pybind11 bindings still compile. It does not — `atx-vol/python` is a standalone
scikit-build-core project the root `CMakeLists.txt` never adds. That claim was retired at
T6 and replaced with a probe-TU compile under the real `/WX` flags.)*

**Step 2 — full gtest from `C:\atx\build-rel` CWD. PASS.**
    **2044 ran / 1998 passed / 43 skipped / 3 failed / 7 disabled**  (149.0 s)
  The 3 failures are EXACTLY the documented pre-existing reds, by name:
  `BoundaryHoist.PriceBitIdenticalToPrechange`,
  `AllQualityModes/SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/Latency`
  and `…/Balanced`. **No fourth failure, so the wave is not blocked.**
  Wave-D delta over the pre-wave baseline: 2020 -> 2044 ran (+24), 1974 -> 1998 passed
  (+24), skipped/failed/disabled unchanged. Every added test passes.
  *(First capture of this run was lost to a `| tail -22` that truncated the summary above
  the count lines — the SKIPPED list is 43 entries. Re-run with full capture rather than
  reporting a reviewer's numbers as my own.)*

**Step 3 — 135-session parity economics. ALL GREEN.** Run as a clean sequential chain on
the COPY `t7-gate` (742 MB, includes `definitions.tsv`, unlike T4's metadata-only copies).
`parity-full`'s `run.atxrun` hashed `D88BFEE04D3EF300` before and after every command in
this gate.

    build-corpus (1-day)  EXIT 0   admitted=11 quarantined=0 source_failed=0
    build-schedule        EXIT 0   rolls=7
    run-backtest          EXIT 0   dates=135 rolls=7 final_nav=125026.0592   EXACT
    project-schedule      EXIT 0   rolls=7
    run-projected-backtest --schedule projected_schedule.tsv --execution cold
                          EXIT 0   dates=135 rolls=7 final_nav=123243.1172   EXACT
                                   `mark divergence rows: 0`
    run-projected-var     EXIT 0   scenarios=135 positions=22
    run-surface-backtest  EXIT 0   dates=135 final_nav=128361.5746

    dump backtest --tsv           a05470c7a6f6572f   expected a05470c7   MATCH
    dump projected_cold --tsv     cbabca44e411d4d9   expected cbabca44   MATCH
    trade_schedule.tsv            b640b3aba5f3f6d5   expected b640b3ab   MATCH
    projected_schedule.tsv        d6793d46a1f29606   expected d6793d46   MATCH
    dump mark_divergence --tsv    c9a04d1bcf0e3c07   == MD-COLD from T4   MATCH
    projected_risk_scenarios.tsv  0cf8ac4b50f34ea6   MATCH
    projected_risk_legs.tsv       0a8b38984c7b6064   MATCH
    projected_var.tsv (field 7 excluded)  d370c78dbb01b513   MATCH
    NEGATIVE CONTROL: backtest vs the projected_cold pin -> *** DIFFERS ***  (the
      comparator can say False, so the eight MATCHes above are not vacuous)

  All six pinned VaR values reproduced to the last digit: `reference_value
  280232.52872350701`; 95% VaR `164113.53597877346` / ES `169286.48040274251`;
  99% VaR `172540.63396786354` / ES `174814.16710811283`; `n_scenarios 135`,
  `n_positions 22`.

  Merged archive holds **exactly 9 sections** — backtest, projected_cold,
  mark_divergence, reconciliation, contract_marks, meta, diagnostics, trade_schedule,
  projected_schedule — so merge-write still unions rather than drops. `projected_nodiv`
  correctly absent (no `--no-divergence` run in this chain); `surface_backtest` correctly
  absent (that command writes a loose TSV, not a section). No `projected_backtest.tsv` and
  no `mark_divergence.tsv` — the Wave A hard cutover still holds.

  **`corr=0.99718` REPRODUCED WITHOUT PYTHON.** The brief sources this from the parity
  report, which is Python and therefore out of scope this sprint. Rather than skip the
  assertion, the controller computed the Pearson correlation of daily `pnl_total` directly
  from the `backtest` and `projected_cold` sections (135 points each): **0.99718**. Same
  economic claim, no out-of-scope dependency.

**Step 4 — configured-route equivalence, POST-DELETION. THE GATE THAT MATTERS.**
  On a fresh metadata copy, with the T5/T6 build (shadow deleted, observer sole source):
    `mark divergence rows: 137`,  `final_nav=132776.9818`
    `dump mark_divergence --tsv` = **9e958a90ae15ac74 == MD-CFG recorded at T4**, N = 137.
  T4 measured that hash with the shadow still present and both sources compared; T7
  reproduces it byte-for-byte with the shadow gone. **The nonzero divergence channel
  survived the deletion exactly** — which is the one thing the empty cold section could
  never demonstrate.

**ALL SIX L12 CALL SITES EXECUTED — the T6 review's Important is now closed.**
The T6 reviewer established that every threaded site was compile-covered only. The brief's
Step 3 sequence reaches three of six. Two commands were added and the site map rebuilt
from the code (the brief mislabels `:320` as `build-schedule`; it is in
`build_corpus_command`):

    :320 all_symbols          build-corpus            ADDED   executed, exit 0
    :541 all_symbols          run-backtest            in brief, executed
    :915 universe_at          run-surface-backtest    ADDED   executed, exit 0
    :992 universe_at          run-projected-var       in brief, executed
    listed_dispersion_pipeline.cpp:205 / :224         build-schedule, executed

`build-corpus` was run as a ONE-DAY corpus over the real 11-symbol universe into a scratch
dir, never against an existing corpus — it is the one command that rewrites `run_spec.tsv`
and would move `run_identity_hash` for that dir. It also executed the other new L12 write
path and confirmed the brief's binding ordering requirement by EXECUTION rather than by
unit test: the resolved `run_spec.tsv` ends
`delta_band / fit_workers / core_mode / index_symbol\tSPY` — `index_symbol` last.

#### Two gate failures, both diagnosed to the corpus copy, neither a regression

1. **`build-schedule` first failed** `InvalidArgument: OCC ESS inventory path escapes run
   envelope`. Not a defect: `occ_ess_inventory.tsv` records ABSOLUTE paths and
   `spy_dispersion_backtest.cpp:276-278` requires each to equal
   `run_dir/occ_ess/<date>.txt`. A copied corpus therefore fails **by design** — that check
   exists precisely to stop an inventory referencing files outside its run dir. Relocating
   the 135 path entries to the copy's own `occ_ess/` preserves exactly the property the
   check enforces (the fingerprint and `n_special` comparisons still validate content), and
   the re-run was EXIT 0, `rolls=7`.
   **This mattered: before the fix, `trade_schedule.tsv` matched `b640b3ab` only because it
   was the un-regenerated copied file.** That would have been a copy-integrity check
   masquerading as a regeneration gate. After the fix the file is genuinely rewritten
   (mtime moves) and still hashes `b640b3aba5f3f6d5` — and the two library call sites are
   executed on the way. Recorded because banking the first hash would have been exactly the
   kind of false green this sprint keeps finding.
2. **`verify` fails** `NotFound: read_quality_report_file: file not found`. **PRE-EXISTING
   and proven so**: `verify` reads `run_dir/quality.tsv` (`:477`), `parity-full` has never
   contained one, and running `verify` read-only against the UNTOUCHED source reproduces
   the identical error. `quality.tsv` is written only by `build-corpus`, which this corpus
   predates. The one-day corpus does have `quality.tsv` but no `occ_ess_inventory.tsv`, so
   no available corpus satisfies both preconditions. **Initially recorded as a gate that
   could not be run, NOT as a pass.**
   **SUBSEQUENTLY RESOLVED.** The Wave E shared fixture (built later in this session with
   `occ_ess_root` set, so `build-corpus` writes BOTH `quality.tsv` and
   `occ_ess_inventory.tsv`) satisfies both preconditions, and `verify` runs green against
   the T5/T6 build:
       `verified artifact envelope: dates=49 admitted=539 rolls=3`   EXIT 0
   So `verify_command` — including `verify_occ_ess_evidence` and the RunDir cardinality
   gate — IS executed post-deletion, just not on the parity corpus, which structurally
   cannot host it. **Gate satisfied, on a different corpus, and the substitution is stated
   rather than hidden.**

   **LATENT TOOL INCONSISTENCY FOUND WHILE DIAGNOSING THIS — worth a future task.**
   `build_corpus_command` writes `occ_ess_inventory.tsv` only
   `if (!spec.occ_ess_root.empty())` (`:336`), but `build_schedule_command` calls
   `verify_occ_ess_evidence` **unconditionally** (`:424`, and `verify` again at `:479`).
   So a corpus legitimately built without an OCC ESS root can NEVER run `build-schedule`
   — it fails `NotFound: cannot open …occ_ess_inventory.tsv` with no way forward short of
   rebuilding the corpus. Either the write should be unconditional or the verify should be
   conditional. Out of Wave D's charter; recorded for the final review.

#### Wave E shared fixture — built as controller prep, pinned here

Every Wave E task must report before/after numbers that are comparable, and per-task
fixture invention is exactly what produced the stale-pin confusion in T3, T5 and T6 (three
separate briefs pinning a fixture that no longer exists). So Wave E gets **one** fixture,
built once, owned by the controller:
`<scratchpad>/wave-e/run` — 2026-01-02..2026-03-13, real 11-symbol universe,
`admitted=539 quarantined=0 source_failed=242` (non-trading days), **49 qualified sessions
/ 3 rolls**, 800 MB (696 MB of it `definitions.tsv`, which is what makes it a fair P1/P3
parse-cost fixture). Build took 38 s.
Validated end to end against the T5/T6 build before being handed out:
    build-schedule           EXIT 0  rolls=3         14.5 s
    run-backtest             EXIT 0  dates=49 rolls=3 final_nav=22635.66476   21.8 s
    project-schedule         EXIT 0  rolls=3
    run-projected-backtest   EXIT 0  dates=49 rolls=3 final_nav=18528.61666
                                     `mark divergence rows: 0`
    verify                   EXIT 0  dates=49 admitted=539 rolls=3
Input pin (any task mutating these invalidates its own numbers):
    run_spec.tsv           38c2826ca8194e30      universe_schedule.tsv  422d9a4c33987fd6
    surface_manifest.tsv   de00c7b137dc4947      occ_ess_inventory.tsv  a570006591741cbb
    quality.tsv            26c3f1cc31cb8013      methodology_map.tsv    5fe90962fe60c5f8
    input_inventory.tsv    71c85fd621281314      trade_schedule.tsv     4712bfd422285b6a
    projected_schedule.tsv 99cf42402326109c      archives 51 / occ_ess 49
Sized deliberately: big enough that `run-backtest` at 21.8 s and `build-schedule` at 14.5 s
are measurable above noise, small enough to iterate on.

### Wave D — WHOLE-BRANCH REVIEW (T7 Step 5), fresh reviewer, read-only

**Wave D spec compliance ✅ / quality APPROVED with findings. 0 Critical, 2 Important,
6 Minor.** Scope `cb875dc..HEAD`, `atx-vol/` only (Wave C is separately closed; the
sprint-wide A-E review comes at sprint close). Review file: `WAVE-D-whole-review.md`.
Run strictly read-only — no build, no binary — deliberately, so it could overlap Wave E
T1's measurement window without perturbing it.

Charter answers, all with `file:line` evidence:

**(a) Observer fires at a point definitionally equivalent to the shadow's read — YES at
both sites.** `backtest.cpp:1868-1880` and `:2011-2023` have only the `if (!st) return Err`
check between `on_step` and the observer. **One honest non-identity named:** the observer's
snapshot comes from `snapshot_cache->load(path, tier, build_policy)` while the shadow used
`MarketSnapshot::load(path, tier)` (`backtest.cpp:1141`). Content-equivalence there is
closed **empirically** by T4/T5, **not definitionally** — which is exactly how T2's review
framed it before T4 ran, and it is the right way to state it.

**(b) No downstream consumer assumes the shadow's row set** — and the finding is sharper
than "no". The encoder is unchanged and positional by construction; C++ `verify` never
mentions `mark_divergence`; Python `_divergence_rows` (`parity.py:258`) reads **by column
name**; and `_add_divergence` (`parity.py:606-610`) **sorts by bps and takes the top 12**.
So the pinned positional row order has **no in-tree consumer at all** — it is enforced
solely by the out-of-process section sha256. Worth knowing: the ordering guarantee T3's
comparator was built to protect is real but is currently load-bearing only for the byte
gates, not for any reader.

**(c) 17 of 18 new tests genuinely falsifiable**, each named with its oracle. The single
green-lock is `DispersionWorkflow.RunSpecIndexSymbolDefaultsToSpy`
(`dispersion_workflow_test.cpp:72`), which restates `dispersion_workflow.hpp:49` and
cannot fail. Zero-row vacuity is otherwise handled correctly, notably by the paired
nonzero control in `MarkDivergenceObserverIsSilentWhenNothingDiverged`.

**(d) One dead field found.** `StepEvent::ref` / `snapshot` / `strategy` are read at
`listed_dispersion_pipeline.cpp:516` / `:499` / `:470`. **`step_index` has NO production
reader** — tests only — and `strategy_test.cpp:1649-1651` asserts a correlation no in-tree
code performs. `RunSpec::index_symbol` is read by eight C++ sites but is invisible to
Python (see Important-2).

**(e) The L12 default reproduces the old outputs element-for-element, argued from source**:
the seed `std::string(string_view{"SPY"})` is byte-identical; the dedup loop and
`std::sort` are untouched and operate on a unique-element set (a total order gives unique
output); `universe_at`'s `std::map` iteration and `Err(Unavailable)` are unchanged; and the
SPY-as-constituent case dedups to one occurrence in `all_symbols` and is filtered out of
`names` exactly as before.

**(f) `record_every_n > 1` is consistent and deliberate.** The observer fires at
`backtest.cpp:1878`/`:2021`, forty lines before the `record` predicate at `:2064`, so
events == `clock.size()` while rows are downsampled. The section is stride-invariant
(`row.date` comes from the clock, not the recorded series), nothing joins the two counts,
and `record_every_n` is never set on this path anyway.

#### IMPORTANT-1 — the evidence-channel contract WAS weakened, and both T5's implementer and T5's reviewer missed it

`spy_dispersion_backtest.cpp:823-837`. The retained `all_rolls_consumed()` gate **cannot
distinguish "the observer ran and found zero divergences" from "the observer was never
installed"** — but the comment T5 carried onto it claims exactly that distinction. The
shadow's original gate COULD make it, because it interrogated the same object its own
collection loop had just read; the priced run's gate interrogates the strategy, which
knows nothing about whether an observer was attached.

This is the single highest-value item in the wave and it is worth being blunt about the
process: the controller's T5 dispatch named this as "the single highest-value check in the
review", T5's reviewer examined it and accepted the carry, and the overclaim still
survived to a third pair of eyes. A comment that asserts a guarantee the code no longer
provides is worse than no comment, because it stops the next reader from looking.

Latent, not live — T4's and T7's N=137 both prove the observer fires today. **Fix ordered
(Wave D T8): the observer wrapper already counts callbacks into the `divergence_replay`
phase at `:811`; gate that count against `clock.size()` so "never installed" and
"installed and silent" become distinguishable, and make the comment true again.**

#### IMPORTANT-2 — the Python RunSpec binding omits `index_symbol`, undocumented

`python/src/bindings/dispersion.cpp:66-95` does not expose `index_symbol`, while
`read_run_spec` IS bound at `:105-110` and the bound `all_symbols` / `universe_at` take no
index argument. So a run spec carrying a non-default index would give the C++ CLI and any
Python tool **different universes, silently**. Inert today because everything is SPY.
**The distinction from the deliberate `step_observer` omission is the point: that one was
recorded as a decision; this one was not.** Python is out of scope this sprint and the
constraints forbid binding edits, so this is recorded as a NAMED FOLLOW-UP rather than
fixed: expose `index_symbol` on the `RunSpec` binding and thread it through the bound
`all_symbols`/`universe_at`, in the wave that reopens Python.

#### Carried items, adjudicated

- **`meta` gaining a row: CLOSED as a non-issue** (was T6 concern 4). The Python reader is
  key-name matched (`runarchive.py:631-636`) and the test is literally named
  `test_meta_section_matched_by_key_name`. No positional consumer exists.
- The other three carried items (dangling `collect_mark_divergence_replay` provenance
  comments; the stale `--no-divergence` "bare-backtest wall-time path" framing; stale
  `"SPY"` prose in the example's error message and methodology map) confirmed correctly
  characterised. To the Wave E comment sweep.
- **The reviewer noticed an unlisted commit in the range**: `017fc5c`, the pre-Wave-D build
  fix, also changes `universe_autofit`'s `parse_preset("populate")` behaviour. Correct
  catch — that was disclosed in the sprint status doc but not in the Wave D commit list.
  Behavioural change inside a build fix is exactly the kind of thing a range review should
  surface, and it did.

#### Controller decision on the dead field

`StepEvent::step_index` stays. Wave B's lesson was "a field nothing reads is an active
trap", and this one IS read — by tests, and it is a natural member of a public
step-observation API whose whole purpose is to let a caller correlate events with steps.
Recording the decision explicitly, with the fact that no PRODUCTION reader exists, so it is
a decision rather than an oversight. If Wave E or a later wave finds it still unread by any
consumer, delete it then.

## WAVE E

### Wave E T1 (measurement substrate + the seven section goldens) — impl + review

T1: implementer DONE_WITH_CONCERNS, commit `d844f26`, 3 files
  (`examples/spy_dispersion_backtest.cpp`, new `include/atx/vol/run_diagnostics.hpp`,
  new `tests/run_diagnostics_test.cpp`).
  **Review: Spec ✅ / Code quality APPROVED. 0 Critical, 1 Important, 5 Minor.**
  Suite 2047/2001/43/3/7 = baseline + 3 new tests, the 3 reds sanctioned.
  RED observed as a real RUNTIME assertion failure (not a compile error), with an
  anti-vacuity control passing alongside it.

  **THE SEVEN GOLDENS (sha256[0..16), bash capture / `Get-FileHash`, data_rows = lines-1).
  A later Wave E task that cannot reproduce these has failed:**
    backtest            5c5cbc8f936e754b    49
    reconciliation      35f0625d917f9dc5    49
    contract_marks      04162f9a4157e03d  1122
    trade_schedule      8d4f223f3b83b8bf    66
    projected_cold      ef3505e073da2200    49
    projected_schedule  0d84ddc6368b1c5f    66
    mark_divergence     c9a04d1bcf0e3c07     0
  `final_nav` pins: run-backtest **22635.66476**, projected cold **18528.61666**.
  **The reviewer reproduced ALL SEVEN plus both navs from its own fresh fixture copy with
  its OWN independent relocation**, with five negative controls (3 False, 2 throw). It also
  regenerated `trade_schedule.tsv 4712bfd422285b6a` and `projected_schedule.tsv
  99cf42402326109c` and matched controller pins that **predate T1** — stronger economics
  evidence than the report itself claimed.
  Binary provenance was verified rather than assumed: the reviewer proved its snapshotted
  binaries were built from `d844f26` (they contain the new phase names and do NOT contain
  the observer-gate string from the concurrently-dirty worktree) and `Compare-Object`'d
  T1's two edited regions committed-vs-worktree at 0 diff lines.

  **THE MEASUREMENT — this is the deliverable, and it reshaped the wave:**
    `definitions_parse` = **95.2%** of build-schedule, **71.8%** of run-backtest
      (reviewer independently: 95.2-97.1% / 73.4-74.5%). The rest of `setup_read` is
      44.6 ms and 1.4 ms respectively.
    old `reconciliation` decomposes to `snapshot_load` **0.23%** / `quote_join` **81.5%**
      / `reconcile` **17.4%** (reviewer: 0.21-0.22 / 80.1-80.6 / 19.2-19.7).

  **IMPORTANT (IMP-1) — PAGE CACHE, and it is NOT the "indicative timings" point.**
  The reviewer measured `definitions_parse` swinging **24885 -> 11506 ms (2.16x) between
  two CONSECUTIVE runs of the same binary**, purely from OS page-cache state on the 697 MB
  `definitions.tsv` (13013 -> 18388 ms the other way in run-backtest). T1 published its
  decomposed table as the wave's A/B reference with no cache caveat. **Under the one-run
  measurement the controller had just authorized, a P3 result read against that table is
  indistinguishable from cache noise — you could "prove" a 2x win by measuring cold-before
  and warm-after.**
  **CONTROLLER ACTION, taken immediately:** the user's relaxation removed a CPU-contention
  control; page-cache state is an ORTHOGONAL axis and a much larger confound, so it is
  explicitly carved out. `SPRINT-CONSTRAINTS.md` now binds any task measuring
  `definitions_parse` / `quote_join` / `reconcile` to: never A/B against another session's
  table; take before and after yourself back-to-back; **warm both sides with a discarded
  run and say so**; report the discarded cold number; and if cold-vs-warm exceeds ~1.5x the
  claimed win, declare the claim unsupported by that measurement. One extra run per side,
  removes a 2.16x systematic error.

  **THE PARTITION ARGUMENT WAS INVERTED — conclusion right, reasoning wrong.** T1 offered
  its within-run coverage residual (whole-command total minus phase sum, 0.11-0.22% and
  0.02-0.04%, positive every rep) as proof that the split is disjoint. The reviewer showed
  a residual is a **NET** measure and is therefore **blind to a double-count cancelled by
  an equal gap**, so it cannot prove disjointness and the commit message's "none is counted
  twice" is not what it demonstrates (Minor M1). **The partition is nonetheless CONFIRMED**,
  structurally: the reviewer walked the committed blob line by line and established the
  brackets are sequential and lexically non-overlapping, that the first new bracket opens at
  the identical statement the old one did and the last closes at the identical one, and that
  the only uncharged intervals are the `add()` calls themselves. Its own residuals were
  +20.6/+17.5 ms (build-schedule) and +9.2/+7.9 ms (run-backtest), positive every rep, with
  the phase-sum identity exact to 3.6e-12.
  Worth keeping as a general lesson: **a net-zero check cannot prove a pairwise property.**

  CONCERNS, all adjudicated by the reviewer:
  1. **Fixture not relocatable — CONFIRMED empirically with a matched control** (`verify`
     EXIT=1 "path escapes run envelope" with the pristine inventory on a copy, EXIT=0 after
     relocation). **Neutrality claim CORRECT**: `row[1]` is read at exactly one site (the
     guard at `:277`); `:280` reads from `expected`, not from the row. The reviewer's
     independent rewrite produced byte-identical non-path columns (`9f04736f835668ca` both
     sides) and all seven hashes still reproduced.
     **CONTROLLER ACTION: shipped `<scratchpad>/wave-e/relocate-fixture.sh`**, which copies
     the pristine fixture, rewrites the one column via PowerShell `String.Replace` (not
     regex — the paths contain backslashes), **fails loudly if it replaces nothing**, and
     deletes `run.atxrun` so merge-write cannot carry a stale section into a comparison.
     Every remaining Wave E task uses it instead of reinventing the rewrite.
  2. `parity.py:729` — CONFIRMED, and **less severe than reported**: no in-tree caller
     supplies `listed_diagnostics_tsv` at all, and the note it drops was **already false**.
     "Silently drops" is the right characterisation. Correct not to escalate.
  3. Third file (`run_diagnostics.hpp`) — **JUSTIFIED, not scope creep.**
     `CMakeLists.txt:321-322` proves `atx-vol-tests` cannot see the example binary, so the
     brief's Step 3 was unimplementable as written.
  4. Indicative timings — expected and not raised, per the relaxed protocol.
  5. Stale brief pins and line numbers — substitutions correct, both navs reproduced.

#### Controller decisions on T1's measurement

**1. WAVE E T2 (P5) IS DROPPED — ALREADY DELIVERED BY WAVE D T5, not skipped.**
P5 was chartered to route the mark-divergence replay's duplicate per-session
`MarketSnapshot::load` through the shared `SnapshotCache`. **That replay no longer exists**
— `collect_mark_divergence_replay` was deleted by Wave D T5, and with it the entire second
pass over the clock. Verified: the symbol greps to zero tree-wide.
**Evidence corrected by the reviewer, and the correction matters:** the decisive fact is
that `run-projected-backtest`'s `archive_load` phase now measures **exactly 0 ms / count 0**
— i.e. the subcommand P5 targeted performs no archive loads at all. The controller had
first cited `snapshot_load` at 0.23%, which is a **different subcommand** (`run_backtest`)
and is corroborating, not decisive. Recorded because citing the wrong evidence for a right
conclusion is exactly the habit this sprint keeps catching.

**2. P3 IS REORDERED AHEAD OF P2.** Reviewer agrees "emphatically". P3's target is
71.8-95.2% of the two subcommands; P2's `quote_join` is 81.5% of a phase that is a much
smaller share of run-backtest and **0% of build-schedule**. Ordering P3 first also feeds
Task 7's P1 GO/NO-GO decision sooner, which the plan already gates on P3's measurement.
New order: **T4+T5 (P3) -> T3 (P2) -> T6 -> T7+T8 (P1) -> T9 (gate)**.

**3. Python follow-up recorded** (out of scope this sprint): the parity report's
"reconciliation dominates" note is now **measurably wrong** — definitions parsing dominates
both subcommands by a wide margin — and its lookup is dead code besides.

### STOP POINT — 2026-07-25 (second), user instruction "stop here"

HEAD `79b2fa6`. Wave D CLOSED (T1-T8, gate PASS, whole-branch review Approved).
Wave E: T1 closed and reviewed; T2 (P5) DROPPED as already-delivered by Wave D T5;
T4 (P3a+P3c) COMMITTED BUT UNREVIEWED.

**`79b2fa6` is the only commit on this branch no reviewer has seen.** The implementer
committed the code and was stopped while writing its report, so
`2026-07-24-atx-vol-backtest-framework-wave-e/task-4-report.md` does NOT exist and the
measurements survive only in the commit message. Same failure shape as the earlier T3
stop, and the same remedy applies: **review `0622d52..79b2fa6` before anything else**, and
re-run the byte gate (all seven T1 section hashes + both `final_nav` pins) and the full
suite, because neither is on the record for this commit.

T4's numbers, from the commit message (shared box, both sides warmed with a discarded run,
median of 3):
    definitions_parse build-schedule  20317 -> 17509 (a) -> 13699 ms
    definitions_parse run-backtest    20560 -> 16892 (a) -> 13776 ms
    peak working set  2797.5 -> 1806.4 MB  (-991 MB, -35.4%, moves only with (c))
~33% off the dominant phase; the win is attributed to (a) and (c) separately as required.
Verified at the stop point: `ListedOpra.*:StandardMonthlyClassifier.*` **24/24 PASS**.

Two design notes from T4's commit message worth carrying into its review: `std::once_flag`
was deliberately NOT used (neither copyable nor movable — a member of that type would
delete the class's copy AND move constructors, and every read path moves the table out of a
`Result`), and `fingerprint()` drops `noexcept` because its first call now allocates and can
throw. Also flagged there: a DEFAULT-CONSTRUCTED table now reports a different fingerprint —
the reviewer should decide whether that is reachable in-tree.

**CONTROLLER ERROR #4, MINE — live, and caught before it corrupted a result.** The shipped
`relocate-fixture.sh` hard-coded an all-backslash `PRISTINE` constant while `build-corpus`
records the inventory path MIXED (`C:/.../run` + `\occ_ess\DATE.txt`). `String.Replace`
matched nothing, the script aborted before deleting `run.atxrun`, and a "relocated" copy
could keep a stale archive — which `write_run_archive` MERGES, so a carried-forward section
would have silently contaminated byte gates. Found by Wave D T8's reviewer. Fixed to derive
the prefix from the file itself, delete `run.atxrun` first and unconditionally, report
`REPLACED=n`, and fail loudly on zero replacements. Self-tested: `REPLACED=49`,
`build-schedule` EXIT 0 on the copy, pristine fixture's six pinned inputs all MATCH.
Note the helper deletes `run.atxrun` by design, so `verify --run <COPY>` fails until the
pipeline has run — expected, not a relocation failure.

This is the **sixth** distinct way a byte gate has silently lied in this sprint, and the
first authored by the controller rather than found in the tooling.

Working tree clean for every sprint source file; the pre-existing unrelated uncommitted work
(Python bindings split, `atx-core` sqlite, `atx-db/`, `atx-kb/`, surface-db docs) was never
staged at any point.

Status doc: `docs/superpowers/2026-07-25-atx-vol-backtest-sprint-status-2.md`

---

## RESUMED — 2026-07-25, user instruction "finish implementing this sprint end to end"

Resume point taken verbatim from the STOP POINT above. Order of work:
T4 review (the unreviewed commit) -> T5 (P3b) -> T3 (P2) -> T6 -> T7/T8 (P1) -> T9 gate -> final A-E review.

**Controller sequencing decision.** T6 touches `run_archive.{hpp,cpp}` + `run_archive_test.cpp`;
T4/T5 touch `listed_opra.*`. No file overlap, so T6 runs in PARALLEL with T4's review. T5 does
NOT, because it rewrites the same function region T4 changed and a T4 fix round would collide.
Build-dir contention resolved the established way: the reviewer gets SNAPSHOTTED binaries
(`scratchpad/wave-e/snap-79b2fa6/`, both exes + all DLLs, verified 16:37 == T4 source mtime)
and is forbidden to build; T6 owns `C:\atx\build-rel`. ONE build at a time still holds.

`task-4-report.md` did not exist, so it was seeded with `git log -1 --format=%B 79b2fa6`
(63 lines). The reviewer is told plainly that this is the commit message and NOT a report,
that every claim in it is unverified, and that absent evidence is a finding rather than a pass.

### Wave E T4 (P3a+P3c) — review of the orphaned commit `79b2fa6`

**Review: Spec ❌ (evidence-only) / Code quality APPROVED. 0 Critical, 1 Important, 5 Minor.**
No code change requested. Review file: `…wave-e/task-4-review.md`.

**BYTE GATE PASS** — all seven sections match, `trade_schedule` computed `8d4f223f3b83b8bf`
(the hash the brief singled out, since it depends on `definitions.find` through the whole
selection path), 66 rows; `projected_schedule` 26748 bytes. `run-backtest final_nav=22635.66476`,
`run-projected-backtest [cold] final_nav=18528.61666`, `mark divergence rows: 0`,
`verify dates=49 admitted=539 rolls=3`, all rolls=3. Captured bash `>`, hashed `Get-FileHash`.
**Negative control: one byte of `trade_schedule.tsv` flipped -> `7aad139587f9b4d9`, match=False
(`COMPARATOR_CAN_FAIL = True`).** Suite **2053 ran / 2007 passed / 43 skipped / 3 failed / 7
disabled** — the 3 are exactly the sanctioned pre-existing REDs, confirmed from the complete
FAILED block rather than a tail. Targeted `ListedOpra.*:StandardMonthlyClassifier.*` 24/24.
Suite grew 2047 -> 2053 = T4's six new tests. Provenance established, not assumed:
`--gtest_list_tests` on the snapshot shows all six new names, `ListedOpra` = 17 (11 + 6).

**CONTROLLER ERROR #5, MINE — a Critical hypothesis that does not exist.** I told the reviewer
the memo's entire correctness rests on `trade_date` being non-decreasing, and that a one-slot
memo is "silently wrong under any other order" — Critical if not airtight. It is not a
hypothesis the code supports. The memo is **compare-then-refresh** (`listed_opra.cpp:184-188`):
it recomputes whenever the date differs from the memoized one, so `trade_end ==
end_of_day_ns(trade_date)` for **every row under any input order**. Sortedness buys hit rate,
not correctness. The reviewer confirmed the ordering claim anyway (`definition_key` =
`std::tie(trade_date, …)`, sort `:160-161`, loop `:173`, only two declarations between) — but
the point stands that I specified the wrong failure mode and would have accepted a fix for a
defect that was never there. Third time this sprint that a plausible mechanism was asserted
ahead of reading the code.

Also ruled by the reviewer, as delegated: buffer cannot overflow (guard at `total > 32`) and
un-termination is safe (every `parse_iso_ns` read is length-guarded); the `noexcept` drop is
safe (sole caller sits inside `try/catch(std::exception)` at
`databento_spy_dispersion_definitions.cpp:541-543`); and the **default-constructed-table
fingerprint change is UNREACHABLE in-tree** — the only default construction is inside `create`
itself. Acceptable.

**IMPORTANT I1 — the perf claim is unsupported, and it is the one thing the page-cache carve-out
was written to prevent.** No measurement log exists anywhere; the discarded warm-up (cold)
number is not reported for either side, and no method is stated for the RSS figure. The known
cold/warm swing on this file is 13.4 s — **~2x the claimed 6.6 s win**, i.e. exactly the regime
where an uncontrolled measurement can manufacture the result. Direction is mechanically
credible; magnitude and the (a)/(c) attribution are not established. Fix is a re-run, not a code
edit — so this is a measurement fix round, and it must own the build dir.

Deferred minors (roll up to the final review):
- Task 4: minor (deferred): M1 — comments in code, commit message AND test wrongly state that
  correctness depends on the sort. It depends only on compare-then-refresh. Misleading in the
  exact direction that invites a future reader to "restore" a guarantee that was never load-
  bearing. Cheap to correct; folded into T5's dispatch since T5 owns that file next.
- Task 4: minor (deferred): M2 — a parse test asserts only `has_value()`, so a subset of cases
  cannot distinguish the parser gate from the `create()` gate. Matters because T5 rewrites
  exactly the parser those tests are supposed to lock; folded into T5's dispatch.
- Task 4: minor (deferred): M3 — the over-long-date test cannot distinguish a guard from a
  truncation, and no case sits at the exact 32-byte boundary.
- Task 4: minor (deferred): M4 — a moved-from table keeps a warm fingerprint memo (pre-existing).
- Task 4: minor (deferred): M5 — the `mutable` memo would race if a future caller parallelizes
  and asks for the fingerprint. Unreachable today.

### Wave E T6 (widen `run_identity_hash`) — implementer DONE, commit `f805655`

3 files (+274/-25), exactly the brief's list. `RunDir.*:RunArchive*:Tearsheet*` 37/37 PASS;
full suite **2056 ran / 2010 passed / 43 skipped / 3 failed / 7 disabled**, the 3 sanctioned.
**Byte gate PASS 7/7** with THREE negative controls each returning False (wrong pairing through
the same comparator, a 1-byte perturbation `f247be14540c390e` vs `5c5cbc8f936e754b`, and the
helper throwing on a missing file). All controller pins reproduced.

**RED real and observed at runtime**, not assumed: identity `18243837031959336474` on BOTH sides
of a `surface_manifest.tsv` / `input_inventory.tsv` / `trade_schedule.tsv` mutation, and the merge
test found **5 sections where 2 were expected** — old-corpus `backtest`/`reconciliation`/
`contract_marks` survived a corpus change. That is the stale-carry-forward hole, demonstrated.
The implementer labelled the `run_spec`/`universe_schedule` legs **positive controls** in its own
report and did not count them as gates — the discipline holding without being asked.

Schema freeze intact: `ra_schema_hash()` still `0xdcce47781ac8390d` (verified as the constant AND
at offset 24 of a produced archive header), `kRaMinor` 0, `MatchesCommittedPythonFixture` green,
`wave_a_fixture.atxrun` sha256 unchanged. No bump needed.
**Union survived** — 9 sections from four route invocations (7 economic + `meta` + `diagnostics`),
`section_count` 9 before and after, plus a library-level positive-control test.
Blast radius verified rather than assumed: A/B against the pristine fixture's pre-change
`run.atxrun` on byte-identical folded inputs moved `run_identity_hash`/`created_ts_ns`
`17113646873963503754 -> 6795837171609484144` while **all seven economic dumps hashed identically**.

Two implementer concerns delegated to its reviewer: (i) the pre/post identity A/B compares wyhash
values from two different builds; (ii) the suite is +22 ran vs the Wave D T2 baseline, 3 attributed
to itself and 19 to intervening tasks without individual attribution.

### Wave E T4 fix round 1/5 — measurement only, no code finding

I1 is the only open finding and it requests a re-run, not an edit, so the fix dispatch is a
measurement task. Fresh implementer (the original was stopped and is not resumable). It owns the
build dir; T6's reviewer runs concurrently on snapshot `snap-f805655`. Scope: three build variants
(BASE `0622d52`, BASE+(a), BASE+(a)+(c)) to attribute the win separately, page-cache carve-out
enforced (both sides warmed with a discarded run, **the discarded cold number reported** — the
specific omission I1 names), an explicit peak-RSS method, and a verdict of SUPPORTED /
UNSUPPORTED / CORRECTED-TO-X on each of the three claimed numbers. It is told that "below the
noise floor" is a legitimate result and not to manufacture attribution the data cannot support.
Temporary working-tree edits are expected for the variants; exact restoration is required and must
be proven. Deferred minors **M1 and M2 folded in** as the only permitted committed-code changes,
since that file is next rewritten by T5 and M2's tests are T5's net.

### Wave E T6 — review of `f805655`

**Review: Spec ✅ / Code quality REQUEST CHANGES. 0 Critical, 1 Important, 5 Minor.**
Narrowly scoped: a comment correction, no behaviour change required. File: `…wave-e/task-6-review.md`.

**Byte gate PASS** — own fixture copy (`REPLACED=49`), five pipeline steps EXIT 0, every pin
reproduced, all seven goldens match with correct row counts, dumps bash `>` (0 CR bytes, no BOM)
hashed `Get-FileHash`, **four** negative controls all False/throwing (cross-pairing, one flipped
bit, missing file, empty file).
**Union SURVIVES** — and the reviewer answered the two-sided question structurally rather than by
the section count alone: it traced the identity at **all four write points** and established that
`build_schedule_command` writes `trade_schedule.tsv` at `:471-472`, **five lines before** its own
`write_run_archive` at `:477`, and that no later route writes a folded input — so the identity is
constant across all four processes. Empirically `section_count = 9`, `verify` green,
`created_ts_ns == run_identity_hash`, `schema_hash @24 = 0xdcce47781ac8390d`, `minor @82 = 0`,
`wave_a_fixture.atxrun` unchanged, and no economic section carries a `created_ts_ns` column
(checked all seven dump headers).
Suite **2056/2010/43/3/7**; the +22 vs the Wave D T2 baseline is now **individually attributed** —
3 T6, 10 Wave D T6, 6 Wave E T4, 3 Wave E T1; 22 `+TEST` lines added, 0 removed, none
parameterized. That closes the implementer's own second concern.
Provenance established four ways: HEAD == `f805655` with a clean tree for all three files, snapshot
exes sha256-identical to `build-rel\bin`, PE link stamps (17:09:31 / 17:11:19) postdating the final
source writes (17:08:27-17:08:58) and predating the commit, and
`RunDir.RunIdentityIsSensitiveToEachFoldedInput` passing — which pre-T6 library code cannot do.
Honest limit stated rather than papered over: the reviewer could not re-observe the RED without a
rebuild (forbidden), so it verified internal consistency two independent ways instead — an
identical `18243837031959336474` on both sides of all three legs is exactly what the pre-change
two-file fold must emit, and the 4+2-1=5 merge count is arithmetically forced.

**IMPORTANT I-1 — the transitive-coverage claim is FALSE, and it was falsified on the real driver,
not argued.** Changed `definitions.tsv` (730526177 -> 730526178 B) with **all five folded inputs
byte-identical**: `run_identity_hash` did NOT move (`6795837171609484144` before and after), and a
6-section `run-backtest` write produced a **9**-section archive — `projected_cold` /
`projected_schedule` / `mark_divergence` carried forward **across a definitions change**. Root
cause: neither folded file is derived from definitions at all. `build_corpus_command` **copies**
`definitions.tsv` at `:396` without reading it, and `write_input_inventory` (`:182-202`) consumes
only `OpraBatchResult`.

**This finding is PLAN-MANDATED — the false claim is the brief's own premise, faithfully
transcribed by the implementer.** The brief says definitions is "covered *transitively*" by
`input_inventory.tsv` and `surface_manifest.tsv` and instructs: "State this reasoning in the header
comment so the next reader does not 'fix' it." The implementer did exactly as told. Standing
instruction is not to stop for questions, so I adjudicate:

**RULING — the exclusion stands, the safety claim goes, and the residual hole gets documented and
pinned by a test.** Excluding definitions from the fold is still right (hashing 696 MB on every
archive write is a real perf regression inside a perf wave). What is wrong is the *reason* given.
The honest statement is: definitions is excluded on cost, it is **not** transitively covered, and
a definitions-only change therefore does **not** invalidate the merge — a known, bounded,
documented gap. A comment asserting a guarantee the code does not provide is the exact defect the
Wave D whole-branch review caught, and it must not ship twice in one sprint.
**Deferred to Wave E T8, conditionally:** T7/T8 compute a content hash of `definitions.tsv` on the
read path where the bytes are already resident, so folding that already-computed value into
`run_identity_hash` would close the hole at no extra I/O. That is only available if T7's GO/NO-GO
returns GO; if it returns NO-GO the gap stays open and documented, and the final review sees it.
Note T7's own cache key does NOT depend on the false claim — it hashes definitions bytes directly —
so nothing downstream is built on the falsehood.

Deferred minors (roll up to the final review):
- Task 6: minor (deferred): M-3 absent-vs-present-but-empty untested (code is correct).
- Task 6: minor (deferred): M-4 a `filesystem::exists` error is silently treated as "absent", now
  across 4 files rather than 2.
- Task 6: minor (deferred): M-5 §6's cross-build A/B is corroboration, not a gate.

### Wave E T4 fix round 1/5 — the measurement, and it overturns the commit message

Fresh implementer, DONE_WITH_CONCERNS — the concern is the RESULT, not the code. **127 pooled
runs**, three variants **interleaved within each rep** (BASE `0622d52`, BASE+(a), BASE+(a)+(c)).
Full detail appended to `task-4-report.md` under `## Fix round 1 — measurement`.

    claim 1  build-schedule 20317 -> 17509 (a) -> 13699 ms   UNSUPPORTED
    claim 2  run-backtest   20560 -> 16892 (a) -> 13776 ms   UNSUPPORTED
    claim 3  peak WS 2797.5 -> 1806.4 MB, moves only with (c)  SUPPORTED

Claims 1 and 2 are disqualified **by the page-cache carve-out itself** — the discarded warm-up vs
settled median on that exact side swung `23858 -> 13855 = 10003 ms`, above the 1.5x-of-6618 ms
threshold of 9927 ms. Independently, a single variant re-run **against itself** spreads 51%
(build-schedule) and 101% (run-backtest). That is the control doing precisely the job it was added
for: I1 was raised because the cold number was never reported, and once reported it invalidated
the headline.

    CORRECTED   build-schedule  +1972 ms (14.2%) settled   / +5719 ms (32.1%) under memory pressure
    CORRECTED   run-backtest    +2481 ms (18.0%) settled   / +5351 ms (29.5%) under memory pressure
    (a) IS BELOW THE NOISE FLOOR — median +707 / +651 ms but NEGATIVE in 5/17 and 7/16 paired reps

So the sprint's headline "~33% off the dominant phase" is wrong: it is **14-18% settled**, reaching
~30% only under memory pressure. Direction is not in doubt — the combined win was positive in
**31 of 33** paired reps. Magnitude and attribution were.
**(c), not (a), carries the larger and more consistently-signed share — the REVERSE of the commit
message.** Claim 3's method is now on record and reproducible: Win32 `GetProcessMemoryInfo`
`PeakWorkingSetSize` on the **retained handle after `WaitForExit()`** — .NET's `PeakWorkingSet64`
returns empty post-exit, which is the likely reason no method was ever stated. Only the word
"deterministic" is corrected to "reproducible in steady state" (the OS trimmed it twice under
pressure).

**`79b2fa6`'s commit message now contains numbers known to be wrong.** History is not being
rewritten for it; the corrected figures live in `task-4-report.md`, in this ledger, and must appear
in the wave-gate commit message and the final status doc. Anyone reading only the git log would
otherwise carry the overturned claim forward.

M1 and M2 both done, committed **separately** as `ba06428` (+128/-58, explicit paths, exactly
`atx-vol/src/listed_opra.cpp` + `atx-vol/tests/listed_opra_test.cpp`). M1 comment-only — 0 of 25
changed diff lines are non-comment. **M2 required correcting the review's own prescription:**
asserting `ParseError` vs `InvalidArgument` cannot work because the parser **re-wraps `create()`'s
error as `ParseError`**, so the code is not a discriminator and the wrapped message is; all three
parse tests now `EXPECT_EQ` the exact gate. Proven non-vacuous with a flipped-assertion negative
control (observed RED, reverted, re-verified green). Targeted filter on the final build **24/24
PASSED, EXIT=0**, clean under `/WX`.
Tree restored exactly: `git status --short -- atx-vol/src atx-vol/include atx-vol/tests` empty,
`listed_opra.hpp` byte-identical to `79b2fa6`. All 127 runs held their pins (`rolls=3`;
`dates=49 rolls=3 final_nav=22635.66476`), zero nonzero exits — **21 independent BASE-vs-HEAD
reproductions of the economics** as a side effect of the measurement.

### Wave E T4 fix round 1 — scoped re-review: I1 / M1 / M2 all ADDRESSED

Not taken on trust: the re-reviewer **recomputed every settled figure from the 127 raw rows**
(`scratchpad/t4meas/results{,2,3,4}.tsv`) and all match to the decimal — +706.6/+2238.1/+1971.7
build-schedule, +650.9/+1274.7/+2481.3 run-backtest, sign counts 12/17 14/17 16/17 and 9/16 11/16
15/16, spreads 7036.7/13932.1. All six discarded cold numbers present and matching rep 0.
Carve-out arithmetic correct (6618 -> 9927 threshold vs 23857.8-13854.6 = 10003) and applied to the
right quantity. RSS method verified **in the script**, not from prose: `psapi.dll
GetProcessMemoryInfo` -> `PeakWorkingSetSize` on the retained handle after `WaitForExit()`, `/1MB`,
throws on <=0. M2's prescription correction verified against the code: `listed_opra.cpp:291-294`
re-wraps every `create()` error as `ErrorCode::ParseError`, so the code genuinely cannot
discriminate; `error.hpp:83-90` renders `"Code: message"` and the four test constants match `:262`,
`:282`, `:191`, `:205` verbatim. Assertion count preserved 18->18 / 8->6+2 / 6->5+1 — nothing
weakened. Provenance established rather than assumed: `ba06428`'s two new constants appear in
`snap-ba06428/atx-vol-tests.exe` (1 each) and are ABSENT from `snap-79b2fa6` (0 each). 24/24 PASS.

**RULING ON THE NOISE BAND — the sign survives decisively, the point estimates do not.**
Pairing cancels exactly the between-rep drift the 51%/101% self-spreads measure, and combined is
positive 16/17 (p~2.7e-4) and 15/16 (p~5.2e-4). But the distribution-free 95% CI on the median is
~[903, 4161] ms = **[6.5%, 30%]** and ~[811, 3508] ms = **[5.9%, 25%]**. So this is NOT the same
defect with a smaller number — the estimator genuinely changed — but the figure to carry forward is
**"order 5-30%, median ~15%", NEVER "14.2% / 18.0%"**. Every downstream artifact (wave-gate commit
message, final status doc, T7's GO/NO-GO input) uses the interval, not the point.

**Three qualifications the fix report got wrong, found by recomputation:**
1. The PRESSURED table **includes rep 0 — the discarded cold warm-up** — contradicting §5's own
   claim that it was excluded. Its medians reproduce only with rep 0 in; dropping it takes combined
   5719 -> 5134. Rep 0's (a) delta of +5222.7 ms is the largest in the entire log: a cache artifact
   sitting inside the headline pressured number.
2. `results.tsv` and `results2.tsv` have **no `free_mb` column**, so the "~2.6 GB" that defines the
   pressured regime is nowhere in the log — the regime label was **necessarily post-hoc**.
   Mitigated but not erased: nothing was discarded, the post-hoc-ness is self-flagged, and the
   mechanism was predicted a priori.
3. **"Below the noise floor" was applied asymmetrically.** (c)/run-backtest sits at 11/16
   (p~0.21, 31% sign flips) and still gets a headline +1275 ms, while (a) is retired at 29% flips.
   One rule, two outcomes. (c)'s RSS claim is unaffected and remains SUPPORTED — non-overlapping
   2797.x vs 1806.x with n>20/cell, established by the three-variant design rather than assumed.

New breakage: none Critical/Important. **One Minor that Wave E T5 MUST be told:** the layer pins are
exact-message equalities, so a cosmetic reword during T5's parser rewrite turns ~40 assertions RED.
Mitigated by the four constants being centralized (a one-line update) — but a T5 implementer who
does not know this will read it as having broken the parser.

**Task 4: complete (commits 0622d52..ba06428, review clean after 1 fix round).**
Deferred minors carried to the final review: M3, M4, M5 (from the original review) + the
exact-message-pin brittleness above.

### Wave E T6 fix round 1/5 — DONE, commit `ad3a6b5`

4 files, +264/-64. The 4th (`examples/spy_dispersion_backtest.cpp`) is outside the brief's list and
was added for M-1's "document at the call site" — comment-only. **No behaviour change:** the fold
loop is byte-identical to `f805655` and the pipeline stamps the same identity
`6795837171609484144` the original reviewer measured.

**I-1 FIXED** — transitive-coverage claim gone from both `.hpp` and `.cpp`, replaced with: excluded
on **cost grounds alone**, **not** transitively covered, so a definitions-only change does **not**
invalidate the merge-write guard; a known, bounded, documented gap. Mechanism inline
(`build_corpus_command` `fs::copy_file`s it without reading; `write_input_inventory` consumes only
`OpraBatchResult`), the reviewer's falsification recorded, and the cheap remedy noted (T7 already
computes `hash_bytes` over the whole file on the read path). Pinned by
`RunDir.RunIdentityIsDeliberatelyBlindToDefinitionsContent`, whose **banner and failure message
both say it records a gap and must be DELETED, not reverted**, when the fold closes — which is the
right shape for a test that pins a known-bad property.

**M-1 FIXED, test half not dropped.** Invariant documented at three points (fold contract in `.hpp`
and `.cpp`, generalised past the single call site; the schedule write; the archive write). Pinned by
`MergeWriteDropsCarriedSectionsWhenAFoldedInputAppearsLate`. **Honest limit volunteered:** the test
pins the *consequence*, not the statement order — the example driver has no gtest harness, so a
reorder there is caught by the byte gate (`trade_schedule` golden), not by the suite. Flagged to its
re-reviewer to rule whether that is adequate or actually Important.

**M-2 FIXED, and the RED is a three-way discrimination rather than a single observation.** Duplicate
deleted; its one bit of new coverage moved into `MergeWriteCarriesUnsupersededSectionsOnSameInputs`
in one line, **labelled a POSITIVE CONTROL and not counted as a gate**. Replacement covers
absent -> present between two writes. RED observed at runtime by **rebuilding the pre-T6 two-file
fold**: same identity both sides (`16232834906411377749`), then **count 5 where 2 expected** with
`backtest`/`reconciliation`/`contract_marks` carried. In that same run the definitions test's
non-vacuity control FAILED (correct for pre-T6) and the union positive control PASSED — three tests,
three different required outcomes, all observed. Fold restored and verified byte-identical **before**
any verification number was taken.

**Byte gate PASS** — five pipeline steps exit 0, every pin exact, all seven goldens and row counts
unmoved, bash `>` (CR=0, no BOM), `Get-FileHash`, four negative controls all False/throwing.
`schema_hash` on disk `0xdcce47781ac8390d`, `minor` 0, `wave_a_fixture.atxrun` sha256 unchanged,
`section_count` 9. Filter 38/38 (37 + 2 - 1). Suite **2057 / 2011 / 43 / 3 / 7**, the 3 sanctioned.

### Wave E T5 (P3b) dispatched — carrying the wave's measurement lesson forward

T5's dispatch bakes in what T4 cost to learn, as requirements rather than advice: interleave
variants **within** each rep (pairing is what cancels the drift behind the 51%/101% self-spreads),
report the discarded cold number for every side, report a **sign count and a distribution-free
interval** rather than a median, do not define a regime post-hoc unless the defining variable was
logged during the run, and apply the noise-floor rule **symmetrically** in both directions.
Also warned about the exact-message pins: a cosmetic reword of a parser error message turns ~40
assertions RED without the parser being broken, because `listed_opra.cpp:291-294` re-wraps every
`create()` error as `ParseError` and the message is the only discriminator.

### Wave E T6 fix round 1 — scoped re-review: I-1 / M-1 / M-2 all ADDRESSED, one new Minor

I-1 verified true against the code, not just read: `build_corpus_command:396` copies definitions via
`fs::copy_file` without reading and persists the fixed literal at `:400`; `write_input_inventory:182`
takes only `(path, OpraBatchResult)`. The gap test's banner and failure message both say
delete-not-revert — ruled "not a trap". M-2's RED confirmed genuine rather than fitted: all four
cited line numbers land exactly on the claimed assertions (`:2067`, `:2085`, `:2089`, `:2154`), and
the **non-obvious prediction holds** — the definitions test failed only at its CONTROL leg `:2154`
while its gap leg `:2147` passed, which is what the pre-T6 fold forces. Restore verified before any
measurement: zero non-comment changed lines in `run_archive.cpp`. Fourth file confirmed comment-only
(zero non-comment changed lines across all three non-test files). Arithmetic checked: +2/-1 TEST
macros => 2056->2057 and 37->38. Targeted 38/38 PASS.

**M-1's RESIDUAL HAZARD RE-RATED IMPORTANT — the compensating control was FALSE.** The fix report
claimed a reorder of the schedule-write and archive-write statements would be caught by the byte
gate. It would not. The pristine fixture already contains `trade_schedule.tsv`,
`relocate-fixture.sh` deleted only `run.atxrun`, and the post-pipeline file is byte-identical to the
pre-existing one (`4712bfd422285b6a`). So under a reorder the identity never moves, nothing is
dropped, and all seven goldens reproduce. No gtest executes `build_schedule_command`. **The reorder
passes BOTH gates.**

**CONTROLLER FIX, MINE — `relocate-fixture.sh` now deletes the pipeline's own text outputs.**
`run.atxrun` + `trade_schedule.tsv` + `projected_schedule.tsv`. A file present before its producing
step runs cannot distinguish "the step ran and produced identical bytes" from "the step never ran".
**Self-tested with `snap-ad3a6b5`** (no build needed, T5 owns the build dir): relocation reports all
three absent, five pipeline steps EXIT 0, `final_nav=22635.66476` / `18528.61666`,
`mark divergence rows: 0`, `verify dates=49 admitted=539 rolls=3`, both files regenerate, and
**ALL SEVEN goldens PASS with byte sizes matching T1's record exactly** (17734 / 6851 / 231971 /
25569 / 17881 / 26748 / 98). Negative control: one byte of `trade_schedule.tsv` flipped ->
`c33397790d421759`, match **False**.
Recorded in SPRINT-CONSTRAINTS as **byte-gate hazard #7**. This is the SEVENTH distinct way a byte
gate has silently lied in this sprint, and the SECOND time this specific one has been found —
Wave D T7 hit it and the remedy never made it into the shared helper. It has now.

**TOOLING ALARM INVESTIGATED AND REJECTED — the re-reviewer misdiagnosed it.** It reported bash
`grep` output being rewritten by the rtk proxy and asked for SPRINT-CONSTRAINTS to be revised.
Not reproduced: `git diff | wc -l`, the same diff as a file, and `rtk proxy git diff` all return
**37**. The 8-vs-9 discrepancy is **case sensitivity** — `grep` is case-sensitive, PowerShell
`Select-String` is case-INSENSITIVE by default, and the extra line was
`…BlindToDefinitionsContent` with a capital D; `grep -c -i` returns 9. Its own conclusions stand
(it re-took every diff claim with PowerShell), but the constraints file now records the CASE
asymmetry as the real hazard and explicitly refuses to record "grep is unreliable" — a false hazard
would make every future agent distrust a working tool.

Deferred minor (roll up to the final review):
- Task 6: minor (deferred): the replacement comment over-claims "NO folded input is derived from the
  definitions bytes" (`run_archive.hpp:616`, `run_archive.cpp` body, `run_archive_test.cpp:2101`).
  False for folded input (5): `build_schedule_command:427-428` reads definitions and passes them to
  `build_listed_dispersion_schedule` (`listed_dispersion_pipeline.cpp:192-197`, `:238`), which
  produces `trade_schedule.tsv`. Safe-direction (the operative "change *confined to* definitions"
  sentence stays true) and one clause to fix — scope it to inputs (3) and (4). **Queued as T6 fix
  round 2** rather than parked, because it is the third false comment in this area and the sprint's
  own rule is that a comment asserting what the code does not provide gets corrected.

### Wave E T5 (P3b — single forward scan) — implementer DONE_WITH_CONCERNS, commit `18ee3cb`

2 files, +295/-18, explicit-path stage only (the tree's 52 unrelated entries untouched).
Suite **2060 / 2014 / 43 / 3 / 7**, the 3 sanctioned. Targeted **27/27** (20 `ListedOpra` incl. 3
new + 7 `StandardMonthlyClassifier`).

**RED observed at runtime against a deliberately naive first-8-tabs scan** — 3 tests failed with
`"ACCEPTED"` where `"listed definitions: malformed row"` was expected, including Task 4's own `:765`
10-field assertion, and a case where `field 8 dropped from row 1` caused row 1 to parse **carrying
row 0's `source_fingerprint`**. That is the exact silent-corruption mode invariant 1 exists to
prevent, demonstrated rather than argued. **No error message was reworded and all four
expected-message constants are untouched** — the ~40-assertion tripwire I warned it about was
avoided deliberately.

**Marginal effect on `definitions_parse`** — 8 paired interleaved reps x 2 subcommands, both sides
warmed by a **reported** discarded rep 0:
    pooled median  -23.7%, positive 14/16, 92.3% distribution-free interval [6.5%, 29.1%]
    full range     [-97.6%, +63.8%]
    build-schedule -28.4% (7/8, [18.3%, 46.4%])
    run-backtest   -13.6% (7/8, [3.2%, 29.1%])
    peak WS        -99.9 MB, 16/16, [99.8, 100.0]  <- the clean result
The memory number matches the removed 104.7 MB line index arithmetically.

**The measurement discipline held, including against its own result.** It applied the noise-floor
rule symmetrically and that rule **retired its own run-backtest wall (5/8) and CPU (4/8) claims** —
the failure mode T4's audit caught (one component retired at 29% sign flips while another kept a
headline at 31%) did not recur. It also flagged, unprompted, that `run-backtest`'s cold/warm swing
(5747 ms) **exceeds** 1.5x its claimed win (3610 ms) — the carve-out's own disqualification
threshold — and told the reader to quote build-schedule or the memory number instead. Its reviewer
is asked to rule whether that leg is defensible in paired both-sides-warm form or must be withdrawn.

**Brief correction, verified arithmetic:** the brief's "~8.7M" residual `raw_symbol` allocations is
wrong for this fixture — measured **6,545,634** (~210 MB) from 730,526,177 B / 6,545,636 lines. The
allocation is untouched by this pass, as the brief required it to state.
**Self-declared positive control:** `ParseIsByteIdenticalToPriorImplementation` **passed against the
naive scan**, so it is labelled a positive control and not counted as a gate.
Scope note for the reviewer: one extra sequential `memchr` pass (`count_newlines`) was added to
preserve the exact `reserve`; it sits inside the measured phase, so the reported win is net of it.
Harness defect, disclosed: its own `tail -f` locked `results.tsv` mid-run and truncated it; 36/36
rows were recovered from `measure.log` into `paired.tsv`. **Recovered-after-failure data is exactly
where a silent selection effect hides**, so its reviewer is told to verify provenance rather than
accept the recovery.

Byte gate PASS — full pipeline twice on two independent fixture copies, all seven goldens and all
four pins, `trade_schedule 8d4f223f3b83b8bf`/66 exact, four negative controls False/threw,
`cr_bytes=0` on all 14 dumps. Note it ran with the OLD helper; its reviewer re-runs with the
stricter hazard-#7 version.

### Wave E T6 fix round 2/5 dispatched — one-clause comment scope correction (dispatched on sonnet)

Fix round 2 DONE, commit `0939ef7`. Scoped "NO folded input is derived from the definitions bytes"
down to: (3) and (4) are not definitions-derived at all; **(5) `trade_schedule.tsv` IS derived from
`definitions.tsv`** — but that does not reopen the gap, because the guard's premise is a change
CONFINED to definitions, in which case `build-schedule` was not rerun and `trade_schedule.tsv`'s
bytes do not move either. Applied at all three sites (`run_archive.hpp:613-629`,
`run_archive.cpp:1537-1550`, `run_archive_test.cpp:2100-2110`). Comment-only proof: `git diff`
filtered to non-`//` added/removed lines is **empty** for all three files. Targeted 38/38 PASS.
Scoped re-review dispatched (sonnet — one Minor, one comment-only commit).

### Wave E T3 (P2 — leg-key-filtered reconciliation join) dispatched

Told plainly that the **narrowed fail-closed gates are the deliverable and the speedup is the
bonus**, not the reverse: filtering preserves the panel-wide missing-identity check (it precedes the
filter) but NARROWS look-ahead/expiry, economics-disagreement and future-quote to the consumed keys,
and each must be proven to still fire for a **wanted** key. Silently losing a fail-closed gate is
this task's failure mode.
Carries forward: the full measurement protocol (interleaved pairing, discarded cold number per side,
sign count + interval, no post-hoc regimes, symmetric noise floor — with Task 5's self-retirement
named as the standard); the exact-message tripwire (~40 assertions); Task 4's memo being exact under
any order; the frozen schema; and the pre-flagged positive control
(`FilteredJoinEmptyWantedIsUnfiltered` passes against old and new code by construction).
It is also the first task to use the **hazard-#7** helper, which now deletes the pipeline's own text
outputs. Brief's stale `final_nav=-456.5769067` overridden as usual.

Fix round 2 scoped re-review: **ADDRESSED.** Verified rather than read: the fold numbering (1-5) in
the comment matches the actual order in `run_identity_hash()`; (3) `surface_manifest.tsv` and (4)
`input_inventory.tsv` confirmed genuinely not definitions-derived (`build_corpus_command` copies
without reading; `write_input_inventory` has no `definitions` parameter); (5) `trade_schedule.tsv`
confirmed genuinely definitions-derived (`build_schedule_command` reads it and passes it into
`build_listed_dispersion_schedule`, used via `listed_quotes_for_date` in the roll loop). Escape
clause logically sound and explicitly framed as a **gap, not a safety guarantee**. Comment-only
verified independently (bash `grep`, case-sensitive, non-`//` added/removed lines across all three
files -> empty; `git show --stat` confirms exactly three files). Old over-claim absent everywhere,
no contradictions between sites, documented-gap framing untouched. Targeted 38/38.
It also diagnosed a tooling trap instead of reporting a false RED: **CWD must be `C:\atx` (or
`C:\atx\build-rel`) for the fixture-comparison test's relative path to resolve — running from
inside the snapshot directory gives a spurious path-only failure.** Recorded in SPRINT-CONSTRAINTS,
since every reviewer this wave runs from a snapshot.

**Task 6: complete (commits 7fbe01e..0939ef7, review clean after 2 fix rounds).**
Deferred minors carried to the final review: M-3 (absent vs present-but-empty untested; code
correct), M-4 (a `filesystem::exists` error silently treated as "absent", now across 4 files),
M-5 (§6's cross-build A/B is corroboration, not a gate).

### Wave E T5 — review of `18ee3cb`

**Review: Spec ✅ / Code quality APPROVED. 0 Critical, 1 Important, 8 Minor.**

**IMPORTANT — unsigned underflow.** `listed_opra.cpp:344` `reserve(count_newlines(tsv) - 1u)`:
on an input with zero newlines this is `reserve(SIZE_MAX)` -> `std::length_error` **thrown out of a
`Result`-returning parser**. The reviewer proved it unreachable today (`!exhausted()` after the
first `next_line()` <=> at least one newline) but it is guarded only by a distant header-validation
gate, in a function the plan slates for further rewriting. One-line fix.

**Both invariants verified from the committed code, not from the report.** Over-count: an explicit
ninth `memchr` at `:127` rejects a 10-field row. Under-count: `false` before any field is read, plus
per-row `fields[9]` at `:352` and short-circuit `||` at `:358`. Empty-line `continue` preserved at
`:349-351`, LF-only intact. The reviewer traced the new cursor walk against the deleted `split`
through **every** edge — empty input, no-newline, `MAGIC\n`, terminating `\n`, unterminated tail —
and found them element-for-element equivalent with the reserve exactly preserved (6,545,635 both
ways). Error strings byte-identical at `:332`, `:336`, `:365`; the test hunk is **+194/-0**, so all
four message constants and Task 4's ~40 assertions are untouched — the tripwire held.

**RED confirmed genuine and mechanistically inevitable:** `red.log` shows 3 FAILED including Task
4's `:765`, with `ParseIsByteIdenticalToPriorImplementation` **[OK]** — which independently confirms
the implementer's own positive-control self-assessment. Exactly one of nine row-1 drop cases fires
(`field 8`); `dropped from row 0` appears **0** times, as the mechanism requires.

**RULING ON THE run-backtest LEG — do NOT withdraw; demote to directional-only.** The carve-out's
disqualification rule is scoped to **cross-cache-state A/B**; this measurement is within-rep
interleaved with both sides warm and rep 0 discarded on both, so that confound is controlled by
construction. The 5747-vs-3610 ms arithmetic is right and establishes that the **magnitude** is
unresolvable on this box — not the direction, which holds at 7/8 with interval [729, 5101] ms
excluding zero and is mechanistically corroborated by peak WS at 16/16. Withdrawing it would falsely
imply the parse did not improve there. Required: the rule-3 caveat inline wherever 13.6% appears,
and quote **per-leg, not pooled**.

**The measurement was independently recomputed from `paired.tsv` and every published figure
reproduces exactly** — medians, order statistics, 14/16 sign count, coverages 92.97%/92.32%. Noise
floor symmetric with a clean gap: every retained metric <=12.5% sign flips, both retired ones at
37.5% and 50%. Peak-WS arithmetic confirmed **better than claimed**: 6,545,637 x 16 B = 99.878 MiB
vs 99.9 measured. Fixture verified `6545636 730526177`, so the brief's "~8.7M" -> 6,545,634
correction is right. `count_newlines` justified rather than waved through: it materialises nothing,
the task removed an allocation not a scan, and the exact reserve is precisely what makes -99.9 MiB
solely attributable to the index.

**Concern 7 (data recovered after the `tail -f` lock) — provenance CLEAN.** All 36 `paired.tsv` rows
appear verbatim in `measure.log` with none extra, and all 6 surviving `results.tsv` rows are
byte-identical in `paired.tsv`. Nothing selected, nothing reconstructed. The reviewer's own initial
one-row mismatch turned out to be `measure.log`'s BOM defeating a `^before` anchor — and it chased
that to ground rather than reporting it.

**Byte gate PASS, and it validates hazard #7's fix.** Own copy with the stricter helper,
`REPLACED=49`, **all three deleted artifacts regenerated** (`run.atxrun` 188496 B,
`trade_schedule.tsv` 23275 B, `projected_schedule.tsv` 25484 B). Five steps EXIT 0, all four pins,
all seven goldens with correct row AND byte counts, `cr_bytes=0`. **`trade_schedule`
`8d4f223f3b83b8bf` reproduces from a copy where it was deleted first** — that golden is now a real
regeneration gate rather than a copy-integrity check. NEG1 False / NEG2 True / NEG3 False
(`c99160a866698006`) / NEG4 threw. Suite **2060 / 2014 / 43 / 3 / 7**, no fourth failure.
Provenance: `snap-18ee3cb` carries all three new test strings and `snap-ad3a6b5`/`ba06428`/`79b2fa6`
carry none (case-sensitive `grep -c`), while all four carry Task 4's test; backtest exe sha16
`797FC5B1966C965D`, unique per snapshot.
Honest limits stated: the RED was not re-executed (rebuilding the naive variant is a build, which
was forbidden), and no `/WX` build log was available.

Deferred minors (roll up to the final review) — the notable ones:
- Task 5: minor (deferred): M1 — source comments still carry the stale "~8.7M" figure the report
  corrected to 6,545,634. Folding into the fix round.
- Task 5: minor (deferred): M6 — only ~14 of ~40 parse assertions actually discriminate; the nine
  row-0 drop cases and the `"\t"x7` case are non-discriminating and **must not be counted as this
  task's coverage**. Recorded so the wave gate does not credit them.
- Task 5: minor (deferred): M8 — quote the run-backtest leg per-leg with the rule-3 caveat inline,
  never pooled. Folding into the fix round.

### Wave E T3 (P2 — leg-key-filtered join) — implementer DONE_WITH_CONCERNS, commit `cff8f8e`

8 files, exactly the brief's Step 7 paths; the 53 unrelated working-tree entries untouched.
Suite **2067 / 2021 / 43 / 3 / 7** (+7 ran/+7 passed vs `18ee3cb` — the seven new
`ListedOpra.Filtered*`), the 3 sanctioned. RED observed at runtime against a stub that accepted
`wanted` and ignored it (4 of 7 failed).

**NARROWED-GATE VERDICT: PASS, with two-way mutation evidence.** This is the deliverable and it was
delivered in the right shape. A temporary mutation dropping every filtered row turns all three
narrowed gates (look-ahead, economics, future quote) RED **and only those**; a second mutation
letting the identity branch `continue` under a filter turns the panel-wide identity gate RED **and
only that**. Both reverted, residue grep clean. `FilteredJoinEmptyWantedIsUnfiltered` labelled a
positive control and not counted as a gate — flagged in advance by the controller and honoured.

**Measured** (5 paired interleaved reps, one fixture, one session; rep 0 discarded warm-up reported
on both sides: `quote_join` before 6858.7 / after 1742.5 ms):
    quote_join         2.09x median, 93.75% interval [1.74x, 2.16x], sign 5/5
    reconcile          12.7x, [9.1x, 18.0x], sign 5/5
    definitions_parse  sign 2/5   -> BELOW NOISE FLOOR, no effect claimed
    snapshot_load      sign 1/5   -> BELOW NOISE FLOOR, no effect claimed
    peak working set   unchanged (1706.5 MB both sides)
`definitions_parse`'s median actually moved **+137 ms the wrong way** and was correctly NOT reported
as a regression — the same symmetric discipline Task 5 showed. Peak WS unchanged is explained
mechanistically: peak is set during `definitions_parse`, before the join loop.

**Concern 2 is the most interesting result of the task and must not be buried.** The predicted
speedup did NOT materialise at the phase level. A temporary probe (reverted) measured quotes emitted
falling **741x** (2,021,228 -> 2,727) and the join loop **6.4x**, yet `quote_join` moved only 2.09x
— because `load_opra_daterange` is ~880 ms and **78% of the filtered side**. **The phase is now
parquet-load bound.** The brief predicted "quote_join drops by roughly the panel-rows-to-legs ratio";
it did not, and the reason is now known rather than guessed.

Other concerns raised for its reviewer to rule on:
1. **The brief's `wanted` spec is not literally implementable** — a panel row's `expiry_ts_ns` comes
   from `definitions.find`, the very lookup being skipped. Implemented as a two-stage filter
   (coarse `raw_symbol` pre-lookup, exact key pre-emit). Stage 1 is deliberately coarse **so a row
   lying about its strike still trips the economics gate**.
3. Cold/warm swing exceeds the claimed win (6858.7 -> 4048.5 = 2810 ms vs a 2178 ms median). Stated
   as the protocol requires; the paired interleaved design and 5/5 sign are what carry it.
4. `ListedQuoteKey` uses the brief's `= default` `<=>` rather than the promoted-verbatim `std::tie`
   `operator<`; claimed to differ only for a non-finite strike (unreachable, fail-closed there).

Byte gate PASS — all seven goldens MATCH from a fresh relocation with hazard #7 checked and printed
(all three outputs absent pre-run). `reconciliation 35f0625d917f9dc5` and `contract_marks
04162f9a4157e03d` reproduce, so `quote_mid_coverage`/`n_quote_mid_lots` did not move. All four pins.
Negative controls: one-byte perturbation -> `MATCH=False`, helper throws on missing/empty — **plus an
incidental real one**: a first attempt that omitted `--schedule projected_schedule.tsv` had the gate
correctly report `projected_cold` DIFFER. Schema untouched, no parser message strings changed, no
Python touched.

### Wave E T5 fix round 1/5 dispatched (sonnet) — underflow guard + M1 + M8

### Wave E T3 — review of `cff8f8e`

**Review: Spec ✅ (1 ⚠️ — mutations unverifiable without a build) / Code quality REQUEST CHANGES.
0 Critical, 2 Important, 5 Minor.**

**NARROWED-GATE EVIDENCE: GENUINE, and the reviewer proved it the right way round.** It derived the
predicted RED partition **from the committed code before reading the mutation output**; Mutation A's
5-RED/2-green and Mutation B's 1-RED/6-green splits are disjoint, complementary, and exactly what the
structure predicts, with correct per-assertion texts. Better still, it found gates 2 and 4 carry
**in-test two-way controls** needing no mutation at all (same corrupted table -> `Err` when wanted,
`Ok` size=2 when not) — both OK in its own run. Honest limit: gates 1 and 3 have no in-test control,
so for those it validated the predicted partition but could not observe the RED (builds forbidden).
Verified from code: the identity gate at `:497-500` precedes the filter at `:515`; gates at
`:567`/`:573`/`:595` all follow stage 1 and precede stage 2 at `:606`; `wanted`-empty is
**structurally inert, not merely tested**; the header does document the narrowing.

**IMPORTANT 1 — the contract says FOUR fatal checks; there are SEVEN.** `listed_opra.hpp` asserts
"Four checks are fatal for any panel row". The loop has seven fatal exits, and three narrowed ones
are undocumented and untested: `parse_osi_symbol` at `listed_opra.cpp:538` and `:571` — **live gates
under the production `SkipUnlisted` policy** — and "contract definition missing" at `:565`. The
*behaviour* is within the accepted trade class; the *mandated contract statement is factually wrong*.
**PLAN-INHERITED — the brief made the same error.** Standing instruction is not to stop for
questions, so I adjudicate: **the brief's "four" was wrong and the contract must state all seven,
with the three newly-identified narrowed gates covered by tests.** The entire point of the brief's
"(a) stated in the header contract" requirement is accuracy; shipping "four" when there are seven is
the same false-comment defect this sprint has now hit in three separate files. Fix it.

**IMPORTANT 2 — `wanted`'s SORTED+DEDUPED precondition is documented but unenforced** (`:483`,
`:516-531`). An unsorted span silently drops legs -> `NoRawQuote` -> moved coverage, **with no
error**. No live defect (the sole caller sorts at `spy_dispersion_backtest.cpp:51-52`), but this is a
silent-wrong-answer path guarded only by caller discipline. A `std::is_sorted` guard closes it.

**Concern 1 (two-stage filter): the implementer is RIGHT and the brief was wrong.**
`quote.expiry_ts_ns = definition->expiry_ts_ns` (`:584`) comes from the lookup at `:532`, so the
brief's single exact-key pre-lookup filter is impossible. The two-stage version is semantically
exact: `quote.raw_symbol = identity->raw_symbol` (`:583`) is the same string stage 1 searched,
`wanted`'s primary sort member is `raw_symbol` so the run is contiguous, and both searches use the
same `char_traits::compare` relation. Nothing dropped that single-stage would admit; nothing admitted
that it would drop. **Stage 1 is a SUPERSET of the exact-key set, so gates 2-4 fire for MORE rows
than the brief specified — the deviation widens gate coverage.** Not Critical.

**Concern 4 (`<=>` vs `std::tie`): claim verified, and the substitution is REQUIRED.** Only
`double strike` can yield `partial_ordering::unordered`; string/int64/enum are all `strong_ordering`
and `std::string`'s `<=>` is the same relation as `tie`'s `<`. Non-finite strike unreachable on both
sides — quotes rejected by the economics gate (`:573-577`, `osi.strike` always finite, `NaN !=
finite`), legs by `finite_positive(leg.strike)` (`listed_dispersion_schedule.cpp:196`). Membership
unaffected. The `<=>` version is **more** fail-closed than `tie` (collapses to `Err(AlreadyExists)`
where `tie` would admit both) and is **required**: it supplies the `operator==` that
`std::unique(wanted)` needs, which a bare `operator<` never did.

**Concern 3: the implementer OVER-flagged its own result.** Protocol threshold is 1.5 x 2178 =
**3267 ms**; the observed swing was **2810 ms** — the caveat is not actually triggered. Within-rep
interleaving eliminates the cross-cache-state confound by construction (same ruling as Task 5).
Magnitude quotable as **~2x**, not "2.09x" (per-rep ratios 2.06 / 1.74 / 2.09 / 2.16 / 2.16).
**Concern 2 verified and correctly headlined:** 741.2x, 6.44x, 77.8%, 2.246x all recompute exactly,
and the probe is absent from both the commit and the working tree (grep rc=1, case-sensitive AND
case-insensitive).

Byte gate PASS — own copy, `REPLACED=49`, hazard-#7 pre-check showed all three outputs absent, five
steps EXIT 0, all four pins, all seven sections MATCH with expected `data_rows`, and the
`reconciliation` header confirmed to carry `quote_mid_coverage`/`n_quote_mid_lots` so no leg key was
missed. Dumps via bash `>` (0 CR bytes verified), `Get-FileHash`. Three negative controls: appended
byte -> `88283ca738c49a7a` MATCH=False; helper throws on missing/zero-length; cross-section
MATCH=False. Suite **2067 / 2021 / 43 / 3 / 7**, the 3 sanctioned only. Provenance by
`--gtest_list_tests` differential: the +7 are exactly `ListedOpra.Filtered*` (in `snap-cff8f8e`,
absent from `snap-0939ef7`). Schema untouched, no parser message strings changed, `atx-vol/python/`
untouched, commit scope exactly the brief's 8 files.
It also noticed working-tree drift in `listed_opra.cpp`/`listed_opra_test.cpp` and correctly
identified it as **another agent's in-flight work** (hunks at `:92`/`:308`/`:340`, not touching
`listed_quotes_from_opra`) rather than reporting it as T3 residue.

### Wave E T5 fix round 1 — DONE, commit `2d5b74f`

Important I1 fixed: `reserve(newlines > 0u ? newlines - 1u : 0u)` replaces the unguarded
subtraction, with a comment stating why the guard stays despite today's unreachability (so a later
reader does not delete it as dead code). Reserve value on the real fixture unchanged — 6,545,636
newlines -> 6,545,635 either way — which matters because the task's -99.9 MB result is only
attributable to the removed index if the reserve stays exact. Regression test
`ParseRejectsZeroNewlineInputWithoutThrowing` added. M1 fixed: all three stale "~8.7M" comments
corrected to the measured 6,545,634 rows / ~104.7 MB index / ~733 MB vector. M8 fixed (report only):
rule-3 caveat inline at both sites where the 13.6% figure appears, legs quoted separately, pooled
figure demoted from headline.
Focused filter **35/35** — the count rose from the dispatch's expected 27/28 because Task 3 landed
7 `FilteredJoin*` tests after T5's review was written; the implementer noticed and explained the
discrepancy rather than reporting a mismatch. Full suite **2068 / 2022 / 43 / 3 / 7**, reconciling
exactly as baseline 2060 + Task 3's 7 + this round's 1.

**Note on agent stops — third of this shape.** The T5 fix agent stopped mid-round waiting on its own
background test run and returned no status or commit. Its work was intact and uncommitted in the
tree (guard + comment + test, diff stat 24/8 and 16/0); I verified that from git before assuming
anything, then resumed it by message to run the suite, commit and report. Same as the earlier T3 and
T4 stops. **Checking the tree before re-dispatching is what keeps this cheap** — a controller that
assumed loss would have re-run the whole round.

### Wave E T3 fix round 1/5 dispatched — contract accuracy + an unenforced precondition

Scope: Important 1 (contract says four fatal checks, code has seven — the three undocumented ones
are `parse_osi_symbol` at `:538` and `:571`, both LIVE under the production `SkipUnlisted` policy,
plus "contract definition missing" at `:565`), Important 2 (`std::is_sorted`-style fail-closed guard
on `wanted`'s unenforced SORTED+DEDUPED precondition), and the Concern 3 report correction.
The implementer is told explicitly that its two-stage filter and its `<=>` substitution were both
**verified correct and upheld** — do not change them — and to verify the seven-count against the
code itself rather than trusting my paraphrase.

### Wave E T5 fix round 1 — scoped re-review: all three ADDRESSED, and the new test is a POSITIVE CONTROL

Important ADDRESSED — the ternary reduces algebraically to the identical `newlines - 1u` for all
`newlines >= 1` (traced, not input-sampled), and `newlines == 0 -> reserve(0)` is **correct** (zero
data rows are possible there), not merely safe. M1 ADDRESSED — all three sites verified directly in
source (`:95`, `:311`, `:350-352`), now citing 6,545,634 rows / ~104.7 MB index / ~733 MB vector,
matching `wc -lc` and the original review's own recomputation. M8 ADDRESSED — caveat inline in §0
immediately after the 13.6% figure and again directly under the §6.3 table, legs quoted separately,
pooled figure explicitly demoted rather than relabelled. No new breakage: Task 3's `wanted` filter
(`listed_quotes_from_opra` at `:469+`) has zero overlap with this diff's hunks, and no parser error
message string changed. Targeted 35/35 run by the reviewer itself.

**RULING — `ParseRejectsZeroNewlineInputWithoutThrowing` is a POSITIVE CONTROL, not a gate, and it
cannot be made into one.** The dispatch named this as the most likely way the fix would be weaker
than it looks, and it was. The reviewer traced `parse_listed_definitions` by hand for all three
inputs (`""`, a no-newline string, a bare magic line): each hits the pre-existing, unmodified
"fewer than two lines" gate (`exhausted()` at `:332`) and returns **before** the magic/header check
and long before `reserve` at `:354`. The reserve line is unreachable for those inputs whether the
guard is present or reverted — so the test passes identically either way and pins the header gate's
contract, not the underflow fix.
This is not a defect in the fix; it is a property of the fix. The guard exists **precisely because**
the only thing that makes the subtraction safe today is a non-local gate 14 lines away, and it is
defence against a future reorder of that gate. By construction no reachable input can exercise it,
so no test can gate it without a mutation harness the suite does not have. The report already
discloses the limitation. **Parked with this ruling: the guard stands, the test must be LABELLED a
positive control in the source so a later reader does not count it as coverage, and the final review
sees both sides.**

**Task 5: complete (commits ad3a6b5..2d5b74f, review clean after 1 fix round, 1 parked).**
Deferred minors to the final review: the positive-control label above; M6 (only ~14 of ~40 parse
assertions discriminate — the nine row-0 drop cases and the `"\t"x7` case must NOT be counted as
this task's coverage); plus the six other minors in `task-5-review.md`.

### Wave E T3 fix round 1 — DONE, commit `fd52934`

5 files, explicit paths, +289/-30. Suite **2073 / 2027 / 43 / 3 / 7** (baseline +5/+5, exactly the
new tests), the 3 sanctioned only. Targeted filter 56/56. Byte gate re-run: PASS, own copy
`REPLACED=49`, hazard-#7 pre-check printed clean, all 5 steps EXIT 0 on every pin, all seven hashes
AND row counts MATCH, 0 CR bytes, **four** negative controls False (appended byte, cross-section,
throws on missing/zero-length, row-count).

**Important 1 RESOLVED — and it corrected BOTH the brief and the reviewer.** It verified the count
itself: seven fatal loop exits, but the split is **1 panel-wide / 6 narrowed**, not the reviewer's
1/3. Pre-fix `:499` panel-wide (precedes stage 1); `:538 :565 :569 :571 :575 :596` all narrowed. It
excluded the two preamble `Err`s at `:476`/`:480` because they run once before any row, and **said
so** rather than silently choosing a boundary. It then found the four-vs-seven claim had
**propagated to three further sites** — `listed_dispersion_pipeline.hpp:98`,
`spy_dispersion_backtest.cpp:585`, and the test-file preamble — all corrected. The rewritten
contract enumerates all seven in loop order with error codes and additionally records that (2)/(5)
are the same condition on mutually exclusive branches, that **(2) is the one missing-definition exit
`SkipUnlisted` does NOT disarm** — which the policy doc block had also omitted, now fixed — and that
(3) is inert under `SkipUnlisted`. This is the fourth iteration of this statement; its re-reviewer is
told an enumeration confidently wrong in a NEW way would be worse than the "four" it replaced.
Three new tests, each with a **two-way in-test control**. They characterise already-correct
behaviour so they do NOT redden against unmutated HEAD — **labelled as such rather than counted** —
and discrimination is proved by two mutations: **A (stage 1 drops all) 10 RED / 2 green**,
**B (filter never applied) 9 RED / 3 green**. B's surviving gate test is exactly the economics test
the reviewer had flagged in Minor 1, **reproducing that finding independently**. Residue grep rc=1.

**Important 2 RESOLVED with an observed RED that demonstrates the silent loss.** `std::adjacent_find`
on `!(lhs < rhs)` before the loop -> `Err(InvalidArgument, "listed OPRA join: wanted keys must be
sorted and deduped")` — one O(n) pass enforcing BOTH halves, `Err` not `assert` (assert is a no-op
in Release, the only build that matters here), new string with no existing message touched.
**Pre-guard, the join returned `Ok with 1 quotes` on a 2-key descending span — one leg silently
lost, exactly the described defect.** The duplicate case returned `Ok with 2` and is stated plainly
as contract enforcement rather than a latent wrong answer; its re-reviewer is asked to rule whether
that framing understates it.

**Concern 3 CORRECTED.** §5.2 retitled "reported, and NOT triggered" with the arithmetic shown
(1.5 x 2178 = 3267 ms vs observed 2810 ms = 0.86x the win); magnitude requoted **~2x** in the effects
table, §5.1 and the Concerns entry; per-rep ratios (2.06/1.74/2.09/2.16/2.16) recorded with the
reason; §2's stale four-check table marked SUPERSEDED.

### Wave E T7 (P1) dispatched — with Step 1 as a REAL exit

The dispatch tells it plainly that I am not signalling which way the GO/NO-GO comes out, that it must
compute the ratio from its own measurement of current HEAD stating numerator, denominator and ratio
explicitly, and that **a NO-GO is a completely acceptable and useful outcome** — cheaper to not build
a cache than to build one nobody needed. Prior measurements are given as context only, explicitly
"do not substitute for your own", including T4's corrected interval rather than the retracted
14.2%/18.0% point estimates. The brief's second exit is also flagged: if measured hash + blob-load
exceeds ~50% of post-T5 parse time, report rather than land.
It is told NOT to widen `run_identity_hash` (that is T6's documented gap and a possible follow-up,
not this task) but to state whether its content hash is reusable for closing it.

### Wave E T3 fix round 1 — scoped re-review: Important 1 / Important 2 / Concern 3 all ADDRESSED

**Seven-exit enumeration INDEPENDENTLY CONFIRMED.** The re-reviewer enumerated every
`return Err`/`ATX_TRY`/`continue`/`return Ok` in `listed_quotes_from_opra` at `fd52934`: the loop
body (`:505-630`) has exactly seven fatal exits — `:515` `NotFound`, `:554` `ATX_TRY` parse (def
absent), `:581` `NotFound`, `:585`, `:587` `ATX_TRY` parse (def found), `:591`, `:612` — and **no
eighth**; the only other exits are five non-fatal `continue`s and the terminal `Ok`, and
`definitions.find` returns a raw pointer so nothing else propagates. Split confirmed **1 / 6**:
stage 1 begins `:531`, exit 1 is `:515`, exits 2-7 are `:554`+. It also checked the pre-fix numbers
the implementer cited and found they are against **`2d5b74f`**, not `cff8f8e` — and match it exactly.
The two preamble `Err`s sit at function scope above the `for` at `:505`, so excluding them is
principled rather than convenient. Header text matches the code line-for-line including all seven
error codes and loop order; **(2) really is the only missing-definition exit `SkipUnlisted` leaves
fatal** (it precedes the numeric-root, 0DTE and policy checks) and (3) really is inert under it.
A **case-insensitive** search over `atx-vol/` finds zero surviving four-check claims.

**Mutations: genuine two-way discrimination, derived before comparison.** The re-reviewer derived
both partitions from the code first: A (stage 1 drops all) must green exactly `EmptyWanted` +
`MissingIdentity` -> 10 RED / 2 green; B (filter never applied) must additionally green
`EconomicsMismatch`, the only gate test whose **both** halves assert `Err` -> 9 RED / 3 green. Both
match test-for-test, **including the non-obvious detail that the precondition tests redden at their
control half under both**. The Minor-1 reproduction is real: `FilteredJoinStillFatalOnWanted
EconomicsMismatch` has two `ASSERT_FALSE`s and no `elsewhere` block, so it is structurally the unique
survivor of B. Honest limit: it could not re-run either mutation (builds forbidden).

**Guard verified stronger than argued.** `adjacent_find` on `!(lhs < rhs)` tests strictly-increasing,
so one pass enforces both halves (adjacent strict increase => global, `<` transitive). And
`partial_ordering` unordered pairs make the predicate **true** -> rejected — **fail-closed, so the
NaN-unreachability argument is not even load-bearing here.** Runs once per call above the loop;
`filtering &&` short-circuits so the empty path gains one boolean and nothing else (`+16/-0`).
Duplicate framing ruled **correct, not understated**: the loop iterates panel rows, a duplicate only
widens `candidates`, and `none_of` short-circuits identically — emitted set unchanged, only `reserve`
over-sizes.
30 deletions across 5 files, **0 non-comment**. T5's `newlines > 0u ? newlines - 1u : 0u` guard
intact at `:353-354`; RunArchive pins intact; `atx-vol/python/` untouched; scope exactly 5 files.
Targeted **56/56** run by the reviewer. Suite **2073 / 2027 / 43 / 3 / 7**, +5/+5 verified
independently via `--gtest_list_tests` (2080 vs 2075 on `snap-2d5b74f`, minus 7 disabled each).
Provenance: `snap-fd52934` lists 12 `ListedOpra.Filtered*` vs `snap-2d5b74f`'s 7.

**TWO NEW MINORS — and they are the same defect class, in the same file, for the FIFTH time.**
(A) The gate renumbering was applied to the header and the new tests but **not** to three
pre-existing test comments (`listed_opra_test.cpp:528`, `:561`, `:594`), so the file now carries
**two "NARROWED GATE 2" and two "NARROWED GATE 3" labels on different gates**. (B) The
`MissingDefinitionPolicy` block this very diff edited still omits exits 5 and 7 from its
policy-independent list.
**RULING — these do NOT get parked, they get a fix round 2.** Normally Minors roll to the final
review and never enter the loop. These are the exception because they are *newly introduced
inconsistency created by the fix itself*: two identically-numbered labels on different gates is
actively misleading, and it undermines the very contract-accuracy finding just closed. This
statement has now been wrong four times and the fix for it introduced a fifth. Cheap to close, and
leaving it would hand the final review a contradiction in the file the whole task was about.
Queued behind T7, which owns the build directory.

### Wave E T7 (P1) — implementer DONE_WITH_CONCERNS, commit `5948772`. **GO/NO-GO = GO.**

**The gate was decided from its own measurement at `fd52934`, as instructed** — 3 warm reps + 1
discarded cold warm-up, numerator `definitions_parse` wall_ms from both subcommands' `diagnostics`
sections, denominator the two command wall totals:
    warm 1  16611.322 / 19455.225 = 85.4%
    warm 2  21393.419 / 24051.469 = 88.9%
    warm 3  28162.752 / 32007.421 = 88.0%
    cold (discarded) 22392.704 / 25181.349 = 88.9%
Median **88.0%** against the 15% floor. GO, and not close.

Suite **2089 / 2042 / 44 / 3 / 7** vs `fd52934`'s 2073/2027/43/3/7 (+16 ran / +15 passed / +1 skipped
— the extra skip is an env-gated measurement harness, which its reviewer is told to confirm is
legitimate rather than a test quietly disabling itself). **Step 3 RED observed at runtime:** a
guards-absent build served a mismatched key, a tampered payload, a **CRC-repaired payload edit**, a
tampered header, and a stale table through the seam — 5 failing tests — and the four guards turned
all five green. The CRC-repaired case is the one that matters: it proves the key catches what the
CRC structurally cannot.

    Step 6 net ratio  2.183x median, [2.043, 2.540], sign 4/4
    parse | key-hash | cache-read   7851.6 | 59.4 | 3407.1 ms (medians)
    hash_bytes micro  58.3 ms over 730,526,177 B (~12 GB/s), 0.7-0.9% of parse

**CONCERN 1 — A LIVE CONTROLLER DECISION, NOT YET MADE.** 2.183x is barely past the brief's 2x bar,
and the Step-20 cost check hits **48.9%** on one rep against its ~50% abort threshold. Sole cause is
the brief-mandated `fingerprint()` verification, **1.36-1.98 s per read**, because it re-serialises
~733 MB. Without it: **~4x** and 21-29%. The implementer implemented it as specified rather than
weakening a fail-closed check unilaterally — correct call, and it escalated instead.
My leaning, handed to T7's reviewer **to attack rather than ratify**: the check looks largely
redundant. Payload CRC covers corruption; `abi_fold` + the `static_assert`s cover layout drift; the
content hash covers stale input; and `CacheRoundTripReconstructsTableExactly` covers codec
correctness **at every build** rather than on every production read. If that holds, the runtime check
is a per-read tax on a property that is deterministic given the code. The reviewer is asked to name a
concrete failure mode the check catches that those four do not — if it can, the cost stands — and to
assess an opt-in flag (default off in Release, on in tests) as the middle option.

**CONCERN 3 MAY MATTER MORE THAN THE WHOLE TASK.** `read_listed_definitions_file` slurps via
`istreambuf_iterator`, costing **3.59-3.86 s of the 7.07-9.75 s parse**. A `fread` would be
byte-identical — one function, no new format, **no stale-serve failure mode** — for roughly **half
this task's win at a fraction of the risk**. If that holds it changes P1's whole risk calculus, and
the reviewer is asked whether the cache is still worth its stale-serve surface once the cheap half is
taken.

**The brief was wrong about the fixture, in a way that helps.** It says "the fixture's 13.2 MB
`definitions.tsv`"; the file is **730,526,177 bytes / 6,545,634 rows**. The fixture **IS** the 696 MB
production case, so the brief's Step 6 instruction that "the 696 MB production case should be
measured by the controller" is **already satisfied** — nothing was extrapolated.
Also confirmed: the key's `content_hash` is directly reusable to close `run_identity_hash`'s
documented `definitions.tsv` gap at no extra I/O (not done here, correctly — that was T6's follow-up
and outside this task). And a pin note worth keeping: `run-projected-backtest [cold]` yields
`18528.61666` **only** with `--schedule projected_schedule.tsv`; the default schedule gives
`22635.66476`.
Not wired to the CLI — that is T8, so `definitions_parse` in the shipped binaries is unchanged.

Fix round 2 DONE, commit `2fc29fc` (comment-only, 10 changed lines, all comments; `git diff -U0`
filtered to non-`//` lines empty). Minor A: three stale labels renumbered — `listed_opra_test.cpp:528`
look-ahead GATE 2->4, `:561` economics GATE 3->6, `:594` future-quote GATE 4->7 — and verified
file-wide afterwards (`GATE [0-9]` gives 7 matches, gates 1-7 each exactly once). Minor B: exits 5
(OSI parse, definition-found path) and 7 (future quote) added to the "remains fatal under both
policies" bullet in `listed_opra.hpp` (~`:133`), which had listed only 4 and 6. Targeted 56/56.
Its re-reviewer is told the bar is **"is it right NOW, not merely different"** — uniqueness is
necessary and not sufficient, since a consistent relabelling can be consistently wrong — and is
asked to identify which fatal exit each of the three tests actually exercises rather than checking
the numbering is self-consistent.

**CONTROLLER SEQUENCING — T8 is deliberately HELD until T7's review rules on the fingerprint check.**
T8 wires the cache into the CLI and measures the end-to-end effect (Step 5: cold cache-disabled,
cold with `--cache`, warm with `--cache`). That measurement is a direct function of the read path's
cost, and the `fingerprint()` decision changes that cost by 1.36-1.98 s per read — the difference
between a 2.18x and a ~4x net. Dispatching T8 first would produce a Step 5 table that has to be
thrown away. The build directory sits idle for one review cycle; that is cheaper than measuring
twice. The `fread` change (concern 3) is held for the same reason — its byte-identical claim is
currently being verified by T7's reviewer, and dispatching it before that verdict risks work on a
refuted premise.

Fix round 2 scoped re-review: **Minor A ADDRESSED, Minor B ADDRESSED, no new breakage.**
The bar was "right now, not merely different", and it was checked that way — each renumbered label
verified against **what the test actually exercises**, not against its name:
    `:528` FilteredJoinStillFatalOnWantedLookAhead      perturbs `definition_ts_ns` -> exit 4 (`:585`)
    `:561` FilteredJoinStillFatalOnWantedEconomicsMismatch  perturbs expiry/strike vs OSI -> exit 6 (`:591`)
    `:594` FilteredJoinStillFatalOnWantedFutureQuote     perturbs `ts_ns` -> exit 7 (`:612`)
all three labels correct. Uniqueness confirmed **twice over**: bash `grep -o 'GATE [0-9]'`
(case-sensitive) and PowerShell `Select-String` (case-insensitive) both return exactly 7 matches,
1-7, no repeats, plus a separate case-sensitive sweep for a differently-cased stray label found none.
Minor B checked against the code rather than the claim: `listed_opra.cpp:548-613` shows exits 4/5/6/7
contain **no `policy` check anywhere**, so all four really are unconditionally fatal; exit 3 correctly
stays excluded (gated by `policy == SkipUnlisted`) and exit 2 correctly lives in the other bullet
(nullptr branch, not the definition-found path). The corrected list matches the header's own
"(4)-(7) are the definition-exists gates" summary — exhaustive, nothing over- or under-claimed.
Comment-only re-verified independently: 10/10 changed lines are comments, no assertion or error
message string touched. Targeted 56/56.

**Task 3: complete (commits 2d5b74f..2fc29fc, review clean after 2 fix rounds).**
Deferred minors to the final review: the five in `task-3-review.md`, of which Minor 1 (the economics
gate test asserting `Err` in both halves, making it the unique survivor of mutation B) was
independently reproduced twice.

### Wave E T7 — review of `5948772`. **Spec ❌ / Needs fixes. 1 Critical, 6 Important, 6 Minor.**

**CRITICAL C1 — the headline is measuring the wrong thing.** The seam
`read_listed_definitions_cached` (`listed_definitions_cache.cpp:891-895`) **re-slurps the full
730 MB source on the HIT path**, and the harness put that slurp in the numerator but not the
denominator. Corrected, the 2.183x headline is really **0.97-1.09x** on the implementer's own reps
and **1.20-1.45x** on the reviewer's. The cache barely beats parsing.
And the re-slurp is not a bug to be deleted — it is **inherent to a content-derived key**. The key
must hash the source bytes, so a hit can never avoid *reading* 730 MB; it can only avoid *parsing*
what it read. That is P1's premise, and it is much weaker than the plan assumed.

**CONCERN 3 VERIFIED, AND IT BEATS THE CACHE.** `listed_opra.cpp:394-404` is exactly ifstream +
`istreambuf_iterator` + parse, running at ~197 MB/s. Byte-identical holds — the stream is already
`ios::binary`, so `fread` loses nothing. **`fread` alone is ~1.5-1.9x on `definitions_parse`: one
function, no new format, no stale-serve surface — LARGER than what the cache delivers at the seam.**
Reviewer's verdict: **P1 is not worth its risk as committed.** Do `fread` first, re-measure the seam
end to end, and only then decide on wiring.

**CONTROLLER ERROR #6, MINE — my fingerprint leaning was WRONG, and the reviewer attacked it as
instructed.** I argued the check was redundant because CRC covers corruption, `abi_fold` covers
layout drift, the content hash covers stale input, and the round-trip test covers codec correctness
at every build. The reviewer named a concrete mode all four miss: `ListedContractDefinition` gains a
field; `parse`/`serialize` are updated; the ATXDEFS1 encoder is not. `abi_fold` moves so **old** blobs
miss, but the **new** writer emits a lossy blob and the **new** reader zero-fills. CRC passes.
Content hash passes. And `CacheRoundTripReconstructsTableExactly` **also passes** — because
`sample_rows()` (test `:1005`) uses 9-value aggregate init, so a trailing field is default on *both*
sides, and `expect_rows_bit_identical` enumerates 9 fields by name. Only a production read catches it.
That is the fourth time this sprint a test was shown blind to the defect it appeared to cover, and
this time I was the one arguing from the blind test.
**RULING (the reviewer's, adopted): opt-in BUT ORDERED.** Land the `sizeof`/`offsetof`
`static_assert` FIRST — it makes that exact drift a **build error for free** — and only then gate the
runtime check behind a flag, default off in Release, forced on in tests. **Do not make it opt-in
before the pin.** The tamper argument for keeping it always-on fails independently: `fingerprint()`
is public and unkeyed, so anyone who can restamp two CRCs can restamp a third. And
`ListedDefinitionTable::create` (`listed_opra.cpp:234-257`) already re-validates every row
unconditionally.

Other Importants: **I1** no `static_assert` on `sizeof`/`offsetof` of `ListedContractDefinition`
(verified absent repo-wide; the sprint constraint mandates it for on-disk structs) — this is the pin
the ruling above depends on. **I2** 8 lines verbatim-duplicated from `listed_opra.cpp:395-402`
including both error strings, so the `fread` fix now needs **two** sites. **I3** the headline interval
`[2.043, 2.540]` and "sign 4/4" **include the discarded warm-up, and it is the most favourable rep**
— the exact Task-4 pattern, recurring. **I4** "~4x without the check" is
`parse/(key+read-fingerprint_ms)`, a difference of independently noisy terms; **the reviewer's rep 2
printed `28.028x` from that formula**. **I5** write and fingerprint-verified read each force the
~730 MB `serialize_listed_definitions` transient that Task 4 made lazy — seam peak ~2.8 GB on a
696 MB input. **I6** no hit/miss logging, and `hash.hpp:13-15` **explicitly disclaims cross-process
hash stability** — so a permanent 100% miss plus a 300 MB write every run would be completely
invisible.

Verification quality: suite **2089 / 2042 / 44 / 3 / 7** confirmed exactly (2042+44+3 = 2089), 3
sanctioned REDs only, `ListedDefinitionsCache` 15 OK / 1 skipped. Provenance by construction (16
tests in `snap-5948772`, 0 in `snap-fd52934`). **The env-gated skip was not taken on trust** — the
reviewer set `ATX_T7_DEFINITIONS_TSV` and ran it 3x, all PASS with real assertions. Fixture
independently `stat`-ed at **730,526,177 bytes**, confirming the brief's "13.2 MB" is wrong and the
implementer right. Byte gate correctly not re-run: a case-sensitive `grep` shows **zero** CLI call
sites, so the pipeline is unchanged by construction.

---

### STOP POINT — 2026-07-25 (third), user instruction "pause here"

HEAD `ca74f68`, **38 commits ahead of `main`**; `main` untouched at `2858cab`.
Wave E: **T1, T2 (dropped), T3, T4, T5, T6 CLOSED.** T7 fix round 1 **PARTIAL**. T8 **HELD**.
T9 and the final whole-sprint A-E review not started.

**T7 fix round 1 got through the first two of its seven ordered items and stopped during the fifth.**
The order was mandated because it was load-bearing, and the two that landed are the two that had to
come first:
    35e8e80  I1 — compile-time ABI pin on ListedContractDefinition (hpp +77, test +69)
    ca74f68  fingerprint verification made OPT-IN (default off Release, forced on in tests)
The pin had to precede the flag: it is what makes a flagged-off runtime check safe.

**UNCOMMITTED AND UNVERIFIED — 5 files, +190/-13**, the `fread` + dedupe (I2) work in progress:
`detail/archive_util.hpp` +35, `detail/archive_util.cpp` +83, `listed_definitions_cache.cpp`,
`listed_opra.cpp`, `listed_definitions_cache_test.cpp` +43. `fread` now appears in BOTH
`listed_opra.cpp` and `listed_definitions_cache.cpp` with a shared helper being introduced in
`detail/archive_util`. **Not built, not tested, not measured**, and `task-7-report.md` has **no
`## Fix round 1` section** — the report was never written. Fourth agent stop of this shape this
session; as every previous time, the work is intact in the tree and nothing is lost.

**NOT STARTED:** the C1 harness rebuild (the honest seam measurement), I3/I4 (measurement
corrections), I5 (the ~730 MB transient), and **I6 — which must come FIRST on resume.**

> **I6 is a possible BLOCKED that can invalidate P1 outright.** `hash.hpp:13-15` explicitly
> disclaims cross-process hash stability. If `hash_bytes` is not stable across processes the cache
> misses 100% of the time forever while still writing ~300 MB per run, and with no hit/miss logging
> that is completely invisible. Resolve it EMPIRICALLY — compute the key in one process, again in a
> separate invocation, compare — not by reading the header.

**RESUME ORDER:** finish or discard the uncommitted `fread`/dedupe work -> I6 -> C1 -> I3/I4 -> I5
-> let the resulting number decide T8 -> T9 wave gate -> final whole-sprint A-E review.

**THE DECISION WAITING ON THAT NUMBER.** T7's review established that the cache's seam re-slurps the
full 730 MB source on the HIT path (inherent to a content-derived key, not a bug), so the reported
2.183x is really 0.97-1.09x on the implementer's reps and 1.20-1.45x on the reviewer's — while
`fread` alone is ~1.5-1.9x with no new format and no stale-serve surface. **P1 is not worth its risk
as committed.** Dropping it on evidence would be the plan working, exactly as P5 was dropped this
wave when `archive_load` measured 0 ms / count 0.

Working tree clean for all other sprint files; the pre-existing unrelated uncommitted work (Python
bindings split, `atx-core` sqlite, `atx-db/`, `atx-kb/`, docs) was **never staged at any point** —
every commit this session used explicit paths.

Status doc: `docs/superpowers/2026-07-25-atx-vol-backtest-sprint-status-3.md`
