# From theo machinery to tradeable alpha — next-iteration map

Date: 2026-08-09. Status: research/roadmap (feeds a future sprint plan; not itself a task list).
Prior work: theo-module sprint (merged `8da2e6b`) — see
`atx-vol/sprints/2026-08-08-theo-module-sprint-summary.md` and
`docs/research/2026-08-08-theo-fair-vol-breakeven-deep-dive.md`.

The 2026-08-08 sprint built the machinery: breakeven-vol (BEV) labels from replayed
hedged paths, a `TheoEngine` that composes fair-vol overlays over live surfaces, an
`IFairVolModel` ML seam, and read-only edge signals in the backtest. It deliberately
produced **no alpha claim** — no trained model, no positions, no costs. This document
maps the iteration that turns the machinery into a tested, net-of-cost alpha pipeline,
grounded in a literature sweep (2024–2026 emphasis) summarized in §2.

---

## 1. Where we stand

### Assets (shipped, review-hardened)

| Asset | Where | Role in this iteration |
|---|---|---|
| BEV label engine | `bev_replay_pnl` (`src/backtest.cpp` append), `solve_breakeven_vol`, `solve_breakeven_batch` | Produces the target: the vol at which a delta-hedged position breaks even, per contract, from replayed real paths |
| Label factory driver | `examples/bev_label_factory.cpp` | Corpus-scale TSV generation. The final-review I1 fix (hoisted path load, date-bounded clock) made multi-year runs feasible (~150× fewer archive opens) |
| Path loader | `load_bev_path` (`breakeven.cpp`) | Session paths from surface archives; `s > 0` postcondition now enforced |
| Theo engine | `theo.hpp`/`theo.cpp` — `TheoEngine`, `value_into`, `compute_theo_sheet` | Serves `theo_vol / theo_price / edge_vol / band_vol` per query; zero-overlay bit-identity to the surface; 256-query chunked hot path (~40k vol-space queries/s measured, noisy capture) |
| Overlays | RV-blend, event-variance, fair-vol-model | Composable dvol sources; fail-open (`ModelMissing`) by contract |
| ML seam | `IFairVolModel`, `load_linear_fair_vol_model`, 8-feature schema v1 | Model inference boundary. Linear v1 loader shipped; schema enforced at overlay construction |
| Event machinery | `event_vol.hpp` — `censored_total_variance`, `event_recombined_vol` | Exactly the practitioner variance-differencing formulation for stripping/injecting event moves (§2.4) |
| RV estimators | `realized_vol.hpp` — CtC, Parkinson, GK, RS, YZ panel | Feature producers, once fed real OHLC bars |
| Backtest probe | `TheoEdgeSignalStrategy` (SPY LEAPS example) | Proven read-only signal channel; NAV byte-identity verified |

### Gaps (each one blocks alpha; all recorded in the sprint's residual register)

| # | Gap | Consequence |
|---|---|---|
| G1 | **No feature producer.** Label TSV carries target + join keys only. `rv_21d/rv_63d` have no real-data producer at corpus scale; `n_events_to_expiry` has no producer at all (no earnings calendar in the C++ corpus) | Cannot train anything beyond degenerate models |
| G2 | **No trained model.** Linear v1 loader exists; nothing has ever been fit | The seam is empty |
| G3 | **Band is a placeholder** (`0.5·|dvol|`; RV-blend reports 0 → served band is the config floor) | No uncertainty signal → no principled sizing |
| G4 | **No cost model.** Backtest is frictionless by construction in the probe example | §2.2 says this is the difference between publishable and tradeable |
| G5 | **No portfolio layer.** Signals are recorded, never converted to positions | No P&L, no Sharpe, no falsification |
| G6 | **Single-name, single-underlier corpus wiring** (SPY 2019 in examples) | Cross-sectional strategies need a panel |
| G7 | **No validation harness** (purged CV / embargo lives Python-side, unbuilt) | Any fit result is untrustworthy |

---

## 2. What the literature says (and how it bends the design)

Full citations in §7. Four load-bearing findings:

### 2.1 The BEV approach is published and validated — we independently built its engine

Hull, Li & Qiao, *"Option Pricing via Breakeven Volatility"* (Financial Analysts
Journal 79(1), 2023): compute BEV for ~400k S&P 500 options from realized hedged
paths, fit a predictive model from current characteristics to future BEV, trade the
gap — with a simulated strategy showing economic value. That is, stage for stage, the
pipeline this repo now has the C++ half of. Their stated core constraint — BEV needs
the option's full future path, so a real-time system must *predict* it — is exactly
the `IFairVolModel` seam's job. Design implication: **our next step (predict BEV from
point-in-time features) replicates a peer-reviewed result before extending it**, which
is the right risk ordering.

### 2.2 Costs are the first-class citizen, not a haircut at the end

O'Donovan & Yu (2024): of 24 published delta-hedged option characteristics, **17 are
profitable gross and zero are profitable net of quoted spreads** under standard
execution; an asymmetric entry/hold rule (stricter threshold to open than to keep)
restores 7 of 24. Christoffersen et al. (RFS 2018): option illiquidity itself is
priced — part of any measured "edge" is compensation for the spread you'll pay.
Goyal & Saretto's own IPCA re-assessment (RFS 2025) collapses the average alpha of 46
published option signals to ~zero once latent factor exposure is priced. Chen, Hu &
Yang (2025): residual predictability concentrates precisely where adverse-selection
spreads are widest.

Design implications, all structural:
- The backtest cost model (G4) ships **before** any strategy is evaluated, and every
  gate in §5 is net-of-cost.
- Position logic uses the **asymmetric open/hold threshold** from day one — it is the
  one documented mitigation that works.
- Edge must clear `half-spread(contract) + hedging drag`, both measured per liquidity
  tier, not assumed constants. Single-name option spreads run low-to-high-teens % of
  premium even for liquid ATM lines.
- Expected live factors, per the post-IPCA survivors: HV−IV gap, vol-of-vol,
  idiosyncratic-vol (the one repeatedly shown cost-robust), term slope. These are
  exactly the features the schema already names — the point of the ML model is the
  *conditional combination*, not a new anomaly.

### 2.3 Model class and validation are settled questions — don't relitigate

- Tabular, low-SNR, ~10⁵–10⁶ rows: **gradient-boosted trees are the default** (Bali
  et al. RFS 2023 for options specifically; Grinsztajn et al. NeurIPS 2022 for
  tabular generally). Neural nets win only with pooled intraday/sequential structure
  we don't have yet. A 2025 Fed study (Kilic) still has regime-switching HAR beating
  ML on raw RV — so **HAR-family stays in as the baseline that ML must beat**.
- Labels defined over an option's life overlap massively → **purged K-fold with
  embargo sized to the label horizon** (López de Prado), CPCV for model selection,
  walk-forward for the final read. Leakage traps specific to us: IV appears in both
  features (`market_vol`, `iv_minus_rv`) and the label denominator
  (`log_ratio = ln(σ_be/σ_entry_iv)`) — feature/label snapshot times must be
  staggered; earnings dates must be as-scheduled-at-the-time, not as-realized;
  normalization must be expanding-window only.
- Uncertainty: **quantile heads (GBT quantile loss) + conformal calibration**
  (Romano et al. 2019; Gibbs & Candès 2021 for the time-series variant). One
  practical 2026 result (Ryan, "Conformal Kelly"): *stable, slowly-adapting* interval
  widths sized positions better than sharp adaptive ones — bands feed sizing as a
  slow scale, not a fast signal.

### 2.4 Event vol is a separate, real, but crowded-at-the-top game

- The implied-earnings-move premium has **no universal sign**: rich in mega-cap
  high-attention names (NVDA-type beat rates ~25%), cheap in low-attention names
  (GXZ 2013: +3.3% gross pre-earnings straddles, concentrated in small/illiquid
  names where costs eat it). Curve *concavity* is an ex-ante predictor of event-day
  moves (Alexiou et al., Review of Finance 2025). Calendars-across-the-event
  backtested best out-of-sample in the one systematic practitioner study (ORATS).
- The extraction identity practitioners use — event variance additivity across the
  bracketing expiries, iterated until the ex-event term structure is smooth — **is
  what `censored_total_variance`/`event_recombined_vol` already implement**. The gap
  is purely data: a point-in-time earnings calendar (G1).
- Design implication: event-vol RV is thesis #2, behind the BEV model, and its first
  deliverable is a *forecast of the event move* (own-name history + concavity +
  term-kink features) plugged into the existing `EventVarConfig::emove_forecast` seam
  — zero new C++ pricing code.

---

## 3. Strategy theses, ranked

**T1 — BEV fair-vol relative value (primary).** Train `IFairVolModel` v2 on corpus
BEV labels (Hull-Li-Qiao replication on our data), serve `theo_vol` with bands, take
delta-hedged vol-space positions where `|edge_vol| > k·band + cost hurdle`, long-cheap
short-rich cross-sectionally within the liquid-name universe. The gamma-P&L identity
(dP&L ≈ ½ΓS²(σ²_real − σ²_imp)dt) makes BEV the label most directly aligned with
hedged P&L. Vol-space (screening) mode of `compute_theo_sheet` is the cheap scan path;
Andersen-Lale reprice only on candidates.

**T2 — Event-vol RV (secondary).** Forecast per-event daily move from own-name event
history + curve shape; compare to the market-implied event move stripped via the
existing event machinery; trade calendars/straddles across events only where the
forecast-vs-implied gap clears the (event-window-widened) cost hurdle. Feeds the same
portfolio layer as T1.

**T3 — Carry/term conditioning (overlay, not standalone).** Term-slope and IV-rank
conditioning gates on T1/T2 entries (both documented as the strongest cheap
conditioners). No new model — features already in the schema.

Explicit non-goals this iteration: index VRP harvesting (Dew-Becker & Giglio 2025 —
the index premium has eroded to ~zero alpha), dispersion (needs correlation infra we
don't have), intraday anything.

---

## 4. Stage map

Ordered so every stage kills a gap and is independently testable. C++/Python split
follows the house rule (trainer and CV tooling live Python-side with the data;
inference and replay stay C++).

**S1 — Feature factory (kills G1).**
Extend `bev_label_factory` to emit the full `kFairVolFeatureSchemaV1` columns beside
the target: `log_moneyness`, `tenor_years`, `market_vol` already come from the entry
surface; `rv_21d/rv_63d/iv_minus_rv` need a spot-history producer (close-to-close from
the archive spot series is available today; OHLC bars are an acquisition item — YZ/GK
estimators stay dormant until then); `n_events_to_expiry` needs a point-in-time
earnings calendar (acquisition item; as-scheduled dates, not as-realized);
`delta_abs` from the entry surface's analytic delta. Schema version bumps to v2 if
columns are added beyond the eight. Update `theo.hpp`'s ML-seam banner (it now
documents that the trainer assembles features offline — S1 makes the label factory do
it instead, restoring the original intent honestly).

**S2 — Corpus-scale labels (kills G6).**
Panel run: the surface-db universe (start: the most liquid ~50–100 optionable names
plus SPY), 3–5 years, entry cadence weekly, tenor ladder 30/60/90/180d, delta lattice
as shipped. QA artifacts: label counts by censor reason, BEV−entry-IV distribution by
tenor/moneyness, dedup ratio. This is a compute job on existing code paths (I1 fix
makes it feasible); its output is the training set.

**S3 — Trainer + validation harness (kills G2, G7; Python).**
Baselines first: HAR-RV, and linear-on-schema (the shipped v1 loader format) — ML must
beat both out-of-sample or the iteration stops here. Then LightGBM/XGBoost with
quantile heads (P10/P50/P90). Purged K-fold + embargo ≥ max label tenor; CPCV for
hyperparameter selection; final walk-forward. Deliverables: model artifacts, feature
importance, per-fold OOS R² and — the number that matters — **spread capture**: mean
realized BEV−IV in the top-vs-bottom predicted-edge deciles, gross and net of the S5
cost model.

**S4 — Model serving + real bands (kills G3; C++).**
`IFairVolModel` v2: flat-array GBT scorer (trees exported from XGBoost/LightGBM JSON
to a compact binary the loader mmaps; no runtime dependency). Quantile heads → band =
(P90−P10)/2, conformally calibrated offline, replacing the `0.5·|dvol|` placeholder
and closing the "quantile heads" residual. Construction-time schema check already
enforces compatibility.

**S5 — Cost model + execution realism (kills G4; C++ backtest extension).**
Per-contract effective-spread model (fraction of premium, parameterized by moneyness,
tenor, and a per-name liquidity tier measured from the corpus), delta-hedge drag, and
the asymmetric open/hold threshold as an engine-level primitive. Extends the existing
`FrictionModel` seam in the backtest — same engine-extension discipline as the BEV
replay (no parallel engine).

**S6 — Portfolio layer (kills G5).**
Convert per-query `edge_vol/band_vol` into vega-budgeted, delta-hedged positions:
enter when `|edge| > k_open·band + hurdle`, hold while `|edge| > k_hold·band`
(k_hold < k_open per §2.2), size ∝ edge/band capped per-name and per-book
(slow-conformal-Kelly spirit: bands as a stable scale). Cross-sectional
dollar-vega-neutral within the universe. Implemented as a real `IStrategy` (the
decorator probe graduates), evaluated by the existing engine with S5 costs.

**S7 — Evaluation gates (the point of it all).**
Promotion requires, on the walk-forward window, all of:
- net-of-cost Sharpe > 0.5 with positive net P&L in ≥ 60% of quarters;
- Deflated Sharpe / PBO from the CPCV paths clearing standard thresholds;
- capacity estimate (position sizes vs. per-name OI/volume) supporting the claimed
  P&L at ≥ 10× intended book size;
- regime slices (low/high VIX, earnings-heavy vs. quiet months) with no single-regime
  dependence;
- the S3 baselines beaten net, not just gross.
Failure at any gate falls back one stage with a written finding — the same discipline
as the sprint's review loops.

Suggested sprint packaging: **Sprint A = S1+S2** (data), **Sprint B = S3** (model),
**Sprint C = S4+S5** (serving+costs), **Sprint D = S6+S7** (trading+gates). A/B are
sequential; C can start once B has a frozen schema; D needs all.

---

## 5. Data acquisition (blocking items, all in S1/S2)

| Item | Need | Source options | Blocking for |
|---|---|---|---|
| Point-in-time earnings calendar | as-scheduled dates per name, history ≥ label corpus span | vendor calendar; SEC 8-K timestamps as fallback (as-filed, imperfect PIT) | `n_events_to_expiry`, all of T2 |
| Daily OHLC bars | per-name, corpus span | already adjacent (atx-db price metrics pipeline); else archive spot series gives CtC-only | YZ/GK/RS estimators (CtC works without) |
| Liquidity measures | per-name option volume/OI, quoted spreads | OPRA-derived if archived; else coarse tiers from underlying ADV | S5 cost model, S7 capacity |
| Borrow/short interest | optional feature (skew channel) | atx-db FINRA short-interest pipeline exists | feature v2, not blocking |

---

## 6. Risks and falsifiers

- **The IPCA null.** If the trained model's edge is fully explained by exposure to
  level/slope/skew option factors, it's beta, not alpha. Mitigation: regress S6
  portfolio returns on the straddle-factor proxies; the gate is *residual* Sharpe.
- **Costs eat everything** (the modal published outcome, per §2.2). This is why S5
  precedes S6 and why the hurdle sits inside the entry rule, not the report.
- **Label leakage** via IV-in-features/IV-in-label. Mitigation is structural (S3
  staggering + purge), and the S3 report must include a leakage audit
  (shuffled-label and future-blind sanity fits).
- **Single-host golden-pin risk** carried from the sprint (BEV bit-determinism pin
  observed on one host only) — first full-suite run on a second machine will tell.
- **Small-sample event studies.** T2 magnitudes in the literature come from
  1996–2010-style samples; the 2026 regime (ORATS: straddles +45% in one window)
  says the premium sign is unstable. T2 ships only with per-name conditioning, never
  as an unconditional harvest.

---

## 7. References

Cross-sectional / costs: Goyal & Saretto (JFE 2009; RFS 2025 IPCA re-assessment);
Vasquez (JFQA 2017); Cao & Han (JFE 2013); Ruan (JFM 2020); Zhan, Han, Cao & Tong
(RFS 2022); Büchner & Kelly (JFE 2022); Horenstein, Vasquez & Xiao (RFS 2025);
Christoffersen, Goyenko, Jacobs & Karoui (RFS 2018); O'Donovan & Yu (SSRN 4806038,
2024); Chen, Hu & Yang (SSRN 5750824, 2025); Dew-Becker & Giglio (Chicago Fed WP
2025-17); Bali, Beckmeyer, Moerke & Weigert (RFS 2023); Borochin & Zhao (JEF 2025).

BEV / targets / models: Hull, Li & Qiao (FAJ 2023); Christensen, Siggaard & Veliyev
(JFEc 2023); Zhang, Zhang, Cucuringu & Qian (JFEc 2023); Kilic (Fed FEDS 2025-061);
Kim & Oh (Fed FEDS 2026); Grinsztajn, Oyallon & Varoquaux (NeurIPS 2022); Krauss, Do
& Huck (EJOR 2017).

Validation / uncertainty: López de Prado (*Advances in Financial ML*, 2018 — purged
CV, embargo, CPCV, DSR/PBO); Romano, Patterson & Candès (NeurIPS 2019); Gibbs &
Candès (NeurIPS 2021); Ryan (arXiv:2608.01494, 2026).

Event vol: Patell & Wolfson (1979/1981); Dubinsky & Johannes (SSRN 2006); Dubinsky,
Johannes, Kaeck & Seeger (RFS 2019); Gao, Xing & Zhang (JFQA 2013); Alexiou, Goyal,
Kostakis & Rompolis (RoF 2025); Milian (JRFM 2023); de Silva, So & Smith (RoF 2025);
Roll, Schwartz & Subrahmanyam (JFE 2010); Xing, Zhang & Zhao (JFQA 2010); Zhong
(arXiv:2606.12872, 2026); ORATS earnings backtests; moontower.ai event-variance
extraction; Cboe DSPX documentation.
