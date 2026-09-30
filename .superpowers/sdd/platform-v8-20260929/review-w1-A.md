# Review W1, area A: book construction and execution

Snapshot `C:/atx-wt/pool-9` at 7af37e9d, change `ef11f462..7af37e9d`. Read-only; nothing built or run.
STOPPED at the owner's instruction before the tests were read. Everything below comes from reading source.
Rulings E-26 and E-31 are not in this snapshot's `progress.md` (it ends at E-24), so they were not applied.

Severity: **I** important, **M** medium, **m** minor. No I finding was confirmed in the code that was read.

---

## Findings

### A-1 (M) ADV cap in a construction grid uses the first variant's aim leverage
`atx-impl/src/strategy_nav_replay.cpp:2094-2108` (`same_shared`), `:1184` and `:1241` (`run_books` passes
`ctxs.front().cfg`), `:757-769` (`form_desired_target`); `atx-impl/src/strategy_target_replay.cpp:420`
(`dollars = cfg.aim_leverage * state->nav`).

- What is wrong: the cap is `Q x ADV / (aim_leverage x NAV)`. `--aim-leverage` is a grid variant flag, and
  `same_shared` requires equal `adv_hold_q` but not equal `aim_leverage`. The one shared construction is formed
  with the first variant's config, so every variant is capped with the first variant's L.
- Failing scenario: `--adv-hold-q .1 --construction-grid g.json` with variants `a` (`--aim-leverage 1.0`) and
  `b` (`--aim-leverage 1.5`). Directory `b/` publishes `aim_leverage 1.5` and the rule text
  `Q ADV / (aim_leverage NAV)`, but its desired target was capped at `Q ADV / (1.0 NAV)`, 1.5 times looser.
  `b/` is not the standalone run, which `replay_nav_grid` promises bit for bit.
- Smallest fix: in `same_shared`, require `s.aim_leverage == t.aim_leverage` when `s.adv_hold_q > 0`
  (refusal text: "with --adv-hold-q every variant has the base aim leverage").
- Test coverage: unverified (tests not read). `HoldBand.GridSharesTheBandAndOneCadence` is described as
  covering the band only.

### A-2 (M, partly unverified) spo-v3 (and spo-v1/v2) calibrate gamma on the first warm-up decision
`atx-impl/src/strategy_spo.cpp:1454` (`if (!calibration.done) calibrate_tracking(in, aim)` in `plan_tracking`),
`:1349` (`calibrate_tracking`); v1/v2: `calibrate` at `:1126`.

- What is wrong: the calibration runs at the first `plan` call. Under `--warm-start-sessions K` that is role
  row `score_begin - K`, not the first scored decision. The registration says "sigma_aim the aim's ex-ante
  volatility at the first decision"; the recipe text says "at the first rebalance decision" and the
  warm-start rule text does not mention it. `calibration.session` then lies before the scored window.
- Failing scenario: the R-6 cell on parent B0c carries `--warm-start-sessions 60`. `gamma = 20 / sigma_aim`
  is taken 60 sessions before `score_begin`. A reader who recomputes sigma_aim at `score_begin` gets another
  gamma.
- Unverified, and more serious if true: `prepare` reads the risk slice at every warm-up decision
  (`risk->read(d, ...)`, `:1059`). If the risk store holds no usable rows before `score_begin`, the optimized
  name set is empty, sigma_aim is 0 and the run stops with Unavailable at the first warm-up decision, so
  spo-v3 cannot run on a warm-start parent at all. Confirm by checking which rows the risk verb writes
  (`strategy_risk_verb.cpp`, not in my area) or by one fixture run with a store limited to the score window.
- Smallest fix: declare it. Add "under a warm start: the first warm-up decision" to `tracking_declaration`
  and to the warm-start rule text, or calibrate at the first decision with `d >= x.decision_begin` and run
  the warm-up with aim-partial-v5 moves. Either choice must be ruled before the cell runs.

### A-3 (M, partly unverified) a warm start that does nothing is neither refused nor visible
`atx-impl/src/strategy_nav_replay.cpp:1185-1187` (`start`, `first_return`), `:1107-1128` (`open_book`),
`:1076-1090` (`start_scoring`), `:2426-2433` (`warm_start_record`).

- What is wrong: the replay never checks that the warm-up built a book. If rows
  `[score_begin - K, score_begin)` carry no usable blend (no members, or every member's blend is the tied
  zero), the desired target is flat, the book stays flat, and the run still records `warm_start.sessions K`.
  The row convention changes anyway: `first_return = begin + 1`, so row `begin + 1` becomes a return
  observation on an empty book (one extra zero-return row against the flat start).
- Verified: no check and no reported quantity exists; the summary's `warm_start` record holds only the two
  session stamps.
- Unverified: whether real blends carry finite candidate signals before `score_begin`. The composition ranks
  every date (`strategy_ic_composition.cpp:232-246`) and the blend loader requires membership on every row,
  but the IC runner's signal coverage before `score_begin` is area B. To confirm on TRAIN: gross leverage of
  daily row `score_begin` in B0c should be near .95 L; a value near 0 means the warm start was inert.
- Smallest fix: write `gross_leverage_before_resize` (pre-mark gross / NAV at the boundary) into the
  `warm_start` record, and refuse K > 0 when every book is flat at the boundary.

### A-4 (M) the E-14 criterion is computed on the plan, not on the traded book
`atx-impl/src/strategy_spo.cpp:1420` (`row.aim_correlation = correlation(sol.w, p.target)`),
`atx-impl/src/strategy_spo_v3.cpp:85-91` (`report`).

- What is wrong: Ruling E-14 names "mean correlation of the traded book with the aim". The row holds the
  correlation of the solver's planned weights with the aim, over the optimized names only. Fills that are
  capped, blocked or drifted, nonmember exits and unpriced members are all outside it. The plan's correlation
  is an upper reading of the held book's.
- Failing scenario: a period where many orders are liquidity-blocked or capped at EXECUTE. The plan tracks at
  .95 while the held book sits at .85; the cell passes the ">= .9" criterion.
- Smallest fix: add `aim_correlation_current = correlation(current over the optimized names, p.target)` (the
  book DECIDE read, the twin of `tracking_error_current`) to the row and the report, and read the criterion
  from it; or amend the ruling text to say "planned book".

### A-5 (m) session ring and hold-band state change admission with the flags off
`atx-impl/src/strategy_nav_replay.cpp:354-357, :405, :2072`; `:68` (`holdings_name_bytes` uses
`sizeof(NavHolding)`); `atx-impl/src/strategy_target_replay.cpp:160-163`.

- Every neutralizing run is now charged the ring (about 23 MB on the v7.1 geometry). Every `--emit-holdings`
  run is charged 16 more bytes per name. `--hold-band 0` is charged 2 f64 per name. No published byte
  changes, but an accepted argv whose `--max-bytes` sat close to the old reserve is now refused.
- `nav_decide` (`:3281`) is charged the ring through `validate_nav_input` although the decide path never
  binds one.
- Fix: none needed if the rerun of each accepted argv is admitted; otherwise charge the ring only where
  `enable_session_ring` is called.

### A-6 (m) warm-up events count against `--max-events`
`atx-impl/src/strategy_nav_replay.cpp:535` (`push_event` cap), `:1083` (`r.events.clear()` at the boundary).

- A warm-up that produces more events than the cap aborts the run with "event cap exceeded" although no
  warm-up event is published. Reachable only with a very long warm-up on a gap-heavy role.
- Fix: do not store events while `t < begin` (the buckets are reset anyway).

### A-7 (m) spo-v3 objective is reported with the unclipped covariance
`atx-engine/src/book/target_tracking.cpp:358-362` (`tracking_terms`, F as given) against `:199-206`
(solver root with negative eigenvalues set to 0).

- When `clipped_eigenvalues > 0`, the published `objective` and `tracking_error` are not the quantity the
  solver minimised. The difference is the clipped part only. Fix: state it in `tracking_units_json`, or
  report both.

### A-8 (m, unverified) names with a very small specific variance converge slowly
`atx-engine/src/book/target_tracking.cpp:388-390` (`rho_i = 10 gamma d_i`), `:92-98` (`shrink`, dead zone
`s / rho`).

- The metric is per name. A priced name with `d_i` orders of magnitude below the rest has a dead zone
  `s / rho_i` far above any weight, so its z-coordinate moves very slowly and the solve may run to the 2,000
  cap (reported as unconverged, never refused). `has_row` admits any positive specific variance. Confirm by
  reading `unconverged` and the minimum specific variance on the first spo-v3 run.

### A-9 (m) the gross bound is a void, not the registered slack cap
`atx-impl/src/strategy_spo.cpp` `plan_tracking` (`budget = 2 x L`, checked only),
`atx-impl/src/strategy_spo_v3.cpp:277-291`.

- The registration sets the gross cap to 2 x L "so it is slack". The code imposes no cap and voids the run
  on a breach. Identical while the bound is slack; on a breach the cell is void instead of capped. Declared
  in the rule text; listed so the PM can confirm the ruling covers it.

### A-10 (m) the exposures verb leaves an unusable directory on a mid-run failure
`atx-impl/src/strategy_exposures_verb.cpp:165-188`.

- The directory is created before the decision loop. An error in a later decision leaves `basis.f64` and
  `forward_returns.f64` without `manifest.json`, and a rerun is refused ("output must not exist"). Fix:
  remove the directory on failure, or write to a temporary name and rename.

---

## Identity with every new flag absent (from reading; not run)

| flag or change | flag-off result | where it is not identical |
|---|---|---|
| `--warm-start-sessions` absent or 0 | same statements: `start = begin`, `first_return = begin + 2`, boundary never taken; no recipe or summary key | none found |
| `--book-workers` absent (1) | sequential path; per book the order is now plan(k), close(k) instead of all plans then all closes; no book reads another book's state | none found |
| `--stage-timers` absent | no key; the steady clock is still read in the ring path (observation only) | none found |
| session ring (always on when neutralizing; not a flag) | one kernel (`interval_returns`) and one estimator block (`write_exposures`) for both paths; same window, same order | admission only (A-5) |
| `--construction-grid` absent | one variant; `same_shared` loop not entered | none found |
| `--hold-band` absent | `desired_target` = `member_ranks` + `demean_gross_one`, split at a statement boundary | none found |
| `--hold-band 0` | kernel runs; kept value equals the fresh rank bit for bit; no key | admission only (A-5) |
| `--adv-hold-q` absent or 0 | `finish_desired` returns `proceed`; no ADV computed | none found |
| E-16 holdings state | 11-value rows, v1 header, no manifest key without a declared band | admission only (A-5) |
| spo-v1 / spo-v2 after the extraction of `fixed_positions`, `market_terms`, `accumulate_plan` | per-name expressions unchanged; the three running sums keep their order | not proven by a pin: the lane report says the spo-v2 digest pin was a 0 placeholder that makes the test SKIP; unverified whether integration filled it |
| decide path optional keys absent | recipe recomputed without them | the seal policy and date now come from `research_window.hpp`: a deploy manifest written with `research-seal-v1` / `2025-01-01` is refused (intended by W0-1) |

## Checked, no finding

- Point in time: the ADV of the cap is `window_liquidity(..., d + 1, i)`, rows `[d + 1 - w, d + 1)`, so rows
  <= d only. Row d's close and volume are known at the decision (after the mark), as for the price exposures.
- Warm-start accounting: the boundary resize scales holdings, cash, orders, anchors and written exposures by
  one factor; `nav_post` and `pending_cost` are set by the boundary's own EXECUTE; `summarize_nav` guards
  `deployment_index < first row`.
- Hold band kernel and wiring: band on the centred tied ranks, state untouched for nonmembers, state passes
  through non-cadence decisions, decide reads and writes the same state.
- Cap kernel: clip, one pro rata pass inside the side, side gross kept when an unclipped name exists,
  residual counted after the pass.
- Solver algebra: constant term of the x-update, Woodbury solve, capacitance, the nine-case limit choice,
  over-relaxation, the 3/2-power prox with the borrow kink, the dual residual and the restoration are
  consistent with the header. Units: daily covariance with gamma = S_prior / annual sigma_aim gives an
  implied annual Sharpe of S_prior; costs are divided by H, borrow is per session and not divided.
- Book pool: lanes write only their own book and row; per-name-v1 and a v7 extension are refused above one
  worker (`strategy_nav_replay.cpp:2138-2141`).
- QR of the exposures verb follows dgeqr2 and dorg2r step for step.

---

## Table of findings

| ID | sev | file:line | one line |
|---|---|---|---|
| A-1 | M | `strategy_nav_replay.cpp:2094`, `strategy_target_replay.cpp:420` | ADV-cap grid caps every variant with the first variant's aim leverage |
| A-2 | M | `strategy_spo.cpp:1454` | gamma calibrated on the first warm-up decision; risk rows before `score_begin` unverified |
| A-3 | M | `strategy_nav_replay.cpp:1185`, `:2426` | inert warm start not refused or reported; adds one zero-return row |
| A-4 | M | `strategy_spo.cpp:1420` | E-14 criterion measured on the plan, not the traded book |
| A-5 | m | `strategy_nav_replay.cpp:405`, `:2072`, `:68` | flag-off admission budget grew (ring, holdings row, band 0) |
| A-6 | m | `strategy_nav_replay.cpp:535` | warm-up events count against the event cap |
| A-7 | m | `target_tracking.cpp:358` | objective reported with unclipped F |
| A-8 | m | `target_tracking.cpp:388` | tiny specific variance slows convergence (unverified) |
| A-9 | m | `strategy_spo_v3.cpp:277` | gross bound voids instead of capping |
| A-10 | m | `strategy_exposures_verb.cpp:165` | partial output directory blocks a rerun |

Counts: I 0, M 4, m 6.

## Files fully reviewed

- `atx-engine/include/atx/engine/book/target_shaping.hpp`, `atx-engine/src/book/target_shaping.cpp`
- `atx-engine/include/atx/engine/book/target_tracking.hpp`, `atx-engine/src/book/target_tracking.cpp`
- `atx-engine/include/atx/engine/parallel/lockstep_grid.hpp`
- `atx-impl/src/strategy_nav_replay.cpp`, `strategy_nav_replay.hpp`, `strategy_nav_replay_detail.hpp`
- `atx-impl/src/strategy_target_replay.hpp`, `strategy_target_replay_detail.hpp`;
  `strategy_target_replay.cpp` lines 60-1382
- `atx-impl/src/strategy_price_exposures.cpp`, `strategy_price_exposures.hpp`
- `atx-impl/src/strategy_exposures_verb.cpp`
- `atx-impl/src/strategy_spo_v3.cpp`, `strategy_spo_v3.hpp`
- `atx-impl/tools/equity_strategy_targets.cpp` and the three CMake diffs
- The six lane reports and plan sections 3, 4, 7 (W0-4), 8 (D), 9 (R-4, R-5, R-6)

## Files read through the diff only (hunks, not whole functions)

- `atx-impl/src/strategy_live.cpp`, `strategy_live.hpp`
- `atx-impl/src/strategy_holdings.cpp`, `strategy_holdings.hpp`
- `atx-impl/src/strategy_spo.cpp` (diff plus lines 236-435, 880-1137, 1500-1757), `strategy_spo.hpp`
- `atx-impl/src/strategy_nav_v7.cpp` (diff plus lines 300-480, 585-825), `strategy_nav_v7.hpp`

## Not reached

- Every test file: `atx-impl/tests/strategy_nav_replay_test.cpp`, `strategy_live_test.cpp`,
  `strategy_target_replay_test.cpp`, `strategy_price_exposures_test.cpp`, `strategy_exposures_verb_test.cpp`,
  `strategy_spo_test.cpp`, `strategy_spo_pin_test.cpp`, `strategy_spo_v3_test.cpp`,
  `strategy_spo_v3_pin_test.cpp`, `strategy_spo_digest.hpp`; `atx-engine/tests/book/book_target_shaping_test.cpp`,
  `book_target_tracking_test.cpp`; `atx-engine/tests/parallel/parallel_lockstep_grid_test.cpp`. Hunt item 6
  (tests that cannot fail) is therefore not covered at all.
- `atx-impl/src/strategy_exposures_verb.hpp`.
- The risk store (`RiskStore::read`, rows before `score_begin`) and the IC runner's signal coverage before
  `score_begin`: both decide A-2 and A-3 and belong to other areas.
- Rulings E-26 and E-31 (absent from this snapshot).
