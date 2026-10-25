# 2026-10-24 — risk engine sprint: P&L explain, projection, SpiderRock bench, historical VaR, persistence

Branch `feat/vol-risk-engine-20261024` (pool-1), base `main @ 96cd56db`; frozen API on
`feat/vol-risk-base-20261024 @ 7b26e69e`; `main @ 163c4f8d` merged in session 2 (`c534bfca`).
Integration tip at writing: `1defdc7c` (+ these docs). Run id `risk-20261024`. Design of record (D1–D23, status
log): `docs/superpowers/specs/2026-10-24-atx-vol-risk-engine-design.md`. Research brief:
`atx-vol/docs/2026-10-24-pnl-explain-research-brief.md`. Working state (gitignored):
`.superpowers/sdd/2026-10-24-risk-engine/` — `HANDOFF.md`, `progress.md`, `lane-*-{brief,report,review-N}.md`,
`artifacts/` (bench JSON, per-leg dumps, parity notes, scripts).

**Status.** Integrated on the sprint branch; NOT merged to `main` (the user asked for local main to
be merged into the worktree, then the worktree into local main, no push; a trial merge of
`main @ 2c08202a` is being scouted). Release qualification ran on `7daeb614`: `atx_vol_fast`
4956 / 5005, the 49 failures outside the sprint's code pending attribution (§9). Pending at writing:
that attribution; the final quiet-host perf runs and the
`bench/baselines/i7-1260p-clang18-avx2-risk-bench-{synthetic,spiderrock-2026-10-14}.json` pins (not yet
committed); lane P2's last minor round; lane F (in flight). **Every timing below is UNQUALIFIED**
(host shared with 2–4 concurrent build / bench lanes, 2–97 % CPU) unless the row says otherwise.

## 1. Ask

P&L explain (base vs shifted market environment, options + shares, Greek-level breakdown) and the
interpolation / projection engine (log-moneyness ↔ delta ↔ K, vol at theoretical expiries, "the 20-delta
put expiring 2026-10-30 seen on a historical surface with TTE and moneyness fixed") as a first-class
citizen at HFT throughput; a benchmark on the licensed SpiderRock intraday drop (10k options on 100
underliers + 100 share legs, 10:00 → 15:00, trading date 2026-10-14); fix correctness bugs; no backward
compatibility for the VaR pipeline. Wave 3 (session 2) added binary persistence and a universe fit
robustness benchmark.

## 2. Lanes and what each final SHA is

| lane | scope | branch (pool) | reviewed / final SHA | merged into integration as |
|---|---|---|---|---|
| A | core engine | `lane/A-risk-20261024` (35) | `24328d9b` | `40f99349` |
| B | projection | `lane/B-risk-20261024` (36) | `39bd5360` | `384c7099` |
| C | loader, synthetic store, bench | `lane/C-risk-20261024` (34) | `0e29124b` | `847abfbb` |
| C2 | C follow-up 2 (framework, ddivPv, clock, implied carry) | same | `bc1ed934` + `4134f621` | `f0caf955`, `b0f75b65` |
| A2 | family-kernel economics (perf) | `lane/A2-perf-20261024` (36) | `01bc9d5b` + `7250759c` | `08f74513`, `a015f4f5` |
| D | historical VaR rebuild, old pipeline deleted | `lane/D-risk-20261024` (30) | `935d9de4` → `47689a41` | `7b099ab7` … `3279a480` |
| E | vendor clock, carry axis, sticky carry mode | `lane/E-clock-20261024` (34) | `11a1fafa` → `1b76ac2d` | `a3ec196d` |
| P1 | PersistFile container, MarketState store | `feat/vol-persist-20261024` (6) | `c6440860` → `1463158b` | `b23a36c0` … `7daeb614` |
| P2 | SurfaceTimeline, RiskRun archive | `feat/vol-persist-p2-20261024` (8) | `83cf911d` → `399bb097` | `595a0291`, `cb13dd3a` (+ `1defdc7c`) |
| F | universe fit / price / risk robustness | `feat/vol-fitbench-20261024` (7) | in flight (`b6782ca8`) | — |

## 3. What shipped

**A — core engine** (`lane-A-report.md`). `MarketState` (marks-first σ, then surface; per-expiry
`ExpiryCarry`), `RiskBook` / `PreparedRiskBook` (exact-bit dedup, (uid, expiry, side) groups, ascending K),
`RiskKernel::family()` (one σ-family per group, typed cold fallback) and `::cold()`, `RiskFrame`, `explain` /
`explain_into` (eight Taylor terms, vega split level / skew / curvature at standardized-moneyness pivots,
shares delta-one, exact remainder), `RiskStream` (one frame per state; commit-on-success after review 1's
lifetime BLOCK). Family vs cold inside the box over 240 lanes: price 3.4e-12 rel, vega 6.2e-6; delta 1.4e-4 on
10 short OTM wings, adjudicated against a Richardson reference (family 5.1e-7 off it, cold 1.4e-4 —
`RiskKernel.FamilyMatchesColdInsideBox`). Taylor identity 0 ulp; pv_base, all eight Taylor columns and every
`d_*` column bit-exact against `PortfolioPricer::pnl_explain` on 61/61 legs (`RiskExplain.MatchesPortfolioPricerOracle`).
Smoke (1 thread, vol-rel): family 10.5–12.3 µs/contract vs cold 146–158 (12.8–14×). Warm allocations:
0.5 per group (call families re-anchor at spot), 0 per row.

**B — projection** (`lane-B-report.md`). `convert_strike_coord[_batch]` under
`DeltaConvention::{Forward, Spot (new), American}`; `resolve_theo`, `resolve_theo_many`, `project_series`
(fixed TTE with fixed k or re-solved Δ); additive `TenorInterp::ShapeBlend` overloads on `CurveSurface` /
`PricedSurface` / views (default path bit-unchanged; one out-of-line core so owned == view bit for bit);
`moneyness_extrapolated(K, T)`. 448 (from, to) pairs round-trip K to 1e-9 rel; a polished probit (Acklam +
2 Halley) because the library exports no inverse normal. Uniform failure rows (NaN strike-level fields,
fingerprint 0).

**C + C2 — loader, synthetic store, bench** (`lane-C-report.md`, incl. "Resume … (session 2)"). Partitioned
licensed store `C:\atx-data\spiderrock-30min` (ingest `scripts/spiderrock_intraday_to_store.py`, 302 s);
`load_spiderrock_bucket` with `american_only` (D16), `standard_roots_only` (D17), `spot_framework_only`
(D20iii), pv_divs from `ddivPv`, optional `synSpot` / `yearsC`; `ImpliedFromCalls` carry (D20ii); a
SpiderRock-shaped synthetic store (440k rows in 14.6 s); `atx-vol-risk-bench` with `--dump-legs`, report
schema v2, `kSpiderRockConventionsVersion`. Bucket 1000: 2,082,476 rows read, 114,149 non-American,
4,736 forward-framework (OEX 4,064 + XAU 672), 31,329 non-standard roots; top-100 = 365,600 marks,
1,913 expiry rows, 1,758 implied / 155 published (10 with < 3 calls, 145 on the IQR guard); inversion
12.5–16.8 ms per bucket. Loader cost 1.4–3.3 s per bucket + 30–100 ms state build. VolTime measured and
NOT adopted (median 0.919 d vs the 0.5 d bar; cause in §5).

**A2 — family economics** (`lane-A2-report.md`, both session-2 sections). Lever 1 economics
(`FamilyUneconomic` route when `n·node_cost ≥ n_eligible·cold_cost`, ×3 / ×1.4 under rho); lever 2 adaptive
node count `n = clamp(⌈ln(σ_hi/σ_lo)/h⌉ + 1, floor, n_sigma)`, recalibrated on BOTH stores after a first
calibration (pad 0.05, h 0.075, n_min 4) passed fixtures and failed the licensed gate at 4.1e-6; lever 3
`ShortTenor` flag + prepared-geometry cold route (bit-identical); lever 4 explain merge walk over marks
+ column-parallel Neumaier (bit-identical, digest `0x5660a61510ac3e32` pinned); lever 5 `dynamic_schedule`;
lever 6 T-aware node floor (`short_box_tau 0.3`, `n_sigma_min_short 4`). Approved header defaults:
`sigma_box_pad 0.25 → 0.05`; appended `n_sigma_min 5`, `sigma_node_spacing 0.06`, `family_economics on`,
`family_node_cost_us 73`, `cold_lane_cost_us 270`, `dynamic_schedule true`, `short_box_tau 0.3`,
`n_sigma_min_short 4`; counters `family_nodes`, `uneconomic_rows`, `short_tenor_rows`. Nodes per family
12 → 5.22 (licensed) / 7.67 (synthetic). Licensed step-1 cold rows: ShortTenor 677, FamilyUneconomic 173,
other ColdFallback 1,688 (calls with q_eff ≤ 0: no boundary family), NearExerciseEdge 2.

**D — historical VaR** (`lane-D-report.md`, all fix rounds + closure). `api/risk/historical_var.hpp`
(`TheoLeg` templates + shares over (base, shifted) surface-backed state pairs; nearest-rank VaR / ES with
`tail_mean`), D21 additions (`keep_resolved_legs`, `resolved_legs`, `summarize_historical_var`), private
test seam `src/risk/historical_var_detail.hpp`. `dispersion_book_var` ported (consecutive pairs; N sets →
N − 1 scenarios). 17 files deleted (`src/analytics/{var,var_report,var_validation,historical_projection}.*`,
four tests, `bench/var_bench.cpp`, the projection gate script, the plotter, the status doc). Admission is
strict (unknown premium never admitted; `excluded_premium < frac·|pv_base|`); only clock refusals are
scenario-level. Dispersion fixture re-pinned: pair P&L −36.0787 / +74.7856, VaR95 = ES95 = 36.0787.

**E — vendor clock, carry axis, sticky mode** (`lane-E-report.md`). `ExpiryCarry::T_override` (D22) through
one helper `risk_detail::expiry_T` (kernel, carry, explain dt); `SpiderRockClock::VendorYears` default
(conventions version 3); explain carry axis `pnl_carry` (D23a, the 21st per-leg column; `ExplainTotals`
152 → 160 B); sticky carry mode (D23b, 5-argument `market_state_from_bucket`); bench `--time vendor`,
`--sticky-carry`, schema v3, dump columns `T_vendor`, `pnl_carry`. Integration ownership: persist re-pins
(ExpiryCarry 40 B, `RiskRunStep` 288, buckets 184, VaR frame / point 192, `FrameCounters` 112 B,
21 explain columns, `sizeof(ExplainResult)` / `sizeof(RiskFrame)` compile-time pins) and review items I-2
(VaR tail mean averages `pnl_carry`), I-3 (dispersion scenario TSV 29 columns), I-4 (`time_refuses` via
`expiry_T`).

**P1 — persistence core** (`lane-P-report.md`, fix rounds 1–3). ATXVPF `PersistFile` container,
`PersistDirectory` manifest + `persist.lock`, MarketState archive `.atxms`, fingerprinted state store
`build_state_store_from_spiderrock` / `StateStore`, tool `atx-vol-state-store`
(`build | inspect | verify | bench | synth`), additive `YieldCurve::pillars()`, WriterLock delete-pending
retry (`src/storage/writer_lock.cpp`). Details in §6.

**P2 — persisted history** (`lane-P2-report.md`). `SurfaceTimeline` (`.atxstl`, verbatim ATXVSA2 records
behind a (uid, ts) index, monthly periods), `RiskRunWriter` / `RiskRunArchive` / `compact_risk_run`
(`seg-*.atxrr`, `SealDurability`, zstd + byte-shuffle cold tier), tool `atx-vol-history-store`. Review 1
BLOCKED `83cf911d` (§8); the fix `399bb097` (wrap-safe VarIndex / step / column checks, one writer per
run directory enforced through the manifest tail, timeline manifest rows cross-checked against their
files, a `Flush` `finish()` also flushes the earlier `Publish` segments) passed review 2 and is merged
(`cb13dd3a`; the dead `publish_segment` declaration dropped in `1defdc7c`).

**F — in flight.** Universe fit / price / risk robustness benchmark (`wave3-fit-robustness-design.md` §4,
`lane-F-brief.md`): `tools/universe_fit_bench.py`, `atx-vol-fit-risk-probe` (every served fit row priced
through `RiskKernel`, family + cold), `FitRiskProbe` suite; 4 commits through `b6782ca8`; no report yet.
Holdout ruling: labels only on development-split datasets; the universe tier is label-free (§9 ledger).

## 4. Performance (licensed 2026-10-14, 10k options / 100 uids / 100 shares, 1000→1500, vol-rel-avx2, 8 threads)

| metric (G3 target) | session-1 first pass | pre-A2 on merged tree | after A2 `01bc9d5b` | after lever 6 `7250759c` / lane E | verdict |
|---|---|---|---|---|---|
| frame evaluate p50 (≤ 150 ms) | 248 ms | 344.0 ms | 138.8 ms | **118.5 ms** | met, unqualified |
| explain p50 (≤ 5 ms) | 8.0 ms | 9.81 ms | 3.98 ms | 3.74 ms / **3.33 ms** (E A/B) | met |
| run excl. I/O, 11 states (≤ 2 s) | 3.20 s | 4.43 s | 2.13 s | **1.936 s** | met, borderline |
| cold-kernel speedup (≥ 10×, design assumption) | 1.0× | 0.80× | 1.81× | — | missed (D19) |
| family vs cold price gate (≤ 1e-6) | — | 5.3e-7 | 9.6e-8 | 3.73e-7 | met |
| thread determinism 1 vs 8 | bit-equal | bit-equal | bit-equal | bit-equal | met |
| state input, top-100 (I/O, outside G3) | Parquet 1.12–1.35 s | | | archive open **11.08 ms** (P1) | 121× |

Conditions: first pass on a host shared with two builds; "pre-A2" = A2's L2 binary with `--family-economics off
--n-sigma-min 12 --sigma-box-pad 0.25`, static schedule, after `main`'s merge raised licensed families 2,218 →
2,907 (so 248 and 344 ms are different books); A2 columns median of 3 runs at 69–87 % host load; lever 6 =
8 interleaved A/B rounds per side (`--short-box-tau 0` vs 0.3, load 2–94 %, median ≈ 70 %); lane E explain =
3 interleaved rounds (`artifacts/s2e-parity-after-E.md`). Synthetic store: evaluate 168.1 → 99.7 ms, explain
1.86 → 1.38 ms (E 1.46), run 1.95 → 1.17 s, cold 1.78 → 2.80×, gate 2.8e-8.

Per lever (licensed eval p50 ms | explain ms | run s | nodes/family | gate): B0 314.0 | 9.37 | 4.19 | 12 | 5.3e-7;
L1 economics 249.0 | 8.98 | 3.35 | 12; L2 first calibration REJECTED 141.2 | 9.87 | 2.19 | 4.23 | 4.1e-6;
L2r 135.6 | 7.89 | 1.96 | 5.22 | 9.6e-8; L4a merge walk explain 8.94 → 3.69; L4b column Neumaier 3.59 → 3.19 (A/B);
L5 dynamic vs static 138.3 vs 150.9 (A/B, −12 % paired); L6 124.6 → 118.5, nodes −7.1 %. Thread sweep (final A2
binary, evaluate p50 licensed / synthetic): 8 thr 112–123 / 103–122, 12 thr 102–105 / 81–84, 16 thr 88–90 / 69–76,
auto 83–103 / 69–81. Where the time goes: family node solves ≈ 78 % of evaluate; ≈ 0.39–0.48 s of each run is
loader state build (≈ 45 ms per state). The final quiet-host run (3 runs per store at 8 and 16 threads, p50 of
p50s) is PENDING; G3 is judged on it (D19), then the two baselines are pinned.

## 5. Vendor parity and explain quality (licensed, per-leg dump at the 1000 base frame)

| metric | first pass (integration 1) | after C2 (`f0caf955`) | after E (`38d5fe54`) | E full day 1000→1500 |
|---|---|---|---|---|
| price sym-rel, mean | 4.41 % (report 4.35 %) | 4.27 % | **0.404 %** | 0.412 % |
| sym-rel T ≤ 30 d | — | 12.8 % | **0.090 %** | 0.101 % |
| max \|Δprice\| | 54.66 (OEX) | 7.18 (SNDK 2 d) | 6.37 (BLK 2028, published) | |
| ours inside bid/ask (vendor's own 98.2 %) | 82.1 % | 82.7 % | **96.51 %** | 96.41 % |
| Σ\|unexpl\| / Σ\|pnl\|, option legs | 2.27 % | 3.01 % | **0.762 %** | 0.989 % (sticky off 1.045 %) |
| same, report incl. share legs | 2.12–2.2 % | 2.79 % | 0.708 % | 0.917 % |
| carry-mode flips, events / expiries (day) | — | 952 / 316 | — | 110 / 80; ≥ 2 flips: 19 |

The story (`sprint-note-draft.md`, `artifacts/s2-parity-after-c2.md`, `artifacts/s2e-parity-after-E.md`):
1. **Diagnosis (D20).** The first-pass gap was not a kernel bug: the clock (SpiderRock `years` is its own
   vol-time; Black–Scholes at `years` reproduced same-day srPrc to 1.3 % vs 60 % at calendar T), the forward
   basis (SpiderRock escrows dividends to an undisclosed horizon; DIA 2028-12 calls imply ONE flat forward
   562.2 = S·e^{(r−sdiv)T}), the price framework (OEX carries a per-expiry forward in `uPrc`) and `ddiv`
   read where D4 meant `ddivPv`.
2. **C2** fixed framework and basis: T > 1 y 1.14 → 0.71 % (0.30 % on implied rows), max |Δ| 54.8 → 7.2. The
   mean barely moved (4.41 → 4.27 %) because 78 % of its mass sits in T ≤ 30 d: the clock. VolTime reproduces
   `years` to ≤ 0.02 d until the first NYSE early close, then +0.46–0.48 d per early close crossed
   (`VolTimeCalendar` has full closures only). Repricing on the vendor's `years` with the forward held gave
   0.40 % / 96.6 % (error regresses on the clock term, slope 0.99, R² 0.77) → **D22**.
3. Explain unexplained ROSE to 3.0 % after C2: forward-basis re-marks landed in unexplained (no carry axis;
   corr 0.95, removing it leaves 0.82 %) plus implied ↔ published chatter (274 / 1,913 expiries) → **D23**.
4. **E** built both: 0.404 % / 96.51 % / 0.762 %, recomputed independently by `lane-E-review-1.md`
   (0.4042 %, 96.51 %, 0.762 %). The vendor clock also removes late-day same-day NumericError legs (2–17 per
   step from 1200 on calendar365 → 1). What remains: Published-fallback long-dated dividend payers (BLK, GS
   2028–2029; 817 legs at 2.37 % vs 0.23 % on 9,183 implied legs) and MULL (2× leveraged ETF) — an
   American-consistent forward is needed there.

FRTB-style quality (diagnostic, D9): per-step Spearman 0.986–0.997 / KS 0.003–0.006 at the first pass,
0.997–0.999 / 0.002–0.003 after E, green on every step; the 10-point totals series grades red (weak statistic).

## 6. Persistence (P1 + P2; `docs/persist.md`, `docs/persist-history.md`, `docs/storage/PERSIST_{FORMAT,HISTORY_FORMAT}.md`)

| kind | file | schema fingerprint | notes |
|---|---|---|---|
| container ATXVPF (`PersistFile`) | any | per kind: FNV-1a/64 + fmix64 over a field-level schema table | 256 B header, 64 B section entries, 64 B-aligned payloads, 4096 B file size, 64 B footer; CRC32C on header, table, every section (stored + raw), footer, file; optional zstd + byte shuffle per section; verify Framing / Lazy / Full / Paranoid; no allocation after open; 43 crafted corruptions each refused with its own `PersistStatus` |
| `PersistDirectory` | manifest + `persist.lock` | — | file-then-manifest publish (temp + flush + rename); keys ≤ 15 bytes; canonical names, no traversal |
| MarketState archive | `.atxms` | `0xb9738a6408b9ba89` (was `0x2e40ab410db4469d` before `T_override`) | marks / term carry / dividends served as spans into the mapping; 41 B per mark core + 56 extras; default verify Full ≤ 64 MiB, Framing above (`kMarketStateFullVerifyMaxBytes`) |
| state store | `<root>/…/bucket_et=HHMM-g<seq>.atxms` | source fingerprint (conventions version, `ATX_VOL_VERSION`, the P1-era store spec fields, every parquet's name / size / mtime; not yet `clock` / `carry_mode` / `spot_framework_only`, §10) | rebuilds sha256-identical 11/11 × 3 at `--parallel 1/2`; uids equal to one serial load |
| SurfaceTimeline | `<YYYY-MM>-g<seq12>.atxstl` | `0x8e3c8cc7c33a7ea1` | verbatim ATXVSA2 records; views bit-exact to the partition's |
| RiskRun | `seg-*.atxrr` + manifest | `0x07b280ae55b35c46` (was `0xc881a1e8c5e87255` before `pnl_carry`) | 16 frame + 21 explain f64 columns, narrow route / flag / status, Pod buckets / VaR; `segment_bytes` 16 MiB default |

Measured (vol-rel, i7-1260P; P1 open numbers on a 3.5–21 % busy host, the rest unqualified): top-100 bucket 1000
(367,664 marks, 35.7 MB) opens Full in **11.08 ms p50** (p95 15.4) vs Parquet load + state 1.345 s (**121×**),
Framing 2.25 ms; uncapped (1.94 M marks, 189.6 MB) Full 67.5 ms (miss vs 60 ms → Framing default 16.2 ms; Lazy
= Full here because `load()` touches every section). Encode + publish 86 ms (top-100) / 0.43 s (uncapped); after
E's re-pin all 11 buckets rebuilt in 18.0 s (35,553,280 B each). `PersistStateStoreReal.ElevenBucketExplainTotalsBitEqual`:
explain over the 11 archived states bit-equal to Parquet-built states on all 10 transitions. Timeline: `range()`
p50 155 ns, `view()` 3.2–3.6 µs, `project_series` over 242 views 1.02–1.11× owned, bit-equal. RiskRun (10,260
legs): 3.10 MB per step; append p50 0.38–0.47 ms; seal at 64 MiB 113–135 ms (framing 46–66 + OS write of a new
70 MB file ≈ 80 ms; `FlushFileBuffers` adds 0–12 ms on this NVMe) vs 29 ms at 16 MiB → default 16 MiB;
compaction zstd + shuffle 1.66× (1.13× without shuffle); cold-tier reopen 385–556 ms (serial decode);
recorded 11-state stream 0.10 ms per step, reopened totals bit-equal.

## 7. Decisions (design §9; one line each)

- **D1** flat namespace `atx::vol`, new headers under `api/risk/`, Tier-B. **D2** options keyed by absolute
  `expiry_ns`; T derived per state; `retime` gone. **D3** the family kernel is `RiskStream`'s default; cold is
  oracle + fallback; no serving-record kernel.
- **D4 (rev.)** one `ExpiryCarry{expiry_ns, pv_divs, rate, q}` per (uid, expiry) from the vendor's per-expiry
  tuple, `F = (S − pv_divs)·e^{(r−q)T}`; now the published-data fallback. **D5 (rev.)** `VolMoveConvention`:
  `ContractOwn` default (roll-down in the vol move), `CommonBaseMaturity` for oracle parity; veta → unexplained.
- **D6** Neumaier totals in leg order on the calling thread; thread determinism is a gate. **D7 (rev.)** lane C
  tested on the synthetic store; the licensed run belongs to integration with its own pin.
- **D8** no Python bindings, serving-record kernel or sticky-delta explain this sprint. **D9** FRTB PLA
  cutoffs as diagnostics; trapezoid second order opt-in; Shapley / share financing / calendar-safe tenor
  interpolation deferred. **D10** names fixed by the compile check (`TenorExtrapPolicy`, `CarryFrom`).
- **D11** near the edge the WHOLE lane goes cold (`cold_near_edge`) with `NearExerciseEdge | ColdFallback`.
  **D12** spot is the state's; AL options precedence policy → surface → `al_fast_opts()`; cold kernel prices
  resolved inputs. **D13** historical VaR = one-period P&L over (base, shifted) surface-backed pairs; templates
  re-resolved per base; dispersion re-pinned (BREAKING).
- **D14** `StandardMoneyness` = K/S, `StandardizedMoneyness` = z added; `MoneynessExtrapolated` best effort.
  **D15** family allocation is bounded, not zero (pinned). **D16** loader keeps American exercise only (6 % of
  rows are European index products). **D17** non-standard roots dropped before top-N.
- **D18 (deferred)** `explain_into(..., RiskWorkspace&)` replaces the thread-local scratch. **D19** G3 judged on
  the licensed run after A2 (the design's density assumption failed: 3.4 contracts per family);
  `kAlSigmaFamilyMinTau` not lowered.
- **D20** licensed environment = vendor clock + call-implied forward (≥ 3 calls, IQR ≤ 1e-3; published
  fallback) + Spot framework only + `ddivPv`. **D21** `historical_var.hpp` additive: `keep_resolved_legs`,
  `resolved_legs`, `summarize_historical_var`.
- **D22** `ExpiryCarry::T_override` (NaN = TimeSpec) drives kernel T and explain dt; loader fills the vendor's
  per-expiry median `years`. **D23** carry explain axis `pnl_carry` (exact remainder kept) + sticky carry mode
  (hold at 3× the guard, enter at 0.5×); flip target restated as ≤ 20 expiries flipping ≥ 2 times (19).

## 8. Review trail (fresh Opus reviewer per SHA; files `lane-*-review-N.md`)

| lane | SHA | verdict | what it found |
|---|---|---|---|
| A | `bca8df54` | BLOCK | failed `RiskStream::step` left a pointer to a released state; oracle bits printed not asserted; 5 minors |
| A | `15ea7410` | APPROVE | 2 minors (edge-flag wording, exception safety) → `24328d9b` commit-on-success, orchestrator-verified |
| B | `64d2fcd9` | APPROVE | major: `moneyness_extrapolated` TRUE branch untested; 5 minors → `97ec2117` |
| B | `97ec2117` | APPROVE | 3 minors → `39bd5360` + integration doc `023dea8b` |
| C | `75293b21` | APPROVE | 8 minors → follow-up 1 |
| C | `0e29124b` | APPROVE | 2 minors (folded into C2) |
| C2 | `bc1ed934` | APPROVE | vacuous ddivPv-fallback assertion; state-clock q untested; solver fuzz 200k cases worst 1.0e-13 → `4134f621` |
| A2 | `01bc9d5b` | APPROVE | `n_sigma = 4` refused; flag comments; merge-walk test gap; DONE overstated (2.13 s, 1.81×) → `7250759c` + lever 6 |
| D | `019e8fa6` (code `935d9de4`) | APPROVE | 7 minors (premium-0 exclusions admitted, structural errors swallowed, NaN into the sort, …) → `8f84ceef` |
| D | `8f84ceef` | APPROVE | 4 minors (verifier trusts `admitted`, strictness unwitnessed, clock-refusal path untested) → `47689a41` |
| E | `11a1fafa` (gated `35b174d0`) | APPROVE | I-1 major (merge `3b32e070` breaks persist static_asserts), I-2..I-4, 2 doc minors → `1b76ac2d` |
| E | `1b76ac2d` (re-pin) | APPROVE | 3 minors: the state-store source fingerprint omits clock / carry_mode / spot_framework_only; `pnl_carry` round-trips only zeros in the RiskRun test; no post-merge perf receipt |
| P1 | `c6440860` | BLOCK | zstd `raw_length` 2^44 → `std::bad_alloc` escapes `open()` (CLI 0xC0000409); default verify still Full; 4 minors |
| P1 | `da206ba8` | APPROVE | blocker closed (160 fuzz runs clean, WriterLock red/green proven); 2 minors → `1463158b` |
| P2 | `83cf911d` | BLOCK | `check_var` u64 count wrap → `std::length_error` / OOB subspan; single-writer contract unenforced; 3 minors → `399bb097` |
| P2 | `399bb097` | APPROVE | 409 fuzz runs clean, concurrency races all refused correctly; 3 minors (a `Flush` `finish()` is `Busy` while any reader maps a segment; POSIX post-rename fsync error marks the writer lost; dead `publish_segment` declaration → `1defdc7c`) → last round requested |
| F | — | — | not yet submitted |

Integration gates (pool-1, vol-rel, `-DATX_VOL_RISK_E2E=ON`): `08f74513` risk 124/124, umbrella 10/10;
`7b099ab7` risk 133/134 (HistoricalVar witness vs A2 economics → D fix), analytics 254/256 (§9), umbrella 9/10
(P1 literals → P1 fix); `e9bef13e` risk 142/142, dispersion 77/77, persist + storage 49/49, umbrella 10/10;
`595a0291` did not build (P2 pinned the pre-A2 `FrameCounters`); `1b76ac2d` (E) risk 153, persist 58,
dispersion 77, HistoricalVar 16, umbrella 10, hygiene clean, licensed 1000→1030 bit-equal at sym-rel 0.4074 %;
`7daeb614` qualification: configure + 19 slice / tool targets + `atx-vol-tests` built, `atx_vol_fast` 4956 / 5005
(§9); gates on `1defdc7c` running at writing. The session-2 claim "regression 246/246 at `c534bfca`" was made
on STALE slice binaries and is withdrawn (ledger trap).

## 9. Known reds outside the sprint

- Release qualification on `7daeb614` (`-Ctest -L atx_vol_fast`): 4956 / 5005 passed; 49 failed = 42 named
  + 7 exe-missing / Not Run. All 42 sit in pricing / fitting / surface / backtest code the sprint did not
  touch and are ~1e-14 numeric drifts or exact-equality / bit-identity fixture pins (e.g.
  `AmericanDelta.MatchesFd_PutCallGrid` −0.99999999999994316 vs −1, `SimdNanSafety` 1 ulp, `*IsBitIdentical`,
  `PricerFitterPolicy`, `SurfaceV2*`, `MarkRescue`, `AdjointGreeksAmerican`, plus the two below); they
  reproduce in the pricing slice, and no compile flag changed. Attribution (expected: `main`'s r7 pricing
  changes) is being run on pristine `main @ 2c08202a` (`progress.md` QUALIFICATION entry; list in the
  session scratchpad `label-failures-7daeb614.txt`).
- `analytics/ContractProjection.BatchLanedDeltaMatchesScalarColdOracleAtSolverRequestShape`: laned vs scalar cold
  delta gap 3.0356e-6 at row 169 (K = 199.03, T = 0.05) against 5e-8, deterministic ×3
  (`contract_projection_test.cpp:1218`). The code under test is byte-identical to `c534bfca`; the change came
  with `main @ 163c4f8d` (r7 pricing: `priced_surface.cpp`, `american_greeks_avx2.cpp`, `american.cpp`,
  `american_batch.cpp`, +356 / −156). Owner: the r7 parent (`lane-D-report.md`, `lane-D-review-1.md`).
- `analytics/Breakeven.GoldenPathPnlIsPinned`: last-bit pin 1.3832639438392142 vs …393373
  (`breakeven_test.cpp:223`), already `docs/release/known-issues-r6.md` Q0-F3.

## 10. Deferred (recorded, not built)

- Early-close table for `VolTimeCalendar` — a core-clock change that moves every VolTime consumer; **user
  decision** (D22 contained the fix to the risk engine instead).
- Exact carry revaluation for ITM American puts (the delta proxy over-captures 1.10–1.18× where early exercise
  binds; unexplained absorbs it).
- T-aware node floor further tuning (lever 6 took the knee at 0.3 y; a 4-node floor at every tenor fails at
  1.12e-6); explain pivot sampling on `run_dynamic`.
- Python bindings for `api/risk/` and `api/persist/`.
- European / Forward-framework route (SPX, NDX, XSP, RUT, OEX, XAU) — needs a Black-76 forward kernel.
- Sequential-revaluation / Shapley attribution; share dividend / financing lines; calendar-arbitrage-safe tenor
  interpolation (D9).
- `explain_into(..., RiskWorkspace&)` (D18); record `RiskWorkspace::reserve`'s rebind contract with it.
- `PersistDirectory::replace_all` (compaction writes its own manifest under the lock today).
- Async / parallel verify and decode (uncapped Full CRC ≈ 51 ms single-stream; cold-tier decode serial;
  3-way interleaved CRC32C not taken); async seal.
- `tools/state_store_main.cpp` passes conventions version 1 (`TODO(integration)`: `kSpiderRockConventionsVersion`,
  now 3), and the state-store source fingerprint omits the spec's clock / carry_mode / spot_framework_only
  (lane E review 2: a library build with a non-default spec into a root built with the defaults would skip
  every bucket and serve the default-spec states; the tool uses the default spec, so no caller is hit
  today); `--state-store` input for `atx-vol-risk-bench`.
- Flagged to the user: the bench's top-100 srVol marks may include vol-sota validation names (it fits nothing,
  judged low risk); `C:\atx-data2` (empty, guard-protected) to delete by hand.

## 11. Evidence index

Reports: `.superpowers/sdd/2026-10-24-risk-engine/lane-{A,B,C,A2,D,E,P,P2}-report.md`; reviews
`lane-*-review-N.md`; trail `progress.md`, `HANDOFF.md`; session-1 raw numbers `sprint-note-draft.md`.
Bench and parity: `artifacts/risk-bench-sr.{json,md}` (first licensed run), `artifacts/s2-parity-after-c2.md`,
`artifacts/s2e-parity-after-E.md`, `artifacts/s2e-risk-bench-*.{json,md}`, `artifacts/s2e-legs-1000.csv`,
`artifacts/s2e-sticky-sim.json`; A2 receipts and helpers `artifacts/a2-*.{ps1,py}`, pinned binaries
`C:\atx-wt\pool-36\build-vol-rel-avx2\bin\atx-vol-risk-bench-{L2,L3,L4a,L4b,L6,FINAL}.exe`. Persist measurements:
`C:\atx-cache\persist\measure\` (P1), `C:\atx-cache\persist\p2\` (P2). Docs: `docs/risk-engine.md`,
`docs/risk-bench.md`, `docs/historical-var.md`, `docs/persist.md`, `docs/persist-history.md`. Plans:
`wave3-persist-design.md`, `wave3-fit-robustness-design.md`.
