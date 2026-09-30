# Task R-5 report: ADV holding cap `adv-hold-v1` (`--adv-hold-q Q`, registered Q = .10)

Lane R45, worktree `C:/atx-wt/pool-11`, branch `feat/platform-v8-r45-20260929`, on top of R-4 (`63e65514`).
Brief: `task-R-5-brief.md`. Stopped at the owner's break point (PM instruction): the code is complete and compiles by
inspection; two optional tests are not written (see "Remaining"). Not built and not run (lane rules).

## What was built

Rule, as registered: after the projection, `|desired_i| <= Q x ADV_i / (L x NAV)`; the clipped mass is redistributed
pro rata inside the same side, one pass; the residual breach is reported.

Generic kernel (atx-engine, `book/target_shaping.{hpp,cpp}`, the unit R-4 created):
- `Result<CapStats> cap_pro_rata_one_pass(span<f64> weights, span<const f64> caps)`; `CapStats {longs, shorts}` of
  `SideCapStats {clipped, clipped_mass, unplaced_mass, residual_names, residual_mass, residual_max}`.
- Per side: names with `|w| > cap` are set to `sign(w) cap` (+0 for a zero cap); the clipped mass E goes to the side's
  unclipped names, `w *= 1 + E / U` (U their summed |w|), so side gross is preserved; with U = 0 the mass is
  `unplaced`. One pass: names lifted above their own cap stay and are counted as residual breach. A side with no
  clipped name is not touched (bit for bit). Caps: >= 0 or +inf; NaN/negative caps on nonzero weights refused.

Strategy wiring (atx-impl):
- `TargetReplayConfig::adv_hold_q` (0 = off; finite >= 0; on requires aim-partial-v5); `adv_hold_on(c)`.
- `form_desired`: on a rebalance that proceeds, after the post-processing (neutralization and its rescale, or the
  tied-rank gross-1 target without neutralization), `adv_capped` computes `cap_i = Q * ADV_i / (L * NAV)` for every
  name with a nonzero desired weight and runs the kernel; the two `return` statements of `form_desired` now go
  through `finish_desired`, which returns `proceed` unchanged when the cap is off (no arithmetic touched).
- ADV (no second definition): the NAV replay's `window_liquidity(x, volume, cfg, d + 1, i).adv`, i.e. exactly the ADV
  the execution trade limit reads for this decision's fills (session d + 1, rows [d + 1 - w, d + 1), rows <= d only),
  the same value `detail::execution_adv(in, cfg, d + 1)` returns. NAV = the run's `initial_nav`; L = `aim_leverage`.
- `detail::DesiredState` gains `adv_dollars`, `nav`, `caps` (filled by the NAV replay's `form_desired_target`).
- The target replay refuses the option (`replay_targets`: "adv_hold_q is a NAV replay option").
- `ConstructionDay::adv_clipped, adv_clipped_mass, adv_unplaced_mass, adv_residual_names, adv_residual_mass,
  adv_residual_max` (both sides summed, desired-weight units; no CSV column).
- Recipe (Q > 0): `adv_hold_q`, `adv_hold_rule`; rule id suffix `+adv-hold-<Q>` (e.g. `...+adv-hold-0.1`).
- summary.json, new key per scenario entry: `scenarios[k].construction.adv_hold` =
  `{q, rule, units, decisions, clipped_names_total, clipped_names_mean, clipped_names_max, clipped_mass_mean,
  clipped_mass_max, unplaced_mass_total, unplaced_mass_max, residual_breach {decisions, names_total, names_max,
  mass_mean, mass_max, excess_max}}`. Masses are desired-weight units (x L x NAV = dollars).
- CLI `nav ... --adv-hold-q Q` (passes through the v7 parser unchanged, so it composes with `--capacity-curve`).
- Decide: optional deploy key `nav.adv_hold_q` (0 hashes as absent, Q > 0 part of the pin); `decision.json`
  `adv_hold {q, nav, clipped_names, clipped_mass, unplaced_mass, residual_names, residual_mass, residual_max, rule}`.

Composition with R-4: hold band on the ranks, then demean, gross 1, locate zeroing, neutralization, then the ADV cap.

Flag-off path: Q = 0 never reads the state, computes no ADV, and `finish_desired` returns the same boolean; no
floating-point operation is added or reordered. Large Q: the kernel runs but a side with no clipped name is not
touched, so the desired target is bit-identical; only the recipe (`adv_hold_q`, `adv_hold_rule`, rule id) and the
summary (`construction.adv_hold` with zero clips and zero breach) differ.

## How root verifies

Build: `atx-impl-strategy-target-tests` (includes the engine kernel tests) and `atx-equity-strategy-targets`.

gtest filter: `AdvHold.*:HoldBand.*:BookTargetShaping.*:StrategyLive.*:StrategyTargetReplay.*:TargetReplayV5.*:TargetReplayV6.*:StrategyNavReplay.*:NavWarmStart.*:NavV5.*:NavV6.*`

Tests written: `AdvHold.NoNameAboveCapAfterOnePassOrReported` (residual names, mass and max equal what is left above
caps recomputed from `execution_adv`), `AdvHold.SideGrossPreserved` (clipped names exactly at cap, one common factor
per side, side gross preserved), `AdvHold.LargeQIsByteIdentical` (NAV CLI: every daily and events CSV byte-identical
at Q = 1e9; recipe equal after removing the adv keys; summary zero clips and breach),
`AdvHold.DeployPinAndDecisionRecord`, `BookTargetShaping.Cap*` (5 kernel tests).

Identity cell (large Q) on the accepted v7.1 NAV argv (see the R-4 report for the full argv), append:

```
... --locate-in-aim --liquidity-cache --adv-hold-q 1e9
```

Expected: the 5 `daily_*.csv` and 5 `events_*.csv` byte-identical to the run without the flag; `recipe.json` and
`summary.json` differ only by `adv_hold_q`, `adv_hold_rule`, the rule id suffix `+adv-hold-1e+09`, and
`construction.adv_hold` with `clipped_names_total` 0 and `residual_breach.names_total` 0 in every scenario. (If a
member ever has zero trailing volume its cap is 0 and even a large Q clips it; `clipped_names_total` shows it.)

Trial (N 45): the parent's argv plus `--adv-hold-q .1 --capacity-curve`. With R-4 accepted the parent carries
`--hold-band .1`; both flags compose.

## Deviations and rulings

- Ruling R5-a: ADV_i is the execution ADV of the fill session d + 1 (`window_liquidity` at t = d + 1), not the
  decision-row window [d - w, d) the per-name rate uses -- the brief says "the same measure the existing trade limit
  uses", and the trade limit applies exactly this value to the orders this decision places; it reads rows <= d only
  -- cost if wrong: the window differs by one session (row d in, row d - w out).
- Ruling R5-b: NAV is the run's `initial_nav` for every book, including the capacity pass's books (root's instruction
  "NAV and L are the run's values"; the capacity books run at the initial NAV and share one desired target) -- so on
  the capacity curve at multiple m the cap is Q at 1x, i.e. holdings reach m x Q of ADV at NAV x m -- cost if wrong:
  the 4x acceptance measures a cap set for $1bn, not for $4bn; a per-multiple cap needs a per-book desired target.
- The cap applies only on a rebalance that proceeds (a guard-skipped decision keeps current weights anyway).

## Cross-lane edits

`atx-impl/src/strategy_nav_replay.cpp` (lane D), on top of the R-4 edits:
- l.733-747 `form_desired_target`: takes `const NavReplayConfig& cfg` (was `TargetReplayConfig`); fills
  `shared.state.adv_dollars` (members, `window_liquidity(..., d + 1, i).adv`) and `shared.state.nav` when Q > 0.
- l.810 and l.821 `decide_construction`: parameter `const NavReplayConfig& cfg` (was `TargetReplayConfig`) and the
  forwarded call.
- l.1133 `run_books`: passes `base` instead of `target`; l.2799 `nav_decide`: passes `cfg` instead of `cfg.target`.
- l.2597-2598 help fragment; l.2650 `else if (key == "--adv-hold-q") cfg.target.adv_hold_q = real();`
`atx-impl/src/strategy_live.{cpp,hpp}`: optional `nav.adv_hold_q` and the `decision.json` record (seal lines
untouched).

## Remaining (not done at the break point)

- Test not written: refusals of the option outside the NAV path (`replay_targets` with Q > 0; negative or NaN Q;
  Q > 0 under baseline-v1) -- the code paths exist (`validate_config`, `replay_targets`).
- Test not written: a `--capacity-curve` smoke run with `--adv-hold-q` (the v7 parser forwards unknown tokens, so
  the flag reaches the replay in both passes; not exercised by a test).
- Nothing built or run; root's build is the first compile.

## Open risks

- Ruling R5-b above (capacity-curve NAV) is the main interpretive risk for the 4x acceptance.
- The redistribution raises unclipped names by the side's factor; on a thin side the residual breach can be large.
  It is reported, never re-clipped (registered one pass).
- The cap acts after the neutralization, so the beta/vol/log-ADV exposures of the capped target are no longer zero
  (registered order); the summary does not measure that drift.
