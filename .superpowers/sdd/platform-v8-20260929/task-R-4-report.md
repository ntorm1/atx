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

- (Closed by ruling E-16, `1e4a50c8`, below.) `decide --check-replay` against a hold-band NAV run's emitted holdings
  did not match because the export carried no hold state; it now does.
- Operators chaining decide must join `targets.csv`'s state columns onto the next positions file (the broker file has
  none); the decide verb itself carries them only through the positions file (or the replay's own export).
- The kept names hold a pre-demean value while the others move; after demean, gross 1 and the neutralization their
  weights still change slightly, so the dust band (not the hold band) absorbs those residual moves.

## PM session 2 (2026-09-30): ruling E-16, decide parity from the holdings export (`1e4a50c8`)

Also in this session (see the R-5 report): root merged in (`a51ab1f4`) and the grid fix `dca2165a` (a hold-band
construction grid shares the band and one cadence; `HoldBand.GridSharesTheBandAndOneCadence`).

What was built. With a hold band declared (b > 0, `hold_band_declared`) and `--emit-holdings`, the replay snapshots
the band's state at the start of every session, before that session's construction advances it
(`Construction::hold_in`, copied into reused storage; `Construction::emit_hold` gates it). That is the state the
session's DECIDE reads, which is exactly what `decide --asof t` needs (the export's `held_dollars` is likewise what
DECIDE at t read). `emit_holdings` then:
- reports a name holding a set rank even when it has no other state (so a name missing or flat for a while keeps
  its state in the export, as the replay does), and
- fills `NavHolding::rank_set` / `desired_prev` (new fields, NaN = unset; NaN by default, untouched without a band).
Layouts (lane D / v7 W4 files):
- f64: `holdings.f64` rows are `hold_row_width` = 13 values (the 11 v1 values, then `rank_set`, `desired_prev`);
  `holdings_index.json` names the columns (`data.columns` gains the two names, `data.row_width` 13) and adds a
  `hold_state` declaration; the reader (`holdings::read_session`) accepts either layout by its column names and
  returns `SessionRead::hold_state`; a row with one state value NaN and the other not is refused. New:
  `holdings::pack_hold`, `HoldRow`, `hold_column_names`, `col_rank_set`, `col_desired_prev`;
  `BinaryAppender::open(path, width = row_width)` and `append(span)`; `write_index(..., hold_state = false)`.
- csv: `holdings.csv` gains the two trailing columns (nan spelled `nan`).
- both manifests gain `hold_band_state` (the declaration).
Decide: `read_f64_positions` takes the state from a hold-band export (`Positions::has_hold_state`); the csv reader
already read the columns (R-4). `decision.json` `hold_band.state_in` says which layout supplied it. The workspace
reserve charges the snapshot (2 f64 per name) with holdings and a declared band.

Identity (band unset or b = 0). `emit_hold` and the emitter's `hold_` are false: no snapshot, no added row, the csv
header and rows are written by the same statements (the two columns are appended only under `hold_`), the f64 rows
are `pack()`'s 11 values, the index JSON has the same keys and values (`row_width` 11, the 11 names, no
`hold_state`), the manifests have no new key. How root verifies: the accepted v7.1 NAV argv (see "How root
verifies" above) with `--emit-holdings H --holdings-format f64` and again with `--holdings-format csv`, run with the
pre-E-16 exe (root HEAD before this merge) and with the merged exe: every file of the NAV directory and of `H`
(`holdings.f64`, `holdings_index.json`, `holdings_days.csv`, `manifest.json`; `holdings.csv`) byte-identical; also
with `--hold-band 0` appended on the merged exe (same bytes). In-build test:
`HoldBand.EmittedHoldingsUnchangedWithoutADeclaredBand` (flag off vs `--hold-band 0`, both formats, byte-identical
directories; 11-value rows with the v1 column names, the v1 csv header, no `hold_band_state`).
One non-output effect: `sizeof(NavHolding)` grows by 16 bytes, so the conservative `--emit-holdings` workspace reserve
(`holdings_name_bytes`) grows by 16 bytes per name for every holdings run; no published byte depends on it (a
budget within 16 x names bytes of the old reserve would now be refused).

Tests (`strategy_live_test.cpp`):
- `HoldBand.CheckReplayMatchesEmittedHoldings`: a NAV run with `--hold-band .1 --emit-holdings` (f64) and the same run
  with `--holdings-format csv`; the NAV directory is byte-identical to the run without `--emit-holdings`; the f64
  index has row width 13 with the two named columns, the csv header ends `,rank_set,desired_prev`, both manifests
  carry `hold_band_state`; `decide --check-replay` from each export at sessions 150, 151, 170 and 190: 0
  mismatches over every name, state read from the export (`names_set_in` > 0); at a session where the band kept a
  member, the same positions and targets without the state columns give mismatches > 0.
- `HoldBand.EmittedHoldingsUnchangedWithoutADeclaredBand` (above).
- Changed: `HoldBand.DecideVerbCarriesState` now starts from a positions CSV `instrument_id,held_dollars` built from
  the replay's holdings of 150 (the f64 export of a hold-band run now carries the state, so it no longer starts
  unset); every assertion is unchanged.

How root verifies: build `atx-impl-strategy-target-tests`; gtest filter
`HoldBand.*:AdvHold.*:StrategyLive.*:StrategyNavReplay.*:NavWarmStart.*:NavV5.*:NavV6.*:NavV7Hook.*:ConstructionGrid.*:NavBookWorkers.*:NavTimers.*:BookTargetShaping.*`
(`StrategyLive.*` holds the holdings writer/reader identity tests, e.g. `HoldingsF64CarriesEveryV1CsvColumnBitForBit`,
`EmitHoldingsLeavesNavOutputsByteIdentical`, `DecideReadsF64AndV1CsvIdenticallyAndRefusesTamperedHoldings`).

Deviation (declared): the export carries the state entering the session's construction, not the state after it.
Reason: decide at t must read the state its own decision starts from, and it reads one session of the export; the
state after decision t is `targets.csv`'s (R-4) and the export's next session. On the final execution-only session
the row carries the state after the last decision.

Cross-lane edits (E-16): `atx-impl/src/strategy_nav_replay.cpp` (lane D: `hold_trace_name_bytes`, `Construction`
members, `emit_holdings`, the snapshot in `run_books`, `nav_workspace_reserve_bytes`, `HoldingsEmitter`
open/session/write_name/close/`hold_state()`, `hold_band_state_declaration`, `publish_holdings`, the
`run_nav_replay` call of `open`); `atx-impl/src/strategy_nav_replay.hpp` (`NavHolding` fields and comment, `<limits>`,
the reserve comment); `atx-impl/src/strategy_holdings.{hpp,cpp}` (v7 W4 layout, as above);
`atx-impl/src/strategy_live.{cpp,hpp}` (f64 state read, `state_in`, the positions doc; seal lines untouched).
