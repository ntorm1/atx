# atx-vol — Fitting + Backtesting SOTA Sprint (2026-07-19)

> ## ✅ ACCEPTANCE (2026-07-19, complete)
> **Gate:** `atx_vol` label **1865/1869 pass**. The 4 remaining failures — `SurfaceV2Provenance.ValidationFallbackAdmission...` (#308), `PricerFitterTest.LocalRiskRefit...` (#1755), `SurfaceV2Qualification...InversionBudgets/{Latency,Balanced}` (#1869/#1870) — were **each independently rebuilt-and-confirmed failing at the merge-base `main` 99f332f**. **Zero sprint-introduced gate regressions;** `feat/bt-sota` is one *better* than the base (synced main's #1804 golden re-pin).
> **Reproducibility:** dispersion golden **`final_nav = 247.4065016443293` byte-identical** before→after the entire sprint (default Auto ISA).
> **Delivered:** WS-C static-arb SVI rejection (HIGH) + wide-smile fit RMSE 3.5e-5→4.6e-10 · WS-B `record_every_n` metric-corruption fix (HIGH) + stride-invariant stats · WS-K/WS-H AVX2 American **marks 2.5–3.1×** + **greeks 3.84×** validated, gated **ForceAvx2** (Auto stays scalar + cross-host bit-reproducible, mirroring the greeks contract) · WS-H thread-count determinism restored + workspace reuse + two-pass fusion verified already-realized · WS-F populate al_fast tier (~3–5× less AL work/seed, Populate-only) + `calib_pool` quarantined · CRLF universe-header robustness fix.
> **WS-S (surface-load mmap) — DONE + LANDED.** The dispersion bench was load-bound; `SurfaceArchiveV2::open_mapped` (mmap via `atx::tsdb::Mapping` on the read-only backtest path only — mmap kept off the read-write SurfaceDb store because a resident Windows mapping holds the file open) + single-probe subset load cut **`archive_open` ~23× (283.7→11.5 ms)**, **`snapshot_load` −33% (373→249 ms)**, **`backtest_total` −10% (420→379 ms)**. Golden NAV byte-identical; the remaining wall is now pricing-bound. S1 (serve `PricedSurfaceView` to the pricer) deferred — crosses WS-H's `portfolio_pricer` ownership; mmap already captured the read win (67% of load).
> **LANDED ON LOCAL `main` @ 8627ccb** (both-way sync with WS-G done). 4 pre-existing reds (#308, #1765, #1879/80) are unrelated main debt, documented, left for owner review.

**Branch:** `feat/bt-sota`  ·  **Worktree:** `C:/atx-wt/wt-bt-sota`  ·  **Base:** local `main` @ 99f332f
**PM:** primary session (context-preserving).  **Implementers:** Opus 4.8 subagents, one per workstream.
**North star:** the fastest and most robust options backtesting platform.
**Speed benchmark:** vega-flat SPY dispersion backtest (`atxvol_spy_dispersion_backtest`), data at `C:/atx-data/spy-dispersion/`.

Full review evidence: `C:/atx/.agents/research/atx-vol-review/00-SYNTHESIS.md` (+ `01`–`06` per-area reports).
The review was 6 parallel scoped audits (fit-core, fit-pipeline, surface-store, backtest-engine, pricing-hot-path, dispersion+gaps). Headline: **no critical numerical or look-ahead bug**; the wins are (a) a large amount of **validated-but-unwired performance** on the pricing hot path, and (b) two **HIGH correctness landmines** (silent metric corruption under downsampling; servable static-arb SVI smiles).

---

## 0. Why this sprint (evidence-ranked)

### Performance levers ("how fast are we" on the dispersion bench). American reprice dominates the hot loop.
| # | Lever | Evidence | Est. impact |
|---|-------|----------|-------------|
| P1 | **Production Auto-ISA uses NO AVX2 for American marks OR greeks** — `kShipAvx2Boundary=false`, `kShipAvx2Greeks=false`; greeks AVX2 also unwired | 05 | Largest single lever |
| P2 | FullGreeks risk loop is **scalar per-contract**; laned `american_greeks_batch` (american_batch.cpp:257) is tests/bench only | 05 (priced_surface.cpp:1009-1036) | ~4× on risk loop |
| P3 | **First-order greek tier unwired** — delta-only cadence pays the full 2nd-order bundle (3-layer gap) | 05 (portfolio_pricer.hpp:244; priced_surface.cpp:794-810,638) | 80–94% greek-solve waste on hedge cadence |
| P4 | **Two full-book American passes per step** recompute base greeks twice (Execution FullGreeks + StepPnl) | 06 (backtest.cpp:1505 & :786) | ~2× base-greek work |
| P5 | `FitPreset::Populate` fast-AL opts clobbered by `apply_risk_policy` → every populate board pays 4–8× AL solve | 02 (pricer_fitter.cpp:1084-1225) | populate throughput |
| P6 | `price()`/`pnl_explain()` convenience wrappers rebuild PreparedPortfolio + realloc every call | 05 (portfolio_pricer.cpp:1181,1670) | per-bar API reuse |
| P7 | Zero-copy `PricedSurfaceView` + mmap unwired; `MarketSnapshot::load` owned-reconstructs whole partition/date | 03,06 (backtest.cpp:968-1041) | load-path + RSS |

### Correctness landmines
| # | Bug | Evidence | Severity |
|---|-----|----------|----------|
| C1 | `record_every_n>1` silently corrupts Sharpe / ann_return / ann_vol / hit_rate / attribution (nav stays right, no warning) | 04 (backtest.cpp:1325/1332/1792; tearsheet.cpp:79-132) | HIGH |
| C2 | SVI butterfly violations **tallied but never projected/rejected**; `calib_pool` serves `Svi` surfaces directly → static-arb smiles servable | 01 (svi_calib.cpp:1417-1420; calib_pool.cpp:181) | HIGH (bounded by whether Svi is served without `fit_slice_curve`) |
| C3 | American early-exercise/assignment not modeled; synthetic-tenor lots can't settle | 04 (backtest.cpp:707-768; strategy.cpp:50-71) | MED |

**Positives to preserve:** look-ahead is CLEAN; v2 (ATXVSA2) archive round-trip is byte-deterministic and sound; adjoint-vs-FD greeks, masked-lane AVX2, and all fit Jacobians are hand-verified correct. **Do not regress these.**

### Unwired / dead (user explicitly asked)
U1 `calib_pool.cpp` (`calibrate_pool`+`CadenceQueue`+cadence scheduler) test-only · U2 CStar/C16M calibrator test-only · U3 AVX2 American kernels dark+unwired · U4 `EvalField::Delta/Vega`+`american_delta`+K4 `need_*` selectors never forwarded · U5 `refit_expiry`/`refit_risk_slice` no prod caller · U6 `FitPreset::Populate` al_fast clobbered · U7 `SurfaceDb::map_surface`/S5 LRU/`open_borrowed` test-only · U8 `retain_position_frames` dead · U9 interval/band loss + joint/TS eSSVI ρ NotImplemented.

---

## 1. Execution model (READ FIRST — every implementer)

**Repo build facts (Windows, clang-cl + Ninja, MSVC env required):**
- Correctness gate preset = **`dev`** (Debug, `build/`).  Perf preset = **`rel-avx2`** (`build-rel-avx2/`).
- Build/test ONLY through the worktree's own `scripts/atx-build.ps1` **by absolute path**, standing in that worktree (a wrong-tree guard refuses otherwise). It sources `vcvars64`, puts VS-bundled Ninja + `mt.exe` on PATH.
- `configure` verb hardcodes the `dev` preset. For `rel-avx2` or extra `-D` flags use the raw pass-through (`atx-build.ps1 --preset rel-avx2 -D...`). **Quote every `-D...=C:/...` arg** (PowerShell splits at the drive colon otherwise).
- Isolated deps per worktree: pass `-DFETCHCONTENT_BASE_DIR=C:/atx-wt/<wt>/deps/<preset>` at EVERY configure.
- Examples/benchmarks are gated: add `-DATX_BUILD_EXAMPLES=ON` (dispersion bench) and `-DATX_BUILD_BENCH=ON` (google-bench); numbers need `-DATX_VOL_PROFILE=ON -DATX_VOL_COUNTERS=ON`.

**Canonical commands (substitute your worktree path):**
```
# configure dev (test gate):
Set-Location C:\atx-wt\<wt>
& C:\atx-wt\<wt>\scripts\atx-build.ps1 configure          # dev preset, build/
# build a target:
& C:\atx-wt\<wt>\scripts\atx-build.ps1 build <target>
# run tests by regex:
& C:\atx-wt\<wt>\scripts\atx-build.ps1 -Ctest -R <regex>
```

**Git discipline (per workstream):**
1. Work in the assigned worktree/branch ONLY. Never touch `C:/atx` live tree.
2. Small, conventional commits (`feat(vol):` / `fix(vol):` / `perf(vol):` / `refactor(vol):` / `test(vol):`). End every commit body with the Co-Authored-By trailer the harness requires.
3. Keep changes inside your file-ownership set (below). If you must touch a shared file, touch only your named sections/line-ranges.
4. Before returning: your gate MUST be green on `dev`. Return a compressed report (what changed, files, gate result, any follow-up) — NOT a file dump.
5. Do NOT merge to `feat/bt-sota` yourself. PM integrates.

**Determinism rule:** any perf change must produce **bit-identical** PnL/greeks vs the pre-change build on the dispersion smoke (double-run + compare). If a change alters numerics, it is a bug unless explicitly a correctness fix with a new golden.

---

## 2. Workstreams

Dependency graph: `WS-K, WS-F, WS-C` (Wave 0, parallel, disjoint) → merge → `WS-H` (Wave 1, needs WS-K kernel) → `WS-B` (Wave 2, after WS-H settles backtest.cpp).  `WS-S` = stretch/backlog.

### File-ownership map (no two concurrent WS share a file)
- **WS-K:** `src/simd/american_boundary_avx2*.{cpp,hpp}`, `american_boundary_avx2_kernel.hpp`, `american_greeks_avx2.{cpp,hpp}`, `src/simd/cpu.cpp`, `include/atx/vol/simd/*`, the `kShipAvx2Boundary`/`kShipAvx2Greeks` flag site(s), `src/american_batch.cpp` (dispatch only).
- **WS-F:** `src/pricer_fitter.cpp`, `src/calib_pool.cpp`+hpp, `src/surface_db_populate.cpp`, `src/surface_db.cpp` (config path only), `src/corpus.cpp`, `src/fit_scheduler.cpp`, `include/atx/vol/{fit_policy,surface_policy,prepared_fitting}.hpp`.
- **WS-C:** `src/svi_calib.cpp`, `src/calib.cpp`, `include/atx/vol/calib.hpp`.
- **WS-H (Wave 1):** `src/priced_surface.cpp`+hpp, `src/portfolio_pricer.cpp`+hpp, `src/pricing_executor.cpp`, `src/backtest.cpp` (pricing/fusion sections), `include/atx/vol/priced_surface_view.hpp`.
- **WS-B (Wave 2):** `src/backtest.cpp` (metrics/record sections), `src/tearsheet.cpp`, `src/run_report.cpp`, `src/strategy.cpp`, `include/atx/vol/backtest.hpp`.
- **WS-S (stretch):** `src/surface_archive.cpp`, `src/surface_db.cpp`, `src/priced_surface_view.cpp`, `src/snapshot_cache.cpp`, `src/backtest.cpp` (`MarketSnapshot::load`).

---

### WS-K — Activate + validate AVX2 for American marks & greeks  [P1, U3]
**Goal:** default Auto-ISA production dispatches AVX2 for American *marks* and *greeks*, gated by parity vs the scalar reference, so the dispersion reprice loop stops running scalar.
**Findings to action:** 05 P1 (kShip flags false), 05 P2/U4 (laned greeks kernel exists, tests-only). See `05-pricing-hotpath.md`.
**Approach:**
1. Locate `kShipAvx2Boundary` / `kShipAvx2Greeks` (grep). Understand WHY they were shipped `false` (git log/blame the flag site + any note in `docs/simd_fastpath.md`). If they were false pending a parity validation that now exists, flip after re-proving parity. If false due to a known numerical gap, FIX the gap first.
2. Prove parity: for a representative grid (strikes × expiries × moneyness, incl. deep wings, near-expiry, high/low vol), assert AVX2 marks and each greek match scalar within the tolerances already used by `american_batch_test`/`adjoint_greeks_test`. Add/extend a parity test if coverage is thin.
3. Ensure Auto-ISA (`SimdIsa::Auto` + `cpu.cpp` detection) selects AVX2 at runtime when the CPU supports it, not just `ForceAvx2`.
4. Confirm `american_greeks_batch`/`american_put_greeks_batch_avx2` is production-callable and document its exact signature + preconditions in your return (WS-H consumes it).
**Gate (dev):** `-R "American|american_batch|AdjointGreeks|Greeks"` green; new/updated parity test green.
**Perf check (rel-avx2):** a microbench (existing `chain_pricer_bench` or `american_iv_bench`, or a tiny added loop) shows AVX2 marks+greeks faster than scalar on this host. Report ns/op both ways.
**Risk:** HIGH (numerics). Parity gate is non-negotiable. If any greek can't reach tolerance on AVX2, ship marks-AVX2 only and leave that greek scalar with a TODO + reason.

### WS-F — Fit/populate throughput + dead-code  [P5, U1, U6, P8]
**Goal:** populate stops overpaying AL solves; dead scheduler code is wired or removed; no numeric change to served surfaces.
**Findings:** 02 (H1 Populate al_fast clobber, H2 calib_pool dead, M3 warm-start off, M5 refit uncalled). **First reconcile the 02↔03 tension:** 03 says preset/al_override ARE conveyed via `pricer_config_for_symbol`+`apply_symbol_config` (surface_db_populate.cpp:310-315); 02 says `apply_risk_policy` then pins `al_default_opts()` (pricer_fitter.cpp:1084-1107) AFTER the overlay. **Read both paths and establish the truth before editing.**
**Approach:**
1. F1 [P5/U6]: Make the Populate path honor fast-AL opts when the quality mode permits (or add a de-Am AL tier keyed on the preset) so populate doesn't pay full audited AL per board. Verify served-surface bytes are UNCHANGED where quality mode is unchanged (determinism), and quantify solve-count drop.
2. F2 [U1]: `calib_pool.cpp` — decide wire vs quarantine. If no production consumer is justified this sprint, move it behind a clearly-labeled test-only TU / `ATX_VOL_EXPERIMENTAL` guard and delete the dead exports, OR wire `calibrate_pool` into a real driver if cheap. Document the decision.
3. F3 [U6]: add the missing `Populate` case to `map_legacy_fit_preset` (surface_policy.hpp:198) so it stops silently defaulting to `{Balanced,Risk}`.
4. F4 [P8]: engage the C2 warm-start cache in `populate_surface_db` (currently `out_caches=nullptr`, surface_db_populate.cpp:313) for cross-date reuse — OR document why it's unsafe/off. Determinism must hold.
5. F5 [L1/L2]: hoist the redundant `select_fit_policy` (corpus_board_fit.cpp:97,205) and repeated `apply_risk_policy`/`configure_common` (respect the documented ordering near pricer_fitter.cpp:1206).
**Gate (dev):** `-R "CalibPool|CurveFitParallel|SurfaceDbPopulate|Corpus|FitScheduler"` green; served-surface determinism check unchanged.
**Perf check (rel-avx2):** populate a small universe slice before/after; report boards/sec + mean AL solves/board.

### WS-C — Fit correctness: kill servable static-arb SVI  [C2, under-fit, robustness]
**Goal:** no static-arb SVI smile can be served; wide/skewed smiles fit to intended depth.
**Findings:** 01 (H1 butterfly tally-not-reject, M5 NM cap→12, IRLS non-robust scale). See `01-fit-core.md`.
**Approach:**
1. C-1 [C2]: FIRST prove the exposure — does any path serve a `Parametrization::Svi` `VolSurface` without going through `fit_slice_curve`'s gating (calib_pool.cpp:181)? If yes, move `svi_project_mm` + butterfly re-check/skip into that driver so a violating smile is repaired or dropped, never served. Add a regression test that feeds an arb-inducing quote set and asserts the served smile is butterfly-arb-free (reuse `arb_test`/`curve_noarb_test` machinery).
2. C-2: raise the quasi-explicit Nelder-Mead cap so it reaches the intended ~200 (svi_calib.cpp:787-788) instead of collapsing to `max_inner_iter`=12; confirm no runtime blowup on the fit bench.
3. C-3: switch SVI IRLS scale to the q90 robust scale used elsewhere (svi_calib.cpp:837) so outliers don't inflate the Huber scale.
**Gate (dev):** `-R "Svi|Calib|CalibRobust|Arb|NoArb|CurveNoArb"` green; new arb-rejection regression green. Existing fit-quality goldens must not regress (tighten only).
**Out of scope this sprint (backlog):** interval/band loss objective [G8], joint/TS eSSVI ρ.

### WS-H — Pricing hot-path: greek-tier + laned dispatch + pass fusion  [P2, P3, P4, P6]  (Wave 1)
**Goal:** the per-bar price+risk loop computes only the greeks requested, once, through AVX2 batch kernels. This is the sprint's throughput centerpiece.
**Depends on:** WS-K (production `american_greeks_batch`) merged into `feat/bt-sota`.
**Approach:**
1. H1 [P2]: route the FullGreeks risk loop (priced_surface.cpp:1009-1036) through the laned `american_greeks_batch` instead of per-contract scalar. Parity vs current within adjoint/FD tolerance.
2. H2 [P3]: implement first-order greek tier end-to-end — (a) add a first-order request to `PriceFieldMask` (portfolio_pricer.hpp:244) or a granular greek mask on `PriceOptions`; (b) make `EvalField::FirstOrder` actually narrow vs `SecondOrder` (priced_surface.cpp:794-810); (c) forward `EvalField`→`american_greeks_al(need_vega,need_rho,need_charm)` need_* selectors (priced_surface.cpp:638; american.hpp:415-425). Prove a delta-only request skips 2nd-order solves (counter/ledger).
3. H3 [P4]: fuse the two full-book American passes per step so base-surface greeks are computed once and shared between Execution-FullGreeks (backtest.cpp:1505) and StepPnl (:786). Bit-identical PnL required.
4. H4 [P6]: give `price()`/`pnl_explain()` a reuse path (cache/rebuild PreparedPortfolio + workspace only on book change) for per-bar callers.
5. H5: honor `resolved_price_isa` in the P&L path instead of hardcoded `SimdIsa::Auto` (portfolio_pricer.cpp:1288-1307).
**Gate (dev):** `-R "Backtest|PortfolioPricer|AdjointGreeks|PricingExecutor|BacktestExec"` green; determinism double-run bit-identical.
**Perf check (rel-avx2):** dispersion benchmark end-to-end throughput before/after (PM supplies baseline); PnL byte-identical.

### WS-B — Backtest correctness + reporting  [C1, U8, G7]  (Wave 2)
**Goal:** downsampled runs report correct risk metrics; dead field gone; benchmark-relative stats added.
**Approach:**
1. B1 [C1]: fix `record_every_n>1` metric corruption. Either (preferred) accumulate per-step pnl/attribution across skipped steps into the recorded row so Sharpe/ann/attribution are computed on the true per-step series, or hard-guard: refuse metric-dependent report output when stride>1. Add a regression test: same run at stride 1 vs stride k must yield identical annualized stats (with block-summing).
2. B2 [U8]: remove or wire `retain_position_frames`.
3. B3 [G7] (if time): benchmark-relative stats (beta/alpha/IR vs a supplied benchmark series) + tidy per-leg/factor attribution in the tearsheet.
**Gate (dev):** `-R "Backtest|TearSheet|BacktestReal|RunReport"` green; new stride-invariance regression green.

### WS-S — Surface zero-copy views + mmap  [P7, U7]  (STRETCH / backlog)
Route production `MarketSnapshot::load` through zero-copy `PricedSurfaceView` + `open_borrowed`/mmap (backtest.cpp:968-1041; surface_archive.cpp:1860-1865); fix subset-load double-probe (backtest.cpp:1014-1018). Contends `backtest.cpp`/`surface_db.cpp` — schedule AFTER WS-H+WS-B. Defer unless Wave 0/1/2 land with time to spare.

---

## 3. Integration & acceptance (PM)
1. Merge order: WS-K → WS-C → WS-F (Wave 0) into `feat/bt-sota` (`--no-ff`), gate `dev` green after each.
2. Dispatch WS-H on `feat/bt-sota`; gate + determinism double-run; capture rel-avx2 dispersion throughput delta vs baseline.
3. Dispatch WS-B; gate + stride-invariance regression.
4. Final: full `atx_vol_fast` label suite green on `dev`; dispersion benchmark PnL byte-identical to baseline with improved throughput; write acceptance note (baseline vs final ns/op + boards/sec + reprice throughput).
5. Leave `WS-S` + backlog (G1–G9 feature gaps, interval-loss objective, intraday backtest) documented for the next sprint.

## 4. Baseline (ESTABLISHED 2026-07-19) — full record in `.agents/research/atx-vol-review/BASELINE.md`
- Target: `atxvol_spy_dispersion_backtest` (rel-avx2).
- **Working recipe** (Git Bash, prebuilt exe, no MSVC env):
  ```
  EXE=/c/atx-wt/wt-bt-sota/build-rel-avx2/bin/atxvol_spy_dispersion_backtest.exe
  $EXE build-corpus --spec C:/atx-data/spy-dispersion/scratch-bt-sota/run_spec.tsv --out C:/atx-data/spy-dispersion/runs/bt-sota-baseline
  $EXE run-surface-backtest --run C:/atx-data/spy-dispersion/runs/bt-sota-baseline
  ```
  Two INPUT fixes were required (no source edited; scratch inputs at `C:/atx-data/spy-dispersion/scratch-bt-sota/`): (1) universe TSV must be **LF** (CRLF header is rejected — see robustness bug below); (2) drop `occ_ess_root` from the spec (occ-ess covers only 3 dates and is not read by run-surface-backtest).
- Corpus: `runs/bt-sota-baseline` — admitted=902, 85 date archives, 82 backtest steps.
- **GOLDEN PnL (assert byte-identical after every perf change):** `final_nav = 247.4065016443293` (entry NAV 0; "surface-only projected backtest complete: dates=82"). Held identical across 4 runs.
- Throughput anchor (host-insensitive): 82 steps × 22 lots = **1,804 lot-repricings** (11 straddles × 2 legs). Wall (PROVISIONAL, host busy): best 0.66 s, median 0.76 s ⇒ ≈2,700 lot-repricings/s, ≈123 steps/s.
- NOTE for clean acceptance numbers: rebuild rel-avx2 with `-DATX_VOL_PROFILE=ON -DATX_VOL_COUNTERS=ON` (the baseline exe lacked them, so per-op counters were unavailable). Measure final delta on a QUIET host (P-core lease).

## 5. Robustness quick-wins found during baseline (backlog / opportunistic)
- **CRLF universe header rejected**: `read_universe` (dispersion_workflow.cpp:181) compares the header via `substr(0, first_end)` keeping a trailing `\r`; CRLF-header universe files fail "bad universe schedule header" while data rows are CR-stripped. One-line strip fix. (Not in any Wave-0 ownership set — PM or a follow-up handles it.)
