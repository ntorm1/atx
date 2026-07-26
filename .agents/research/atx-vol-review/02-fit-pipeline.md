# atx-vol Fit Pipeline / Orchestration / Scheduling — Performance & Wiring Audit

Scope: the hot path that turns a board of quotes into a fitted surface at scale.
Read fully: `pricer_fitter.cpp` (2181 LOC), `fit_scheduler.{cpp,hpp}`, `calib_pool.cpp`,
`prepared_fitting.cpp`, `corpus_board_fit.{cpp,hpp}`, `surface_db_populate.cpp`,
`corpus.cpp` (scheduling regions), `fit_policy.cpp`, `session.cpp` (preset machinery),
`surface_policy.{hpp,cpp}`, `pricer_fitter.hpp`, `surface_db.{hpp,cpp}`.
Read-only audit. No build. All line numbers verified against current code.

Paths are absolute where cited in the summary; in-body cites use `src/…` relative to
`C:/atx/atx-vol/`.

---

## TOP 5 HIGHEST-LEVERAGE

1. **[HIGH] `FitPreset::Populate` (the C3 bulk lever) is dead on the populate hot path.**
   `symbol_config_from_preset` (`src/surface_db.cpp:433-437`) maps every preset — including
   Populate — to a v2 `SurfacePolicy{quality_mode, outputs=Risk}`. `pricer_config_for_symbol`
   (`src/surface_db_populate.cpp:44-47`) then engages `PricerConfig::quality_mode`/`outputs`,
   so `is_v2_request()` is TRUE for every populate board. In the v2 risk path, `apply_risk_policy`
   pins `in.deam.al_opts = al_default_opts()` for **all three** quality modes
   (`src/pricer_fitter.cpp:1094,1107,1084`), overwriting the `al_fast_opts` the Populate overlay
   just applied (`session_overlay` runs, then `apply_risk_policy()` runs after it,
   `src/pricer_fitter.cpp:1220-1225`). Result: every populate board pays the ~4-8× `al_default_opts`
   AL solve the Populate tier was built to avoid. `docs/al-preset-ladder.md` §0 confirms the intent
   and that the baked `pricing_.al_opts` is `al_default_opts`. FIX: honor the baked preset on the
   populate route (route Populate through the legacy single-surface path, or let a quality mode
   select `al_fast_opts` for the de-Am/cache lane).

2. **[HIGH] `calib_pool.cpp` is entirely dead production code.** `calibrate_pool`, `CadenceQueue`
   (tier/cadence min-heap refit scheduler), and `profile_tier_priority`-sharded fan-out have NO
   production caller — only `tests/calib_pool_test.cpp`. The corpus/populate hot path deliberately
   bypasses it (`src/corpus_board_fit.hpp:8`, `src/corpus.cpp:333`). The "multi-underlier
   calibration pool + refit-cadence scheduler" (`src/calib_pool.cpp:1-2`) and its tier ordering are
   a C-port stranded behind unit tests. FIX: wire it into a live universe refit driver, or delete it
   to remove the maintenance/attack surface.

3. **[MEDIUM] Cross-date warm-start cache chain (C2) not engaged on the production populate path.**
   `populate_surface_db` calls `fit_board(board, pc, nullptr, overlay)` with `out_caches = nullptr`
   (`src/surface_db_populate.cpp:313`), so no per-side correction cache is ever carried date→date.
   Only `build_corpus` uses the chain, and only when `cfg.warm_start_chain` is set — which defaults
   **false** (`include/atx/vol/corpus.hpp:319`). Doubly moot on the v2 route, which sets
   `use_correction_cache = false` (`src/pricer_fitter.cpp:1078`). FIX: engage a symbol-sharded warm
   chain in populate (mirror `src/corpus.cpp:577-607`) once the preset/cache override (finding 1) is
   fixed so the cache is actually built and used.

4. **[MEDIUM] Populate builds the heaviest possible fit per board.** Because the route is v2 Risk,
   each populate board runs ConvexDense QP + audited inversions + calendar `Project` repair + the
   full risk validation oracle (`src/pricer_fitter.cpp:1063-1121,1282-1329`). If the surface-DB
   consumer needs marks, this is a large over-compute vs the intended right-sized bulk fit. FIX:
   let the manifest request a mark-grade output for bulk populate; reserve risk-grade for symbols
   that need it.

5. **[MEDIUM] Incremental refit is wired into the API but no production driver uses it.**
   `PricerFitter::refit_expiry` / `refit_risk_slice` (`src/pricer_fitter.cpp:1483,1676`) are the
   surface-diff / incremental-refit primitives, but the only callers are tests and one bench — no
   backtest/populate/corpus loop invokes them (grep: `tests/pricer_fitter_test.cpp`,
   `bench/surface_v2_bench.cpp` only). A tick/replay backtest re-cold-fits whole boards. FIX: route
   the replay/tick loop through `refit_expiry` for single-expiry quote updates.

---

## FitPreset / tier / scheduler-mode WIRING TABLE

| Item | Defined | Selected/honored by a driver? | Verdict |
|---|---|---|---|
| `FitPreset::Fast` | session.hpp:219 | Legacy path honors it (`apply_fit_preset`); v2 only as a `quality_mode` fallback source — its AL opts overridden by `apply_risk_policy`. `select_fit_policy` picks it. | WIRED (legacy) / OVERRIDDEN in v2 |
| `FitPreset::Accurate` | session.hpp:222 | Same as Fast; `select_fit_policy` picks it for HtbDividendName. | WIRED (legacy) / OVERRIDDEN in v2 |
| `FitPreset::Robust` | session.hpp:226 | Default `PricerConfig`/`CorpusConfig` preset; legacy honors it; v2 Balanced ≈ it. | WIRED |
| `FitPreset::Hft` | session.hpp:230 | Legacy pinned-dense route AND the explicit v2 mark-build preset (LinearVariance, pricer_fitter.cpp:858,953). `select_fit_policy` picks it for index/dense-event. | WIRED |
| `FitPreset::Populate` | session.hpp:244 | Default `SymbolFitConfig::preset` (surface_db.hpp:122), but its home path (populate) forces v2 and overrides `al_fast_opts`→`al_default_opts`. Never chosen by `select_fit_policy`. Only honored via legacy `build_corpus` with an explicit `fit_template.preset=Populate`. | **DEAD on its default home path** |
| `FitQualityMode::Latency/Balanced/Accuracy` | surface_policy.hpp:23 | Drives `risk_preset` + `apply_risk_policy` in v2. | WIRED |
| `map_legacy_fit_preset` | surface_policy.hpp:198 | No explicit `Populate` case → `default:` = {Balanced, Risk}. Silently treats Populate as Robust-grade. | WIRED but Populate falls through |
| `FitSelectionMode::Auto/CrossValidated` | fit_policy.hpp:21 | Honored in `select_fit_policy` (fit_policy.cpp:217). Default Auto. | WIRED |
| `FitAffinity::None/PerformanceCores` | fit_scheduler.hpp:24 | `PerformanceCores` engaged by populate (`pin_outer_workers` default **true**, surface_db_populate.cpp:252-257) and by the warm-chain corpus branch (corpus.cpp:613). | WIRED |
| `run_bounded_fit_tasks` (bounded atomic-claim scheduler) | fit_scheduler.cpp:168 | Populate (surface_db_populate.cpp:336) + both corpus branches (corpus.cpp:577,615). | WIRED |
| `CadenceQueue` / `calibrate_pool` / tier-cadence scheduler | calib_pool.cpp | Tests only. | **DEAD** |
| C2 `WarmCacheExport` chain | corpus_board_fit.hpp:36 | `build_corpus` (opt-in, default off) only; populate passes nullptr. | PARTIALLY WIRED |
| `refit_expiry` / `refit_risk_slice` | pricer_fitter.cpp:1483,1676 | Tests + 1 bench only. | UNUSED by drivers |

---

## FINDINGS BY SEVERITY

### CRITICAL
None. The scheduler is deterministic and lock-free; no data races or fit-result
non-determinism across thread counts were found (see Correctness, positive notes).

### HIGH

**H1 — Populate preset AL knob overridden on the populate hot path (perf + unwired).**
`src/pricer_fitter.cpp:1084,1094,1107` (apply_risk_policy pins `al_default_opts`),
`:1220-1225` (overlay then re-assert), `src/session.cpp:921-939` (Populate = `al_fast_opts`),
`src/surface_db.cpp:433-437` (Populate→v2 Risk), `src/surface_db_populate.cpp:44-47` (engages
v2). Problem: the al_fast_opts that distinguishes Populate is set by the symbol-config overlay,
then immediately overwritten by `apply_risk_policy()` which runs AFTER the overlay. Impact: every
bulk-populate board pays the ~4-8× `al_default_opts` American solve on every de-Am inversion and
cache-miss cold mark — the exact cost the C3 sprint (`docs/al-preset-ladder.md`) created Populate
to eliminate; the claimed "economic parity vs Robust" is vacuous because the cheaper scheme is
never exercised. Fix direction: either route a Populate/mark request through the legacy
single-surface path (where `apply_fit_preset` is authoritative), or add a de-Am/cache AL tier to
`apply_risk_policy` that honors the baked preset for the inversion lane while keeping the risk
geometry accurate.

**H2 — `calibrate_pool` + `CadenceQueue` tier/cadence scheduler is dead.**
`src/calib_pool.cpp` (whole file), `include/atx/vol/calib_pool.hpp`. Problem: no production TU
references `calibrate_pool`, `CadenceQueue`, or `pop_due` (grep: only `calib_pool_test.cpp`). The
steady-state refit cadence scheduler and profile-tier sharding — the "tier" machinery the sprint
notes reference — are unreachable at runtime. Impact: dead compute surface, misleading "tier
scheduler present" impression, maintenance cost. Fix: wire into a live universe refit loop or
remove.

### MEDIUM

**M1 — C2 warm-start cache chain absent from `populate_surface_db`.**
`src/surface_db_populate.cpp:313` (nullptr out_caches), vs `src/corpus.cpp:577-607` (the working
chain). `include/atx/vol/corpus.hpp:319` (`warm_start_chain{false}` default). Impact: the
production populate path cold-builds correction caches for every (symbol,date) instead of carrying
a symbol's wide cache forward; measurable redundant AL work on multi-date runs. Compounded by H1
(v2 sets `use_correction_cache=false`, so the cache is neither built nor reused on the risk route).
Fix: add a symbol-sharded warm chain to populate after H1 restores cache building.

**M2 — Every populate board fits a full risk surface + oracle.**
`src/pricer_fitter.cpp:1056-1121` (risk preset + policy), `:1282-1329` (validate + admit),
`src/surface_db.cpp:435-437` (outputs=Risk). Impact: ConvexDense QP, audited inversions, calendar
`Project` repair, and the strike/butterfly/calendar validation grid run per board even when the
DB is consumed for marks. This is the single largest per-board cost multiplier and is a routing
choice, not a necessity. Fix: expose a mark-grade populate output; gate risk-grade to symbols that
require it.

**M3 — Incremental refit / surface diffing unused by any hot-path driver.**
`src/pricer_fitter.cpp:1483` (`refit_expiry`, eSSVI-only, NotImplemented otherwise `:1542-1546`),
`:1676` (`refit_risk_slice`). Impact: replay/tick backtests re-cold-fit whole boards where a
single-expiry quote delta could warm-refit one slice (the code path exists and is validated). Fix:
route the replay loop's per-expiry updates through `refit_expiry`; broaden beyond eSSVI.

### LOW

**L1 — Redundant board classification per board.** `src/corpus_board_fit.cpp:205`
(`retain_consumed_fit_parity` → `select_fit_policy`), `:97` (`collect_quality` →
`select_fit_policy` when decision absent), plus the call inside `PricerFitter::fit`. In the
qualified `build_corpus` path `select_fit_policy` (which runs `classifier_inputs_from_underlier`,
an O(quotes) board scan) executes ~2× per board. Fix: pass the fitter's `decision()` into
`collect_quality`/`retain_consumed_fit_parity` (already done for `collect_quality` when present),
avoid the pre-fit reclassification.

**L2 — `apply_risk_policy`/`configure_common`/`apply_fit_preset` re-applied 2-3× per v2 fit.**
`src/pricer_fitter.cpp:1060-1061,1121,1138-1140,1223`. Cheap scalar writes, negligible cost, but
the exact ordering fragility that already produced a documented near-miss bug (the merge note at
`:1206-1219` — overlay silently overwrote the risk contract). Fix: resolve inputs once; assert the
risk contract in a single place.

**L3 — `build_corpus` default (non-warm) branch does not pin to P-cores.** `src/corpus.cpp:615`
calls `run_bounded_fit_tasks(...)` with no affinity arg → `FitAffinity::None`, while the warm-chain
branch (`:613`) passes `PerformanceCores`. Impact: on a hybrid P/E host the default corpus build
spills onto E-cores (the exact regression finding-13 the pin fixes), only the opt-in warm path is
protected. Fix: pass `PerformanceCores` (gated on a config flag mirroring populate's
`pin_outer_workers`) on both branches.

**L4 — `map_legacy_fit_preset` has no `Populate` case.** `include/atx/vol/surface_policy.hpp:198-210`.
Populate falls to `default:` = {Balanced, Risk}. Serialization max is handled (`surface_db.cpp:312`),
but the silent Robust-equivalent mapping is the mechanism behind H1 and hides intent. Fix: make the
Populate case explicit and decide its quality contract deliberately.

---

## CORRECTNESS (positive verification + fragility notes)

- **Determinism across thread counts: PASS.** `run_bounded_fit_tasks` is a bounded atomic-claim
  queue (`src/fit_scheduler.cpp:198-212`); each task writes a disjoint indexed slot; results
  aggregated deterministically (calib_pool sorts by uid; populate keyed by board). P-core pinning
  only steers scheduling, never index→worker mapping — byte-identical by construction and asserted
  by existing gates (`SharedWorkerBudgetKeepsOutputByteIdentical`).
- **No lock contention on the fit fan-out.** No mutexes in the board-fit path; the per-symbol stats
  `std::map` is touched only in the serial drain (`src/surface_db_populate.cpp:362-394`).
- **Exception safety: PASS but note R-12 lineage.** Worker exceptions are caught per-task
  (`fit_scheduler.cpp:206-210`; `corpus_board_fit.cpp:344-353`; `calib_pool.cpp:231-237`). Populate
  streams each date's partition before the join (`surface_db_populate.cpp:396-405,415-429`) so a
  late worker throw cannot lose earlier durable dates.
- **Warm-start contamination: not observed.** The C2 chain re-anchors on freshly-built caches and
  the session stale-gate has final say (`src/corpus.cpp:600-606`, `src/session.cpp:1038`); refit
  clones (`clone_for_refit`/`clone`) isolate candidates before publish
  (`src/pricer_fitter.cpp:1590,1789`).
- **Fragility:** L2's repeated policy re-assertion is the live risk — the v2 input resolution
  depends on `apply_risk_policy()` running strictly last, enforced only by call ordering.

## FEATURE GAPS (SOTA throughput)

- Incremental refit exists but is unused by drivers (M3); no board-level surface diffing in the
  backtest loop.
- No SIMD board-fit batching at the orchestration layer; `simd/*_batch.hpp` and
  `shared_boundary_deam_batch` (`src/prepared_fitting.cpp:413`) operate per-slice inside a board,
  not across boards.
- Scheduler is fork-join per driver call (jthreads created/destroyed each `run_bounded_fit_tasks`),
  not a persistent pool — fine for the one-shot populate/corpus call, but a per-tick refit loop
  would pay thread-spawn cost each call; there is no work-stealing (the atomic-claim queue + LPT
  ordering, `surface_db_populate.cpp:202-236`, gives greedy load balance instead — adequate).
