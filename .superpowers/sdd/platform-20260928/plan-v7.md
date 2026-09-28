# Platform v7 sprint plan -- synthesis of P1 (code review) and P2 (literature) into build decisions

Owner goal: production equity L/S platform (high Sharpe, high capacity, low turnover, daily portfolios) with research
iteration that gets faster each sprint; then the mega strategy; then the pitch. Inputs: code-review-v7.md (P1, 21 findings,
measured timings), literature-v7.md (P2, 7 topics, ranked build list). Baseline book: v6.1 cell S2 net +1.239 (TRAIN
2020-2022), DSR N29 .911 < .95, 32 admitted sleeves, ew-theme-v1, aim-partial-v5.

## 1. Decisions (code review x literature)

| id | decision | evidence (P1) | evidence (P2) | lane |
|---|---|---|---|---|
| D1 | Content-keyed IC cache (alpha DSL sha + per-field payload sha), verify only loaded fields, SHA-NI | A1: one field invalidates 35/39; A2: SHA = 30/76 s (u), 19/31 s (w) | S5: platform scales to 100+ alphas only if admission is cheap | L1 |
| D2 | One research-cycle driver from a spec file, hard-stop on refused receipt, field `--reuse`, shared fit store | A4: 9 hand-copied ladders, 600 lines per version; A3: 2 GB rebuild for one field; C2 | S5 R5.1 trial ledger needs every run receipted | L2 |
| D3 | Backtest-integrity tooling in nav_summ: JSONL trial ledger, effective-N DSR (ONC clusters), CSCV PBO on grids, PSR/MinTRL | -- | S5 rank 1: cheap, judges every later lever; pre-register effective-N from v7 (do not re-score v6) | L2 |
| D4 | Daily decide path: per-name holdings/fills emit, `decide` verb from actual positions, `atx.book-deploy/v1` manifest, no_short mask input; no seal change | B3, B2, B1, B6 | S7: monitoring needs holdings; production prerequisite | L3 |
| D5 | Fundamental risk model `atx-risk-v1` (market + FF49 + ~10 styles, EWMA 84/504, NW, Bayesian specific risk) + bias harness | B5: risk/factor_model.hpp exists unused; price-risk-v1 = 4-column OLS | S3 rank 3: ex-ante risk within 1 +- sqrt(2/T); prerequisite for the optimiser | L4 |
| D6 | Cost model v2: name-level Kyle-Obizhaeva / FIM impact as stress scenarios S2-KO, S2-FIM (S2 stays primary) + replayed capacity curve over L | -- (cost sim in the NAV replay) | S4 rank 2: resolves S1 +1.32 vs S2 +1.18; AUM decision | L4 |
| D7 | Construction interim `aim-partial-v6`: cost-scaled target shrink kappa, cost^(1/3) band, regime rate theta_t; full SPO/GP optimiser deferred to wave 2 (needs D5 + D6 landed) | B5: QP + GP aim exist in atx-engine/risk | S2 R2.2/R2.3 (M, S); R2.1 (L) after R3.1, R4.1 | L4 (rule), wave 2 (SPO) |
| D8 | Keep ew-theme-v1 weights; add decay-and-vol theme weighting only as a pre-registered single trial (Tu-Zhou delta ~ .2) | -- | S1: fitted weights lose 35-60% SR at T = 3 y; v3 TRAIN 1.81 -> VAL -1.27 | wave 2 |
| D9 | Root: Release exes A/B (identity), determinism canary per build tag, disk GC | A5, C3 | -- | root |
| D10 | Deferred: DSL ops (A7), wave-1 library (q5 Eg, nincr, QMJ-safety), per-alpha report card (R5.6), date-blocked runner (A6, U2 only) | | | wave 2 |

Do not build (P2): IC/MV/ML-fitted sleeve weights on 3 y; drawdown stop-losses; finer theta/dust grids on TRAIN.

## 2. Lanes (disjoint files; merge order L1 -> L2 -> L3 -> L4)

| lane | pool | branch | owns | root acceptance |
|---|---|---|---|---|
| L1 fast incremental IC runner | pool-10 | feat/platform-v7-l1-iccache-20260928 | atx-impl/src/strategy_ic_runner.cpp (+test), atx-core/src/sha256.cpp (+test, per-source flags) | v6.1 u on v6u-seeded cache >= 38 hits; orientations.json / train_daily_ic.csv / train_candidates.jsonl byte-identical to mega-v61-train-u-1 (minus timing); u <= 20 s, w <= 15 s; digests equal scalar on every manifest |
| L2 research-cycle driver + integrity tooling | pool-11 | feat/platform-v7-l2-cycle-20260928 | scripts/research_cycle.py (+tests, specs/), atx-engine/tools/prepare_research_fields.py (`--reuse` only), studies/nav_summ.py extensions, atx-impl/tools/fit_composition_weights.py (WorkStore, stretch) | `research_cycle.py run specs/v61.json` reproduces the v6.1 cell S2 daily CSV SHA with zero hand pins; `--reuse` from v6b builds only sv_ratio126 with payload SHAs equal to fields-v7; refused receipt stops the cycle; trial ledger + effective-N DSR + PBO reproduce the v6 N29 numbers when run in legacy mode |
| L3 daily decide path (TRAIN-only) | pool-4 | feat/platform-v7-l3-decide-20260928 | atx-impl/src/strategy_nav_replay.{cpp,hpp} (`--emit-holdings`), new strategy_live.{cpp,hpp}, tools/equity_strategy_targets.cpp (`decide`), new tests/strategy_live_test.cpp, atx-impl/CMakeLists.txt source list | flag off -> every v6.1 NAV output byte-identical; at 3 pinned TRAIN decisions decide == replay planned weights bit-for-bit; deploy-manifest refusal tests; no seal change |
| L4 risk model + cost model v2 + aim-partial-v6 | pool-3 | feat/platform-v7-l4-risk-20260928 | new atx-impl/src/strategy_risk_model.{cpp,hpp}, strategy_cost_v2.{cpp,hpp} (+tests); atx-engine/risk/* only if needed; strategy_nav_replay.cpp touched only through one dispatch hook (<= 30 lines) that root rebases after L3 | bias harness on TRAIN random/factor portfolios within band; S2-KO/S2-FIM scenarios run on the v6.1 cell; aim-partial-v6 is a pre-registered construction trial (kappa, b, theta clip declared in v7-prereg.md before any read) |

Cross-lane contract (declared now): L1 sidecar key `field_payload_sha256` = map field -> payload sha256; payloads named
`<id>.<dsl_sha16>.{f64,json}`; old layout stays readable. L2 reads that key; L2 does not edit strategy_ic_runner.cpp.
L3/L4 both compile against strategy_nav_replay.cpp: L3 owns it, L4 adds its rule behind one hook and rebases on L3.

## 3. Trials and statistics
- L1, L2, L3: identity work (same book, same window) -- no new trials.
- L4 risk model + cost scenarios: descriptive on the existing book -- no new trial. aim-partial-v6 cells: construction
  trials, pre-registered in v7-prereg.md (kappa, b, theta clip; grid size fixed; PBO reported), cross-cell N continues
  from 29.
- Effective-N DSR reported beside cell-count DSR from v7 on; the v6 gate is not re-scored (R5.2).
- TRAIN 2020-2022 only. U1 (validation trial #3) remains an owner decision.

## 4. Wave 2 (after merge of L1-L4)
SPO optimiser around the GP aim with Sigma_risk from D5 and costs from D6 (R2.1); DSL ops (A7) then wave-1 library
(q5 Eg, nincr, QMJ-safety) as one pre-registered revision; per-alpha report card (R5.6); monitoring M1-M4 (R7); decay-and-vol
theme weights as one trial (D8); pitch iteration 3 from the regenerated report with holdings, capacity curve, risk bias.
