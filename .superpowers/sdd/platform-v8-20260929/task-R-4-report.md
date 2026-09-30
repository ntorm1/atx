# Task R-4 report: rank hysteresis `hold-band-v1` (`--hold-band B`, registered b = .10)

Lane R45, worktree `C:/atx-wt/pool-11`, branch `feat/platform-v8-r45-20260929`. Brief: `task-R-4-brief.md`.
Not built and not run (lane rules): every claim below comes from reading the code. Root builds and runs.

Also in this lane, before R-4 (PM request, own commit `08931479`): the decide path's recipe pin reads the optional
deploy-manifest key `nav.warm_start_sessions` (absent = 0 hashes as before; K > 0 is part of the pin both ways;
malformed is refused by name). Test `StrategyLive.RecipePinBackwardCompatibleWithNewConstructionFields`. R-4 adds
`nav.hold_band` to the same mechanism and extends that test.

## What was built

Rule, as registered (brief pseudocode, verbatim semantics): on every rebalance decision each member's centred tied
rank `r_i` in [-.5, .5] is compared with `rank_set_i`; `moved = !isfinite(rank_set_i) || |r_i - rank_set_i| > b`;
moved: `desired_i = fresh_i`, `rank_set_i = r_i`, `desired_prev_i = fresh_i`; else `desired_i = desired_prev_i`;
then the existing demean, gross 1, locate zeroing and neutralization (with its rescale). Nonmembers keep their state
untouched and follow the exit rule unchanged.

Generic kernel (atx-engine):
- `atx-engine/include/atx/engine/book/target_shaping.hpp`, `atx-engine/src/book/target_shaping.cpp` (new unit;
  one line in `atx-engine/CMakeLists.txt`).
- `struct HoldBandState { std::vector<f64> rank_set, desired_prev; }` (NaN = unset; empty = sized to NaN on first use).
- `Result<HoldBandCounts> apply_hold_band(span<const f64> rank_now, span<f64> desired, span<const u8> eligible,
  f64 band, HoldBandState&)`; `HoldBandCounts {moved, kept, first_set}`. `rank_now` may alias `desired`.
  InvalidArgument with nothing modified on width mismatch, band not finite or < 0, a non-finite eligible value, or a
  set rank with a non-finite desired_prev.

Strategy wiring (atx-impl):
- `TargetReplayConfig::hold_band` (`std::optional<f64>`); `hold_band_on(c)` (kernel runs) and
  `hold_band_declared(c)` (b > 0: keys). Validation: aim-partial-v5 only, b in [0, 1], NaN refused.
- `desired_target` is split, statements verbatim and in order, into `member_ranks` + `demean_gross_one`;
  `form_desired` calls `held_desired` (ranks, kernel with rank_now = desired = the rank row, demean, gross 1) when the
  band is on, else the unchanged `desired_target`. New trailing parameter `detail::DesiredState* state = nullptr`
  (`strategy_target_replay_detail.hpp`: `struct DesiredState { engine::book::HoldBandState hold; }`).
- `replay_targets` owns a `DesiredState` across its decisions; the NAV replay's shared `Construction` owns one
  (shared by every scenario book, as the desired target is).
- `ConstructionDay::hold_moved, hold_kept, hold_first_set` (no CSV column).
- Recipe (b > 0 only): `hold_band`, `hold_band_rule`; rule id suffix `+hold-band-<b>`
  (e.g. `aim-partial-v5+neutral-price-risk-v1+hold-band-0.1`); summary `construction.hold_band`
  `{band, rule, decisions, moved_names_total, kept_names_total, first_set_names_total, mean_kept_share}`.
- CLI: `nav ... --hold-band B` and the target replay's `--hold-band B`.
- Daily decide path (`strategy_live.cpp`):
  - deploy manifest: optional `nav.hold_band` (absent: off; `0` hashes as absent; number required).
  - state in: positions CSV columns `rank_set,desired_prev` (both or neither, `nan` = unset). An old file without the
    columns, or the f64 holdings layout, loads every name unset, so the first decision sets every member.
  - `detail::nav_decide(..., no_locate, const HoldBandState* hold = nullptr)`; `NavDecision::hold` = the state after
    the decision.
  - state out (b > 0): `targets.csv` gets trailing `rank_set,desired_prev` and a row for every name with a set rank
    (a name missing for a day keeps its state in the file), so `targets.csv` is a positions file whose state columns
    carry into the next decide. `decision.json` gets `hold_band {band, state_in, names_set_in, names_set_out, moved,
    kept, first_set, state_out}`.

Flag-off path: with `hold_band` unset the kernel never runs, the state is never read, and `desired_target` performs
the same statements in the same order (the split is at a statement boundary; no expression changed). Nothing new is
written.

`--hold-band 0` (chosen: the kernel RUNS and carries its state): outputs are bit-identical. Why it is safe: every
state entry is written by the kernel from rows whose fresh value is the rank itself, so `desired_prev == rank_set`
bitwise; a kept name has `|r - rank_set| == 0`, i.e. `r == rank_set` (ranks are never -0: `x - 0.5` is +0 when zero),
so the kept value is the fresh value bit for bit; a moved name keeps the fresh value. The demean and everything
after it then see the identical row. b = 0 writes no recipe, rule-id or summary key. This makes the identity cell a
real test of the kernel and the state carry, not only of the plumbing.

## How root verifies

Build: `atx-impl-strategy-target-tests` (now also compiles `atx-engine/tests/book/book_target_shaping_test.cpp`)
and `atx-equity-strategy-targets`. Optional: `atx-engine-book-tests`.

gtest filter:
`HoldBand.*:BookTargetShaping.*:StrategyLive.*:StrategyTargetReplay.*:TargetReplayV5.*:TargetReplayV6.*:StrategyNavReplay.*:NavWarmStart.*:NavV5.*:NavV6.*`

Named tests (brief): `HoldBand.ZeroBandIsByteIdenticalToV5`, `HoldBand.NameInsideBandKeepsDesired`,
`HoldBand.StateSurvivesMissingDay`, `HoldBand.DecideVerbCarriesState`. Added: `HoldBand.ZeroBandNavRunIsByteIdentical`
(NAV CLI, accepted-cell flags with fields: every output file byte-identical with `--hold-band 0`),
`HoldBand.NavDecideChainReproducesReplay` (nav_decide chained from the first decision with the carried state equals
the replay bit for bit), `HoldBand.RecordsDeclaredBandAndRefusesBadConfigs`, `BookTargetShaping.*` (5 kernel tests).

Identity cell: the accepted v7.1 NAV argv (D-0 report; receipt
`build-equity/mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247-run/receipt.json`) with one flag appended:

```
... --locate-in-aim --liquidity-cache --hold-band 0
```

Expected: all files byte-identical to the same-exe run without the flag (5 `daily_*.csv`, 5 `events_*.csv`,
`recipe.json`, `summary.json`; with `--capacity-curve` also the capacity files). On v8 cells add it to the parent's
argv as it stands (e.g. B0c with `--warm-start-sessions 60`).

Trial (N 44): the parent's argv plus `--hold-band .1`. Read `construction.hold_band.mean_kept_share` in the primary
book's summary entry; turnover acceptance from the usual daily GMV turnover of the S2 book.

## Deviations and rulings

- Ruling R4-a: the band acts on the pre-demean tied rank (fresh value = rank) -- the registration says "then the
  existing demean, neutralise and gross-1 rescale", so the held quantity is the row before those steps -- cost if
  wrong: the kept names' post-projection weights drift slightly with the other names' moves (they are still the
  registered rule's output).
- Ruling R4-b: the state advances on every cadence decision the construction runs, including one the neutralization
  guard then skips -- the pseudocode updates the state in place before the post-processing and its guard -- cost if
  wrong: none measurable (skips are rare after warm-up; replay and decide agree either way).
- b restricted to aim-partial-v5 and [0, 1] (the rule is registered for the accepted construction; rank range is 1).

## Cross-lane edits

`atx-impl/src/strategy_nav_replay.cpp` (lane D):
- l.213 `Construction`: `+ detail::DesiredState state;`
- l.736 `form_desired_target`: passes `&shared.state` to `detail::form_desired`.
- l.2586-2587 `nav --help`: one help fragment for `--hold-band`.
- l.2638 `dispatch_nav_replay`: `else if (key == "--hold-band") cfg.target.hold_band = real();`
- l.2754-2755 `nav_decide` signature: `+ const atx::engine::book::HoldBandState* hold`;
  l.2783 seed `shared.state.hold`; l.2808 `out.hold = std::move(shared.state.hold);`
`atx-impl/src/strategy_nav_replay_detail.hpp`: `NavDecision::hold` (l.44-46) and the `nav_decide` declaration
(l.55-61, new defaulted parameter).
`atx-engine/CMakeLists.txt` l.162-163 (one source). `atx-impl/tests/CMakeLists.txt` l.113 (the engine kernel test
in `atx-impl-strategy-target-tests`).
`atx-impl/src/strategy_live.{cpp,hpp}`: decide-path changes above; the W0E seal constants and their comment lines are
untouched. `atx-impl/tests/strategy_live_test.cpp`: `write_artifact` gains `score_begin = 0`, `make_fixture` gains
`extra` NAV flags (defaults keep every existing test unchanged).

## Open risks

- `decide --check-replay` against a hold-band NAV run's emitted holdings (`--emit-holdings`) will not match: the
  emitted holdings (f64 and v1 csv layouts, lane D's files) carry no hold state, so decide starts unset. The library
  test proves parity when the state is carried. Not needed in v8 (no deployment); adding the two state fields to the
  holdings layouts would close it.
- Operators chaining decide must join `targets.csv`'s state columns onto the next positions file (the broker file has
  none); the decide verb itself carries them only through the positions file.
- The kept names hold a pre-demean value while the others move; after demean, gross 1 and the neutralization their
  weights still change slightly, so the dust band (not the hold band) absorbs those residual moves.
