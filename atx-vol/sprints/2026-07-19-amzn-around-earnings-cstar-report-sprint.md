# Sprint: AMZN-around-earnings VolaDynamics-parity surface report (C\* negative-curvature)

**Branch:** `feat/atx-vol-amzn-earnings`  worktree `C:/atx-wt/wt-amzn-earn`
**Goal:** Reproduce the VolaDynamics *AMZN around earnings* analysis
(https://voladynamics.com/examples/amzn-around-earnings/) end-to-end on **real**
AMZN OPRA marks at **2018-04-26 19:45Z** (15:45 ET, 15 min before the after-close
earnings print). Prove the fitting pipeline handles **extreme negative ATF
curvature** (front-expiry "W-shape", c2 ≈ −1.1) where SSVI/SVI break, staying
butterfly- and calendar-arbitrage-free, and emit a Python-rendered report.

## Data (DONE)
- Real fixture pulled via databento OPRA.PILLAR cbbo-1m, AMZN.OPT, 2018-04-26
  19:45Z window. **Spend: $0.0084** (approved). 5610 rows, 17 expiries
  (180427 @1DTE → 200117 LEAP), strikes 365–2355, implied spot ≈ $1519.
- Committed fixture: `atx-vol/tests/data/amzn_earnings_2018/amzn_opra_cbbo1m_2018-04-26T1945Z.parquet` (114 KB) + `manifest.csv`.

## Curve-family mapping (VolaDynamics ↔ atx-vol)  [research-confirmed]
- **One curve is fit per slice: CStar at C8 tier.** Both VD "views" are
  *projections of that single fit*, NOT separate fits:
  - VD "3-param view" (σ0, s2, c2; c2→−1.1)  = the CStar **base** shape params
    {sqrt(theta/T), s2, c2}. The CStar base is the polynomial `1+2·s2·z+c2·z²`
    which admits negative c2.
  - VD "8-param view"  = CStar **C8 tier** = 5 base {theta,s2,c2,C_left,C_right}
    + 3 modes {shoulder,ATM} = 8 params.
- **Do NOT fit a standalone S3 for the negative-c2 term structure.** Klassen's
  own slides confirm S3≡SSVI is valid only for c2≥0 (`0.25(1+s2 z)²+0.5 c2 z²`
  goes negative in the wings otherwise). `s3.hpp` stays a c2≥0 baseline/reference
  only; the front slice MUST use CStar.
- Feasibility (Klassen slide 15): local ATF bound `c2 ≥ −2 + ½·s2²·(1+¼·σ̂0²)`
  (c2 can reach −2 at zero skew), so c2≈−1.1 is comfortably feasible; the
  binding constraint is the full-curve `min_z g(z) ≥ 0` (`cstar_min_roper_g`,
  240-knot grid) which the shoulder modes satisfy.
- Nested C-family (C5/C8/C12/C16) + earnings decomposition already in-tree
  (`cstar.hpp`, `cstar_calib.hpp`, `event_vol.hpp`, `earnings_repro.hpp`).
- Full research brief (Klassen GD-Chicago-2017 deck, Gatheral–Jacquier, SpiderRock
  eMove, Roper density) archived under `atx-vol/research/`.

## Workstreams

### WS-1  C\* whole-surface fit driver  (core)
`cstar_calibrate_slice` fits ONE slice seeded from an eSSVI slice; the surface
orchestration is a documented PORT NOTE gap. Build a surface driver:
1. `essvi_calib_surface` (with `deam` — AMZN is American) → calendar-arb-free
   eSSVI seed surface.
2. Per slice: `cstar_calibrate_slice(essvi_seed_slice, chain, df, opts)` at a
   tier chosen per slice (C8/C12 near-term for the W-shape, C5 for LEAPs).
3. Assemble a `CStarSurface`; verify/repair **calendar** arb across slices
   (no total-variance crossing near-money |k|≤0.6). Seed-from-calendar-projected-
   eSSVI inherits monotonicity; add a θ-floor cross-slice pass if crossings remain.
4. Acceptance: front slice fits with **c2 < 0** (target ≈ −1.1), butterfly-arb-free
   (min Roper g ≥ 0), all 17 slices fit, calendar-arb-free near-money.

### WS-2  Report emitter (C++ example `examples/amzn_earnings_report.cpp`)
Loads the fixture, runs WS-1 + earnings decomposition, emits CSV/JSON for Python:
- `slices.csv`: per-slice CStar params (theta,s2,c2,C_left,C_right,beta[0..10],tier,
  T,F,expiry,rmse,arb_damping,min_roper_g) + reduced base-3 {sigma0,s2,c2}.
- `total_variance.csv`: per-slice (k=log(K/F), w=T·σ²) on a dense grid (calendar plot).
- `smiles.csv`: per-slice market IV points (K, z, mkt_iv, iv_err_from_spread,
  bid_iv, ask_iv) + fitted IV on K- and z-grids (strike-space + NS-space plots).
- `earnings.csv`: iEMove, st, lt, decay, censored ATM term curve, per-tenor
  event-var share (from `run_earnings_repro`/`event_vol`).
- CLI: `amzn_earnings_report --opra <parquet> --snapshot <iso> --r <rate> --out <dir>`.

### WS-3  Python report `python/amzn_earnings_report.py`
matplotlib, self-contained, reads WS-2 CSVs → the VD figure set:
1. NS-space surface (10 near-term expiries, IV vs z) — W-shape.
2. Front-expiry strike-space (market pts + fit, K-axis) — strongest W.
3. Front-expiry NS-space (negative c2 annotated).
4. Total variance vs log(K/F), 10 terms, "lines don't intersect".
5. Total variance with input error bars.
6. 3-param term structure: sigma0(T), s2(T), **c2(T)** (c2 starts ≈ −1.1, flat after
   3–4 mo) — projected from the C8 fit's base params, NOT a separate S3 fit.
7. 8-param (C8) term structure: all 8 params vs T (base 5 + 3 modes).
8. Multi-expiry fit-vs-market panels (i=0,1,3,4,5,6,9).
Plus an earnings decomposition panel (iEMove, continuous vs discrete variance).
Output: PNGs + an `index.html` (or single PDF) summary.

### WS-4  Correctness test + verification
`tests/amzn_earnings_test.cpp` (skip-if-fixture-absent, like the other real-data
tests): load fixture → fit → assert spot ≈ 1519±, front DTE ≈ 1, front **c2 < 0**,
butterfly-arb-free every slice, calendar-arb-free near-money, ≥15 slices fit.
Wire into `atx-vol/tests/CMakeLists.txt`. Run the atx-vol gate; confirm no
regressions. Report fit timing.

## Constraints
- `.agents/cpp/agent.md` house style; `/W4 /permissive- /WX` clean; Rule of Zero;
  `Result<T>` not exceptions; `[[nodiscard]]`/`noexcept`/`const` by default.
- Web-research-first for any new numerics (negative-c2 arb conditions grounded in
  Klassen/Roper/Gatheral — see research brief).
- No new API spend; the fixture is committed.
