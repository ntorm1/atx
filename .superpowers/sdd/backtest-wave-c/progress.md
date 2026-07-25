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
