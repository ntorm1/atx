# Brief: task R-2

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task R-2: library v8.0, definitional upgrades from existing fields

**Files:** registry entries through `add-alpha`; `scripts/specs/v8/lib-v80.json`; read-only draft lane writes
`library-v8-draft.md` first

Draft DSL below states the semantics. The draft lane checks every string with `--plan-only` and against the field
manifests before the freeze. A string the checker refuses is rewritten mechanically if the semantics hold; otherwise the
candidate is withdrawn, which lowers the trial count.

| id | theme | tier | semantics (the registration) | draft DSL |
|---|---|---|---|---|
| `ear_mom_12m` | earnings_momentum | B+ | sum over 252 sessions of the market-adjusted return from close a-3 to close a+2 around each earnings date a | `rank(ts_mean_mp((((ea_days_since == 2) ? (((close / delay(close, 5)) - 1) - ts_sum(mkt_ret, 5)) : 0)), 252, 126))` |
| `earn_surprise_comp` | earnings_momentum | C+ | mean of the ranks of the three v7.1 members sue, droe and chtax, each taken before its outer rank; replaces the three as members | `rank(decay_linear((((rank(X_sue) + rank(X_droe)) + rank(X_chtax)) / 3), 21))`, where `X_m` is the inner expression of member m copied verbatim from `fund_industry_ic_v71.json` |
| `op_rd` | profitability_quality | A- | (operating income TTM + R&D TTM, missing R&D = 0) / total assets, within FF12 | `group_rank(decay_linear(((oi_ttm + xrd0_ttm) / at), 21), grp_ff12)`; `xrd0_ttm` is the zero-filled field of F-1 |
| `dtc_slow` | short_interest | B+ | short interest shares / mean daily volume over 126 sessions; short high | `rank(decay_linear((-1 * (si_shares / ts_mean(volume, 126))), 21))` |
| `pct_accruals` | profitability_quality | B- | (net income TTM - CFO TTM) / abs(net income TTM); long low | `rank(decay_linear((-1 * ((ni_ttm - cfo_ttm) / abs(ni_ttm))), 21))` |
| `fip_id` | price_momentum | B | share of positive days minus share of negative days over the 12-1 window (231 sessions ending 21 sessions ago): long names whose past return came in many small steps, short the mirror image | `rank(decay_linear(delay(ts_mean(sign(((close / delay(close, 1)) - 1)), 231), 21), 21))` |
| `si_low_io` | short_interest | B- | short interest over institutional ownership; short high; also the borrow proxy of G-3a | `rank(decay_linear((-1 * ((si_shares / shares_out) / inst_own_share)), 21))` |

Also in this revision, as registry edits that are part of the same cell: financial firms are ranked inside FF49 groups for
`ebit_ev`, `gpa`, `cbop`, `noa`, `accruals`, `opex_at`, `asset_growth`, `q5_eg` (signal review S-12).

- [ ] **Step 1:** draft lane writes `library-v8-draft.md` (definitions, deviations from the papers, expected correlations,
  priors with the class-specific haircuts of the literature report). Root appends it to `v8-prereg.md`.
- [ ] **Step 2:** `add-alpha` for each member; `run --screen`; read admission under `v4-prior-v1` (unchanged rule).
- [ ] **Step 3:** full `run`; judge the wave whole.

**Acceptance:** wave accepted whole: dSR > 0 AND mechanics AND book turnover not higher.
**Expected [est]:** net Sharpe +.02 to +.08; earnings-theme turnover down. **Cost:** 7 admission trials + 8 re-screens, 1 cell (N 42).

