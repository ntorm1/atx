# atx-vol Fitting + Backtesting — Master Review Synthesis

PM-collected compressed summaries from 6 scoped review agents. Full reports in sibling files 01–06.
Goal: fastest + most robust options backtesting platform. Review dims: correctness / perf / unwired / gaps.

---

## 02 — FIT PIPELINE (perf + wiring)  [report: 02-fit-pipeline.md]
Counts: Crit 0 · High 2 · Med 3 · Low 4.

TOP:
- **[H] FitPreset::Populate al_fast_opts DEAD on populate hot path.** `symbol_config_from_preset` (surface_db.cpp:433) + `pricer_config_for_symbol` (surface_db_populate.cpp:44) force v2-Risk; `apply_risk_policy` pins `al_default_opts()` for ALL quality modes (pricer_fitter.cpp:1084/1094/1107) AFTER overlay applies Populate (pricer_fitter.cpp:1220-1225). Every board pays ~4-8× AL solve C3 was built to avoid. Fix: route Populate via legacy path or add de-Am AL tier honoring preset.
- **[H] calib_pool.cpp (`calibrate_pool`, `CadenceQueue`, tier/cadence refit scheduler) NO production caller** — tests only. Wire or delete.
- [M] C2 warm-start cache chain NOT engaged in `populate_surface_db` (out_caches=nullptr, surface_db_populate.cpp:313); only build_corpus uses it, default-off (corpus.hpp:319). Cross-date reuse missed.
- [M] Populate builds heaviest fit (ConvexDense QP + audited inversions + Project repair + risk oracle, pricer_fitter.cpp:1063-1329) even when consumer needs only marks.
- [M] Incremental refit (`refit_expiry`/`refit_risk_slice`, pricer_fitter.cpp:1483/1676) wired to API+tests/1 bench only; no backtest/populate driver calls it → replay re-cold-fits whole boards.

DEAD/UNWIRED:
- calibrate_pool + CadenceQueue + tier-cadence scheduler — test-only.
- FitPreset::Populate al_fast distinction — unreachable on default home (populate→v2 override).
- refit_expiry / refit_risk_slice — no prod caller.
- `map_legacy_fit_preset` missing `Populate` case → silent default {Balanced,Risk} (surface_policy.hpp:198).
- In any v2 request, FitPreset AL/iv_tol/n_atm/calendar knobs overridden by quality_mode+apply_risk_policy.

LOW: L1 redundant select_fit_policy 2×/board (corpus_board_fit.cpp:97,205); L2 apply_risk_policy re-run 2-3×/fit, ordering-fragile; L3 build_corpus non-warm branch omits PerformanceCores affinity (corpus.cpp:615); L4 Populate mapping fall-through.

WIRED+CORRECT: run_bounded_fit_tasks bounded atomic-claim scheduler + FitAffinity::PerformanceCores P-core pinning (populate default-on). Deterministic across thread counts. Hft honored.

---

## 06 — DISPERSION BENCHMARK + CROSS-CUT GAPS  [report: 06-dispersion-and-gaps.md]
- **Vega-flat math CORRECT** (dispersion.cpp:488-496; strategy.cpp:724-735 FlatVega). Neutrality preserved under DropRenormalize.
- Benchmark shape: fit in `build-corpus` (HFT preset → .atxvsa); `run-surface-backtest` reloads + does **price+risk only** (full-greek path runs, cfg.price backtest.cpp:1771, NOT short-circuited). "How fast" = price+risk throughput on cached fits.
- **[HIGH perf] Two full-book American-pricing passes per step**: Execution FullGreeks (backtest.cpp:1505, risk+hedge) + StepPnl pnl_totals (:786) both solve same base surfaces → base greeks computed TWICE. **Fusing = biggest lever.**
- [Med/High perf] SnapshotLoad maps ALL universe surfaces per date, prices only traded legs; PricedSurfaceViews (zero-copy) seam noted (:995) but NOT wired.
- [Med wiring] `verify` needs reference_reconciliation.tsv written only by tools/reference_spy_dispersion.py:389 (no C++ writer) — cross-lang gate, doesn't block surface bench.

DEAD-CODE/TODO: 12 markers, ZERO true dead stubs. Mostly fail-closed guards: calib.cpp:864/874/880/885/889/923 (6× "not implemented" rejecting unsupported eSSVI/loss/butterfly/residual opts), vol_curve.cpp:731 (SplineVol local refit→Err), session.cpp:1156-1207 placeholder eSSVI VolSurface (unused not dead).

RUN RECIPE (vega-flat surface benchmark):
- Target `atxvol_spy_dispersion_backtest` (needs -DATX_BUILD_EXAMPLES=ON; +`-DATX_VOL_PROFILE=ON -DATX_VOL_COUNTERS=ON` for numbers).
- Populate REQUIRED first: `build-corpus --spec atx-vol/examples/spy_dispersion_run_spec.tsv --out C:/atx-data/spy-dispersion/runs/surface-bench` then `run-surface-backtest --run C:/atx-data/spy-dispersion/runs/surface-bench`.
- Data: opra_root=C:/atx-data/spy-dispersion/opra (present). FAST PATH: reuse prebuilt runs/* (archives+manifest+run_spec+universe) → skip populate.
- Strangle drivers mag7_dispersion_backtest --db / spy_dispersion_pnl --db run off SurfaceDb (no OPRA step).

TOP 8 FEATURE GAPS: (1) intraday/multi-snapshot backtest (only 1 min/day now); (2) ship the 7 guarded calibrator features; (3) risk limits + margin/capital + drawdown stop; (4) market-impact/liquidity costs + borrow term structure; (5) parameter/scenario sweep harness; (6) official index reconstitution weights + corp-action remap (GOOG/GOOGL hand-coded); (7) per-leg/factor attribution + native tearsheet plots; (8) live/paper parity (no OMS/fill-sim).

---

## 05 — PRICING HOT PATH (perf + greek tier)  [report: 05-pricing-hotpath.md]
Counts: Crit 0 · High 3 · Med 4 · Low 2. No correctness defects (determinism/masked-lane/adjoint-vs-FD sound).

TOP:
- **[HIGH] FullGreeks risk loop is SCALAR per-contract** (priced_surface.cpp:1009-1036 falls out of vectorized arm into per-entry american_greeks_fd/al). Laned AVX2 kernel (american_greeks_batch / american_put_greeks_batch_avx2, american_batch.cpp:257) called ONLY by tests/bench. **~4× unrealized on per-bar risk loop.** Fix: dispatch group through it.
- **[HIGH] First-order/hedge tier UNWIRED** — delta-only cadence pays full 5-solve(analytic)/17-solve(FD) bundle. **~80-94% greek-solve waste.**
- **[HIGH] EvalField::Delta/Vega + american_delta (1-2 solves) complete at surface but DEFERRED**, no portfolio caller (priced_surface.hpp:245-249). Fix: request EF::Delta for hedging.
- [MED] price()/pnl_explain() wrappers alloc fresh workspace + rebuild PreparedPortfolio EVERY call (portfolio_pricer.cpp:1181,1670) — per-bar backtest on convenience API loses all reuse.
- [MED] Adjoint route scalar + re-`resolve` per contract (portfolio_pricer.cpp:725,727); P&L path hardcodes SimdIsa::Auto, dropping resolved_price_isa (:1288,1298,1307).

GREEK-TIER VERDICT:
- Marks-only = HONORED (want_greeks gates EF::Iv|Price only; b_greeks sized 0; marks eligible for AVX2 price-batch).
- First-order-only = IGNORED (3-layer gap): (a) PriceFieldMask binary Marks/FullGreeks — no first-order bit (portfolio_pricer.hpp:244); (b) EvalField::FirstOrder vs SecondOrder collapse to SAME full bundle (priced_surface.cpp:794-810); (c) K4 selectors american_greeks_al(need_vega,need_rho,need_charm) exist + ledger-proven (american.hpp:415-425) but greeks_resolved calls defaults=true — **never maps EvalField→need_*** (priced_surface.cpp:638). laned-greeks.md:94-101 confirms L4 last-mile unfinished.

UNWIRED KERNELS/KNOBS: american_greeks_batch + american_put_greeks_batch(_avx2) (tests/bench only); EvalField::Delta/Vega + SoA cols (DEFERRED); FirstOrder/SecondOrder dead distinction; american_greeks_al K4 selectors (never forwarded); american_delta cheap route (portfolio never asks); **kShipAvx2Boundary=false (marks AVX2 dark, ForceAvx2-only) + kShipAvx2Greeks=false (greeks AVX2 dark AND unwired). NET: default Auto-ISA production uses NO AVX2 for American marks or greeks.** (The 1.6-1.95× AVX2 wins in simd_fastpath.md are Black-76 European kernels for fitting/IV, not this path.)
EXTRA GAPS: no dirty-leg incremental reprice; greek packs never cross uids (thin-book packs unfilled); AVX2-only (no AVX-512/8-lane); fat AoS ContractPx (~104B) gathered per position.

---

## 04 — BACKTEST ENGINE (correctness)  [report: 04-backtest-engine.md]
Counts: Crit 0 · High 1 · Med 4 · Low 4.
**LOOK-AHEAD VERDICT: CLEAN.** Every decision+fill at current snapshot ts; forward PnL = previously-decided book repriced onto next snapshot. nav = exact reprice MTM (not Taylor). Cash+MTM reconcile; frictions/settlement hit cash once + nav once. NO critical PnL bug.

TOP:
- **[HIGH] record_every_n>1 silently corrupts per-step-derived metrics**: nav accumulates every step but pnl_*/attribution stored only recorded step → Sharpe/ann_return/ann_vol/hit_rate/attribution-sums/avg_daily_pnl computed on 1-in-stride sample = WRONG, no warning (nav.back()/total_return/max_dd stay correct). backtest.cpp:1325/1332/1792/1796; tearsheet.cpp:79-132; run_report.cpp:195.
- [MED] American early-exercise/assignment NOT modeled — held+MTM'd at American marks then settled European-intrinsic; short assignment risk absent. backtest.cpp:707-768.
- [MED] Synthetic-tenor lots can't settle: needs EXACT snapshot-ts match but DeclarativeStrategy sets expiry=base_ts+round(T·yr); HoldToExpiry aborts NotFound. strategy.cpp:50-71,834; backtest.cpp:707.
- [MED] Default fills at model MID, zero cost; frictions = synthetic spread over fitted-mid surface — no real bid/ask in declarative path. backtest.hpp:257; backtest.cpp:1532-1596.
- [MED] Financing uses arbitrary refs: cash rate = surfaces().front().pricing().r (archive order); shares carry hardcodes q_eff_at(0.25). backtest.cpp:1723,1744.
LOW: retain_position_frames RunConfig field DEAD; fixed-book overload ignores initial_cash/financing; entry-fill tier vs PnL-base tier mismatch → spurious day-1 unexplained; residual hedge shares linger under Cadence::AtEntry.
MISSING SOTA: real listed bid/ask fills; early-exercise/assignment+pin+physical settlement; margin/capital/buying-power + loop-enforced risk limits; per-name borrow/locate; corp actions; walk-forward + MC overlay; benchmark-relative stats (beta/alpha/IR); multi-leg atomic fills; block-summed attribution under downsampling.

---

## 03 — SURFACE STORE / ARCHIVE / POPULATE  [report: 03-surface-store.md]
Counts: Crit 0 · High 0 · Med 4 · Low 7. Tree = main@99f332f.
**NO data-corruption/determinism bug.** v2 (ATXVSA2) round-trip self-consistent, byte-deterministic (zero-init bufs, memcpy, no wall-clock in bytes). record_crc_v2 zeroes own field; SnapshotCache/S5 staleness design SOUND. v2 reader robust vs untrusted files (bounds+alignment checked).
**v1-isolation status: NOT on main.** No archive_v1_support namespace on main — extraction lives UNMERGED on feat/bt-v1iso (5510342). On main v1(major-3)+v2(major-4) coexist in one 2323-LOC surface_archive.cpp; v1 read path correct.

TOP:
- [Med] v1 "clean break" claimed done but v1 writer+reader still compiled into atx::vol (surface_archive.cpp:71-1221); snapshot_cache.cpp:93 says "v1 is gone" (false). Land S4 deletion / isolation branch.
- [Med/perf] **Zero-copy PricedSurfaceView never reaches production hot path** — MarketSnapshot::load (backtest.cpp:968-1041) owned-reconstructs; SurfaceDb::map_surface/load_surface + S5 LRU (surface_db.cpp:1175-1285) test-only. Repoint SurfaceSet/PortfolioPricer at views.
- [Med/perf] No mmap — SurfaceArchiveV2::open_file (surface_archive.cpp:1865) reads WHOLE partition even for 1-uid subset; open_borrowed seam (:1860) unused. Feed atx::tsdb::Mapping owner.
- [Low/perf] Subset load double-probes hash table per surface (backtest.cpp:1014-1018: reconstruct_symbol then provenance each re-find_slot despite holding dir entry e). Reconstruct from e.surface_offset/size.
- [Low] v1 SplineVol serialization lossy — drops mult_cap+w_offset (surface_archive.cpp:127-140) → misprice. Latent (v1 write bench-only; manifest rejects SplineVol surface_db.cpp:312).
OTHER LOW: v1 provenance() re-CRCs whole blob/call; find() returns partial DirEntry; ConvexSliceFit QP diagnostics unserialized (pricing-safe); v2/v3 naming collision; surface_db.hpp:3 doc says v3 but writes v2; whole-date partition rewrite (no incremental append).
**CLAIM TENSION w/02**: agent-03 says "populate FitPreset options ignored" is INCORRECT — preset+al_override+band_k+pinned curve ARE conveyed via pricer_config_for_symbol+apply_symbol_config (surface_db_populate.cpp:310-315). Reconcile vs 02's al_fast-clobber claim (both may hold: preset arrives, al_fast opts clobbered by apply_risk_policy downstream). FITTING IMPLEMENTER MUST VERIFY.

---

## 01 — FIT CORE (calibration numerics)  [report: 01-fit-core.md]
Counts: Crit 0 · High 1 · Med 5 · Low 4 · Perf 4 · Gaps 5.
**NO critical numerical bug.** Jacobians (SVI-MM 5-col, eSSVI cube→natural, C8 JW→x), quasi-explicit (u,v) box (w≥0), Illinois de-Am solver, spline WLS all hand-verified correct. Heavily reviewed code. (Rejected one suspected bug: set_yield ATX_TRY assigns member correctly.)

TOP:
- **[HIGH] svi_calib.cpp:1417-1420 — svi_calib_surface only TALLIES butterfly violations, never projects/rejects.** calib_pool (calib_pool.cpp:181) serves Parametrization::Svi surfaces directly, bypassing fit_slice_curve gating seam → **static-arb SVI smiles can be served.** Fix: move svi_project_mm + re-check/skip into driver. (Bounded by: is Svi ever served w/o fit_slice_curve? eSSVI primary path arb-free by construction, unaffected.)
- [Med] cstar_calib.cpp — entire CStar/C16M calibrator DEAD in production (no VolCurveKind::CStar; param_supported rejects, calib_pool.cpp:160-163,186); tests/examples only. Wire or quarantine.
- [Med] calib.cpp:862 — interval/band loss NotImplemented; every fitter minimizes squared-to-anchor, NOT zero-inside-[bid,ask]. **This is the direct objective for the "% within bid-ask" goal metric.** (Feature gap w/ correctness flavor.)
- [Med] c8_calib.cpp:437 — C8 writes total-variance-space RMSE into vol-space rmse_vol_vega_weighted diag (SVI/eSSVI write σ-space) → surface RMSE aggregation mixes units. (Bounded: c8_calib_slice test-only.)
- [Med] svi_calib.cpp:787-788 — quasi-explicit Nelder-Mead cap collapses to max_inner_iter (default 12) not C's 200 (unreachable at defaults) → truncated (m,σ) search under-fits wide/skewed smiles.
OTHER: svi_calib.cpp:837 SVI IRLS non-robust weighted-RMS scale (vs q90 elsewhere) → outliers inflate Huber scale. Dead exports: avg_abs_error_e5, SVI↔JW conversions, c8_calib_slice (test-only).
GAPS: joint/term-structure eSSVI ρ (Shared/TermStructure NotImplemented); asymmetric ρ NotImplemented; no hard outlier trimming (soft Huber only); no caller-facing wing-slope clamp.
PERF: weight computed before reject test (calib.cpp:178); cold Black-76 erf/exp per strike/trial in price-domain LM inner loops (svi/cstar); O(cand×expiry×holdout) cold american_price in selector OOS (curve_selector.cpp:559).

---

## ============ CROSS-CUTTING THEMES (PM roll-up) ============

### PERFORMANCE — "how fast are we" levers (ranked)
P1. **Production Auto-ISA uses NO AVX2 for American marks OR greeks** (kShipAvx2Boundary=false, kShipAvx2Greeks=false; greeks kernel also unwired). [05] — biggest single throughput lever on the dispersion bench (American reprice dominates).
P2. **FullGreeks risk loop scalar per-contract**; laned AVX2 american_greeks_batch exists, tests-only → ~4×. [05]
P3. **First-order/hedge greek tier unwired** → 80-94% greek-solve waste on delta-only cadence (3-layer gap: PriceFieldMask binary / EvalField FirstOrder==SecondOrder / K4 selectors never forwarded). [05]
P4. **Two full-book American passes per step** (Execution FullGreeks + StepPnl) recompute base greeks twice → fuse. [06]
P5. **FitPreset::Populate al_fast DEAD** → every board 4-8× AL solve on populate. [02]
P6. **Convenience price()/pnl_explain() rebuild PreparedPortfolio + realloc every call.** [05]
P7. **Zero-copy PricedSurfaceView + mmap unwired**; SnapshotLoad owned-reconstructs whole partition per date. [03][06]
P8. Warm-start cache not engaged in populate; incremental refit uncalled by drivers. [02]

### CORRECTNESS
C1. [HIGH] record_every_n>1 silently corrupts Sharpe/ann/attribution (no warning). [04]
C2. [HIGH] SVI butterfly violations tallied not rejected → static-arb smiles servable via calib_pool. [01]
C3. Look-ahead = CLEAN; v2 archive round-trip = SOUND + deterministic. (positives)
C4. [MED] American early-exercise/assignment not modeled; synthetic-tenor lots can't settle. [04]

### UNWIRED / DEAD (user asked explicitly)
U1. calib_pool.cpp (calibrate_pool + CadenceQueue + cadence scheduler) — test-only. [02]
U2. CStar/C16M calibrator — test-only, param rejected. [01]
U3. AVX2 American marks+greeks kernels — dark (kShip flags) + greeks unwired. [05]
U4. EvalField::Delta/Vega + american_delta + K4 need_* selectors — never forwarded. [05]
U5. refit_expiry / refit_risk_slice — no prod caller. [02]
U6. FitPreset::Populate al_fast distinction — clobbered downstream. [02]
U7. SurfaceDb::map_surface/load_surface + S5 LRU + open_borrowed — test-only. [03]
U8. retain_position_frames RunConfig field — read nowhere. [04]
U9. interval/band loss + joint/TS eSSVI ρ — NotImplemented. [01]

### FEATURE GAPS (SOTA backtester)
G1. Real listed bid/ask fills (declarative path fills at model mid). [04]
G2. Early-exercise/assignment + pin + physical settlement. [04]
G3. Margin/capital/buying-power + loop-enforced risk limits + drawdown stop. [04][06]
G4. Transaction-cost/market-impact model + per-name borrow term structure. [04][06]
G5. Intraday/multi-snapshot backtest (currently 1 min/day). [06]
G6. Parameter/scenario sweep + walk-forward + MC path overlay. [04][06]
G7. Benchmark-relative stats (beta/alpha/IR); per-leg/factor attribution; native tearsheet plots. [04][06]
G8. Interval/band ("% within bid-ask") fit objective + hard outlier trimming + wing clamp. [01]
G9. Corporate actions (splits/special divs/symbol changes); official index reconstitution weights. [04][06]
