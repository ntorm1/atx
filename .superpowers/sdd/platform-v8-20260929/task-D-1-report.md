# Task D-1 report: NAV stage timers, log-return ring, construction grid, books on a pool

Lane D, worktree `C:/atx-wt/pool-4`, branch `feat/platform-v8-d-20260929`. Brief: `task-D-brief.md` D-1.
Not built and not run (lane rules): every claim below comes from reading the code. Root builds and runs.

## What was built

### Engine: a reusable lockstep-grid facility (new, `atx-engine/include/atx/engine/parallel/lockstep_grid.hpp`)

This header is generic and JSON-free, so mining and frontier sweeps can reuse it.

- `GridVariant {id, overrides}` means "the base run plus these (key, value) overrides", in the client's own flag spelling.
- `GridRules {max_variants, allowed_keys}` and `validate_grid` check:
  - variant ids: 1..64 characters of `[a-z0-9-]`, unique;
  - override keys: allowed, and at most once per variant;
  - variant count: 1..max.
- `for_each_lane(DetPool*, lanes, body)` runs one per-lane phase:
  - With no pool, 1 worker or 1 lane, it runs sequentially and stops at the first failure.
  - Otherwise it uses `DetPool::parallel_for` and returns the lowest failing lane's status, which is the one the sequential loop reports.
- Strategy parsing stays in atx-impl. The engine knows no key.

### Stage timers (`--stage-timers`; `NavEmitOptions::stage_timers`)

- `summary.json` gains `stage_seconds` with the keys load, exposures, construction, books, hash, write, wall and definition.
  - These are differences of nested steady-clock intervals, so each stage is >= 0 and the six stages sum to wall.
  - wall runs from entry to the `summary.json` write.
  - load: argument checks, admission, the pinned load and SHA verification.
  - exposures: the session ring's own clock inside `compute_price_exposures`.
  - construction: the shared per-decision construction minus exposures.
  - books: the rest of the replay.
  - hash: the SHA-256 of the published CSVs and the recipe digest.
  - write: the rest.
- A console line repeats the timers.
- **Only behind the flag.** `summary.json` is:
  - pinned by the trials ledger (`backtest_integrity.ledger_record` records `pins.summary_sha256`);
  - compared byte for byte by the `StrategyLive` tests and by the identity checks.

  An always-on clock key would break both. Without the flag, no key is written and every file is unchanged. The
  replay reads the steady clock (ring and construction) only as an observation.

### Log-return ring (`strategy_price_exposures.{hpp,cpp}`)

- `enable_session_ring(scratch, panel)` binds a `PriceExposureRing` inside `PriceExposureScratch`. Unbound, it is inert.
- While bound to that panel (the same close, raw_close and present pointers and geometry):
  - every interval `(t-1, t]` is computed once and kept in a mirrored ring of `max(beta_window, vol_window)` slots per name, plus the market return per slot;
  - a decision computes only the intervals its window lacks, in ascending order, so each new session is logged once;
  - it then reads its window as one contiguous run per name.
- The stateless path and the ring share one kernel (`interval_returns`) and one estimator block (`write_exposures`), with the same operations in the same order. Exposures are therefore bit-identical to a fresh scratch, in any decision order.
  - A config change that resizes the ring drops it.
  - A call on another panel runs the stateless path.
- The NAV `run_books` binds the ring when it neutralizes. The ring is charged in `validate_nav_input`'s budget and in `nav_workspace_reserve_bytes` only then (`session_ring_bytes`). For the v7.1 cell that is about 23 MB of its 1 GiB.
- The flag-off path's floating-point summation order is unchanged, for the ring and for D-0 alike.

### Books on a DetPool (`--book-workers N`, 1..64; `NavReplayConfig::book_workers`)

- `run_books` is split into per-book phases:
  - `open_book` (MARK, the warm boundary, EXECUTE);
  - the shared decision on the calling thread (`decide_construction`, timed);
  - `close_book` (DECIDE via `plan_decision`, close, report, row).
- Each phase is an engine `for_each_lane`. Lanes write only their own book and day; the shared tiers, cache and construction are written only between phases.
- With N = 1 (the default), the operations are those of the old loop.
  - The only reorder is plan(k) then close(k), instead of all plans then all closes.
  - Per-book arithmetic does not change, because no book reads another book's state and fixed-rate planning does not write the cache.
- Refused above 1:
  - with rate per-name-v1, whose `plan_decision` writes the shared rate cache;
  - while a v7 extension is installed, since its hook is thread-local.
- Not recorded in any published file except the grid manifest (console line only).

### Construction grid (`--construction-grid GRID.json`; API `replay_nav_grid`)

- Grid file schema `atx.nav-construction-grid/v1`: `{"schema", "variants": [{"id", "flags": {"--flag": "value"}}]}`.
  - At most 1 MiB, at most 16 variants.
  - Flags come from `nav_grid_variant_flags`: rule, cadence, trade-fraction, monthly-budget, band-multiple, dust-multiple, aim-leverage, exit-rate.
  - Values are strings, parsed by `apply_construction_flag`, which is the same function the command line now uses for those eight flags.
- `replay_books` runs every variant times every scenario as books of one `run_books`, over one pinned load.
  - Variants must agree on every other setting (`same_shared`), else InvalidArgument.
  - The shared construction is formed on the union of the variants' cadence days. A variant whose cadence misses a decision plans it idle, exactly as it does alone (`form_desired` never runs for it there; tiers are classified every decision either way).
- Output:
  - `<output>/<id>/` is the directory the standalone nav run with the base flags plus the variant's flags publishes, byte for byte;
  - then `<output>/grid_manifest.json` is written last (`atx.nav-grid-run/v1`). It holds the grid SHA, the combined and role SHAs, the book count, `book_workers`, each variant's flags, the recipe and summary file SHAs, and `stage_seconds` with `--stage-timers`. Timers stay out of the variant summaries.
- Refused:
  - with `--emit-holdings` (usage error);
  - with a v7 extension;
  - with an existing output.
  - `--rate` is checked against every variant's own rule, as that variant's standalone command line would be.

## Tests

- `NavTimers.SumWithin5PctOfWall` (`strategy_nav_replay_test.cpp`): a 220 x 60 noisy panel with price-risk-v1 on the CLI default windows.
  - The stage sum is within 5% of wall, and wall is within 5% of a clock around the command (best of three tries).
  - The exposures stage is > 0.
  - All other files, and the summary minus `stage_seconds`, equal the untimed run's.
- `LogRing.ExposuresBitIdenticalToWindowRecompute` (`strategy_price_exposures_test.cpp`): the ring against a fresh scratch, bit for bit, covering:
  - ascending decisions through clipped windows;
  - cadence jumps;
  - a jump past the whole ring, and backward jumps;
  - two config changes that resize the ring;
  - another panel (served statelessly, ring kept) and a rebind;
  - an absent row and a guarded split inside the windows;
  - `session_ring_bytes`.
- `ConstructionGrid.EachVariantEqualsItsStandaloneRun`, checked on two setups:
  - live exposures, three variants over cadence, theta, leverage, dust and exit rate, 3 books each: every variant directory equals its standalone run byte for byte, and the manifest SHAs match;
  - the v7.1 cell shape (fields, 5 books, locate-in-aim, delta, cache): two variants.
  - With `--book-workers 4 --stage-timers`, the variant directories are unchanged and the timers appear in the grid manifest.
- `ConstructionGrid.ApiVariantsEqualStandaloneReplaysOnAnyWorkerCount`:
  - four variants (cadence, theta, band, rule) times 5 tiered scenarios, on 1 and 4 workers, each equal to `replay_nav_scenarios` of that variant (days, construction, events, financing);
  - refusals: a shared-setting difference, 17 variants, no variants, per-name-v1 on a pool.
- `ConstructionGrid.RefusesBadGridsBeforeAnyOutput`: a missing file, bad schema, a non-string value, a bad id, a duplicate id, a disallowed flag, 17 variants, a bad value, `--emit-holdings`, `--rate` with a non-aim variant rule, and an existing output.
- `NavBookWorkers.PublishedBytesEqualOneWorker`: v7.1 cell shape with 1, 3 and 64 workers, byte for byte; bad counts exit 2; per-name-v1 with workers exits 1 with no output.
- `ParallelLockstepGrid.*` (`atx-engine/tests/parallel/parallel_lockstep_grid_test.cpp`): validation, lanes bit-identical for 0, 1 and 4 workers, lowest failing lane.

## How root verifies

- Build targets: `atx-impl-tests`, `atx-equity-strategy-targets`, and the engine `parallel` test group target (it globs `tests/parallel/*_test.cpp`).
- gtest filters:
  - atx-impl: `NavTimers.*:LogRing.*:ConstructionGrid.*:NavBookWorkers.*:NavWarmStart.*:StrategyNavReplay.*:NavV5.*:NavV6.*:StrategyLive.*:StrategyPriceExposures.*:StrategyPriceNeutralize*`
  - engine: `ParallelLockstepGrid.*:ParallelDetPool*`

Identity (D-1 step 3): rerun the accepted v7.1 cell argv (receipt `build-equity/mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247-run/receipt.json`) with the new exe into a new output:

```
build-equity/bin/atx-equity-strategy-targets.exe nav --combined build-equity/mega-v71w-train-ew-1/train_combined.json --combined-sha256 bf1af1276fd4d2cb4bd835f97b27897596f74b5855dfe7cac05c61dac2486fd5 --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9/manifest.json --fields-sha256 8fd00e9f44b475116f483e133c03fe031618390060b8374180278f1cd7b8769b --output <NEW> --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache
```

1. As is: the five `daily_*.csv` and five `events_*.csv` must be byte-identical to the accepted cell's. `recipe.json` and `summary.json` should be as well.
2. With `--stage-timers --book-workers 5` appended: the ten files and `recipe.json` are byte-identical again. `summary.json` differs only by `stage_seconds`, which is the timers report. Wall reference: 25.97 s.
3. Grid smoke test (optional): write `{"schema": "atx.nav-construction-grid/v1", "variants": [{"id": "t05", "flags": {}}, {"id": "t10", "flags": {"--trade-fraction": ".10"}}]}` and run the same argv with `--construction-grid <file>`. `<NEW>/t05/` must equal run 1's directory byte for byte.

## Deviations

- The timers are behind `--stage-timers` rather than always on, because `summary.json` is pinned in the ledger and compared in identity checks (above). In grid mode they go to `grid_manifest.json`, so that the variant summaries remain the standalone bytes.
- `--book-workers` is a flag defaulting to 1, and is refused with per-name-v1 and v7 extensions.

## Cross-lane edits

- `atx-engine/include/atx/engine/parallel/lockstep_grid.hpp` and `atx-engine/tests/parallel/parallel_lockstep_grid_test.cpp` are new files in atx-engine (generic machinery). No CMake edit was needed: the engine is header-only here, and the test is picked up by the per-group glob.

## Open risks

- The reserve and budget now charge the ring when neutralizing (about 23 MB on the v7.1 geometry). A caller whose `--max-bytes` sat within 23 MB of the old reserve plus load would now be refused with OutOfRange. The v7.1 cell's 1 GiB is far from that.
- The timing test compares against a wall clock. It retries three times and needs the run to be at least 20 times longer than the excluded `summary.json` write (about 1 ms), so an extremely loaded machine could still flake it.
- A grid runs V x S books at once, so memory is charged for every book. With the default event cap (262144 x 5 books x V), a 16-variant grid needs about 1.5 GB of event charge, so `--max-bytes` must rise with the variant count.
