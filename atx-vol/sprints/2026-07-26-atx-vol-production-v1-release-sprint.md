# atx-vol Production v1 Release Sprint Plan

Date: 2026-07-26
Basis: five-dimension deep review (correctness, performance, feature gaps, dead code, library design) run by five independent reviewers over the full tree at commit `1be0668`. Every finding below was verified against source with file:line evidence by the originating reviewer.

## Review headline

The numerics, test discipline, error taxonomy, and archive durability are already production-grade: zero TODO/FIXME markers (debt is expressed as 52 fail-closed `ErrorCode::NotImplemented` sites), no untested core module, zero include cycles across 127 public headers, layered CRC corruption defense on archives, and a consistently applied `Result<T>` error model with only 2 rethrows in 79k lines of src/.

What is **not** v1-ready:

1. **Correctness debt in the money path**: 2 P0 + 17 P1 + 16 P2 verified defects, including a data race in the documented-thread-safe `PortfolioPricer::price()` and four call sites that silently hedge against SPY when the configured index is not SPY.
2. **The library cannot leave the monorepo**: no `install()`/`export()`, no ABI macro, the umbrella header `vol.hpp` fails to compile in every target except tests, and `PUBLIC` compile definitions (`ATX_VOL_COUNTERS`/`ATX_VOL_PROFILE`) are an ODR trap for any second consumer.
3. **Host-integration gaps**: no logging sink (41 raw stderr/stdout writes in library src/), no cancellation API, production CLIs gated behind `ATX_BUILD_EXAMPLES` (which no preset enables), version pinned at 0.1.0 with an uncut CHANGELOG.
4. **Two half-finished cutovers**: ATXVSA v1→v2 archive (v1 write path still declared in the shipped public header but defined only in a test-only lib) and TSV→RunArchive (src/ still writes ~20 loose TSVs; `.atxrun` is written only by the example CLI).
5. **Public API is an everything-public dump**: 553 exported types across 109 top-level headers where the two real consumers use 8 and 40; positional-aggregate field order is a load-bearing API contract at 6 sites.

Performance is strong but leaves clear wins on the table: no LTO anywhere, `Mapping::prefetch()` implemented on both platforms with zero callers (the structural explanation of the 7.64s cold vs 1.1s warm gap), snapshot prefetch depth hardwired to 1, and AVX2 Greek packs running ~25% lane utilization on dispersion-shaped books.

---

## Sprint structure

Six sprints, ordered so that correctness lands before performance (perf changes must be validated against stable, correct baselines), dead code is purged before the API reshape (less to reshape), and the API reshape lands before packaging (install the final shape, not the interim one).

Determinism gate for every sprint: `rel` acceptance suite green, `rel-avx2` + forcescalar CTest leg green, and the 135-session parity-full dispersion backtest NAV byte-identical to the pre-sprint baseline **except** where a correctness fix intentionally changes economics — those changes must be listed in the sprint's results doc with before/after NAV.

---

## Sprint 1 — Correctness P0 + money-path P1 (est. 4-5 days)

Goal: no known defect that produces silently wrong economics in a shipped route.

### P0 (day 1)
| # | Fix | Where |
|---|---|---|
| 1.1 | `PortfolioPricer::price()`/`pnl_explain()` returning-overload race: lazily-created shared `returning_ws_` mutated under `const` contradicts the documented "query from many threads" contract. Fix: per-call workspace, or `thread_local`, or delete the returning overloads and require caller-owned workspaces. | `src/portfolio_pricer.cpp:1349-1352`, contract at `include/atx/vol/portfolio_pricer.hpp:66-72` |
| 1.2 | Four call sites take the `index_symbol = "SPY"` **default** of `all_symbols`/`universe_at` while `config.universe.index_symbol` is in scope — corpus build, schedule build, and reconciliation silently run against the wrong index. Same defect class already fixed once as REVIEW C-15. Fix: thread the configured symbol through all four sites **and remove the default parameter** so the compiler finds any others. | `src/dispersion_run.cpp:2573,2759,2775,2897` |

### P1 — wrong results on plausible inputs (days 2-5)
| # | Fix | Where |
|---|---|---|
| 1.3 | `evaluate_batch` statuses `(void)`-discarded in the P&L unique solve + workspace SoA never cleared across snapshots → partial batch publishes the previous snapshot's marks stamped Ok. Poison the span like `solve_uniques:801` does. | `src/portfolio_pricer.cpp:1668,1679,1689` |
| 1.4 | `vega_slope` computed after the finite-stamp and never checked → one deep-wing NaN skew slope NaNs the whole book's delta under `skew_adjusted_delta`. | `src/portfolio_pricer.cpp:887,873,688` |
| 1.5 | European IV inverter returns `Unavailable` for true IV < `kIvMin` instead of the documented floor (pre-clamp `step` in termination test). | `src/implied_vol.cpp:307-359` |
| 1.6 | eSSVI dense-residual butterfly guard `w > 0` sentinel unreachable (w floored at 1e-12 upstream) → fully-clamped window scores g=+1 and serves ~zero vol. | `src/essvi_calib.cpp:428-434`, floor at `src/vol_surface.cpp:113-116` |
| 1.7 | Andersen-Lake: node with collapsed `D` frozen **before** contributing to `max_dy` → wholly unconverged sweep returns `Ok` with the BAW seed. Add a `NotConverged` status or count frozen nodes into the residual. | `src/american.cpp:1279-1282,1347` |
| 1.8 | SVI: (a) non-finite Black-76 price `continue`d in SSE → Inf-parameter step scores SSE≈0 and is accepted; (b) non-PD normal matrix zeroes coordinates and returns `Ok` with a=b=rho=0 degenerate slice that then beats the good hardcoded seed. | `src/svi_calib.cpp:585-587,235-239,987` |
| 1.9 | `close_to` NaN-blind → every reconciliation tolerance gate passes when either side is NaN; `expected_quantity = pair_target / pair_vega` has no zero guard. | `src/dispersion_run.cpp:374,487,647,653` |
| 1.10 | `VolTimeCalendar::us_default()` hardcodes NYSE closures 2024-2028 only, wired unconditionally into `time_to_expiry_years(VolTime)`; pre-2007 DST rule also wrong. Fail closed outside the covered window (or load calendar data) — silent full-session treatment corrupts every vol-time backtest outside the window. | `src/vol_time.cpp:145-180,246` |
| 1.11 | Settlement L2 memo: NaN used as in-band miss sentinel but memo admits Ok-status NaN marks → null `sf_ptr` dereference / desynced `solve_ix`. Filter `isfinite` at memo populate. | `src/backtest.cpp:955`, populate at `:171-180` |
| 1.12 | `surface_at(0)` unbounded + subset-miss path legally yields zero-surface snapshot → null deref when `finance_premium=true`. | `src/backtest.cpp:2304,1388`, `include/atx/vol/backtest.hpp:184` |
| 1.13 | `andersen_lake_put_slice` reads `strikes[0]` with no `n == 0` guard (call slice is clean). | `src/american.cpp:2554` |
| 1.14 | `CloseAtHorizon`: cohort-build failure early-returns before the horizon `erase_if` → lots carried past close horizon to intrinsic settlement. Run horizon closes regardless of entry-side failure. | `src/strategy.cpp:928` |
| 1.15 | `SurfaceArchiveV2::find()` fabricates a directory entry (`n_slices`/`kind_bits`/`payload_crc32c` zero) — not equivalent to `directory()` entry. Return the real entry or a distinct lookup type. | `src/surface_archive.cpp:985` |
| 1.16 | `arb_repair_calendar_residual` bisects assuming alpha=0 feasible without verifying → destroys lo's residual layer and returns `Ok` with the arb unrepaired. | `src/arb.cpp:877-899` |
| 1.17 | Degenerate-sigma disagreement: `american_greeks_fd` returns bare spot intrinsic where `andersen_lake_core` returns discounted-forward intrinsic — FD bundle silently disagrees with `american_price` on identical inputs. Unify on `sigma_zero_american_limit`. | `src/american.cpp:3010,3080,3796` vs `:1987` |

Exit criteria: regression test per fix (each has a concrete trigger documented in the review); NAV deltas from 1.2/1.9/1.10/1.14 quantified in the results doc; forcescalar and AVX2 legs green.

---

## Sprint 2 — Correctness P2 + numerical consistency (est. 3-4 days)

Goal: no latent UB on corrupt input, no scalar/SIMD divergence, no lifetime traps.

Corrupt-input hardening (the lazy-CRC design makes `priced_surface_view.cpp` the de-facto untrusted parser):
- 2.1 Unsorted mapped `k[]` underflows `lo = hi - 1` → reads gigabytes past the mapping inside a `noexcept` concurrent query. Validate node ascendance in `create_over_record` (`src/priced_surface_view.cpp:495-497`).
- 2.2 Per-slice payload extents never required disjoint/monotone → 1 MB corrupt record forces ~16 GB allocation (`src/surface_archive.cpp:1209`).
- 2.3 `VolSurface::w` divides by `(T_hi - T_lo)` with no strictly-ascending invariant; `iv` divides by raw unfloored `T` (`src/vol_surface.cpp:366,377`).
- 2.4 Snapshot timestamp monotonicity never validated → negative `dt` flips every carry accrual sign (`src/backtest.cpp:2290-2291`).

Numerical consistency:
- 2.5 `black76_aux`/`black76_value_and_vega`/`black76_greeks` use the cancellation-prone `1-Φ(d)` complement form → deep-OTM put price can go **negative**, disagreeing with `black76_price`. Switch to `Φ(-d)` form everywhere incl. `implied_vol.cpp:318` (`src/black76.cpp:71,94`, `src/greeks.cpp:41`).
- 2.6 AVX2 PnL body vs scalar tail use different association trees → same position's PnL depends on batch index; `total != sum(terms)` on the AVX2 route, contradicting `pnl_batch.hpp:61-62` (`src/simd/pnl_batch_avx2.cpp:85-106` vs `src/simd/pnl_batch.cpp:34-41`).
- 2.7 `arb_check_calendar` NaN-poisons `w_prev` → one non-finite slice suppresses all later violation detection at that moneyness; same defect in `arb_check_total_surface_all:418` (`src/arb.cpp:199,221`).

Lifetime/robustness:
- 2.8 `PricedSurfaceView` move leaves `record_`/`col_kind_`/`n_slices_` intact → moved-from view indexes empty `heavy_curves_` (`src/priced_surface_view.cpp:146,509`).
- 2.9 `encode_backtest_section` spans caller memory in place unlike every other encoder → dangling spans memcpy'd into the archive; arena-park a copy or document + enforce (`src/run_archive.cpp:661`).
- 2.10 `noexcept` functions perform throwing heap allocations (Error message strings > SSO, 46 KB `AloPricer::State`) → OOM calls `std::terminate` (`src/american_iv.cpp:240`, `src/american_batch.cpp:304`, `src/simd/iv_batch.cpp:37`).
- 2.11 `QueryAccelerator::build` spawns up to 16 unbounded `std::async` threads outside the `PricingExecutor` budget; thread-exhaustion `system_error` escapes a `Result`-returning function (`src/priced_surface.cpp:270-277`). Route through the executor.
- 2.12 Session cache-reuse path retains caller-owned raw `CorrectionCache*` for session lifetime (`src/session.cpp:2352` vs the clearing at `:1150-1151`).
- 2.13 `n_memo` post-increments into `std::array<BndCache,7>` with exactly 7 states and no check — add `static_assert`/bound (`src/american.cpp:3033-3035`).
- 2.14 Nested-depth overwrite in `run_context_body` (`t_nest.depth = j.child_depth` instead of max) breaks the two-level nesting bound (`src/pricing_executor.cpp:374-378`).
- 2.15 v1 SplineVol payload never serialized `mult_cap`/`w_offset` (both live terms of `w()`) → migration tool forwards them as 0. Fix the migrator to fail closed on v1 SplineVol records (`src/surface_archive_v1.cpp:153`).

Exit criteria: adversarial-archive test cases for 2.1-2.3 added to `surface_archive_v2_adversarial_test.cpp`; scalar-vs-AVX2 PnL identity test; determinism gates green.

---

## Sprint 3 — Dead code purge + cutover completion (est. 3-4 days)

Goal: one implementation per concern; nothing shipped that nothing calls.

Deletions (evidence: zero production consumers, grep-verified):
- 3.1 `calib_pool.{hpp,cpp}` + the `#error` quarantine + `vol.hpp:168` include — this single deletion also fixes the umbrella-doesn't-compile blocker.
- 3.2 `vola_parity.{hpp,cpp}` (sole export has tests=7, prod=0), `examples/fit_timing_probe.cpp` (self-labeled "NOT for commit"), empty `src/server/` dir, `arb_project_calendar_essvi_total` (`arb.cpp:771`), `derivatives.hpp:262-273` unit-conversion constexprs, `dispersion_run.hpp:600` `run_spec_from` (zero references anywhere).
- 3.3 Tracked detritus: `artifact-cache/*.atxvsa` (names unreadable by current tests; own docs say stale copies cause false test failures), `artifact-cache/hotpath-*.{out,err}` (8 files), `python/src/bindings/surface_db.cpp.local-preserved`, untracked `ref/.mypy_cache` etc. + gitignore entries.
- 3.4 Decide-and-act: `ref/` standalone Python package (zero references from anywhere in the repo — delete or relocate); `research/pg-review-2026-07-19/` (move to docs/reviews/).

v1 archive cutover (finish Wave 2 as originally planned):
- 3.5 Port `bench/surface_archive_bench.cpp:220,286`, `bench/e2e_hotpath_bench.cpp:718`, `examples/surface_archive_bench.cpp:136` from `write_surface_archive` (v1) to v2.
- 3.6 Remove v1 declarations from the shipped `surface_archive.hpp:368-382` (public header declaring symbols a plain `atx::vol` link cannot resolve), then delete `surface_archive_v1.cpp`, the `atx-vol-archive-v1` lib, and `tools/migrate_atxvsa_v1_to_v2.cpp` (self-labeled THROWAWAY).

TSV→RunArchive: **decision required, then execute.** Current split: `src/dispersion_run.cpp` writes ~20 loose TSVs; `.atxrun` is written only by the example CLI. Either (a) declare TSVs the supported human-readable sidecar and document that, or (b) move TSV emission behind a `RunConfig` diagnostics flag and make `.atxrun` the library-level default. Recommendation: (b) — the Python report builder already reads `.atxrun`.

Dispersion seam (`dispersion_run.hpp:703-796` documents the deliberate partial dispatch):
- 3.7 Collapse the CLI/library duplication for the three entry points with shipped callers; for the three without (`dispersion_build_schedule`, `dispersion_run_backtest`, `dispersion_verify`) either wire the CLI through them or delete them. `reconcile_dispersion_reference` reaches no shipped binary — wire in or delete.
- 3.8 Regenerate `vol.hpp` umbrella from the Tier-A list (Sprint 4) — currently missing 36 headers and consumed only by its own test.

Sweep-up: keep `deamer/correction/event_vol/c8/cstar/curve_selector/surface_parity/tearsheet` etc. — all verified wired. Keep `dispersion*` vs `listed_dispersion*` (two deliberate research routes). Fold-or-keep decisions on `derivatives.hpp`, `portfolio_risk.hpp`, `scenario_grid.hpp`, `pnl_attribution.hpp` (all test/bench-only today) belong with the portfolio-engine decision in Sprint 4.

Exit criteria: `git grep write_surface_archive` returns v2 only; umbrella compiles in a non-test TU; no tracked artifacts under artifact-cache/.

---

## Sprint 4 — API v1 shape (est. 5-6 days, breaking changes concentrated here)

Goal: the API you are willing to freeze. All breaking changes land in this sprint so downstream (python bindings, atx-ui, future atx-server) migrates once.

- 4.1 **Tier the public surface**: ~32 Tier-A stable headers, ~25 Tier-B advanced, demote ~29 to `detail/` (incl. `parallel_for.hpp`, `pricing_executor.hpp`, `counters.hpp`, `phase_profile.hpp`, `prepared_*.hpp`, `strip_grid.hpp`, `run_archive_schema.hpp`), relocate CLI-support headers (`surface_db_build/admin/populate/exit_codes`, `run_report`, `tearsheet`) toward an `atx-vol-tools` target, dispersion orchestration headers toward `atx-vol-research`. `spy_fixture.hpp` → `tests/support/`.
- 4.2 **Kill positional-aggregate compatibility as an API contract**: 6 sites ("Appended for positional aggregate source compatibility" at `backtest.hpp:541,545,591`, `american.hpp:65`, `session.hpp:198`, `surface_parity.hpp:357`). Named-field designated-init discipline + a `static_assert` on field count, or builders for `RunConfig`/`AlOpts`/`SessionInputs`.
- 4.3 **Error-model unification**: `Status`+out-span batch functions → `Result<T>` (`batch.hpp:52-137`, `american_batch.hpp:256-284`); `bool configure_pricing_executor` → `Status`; `profile.hpp:230` bool+out-param → `optional`; document `parallel_for` exception semantics or demote it (it currently contradicts `vol.hpp`'s "no exceptions" promise); exit codes out of library headers into CLI targets.
- 4.4 **Surface-type consolidation**: finish what `vol_curve.hpp:5-13` started — canonical pipeline `CurveSurface` (fit) → `PricedSurface`/`PricedSurfaceView` (serve) → `SurfaceSet` (portfolio); demote `Surface<>`, `C8Surface`, `CStarSurface` to detail; `VolSurface` documented as archive-wire-only. Rename rates `curve.hpp` → `rates_curve.hpp` to end the rates/vol `curve*` collision; fold `spline_curve.hpp` into the umbrella.
- 4.5 **Portfolio-engine decision**: canonical `PortfolioPricer` lacks 6 capabilities the deprecated engine still provides (stock/cash legs, aggregation views, bulk selection, multi-shock scenarios, theoretical legs, factor attribution — `vol.hpp:163-167`). Port the subset v1 actually needs, drop the rest with the deprecated engine. Shipping "DEPRECATED but required" is not a v1 contract. This also resolves the Sprint-3 fold-or-keep for `portfolio_risk`/`scenario_grid`/`pnl_attribution`.
- 4.6 **`BacktestResult` invariant**: 30 unguarded parallel public vectors → size-invariant enforcement (private columns + accessors or checked `validate()`).
- 4.7 **Contract documentation**: borrow/lifetime wording for the 35 span-exposing headers lacking it (copy the `priced_surface_view.hpp:30-36` five-line pattern); thread-safety wording for the 38 headers lacking it, starting with `pricing_executor.hpp` (process-global singleton) and `listed_definitions_cache.hpp`.
- 4.8 Fix the stale "WHAT REMAINS" block at `dispersion_run.hpp:756-761` (falsified by the F-6 fix) — the codebase's convention is that these blocks are authoritative.

Exit criteria: python bindings + atx-ui compile against the new shape; umbrella test extended to compile Tier-A in a plain consumer TU; API diff documented in CHANGELOG.

---

## Sprint 5 — Packaging + host integration (est. 4-5 days)

Goal: a second consumer (atx-server) can build, link, observe, and stop the library.

- 5.1 **Install/export**: `install(TARGETS atx-vol EXPORT ...)`, `atx-volConfig.cmake` + version file, `install(DIRECTORY include/)`; resolve the `Result<T>`/`tl::expected` re-export story (the entire public error type is currently a FetchContent-vendored third-party type); decide the `atx::core` PUBLIC-link problem (today every consumer transitively needs Arrow/Parquet/Eigen/spdlog on the link line although no atx-vol public header includes any third-party header).
- 5.2 **ABI/ODR hygiene**: export macro (`ATX_VOL_API`) or explicit no-shared-libs policy; eliminate the `ATX_VOL_COUNTERS`/`ATX_VOL_PROFILE` PUBLIC-definition ODR trap (out-of-line storage or per-consumer-safe design); `#undef` or prefix-namespace the leaking macros in `counters.hpp`/`phase_profile.hpp`.
- 5.3 **Versioning**: single-source version (wire `project(VERSION)` → `version.hpp` → `version.cpp`; today two independent literals), add `ATX_VOL_VERSION` macro, write the API-stability policy, cut `CHANGELOG.md` 1.0.0. Unify archive-format naming (ATXVSA v2/v3 nomenclature currently contradicts itself across `vol.hpp`, CMake, and `surface_db.hpp`).
- 5.4 **Logging**: minimal sink injection (callback + level), route all 41 stderr/stdout writes in 13 src/ files through it. Default sink = current behavior.
- 5.5 **Cancellation**: cooperative `stop_token`-style check on the long-running entries (backtest step loop, corpus build, `calibrate_pool`, surface-db populate). Plumb through `RunConfig`/build inputs.
- 5.6 **Tools out of examples**: `ATX_BUILD_TOOLS` ON-by-default target for `atx-vol-surface-db-build`, `atx-vol-surface-db`, the dispersion backtest CLI (promoted from examples/, it is a 1,288-line production CLI); install them.
- 5.7 **Config surface registry**: document the 9 `ATX_*` env knobs (8 currently source-only) in README; decide which survive v1.
- 5.8 **Docs accuracy pass**: README test counts (claims 584, actual ~2,557 macros / 2,320 gated), 2 dead spec links, Python contract statement (no observer hook — `StepObserver` deliberately unbindable; publish the reduced contract or bind a progress callback).

Exit criteria: out-of-tree `find_package(atx-vol)` smoke consumer builds and runs a backtest; a host can capture all library output and cancel a running backtest.

---

## Sprint 6 — Performance + release gate (est. 4-5 days)

Goal: close the measured gaps, then freeze. Perf last because several items (LTO, association changes) can move FP bits — validated against the now-stable correctness baseline. Bit-parity policy per change documented in results.

Ranked by measured/estimated impact:
- 6.1 **Cold-path mmap prefetch**: `Mapping::prefetch()` (`atx-tsdb/src/mapping.cpp:135,192`) exists on both platforms, zero callers — call it (or touch referenced-record extents) in the async loader so payload pages stop faulting 4 KB-at-a-time inside the step loop. Directly attacks the 7.64s-cold vs 1.1s-warm gap.
- 6.2 **Prefetch depth**: hardwired depth-1 hides ~15% of I/O at ~56 ms load vs ~8 ms compute per session; use the existing `prefetch_depth` config (depth 2-3, private cache capacity = depth+2) (`src/backtest.cpp:1722,2262,56`).
- 6.3 **LTO**: no IPO/LTO anywhere; enable thin-LTO on `rel`/`rel-avx2` presets; expect mid-single-digit gains on the TU-boundary-heavy solve chain. Gate: bit-parity check vs non-LTO — if FP contraction moves bits, decide policy explicitly.
- 6.4 **AVX2 pack utilization**: packs broken per `(uid, side, raw-T-bits)` run → ~25% lane fill on dispersion books (1 strike per uid/side/expiry); accumulate across T-runs before flushing (`src/laned_greek_run.hpp:213-249`, `src/priced_surface_view.cpp:869-876`). Kernel unchanged.
- 6.5 **Dynamic partition for FullGreeks solve**: static contiguous `run_ranges` over sorted uniques hands one worker the long-dated tail (~40% parallel-region loss at 2x cost spread); `run_dynamic` is determinism-safe here (disjoint slot writes) (`src/portfolio_pricer.cpp:932-943`).
- 6.6 Medium batch: `StepMarkMemo` node-map clear/reinsert per step → dense generation-stamped vectors (the file's own documented pattern, `backtest.cpp:171-180` vs `:564-568`); `current_identity` ifstream open per load/prefetch on sealed archives (`snapshot_cache.cpp:103-119`); `resolve_universe_uids` recomputed twice per step over immutable input with O(N²) dup scan (`dispersion_strategy.cpp:237,369`); `uid_of` linear scan → sorted `lower_bound` (`backtest.cpp:1492-1505`); SVI LM scalar Black-76 loop → batch like eSSVI got (`svi_calib.cpp:607-629`); serial `reduce_*_totals` duplicating kernel work (`portfolio_pricer.cpp:1929-1965,1011-1042`).
- 6.7 Small: `BacktestResult` reserve sweep, O(n²) uid dedup, `cache_key` lexically_normal allocs, `kGreekChunk` 128→32.

Release gate (second half of sprint):
- Full test matrix: `rel`, `rel-avx2`, forcescalar leg, adversarial archive suite, python bindings suite.
- Bench suite vs pre-sprint-1 baseline; publish before/after for the 135-session backtest cold + warm.
- External-fixture decision: 134 `GTEST_SKIP` sites / 47 skipped in the gate hinge on the ~19 MB RunArchive e2e fixture — commit it, generate it in CI, or accept and document the skip set.
- Tag v1.0.0, cut CHANGELOG, final README pass.

---

## Out of scope for v1 (explicitly deferred, fail-closed today)

SpiderRock Parquet decoder (`data.cpp:558`), C8/CStar/Wing in `calibrate_pool`, SplineVol warm refit, the 6 deferred calibration research knobs (`calib.cpp:863-923`), `projection.cpp:424` `with_no_arb_check` (only true half-wired config field found — either implement or reject non-default at validation), Python `StepObserver` binding (document instead, 5.8).

## Estimated total

23-29 working days single-engineer; Sprints 1+2 are the critical path and poorly parallelizable (shared files); 3 through 5 parallelize across two tracks (build/packaging vs API) if staffed.
