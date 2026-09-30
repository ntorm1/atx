# Task G report: zero-trial book diagnostics G-1a..G-3d (lane G, task 2)

Lane G, pool-8, branch `feat/platform-v8-g-20260929`. Status: STOPPED at a break point on the owner's instruction
(module, tests and helpers committed in `a5c83802`; open items below). Python only; nothing built, no real data.

Declared (v8-prereg rule 8, already in `v8-prereg.md`; repeated in the tool's docstring and in the output header):
no diagnostic gates, selects or re-weights anything.

## What was built

`atx-impl/tools/book_diagnostics.py` (one module) and `atx-impl/tools/test_book_diagnostics.py`.

One pure function per diagnostic (arrays or parsed documents in, a dict out; reusable by the report renderer):

| id | function | definition (short; the docstrings carry the full rule) |
|---|---|---|
| G-1a | `member_horizon(cards, book, k6)` | per book member, oriented by its sign: ic1, ic21, ic_theta (card `horizon.ic_theta`, else `horizon_stats.ic_theta` of the card's decay curve), retention = ic_theta / (ic1 x weight sum), contribution = weight x ic_theta, f_theta / f_theta_hac_t (card admission, C-2), K6 marginal IC; lists members negative at theta and unscored ones (C-5) with their weight |
| G-1b | `turnover_attribution(signals, members, member_mask, theta, L, book_trades, score_from, fast)` | blend B = sum s_k w_k r_k (the composition); member slice a_k = L s_k w_k r_k / sum_i abs(B_i) (exact linear share of the aim); each slice moved at theta from flat (`theta_trades`); own turnover and own share per member and per theme; attributed share = sum sign(T) trade_k / sum abs(T) against the model combined book (L x desired_target(B) at theta) and, when given, the NAV's planned trades (target_weight - held_weight from --emit-holdings); remainder = the nonlinear part; the four fast members (highest admission tau from the weights provenance) and their shares |
| G-1c | `netting_ratio(combined, sleeves, rows)` | mean combined turnover / sum of the themes' own-aim turnover (1 = nothing cancels); daily median, p5, p95; also against the member sleeves |
| G-2a | `variance_split_session` + `variance_split` | x = X'w on atx-risk-v1.1 (market, 50 industry slots, 11 styles); Euler split of x'Fx into market, industry, style (per style) plus specific w'Dw; mean shares, share of summed variance, `factor (market + style) / industry / specific`, ex-ante vol, per year; a session with an exposed unforecast factor is incomplete |
| G-2b | `ic_by_groups(signals, members, label, groupings)` + `group_ic`, `terciles`, `top_n` | rank IC at h 21 (the card's label, `alpha_report_card.Geometry`) within volatility terciles (SD of valid returns, 63 sessions), ADV terciles (the card's ADV63), top 1,000 by me_company (ADV63 fallback) and all; per member and weight-averaged |
| G-2c | `holding_over_adv(dollars, adv, q=.10)` | held dollars (long, short, all) and aim dollars (L x nav_post x abs(desired)) over the NAV's execution ADV ([t-w, t), w from the recipe): p50, p95, p99, max, share of cells and of gross above R-5's Q = .10 |
| G-3a | `fee_deciles`, `borrow_fee_drag`, `restate_net` | S2-FEE: fee by decile of (si_shares / shares_out) / inst_own_share within the short book of t-1 (fields of t-1), schedule 25, 25, 25, 25, 25, 25, 30, 50, 150, 570 bps per year on short market value, missing ratio = the median fee (25); drag = sum fee x short x days / day count / pre-trade NAV of t-1; restated net = S2 net + S2's tier fee (from the daily CSV's tier short dollars and the summary's fee spec) - S2-FEE fee; net Sharpe of both, annual drags, decile shares, the daily series, a short-dollar reconciliation holdings vs daily CSV. Labelled "descriptive, never primary; S2 stays primary" |
| G-3b | `projection_ic(raw, projected, support, label)` | low-risk members (theme `low_risk`, `--projection-theme`): rank IC at h 21 of the centred-rank book before and of the price-risk-v1 residual after (the fitter's `Context.book`, the admission's neutralised sleeve; Pearson with the label ranks, linear in the residual as F-2); retained = after / before; NW t lag 21 |
| G-3c | `signal_lag(base, lagged)` | per delay k: net Sharpe of the lagged cell and the base on common sessions, difference, Memmel SE (nav_summ) |
| G-3d | `cluster_map(pnl, names, themes)` + `average_linkage`, `cut_clusters`, `adjusted_rand`, `effective_bets` | card `daily_sleeve.csv` PnL: pairwise Pearson, distance sqrt((1-rho)/2), UPGMA cut at the number of themes, adjusted Rand vs the theme labels, contingency, mean rho within / between themes, theme PnL correlation and its effective number of bets (participation ratio) |

CLI (`run`): reads the named inputs, pins each (path + SHA-256 of its primary file), writes one JSON file (schema
`atx.book-diagnostics/v1`): header `schema`, `declaration`, `window_id`, `tool.script_sha256`; `diagnostics` = one key
per id with `question`, `inputs`, `method`, `result`, `status` ok|skipped and `reason` when skipped. An absent input
skips its diagnostics; a present input failing a pin, a schema or the seal refuses the run (exit 2, nothing written);
the output is never overwritten. Seal: NAV daily CSVs via `nav_summ.load_daily_csv`; role, holdings (f64 or csv),
card index window and `daily_sleeve.csv`, K6 window, risk role and book, combined sessions via
`backtest_integrity.refuse_sealed` (the research window of W0-1, as nav_summ reads it).

Helpers: `lag-combined` writes a saved combined signal delayed K sessions (members at d take signal(d-K), 0 when
absent or d < K, non-members NaN; manifest keys kept, two receipts, `finite_cells`, `signal_lag_sessions`,
`derived_from` updated), the input of a G-3c NAV cell (the NAV verb has no delay flag). `book-csv` writes the risk
verb's `session_ns,instrument_id,weight` book from the default f64 holdings.

Input formats were taken from the writers: `strategy_nav_replay.cpp` (daily CSV, summary, recipe, holdings v1/v2),
`strategy_holdings.hpp/.cpp` (f64 layout and index), `alpha_report_card.py` (cards, index, daily_sleeve.csv),
`strategy_marginal_ic.cpp` (K6 and its window), `strategy_risk_verb.cpp` (manifest, arrays, book reader),
`prepare_research_fields.py` / `research_fields_holdings.py` (field manifest, `inst_own_share`),
`strategy_ic_runner.cpp` (combined signal manifest), `fit_composition_weights.py` (weights, role, cache, Context).

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_book_diagnostics.py      # 16 passed
```

Here, with W0-1's `research_window.py`, `research_window.json` and `engine_tools.py` copied in untracked from
`880faac7` (removed afterwards; W0-1 merges first): 16 passed. With the ledger and validation-kit files: 40 passed,
1 skipped. Tests: `test_member_horizon_ic_theta_marginal_and_unscored` (G-1a), `test_planted_fast_member_has_highest_
turnover_share` (G-1b on tiny_world's planted states plus a planted one-day reversal member), `test_netting_ratio_
identical_and_independent_sleeves` (G-1c; identical sleeves: remainder 0, ratio 1, shares = weights),
`test_theta_trades_and_desired_target_follow_the_construction`, `test_variance_split_hand_case` (G-2a, hand x'Fx
with a cross term, an unexposed NaN factor, an uncovered name), `test_ic_by_groups_planted_high_vol_tercile` (G-2b),
`test_holding_over_adv_quantiles` (G-2c), `test_planted_fee_schedule_reproduces_hand_drag` (G-3a: 975 bps on $1M
for 3 days, deciles by hand), `test_projection_removes_the_beta_ic_and_keeps_the_alpha_ic` (G-3b), `test_signal_lag_
costs` (G-3c), `test_cluster_map_recovers_planted_themes` (G-3d), `test_lag_combined_shifts_and_keeps_the_saved_
blend_contract`, `test_cli_end_to_end_every_diagnostic_on_a_synthetic_cell` (all ten `ok` through every loader: the
card tool's own u pass, cache and cards, K6, weights, NAV with financing columns, f64 holdings, fields, risk dir,
lagged cells), `test_cli_skips_absent_inputs_with_reasons`, `test_book_csv_and_the_v1_holdings_csv_read_as_the_f64`,
`test_seal_refuses_a_sealed_session` (a daily CSV and a K6 window on the seal: refused, nothing written).

Identity: new files only; no existing output changes.

## The commands root runs on a finished cell (B0c)

Names below are the B0c cycle's resolved outputs (`research_cycle.py plan scripts/specs/v8/<B0c spec> --lines-only`):
`$U` u pass, `$W` fit output, `$WORK` fit work dir, `$C` cards, `$MARG` marginal dir, `$WT` weighted pass (its
`train_combined.json`), `$N` NAV cell, `$ROLE` role dir, `$FIELDS` fields dir. After W0-1 is merged (the fitter's
role reader and the risk verb accept 2023 only then).

1. NAV flags the cell needs: `--emit-holdings $N-h` (f64, the default). If B0c already ran without it, re-run B0c's
   exact NAV argv with `--emit-holdings $N-h --output $N-hrun`; the daily CSVs must equal B0c's byte for byte (the
   holdings are an observer). Do not ledger the re-run.
2. Cards with the report-only columns (C-2): the card argv with `--ic-theta --marginal-ic $MARG/marginal_ic.json`
   (without `--ic-theta` G-1a computes the same ic_theta from the decay curve).
3. G-2a book and risk run:
   ```bash
   "$PY" atx-impl/tools/book_diagnostics.py book-csv --holdings $N-h --role $ROLE/manifest.json \
       --role-sha256 $ROLE_SHA --output build-equity/b0c-book.csv          # prints its sha256
   build-equity/bin/atx-equity-strategy-risk.exe risk --role $ROLE/manifest.json --role-sha256 $ROLE_SHA \
       --fields $FIELDS/manifest.json --fields-sha256 $FIELDS_SHA --output build-equity/b0c-risk \
       --book-weights build-equity/b0c-book.csv --book-weights-sha256 $BOOK_SHA --emit-exposures all
   ```
4. G-3c lagged cells (descriptive, not ledgered, no `--ledger`), for K in 1 2 3:
   ```bash
   "$PY" atx-impl/tools/book_diagnostics.py lag-combined --combined $WT/train_combined.json \
       --combined-sha256 $COMB_SHA --lag $K --output build-equity/b0c-lag$K-combined   # prints the new sha256
   # then B0c's NAV argv with only --combined / --combined-sha256 / --output changed:
   #   --combined build-equity/b0c-lag$K-combined/train_combined.json --combined-sha256 <printed> --output build-equity/b0c-lag$K
   ```
5. The diagnostics (through the bounded runner):
   ```bash
   "$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
     --output build-equity/b0c-diagnostics-run --bind atx-impl/tools/book_diagnostics.py -- \
     "$PY" atx-impl/tools/book_diagnostics.py run --output build-equity/b0c-diagnostics/diagnostics-v8.json \
       --cards $C --marginal-ic $MARG/marginal_ic.json --weights $W/composition_weights.json \
       --nav $N --holdings $N-h --u-pass $U --role $ROLE/manifest.json --role-sha256 $ROLE_SHA \
       --work-dir $WORK --fields $FIELDS --risk build-equity/b0c-risk \
       --lagged 1=build-equity/b0c-lag1 --lagged 2=build-equity/b0c-lag2 --lagged 3=build-equity/b0c-lag3
   ```
   `--work-dir` lets G-3b reuse the fitter's price-risk context instead of rebuilding it. G-3a needs
   `inst_own_share` in `$FIELDS` (a W5b holdings field); without it G-3a is skipped with that reason.

## STOPPED HERE (owner instruction, 2026-09-29)

Implemented and tested (synthetic arrays, the tiny_world fixture and one end-to-end CLI run on a synthetic cell):
G-1a, G-1b, G-1c, G-2a, G-2b, G-2c, G-3a, G-3b, G-3c, G-3d (all ten), plus `lag-combined`, `book-csv`, the skip rule
and the seal refusal.

Remaining (not started):

1. Real-size runtime and memory are not measured. Timed here at the 4-year size (1,070 rows x 5,627 names):
   `centered_tied_ranks` 0.8 s, `desired_target` 1.0 s, `theta_trades` 0.03 s. Estimates: G-1b two rank passes over
   about 38 members, about 60-80 s; G-2b calls the rank kernel twice per group (9 groups per member), about 3-5 s per
   member, which with G-1b likely exceeds the 180 s cap. Planned fix, not done: G-2b on
   `alpha_report_card.GroupLabels` (label ranks once per grouping, one value order per member, the card's own split
   machinery and coverage rules). Until then root can split the run with `--only` (for example `G-2b` alone) or
   raise the runner caps by ruling. Peak memory estimate 1.0-1.3 GB with `--work-dir` (role payload about 206 MB,
   card geometry about 150 MB, fields about 250 MB cached for the whole run); not measured.
2. G-1b's model books start 63 rows before score_begin and move every name at theta (non-members decay rather than
   exit at once); the NAV's dust band, exit rate and locate are only in the book branch (holdings).
3. The scorecard section (the renderer reads `diagnostics-v8.json`) is not written by this lane.

## Deviations

- One module of about 1,560 lines (pure functions about 580, loaders 400, runners 330, CLI 100), as the dispatch asked
  ("one module"); loaders could move to a second file.
- Two helper verbs beyond the brief (`lag-combined`, `book-csv`): the NAV verb has no signal-delay flag and the risk
  verb reads a CSV book; without them root cannot produce the G-3c and G-2a inputs from the default f64 holdings.
- G-2b ranks within the group over the paired names (runner style), not the card's marginal ranks (see remaining 1).

## Cross-lane edits

None in task 2 (task 1's are in `task-G-0-report.md`).

## Open risks

- The fitter's `RoleManifest` in this tree still refuses sessions after 2022; the 4-year role needs W0-1 merged first.
- G-3a's reconciliation (holdings shorts at t-1 vs the daily CSV's tier short dollars at t) is reported, not enforced;
  a large gap on real data means the rows are misaligned and the restatement should not be read.
- G-3c cells differ from B0c only in the combined signal; their first K decisions trade on a neutral (0) signal.
