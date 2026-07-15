# SpiderRock SRCubic (cubic-spline) universe-fit parity benchmark

**Date:** 2026-07-12
**Branch:** `worktree-atx-vol-spiderrock-integration`
**Deliverable:** `atx-vol/examples/opra_spline_bench.cpp` (target `opra_spline_bench`)
**Data:** `C:\atx\data\opra_universe` — 2734 US single-name/ETF OPRA chains, one snapshot
(`2026-07-01`, `T14:00:00Z`), 93 MB parquet hive.
**Build:** `cmake --preset rel` (Release, clang-cl `/O2`, SSE2), 16 board-parallel workers.
**Refs:** SpiderRock LiveVolSurfaces + OptionPricing docs (v8.6.6.3).

---

## TL;DR

- atx-vol's cubic-spline curve (`VolCurveKind::SplineVol`) is a **faithful port of SpiderRock's
  SRCubic**: identical fixed 29-point standardized-moneyness grid, cubic natural spline over the
  vol multiple `sigma(K)/sigma_ATM`, penalized WLS, flat wings, Lee/Roper butterfly scan.
- **Accuracy where it fits is SpiderRock-grade**: 99.2% of quotes inside the bid-ask channel,
  RMSE(model−mkt vol) = 0.0028 vol pts (≈ SpiderRock `fitAvgErr`), `fitMaxPrcErr = 0` on 74% of
  surfaces (SpiderRock target ≈ 90%).
- **BUT coverage collapses**: pinned SRCubic fits only **427 / 2734 = 15.6%** of the universe.
  The auto-policy fits **2210 / 2734 = 80.8%** of the *same* boards from the *same* de-Americanized
  observations (served as 1321 SVI, 814 eSSVI, 45 C8, 19 LinearVariance, 11 ConvexDense). → atx
  **cannot** yet replicate SpiderRock's "fit-everything-with-SRCubic" behaviour; the spline is the
  wrong horse on the pipeline it currently rides.
- **The spline math is free**: 0.7% of fit CPU (2.1 ms across 23 slices for AAPL). 100% of the
  cost is American→European de-Americanization — dominated by carry/forward resolution.
- **Root cause of the coverage gap** is a single pipeline asymmetry, not a spline defect
  (see §3). Fixable.

---

## 1. What the benchmark does

`opra_spline_bench` pins `SplineVol`, loads the whole hive, and fits every underlier
board-parallel (each fit serial). It reports:

- **Speed:** wall (load / fit+value), throughput (surfaces·slices·quotes/s), fit-latency
  percentiles, and the per-stage hot-path CPU split.
- **Accuracy (SpiderRock-native):** rolled up from `ParityReport::band` per surface —
  `fitMaxPrcErr` (max premium bid-ask violation), call/put bid/ask miss counts, fraction of
  quotes inside the channel, RMSE(model−mkt vol), zero-violation rate, calendar-arb-free rate.
- **Hot path:** aggregate CPU by stage + slowest boards + optional single-board repeat-fit
  micro-profile (and, with `ATX_VOL_PROFILE=1`, the internal de-Am/fit/parity split).

An env-gated diagnostic (`ATX_SLICE_DEBUG=1`, `src/curve_fit.cpp`) surfaces the per-slice
drop reason so a thin expiry can be told from a curve-fit defect. Zero cost when off.

## 2. Headline numbers (full universe, 2734 boards, `--preset fast`)

| Metric | SRCubic (SplineVol) | SpiderRock target |
|---|---|---|
| Boards fit (coverage) | **427 / 2734 = 15.6%** (auto-policy: 80.8%) | ~all (≈90% clean) |
| Pipeline wall | 84.9 s (load 29.9 s + fit 55.0 s) | ~45 s full universe |
| Fit throughput | 7.8 surfaces/s, 1011 quotes/s | — |
| Fit latency (ok) | p50 319 ms, p99 978 ms | index <10 s/update |
| mean frac-in-bidask | 0.9918 | fit within channel |
| RMSE(model−mkt vol) | 0.0028 | ≈ `fitAvgErr` |
| `fitMaxPrcErr` p50 / max | $0.00 / $25.34 | 0 for ~90% |
| Zero bid-ask violation | 74.2% | ≈90% |
| Calendar-arb-free | 33.5% | (implied) |

Accuracy on the boards that fit is competitive; **coverage and the calendar-arb rate are not.**

## 3. Root cause of the 15.6% vs ~84% coverage gap

Isolated by pinning each curve family on 10 dense boards (AAOI, ACN, AKAM, …) that eSSVI fits
cleanly (10/10) — these are liquid names (1000–1600 quote rows), **not** thin data:

| Path | Curve | ok/10 |
|---|---|---|
| `run_surface_parity` (eSSVI-native) | eSSVI | **10** |
| `fit_curve_surface` (polymorphic) | SplineVol / LinearVariance / ConvexDense / SVI | 2 |
| `fit_curve_surface` (polymorphic) | C8 | 0 |

The collapse is **shared by every polymorphic curve**, so it is a *pipeline* property, not a
spline bug. The mechanism:

- Both paths gate an expiry on `kMinPreparedFitRows = 5` surviving de-Am fit rows.
- `fit_curve_surface`'s prepass prepares observations under
  `PreparedObservationPolicy::Configured` — the strict fit-inversion audit that **drops** rows
  failing the cold-reference residual budget, so a thin-but-liquid single-name expiry falls
  below the floor and the whole board yields "no expiry produced a usable slice".
- eSSVI's `run_surface_parity` prepares under `PreparedObservationPolicy::LegacyEssviCompatibility`
  and additionally counts `n_fit_rows + n_audit_dropped` toward the floor — it keeps the
  expiries the polymorphic audit starves.

The **auto-policy reaches 80.8%** because its CurveSelector scores candidates through the
lenient eSSVI-compatible preparation and serves the winner (mostly SVI/eSSVI) — proving the
de-Americanized observations are sufficient. Only *explicitly pinning* a curve routes through
the strict `fit_curve_surface` prepass and starves them. SRCubic has no non-pinned route today
(it is excluded from `default_selector_candidates()`), so it is always on the strict path.

**Fix direction:** bring the polymorphic prepass to eSSVI-path leniency (audit-dropped
re-count / `LegacyEssviCompatibility`-equivalent observation policy), or lower the audit's
drop aggressiveness for sparse slices — or add SRCubic as a selector candidate so it inherits
the lenient preparation. Then SRCubic inherits ~80% coverage for free. (SplineVol's own
`min_obs` floor is secondary: 6→3 only moved 35→39 ok on the 200-board sample.)

## 4. Hot path / CPU profile

Aggregate over the universe (fit portion): **de-Am+spline 78.5%, valuation 21.4%**, load/chain
~0%. Board-parallel scaling 14.9× on 16 cores. Serial **parquet load is 35% of wall (29.9 s)** —
a second, I/O-bound hotspot independent of the fit.

Internal split for AAPL (`ATX_VOL_PROFILE=1`, 24 expiries, 950 quotes, 23 slices):

| Phase | Time | Share of fit |
|---|---|---|
| `prepass_wall` (de-Americanization) | 267.9 ms | **84%** |
| ↳ `forward_borrow_sum` (carry/forward resolve, CPU) | 1728 ms | ~76% of prepass |
| ↳ `obs_eu_sum` (Andersen-Lake IV inversion, CPU) | 544 ms | ~24% of prepass |
| `chain_parity_sum` (re-Americanization scoring) | 49.4 ms | 15% |
| **`fit_slice_sum` (SRCubic spline solve)** | **2.1 ms** | **0.7%** |

**The cubic spline is not the bottleneck — it is ~0.09 ms/slice.** The entire fit cost is the
American→European conversion, and *within that*, co-terminal carry/forward resolution
(`resolve_chain_forward`) outweighs the IV inversion ~3:1. This is exactly the cost SpiderRock
amortizes by precomputing "calibration records offline in data centers"; atx pays it live on
every board.

## 5. Can atx replicate SpiderRock's SRCubic engine?

- **Representation:** yes — the curve is a faithful SRCubic port (grid, spline, wings, butterfly).
- **Accuracy (where it fits):** yes — 99.2% in-band, 0.0028 RMSE vol, matches SpiderRock's
  `fitAvgErr`/bid-ask-channel story on the fitting subset.
- **Coverage:** **not yet** — 15.6% vs SpiderRock's fit-everything. Blocked by the polymorphic
  de-Am prepass policy (§3), not the spline. The auto-policy already reaches 80.8% on this data.
- **Throughput:** the spline is free; the ceiling is de-Am/carry cost + serial parquet I/O.
  Once coverage is restored, the same per-fit cost applies to ~5× more boards, so the current
  85 s wall is optimistic — closing the perf gap to SpiderRock's 45 s needs (a) the offline
  calibration-record amortization SpiderRock uses, and (b) parallel/streamed parquet load.
- **Calendar no-arb (33.5%):** SRCubic slices are fit independently with only a wing-level
  floor; unlike the eSSVI path there is no cross-slice calendar projection for SplineVol (v1),
  so calendar crossings are common. A follow-up gap distinct from coverage.

## 6. Reproduce

```
cmake --preset rel -DATX_BUILD_EXAMPLES=ON
cmake --build build-rel --target opra_spline_bench
build-rel/bin/opra_spline_bench --opra-root C:\atx\data\opra_universe --date 2026-07-01 --preset fast
# internal hot-path split for one board:
ATX_VOL_PROFILE=1 opra_spline_bench ... --symbols-file aapl.txt --profile-symbol AAPL --profile-iters 6
# per-slice drop reasons:
ATX_SLICE_DEBUG=1 opra_spline_bench ...
```
