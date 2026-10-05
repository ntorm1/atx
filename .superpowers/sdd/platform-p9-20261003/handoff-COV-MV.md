# Handoff COV-MV (lane COV-MV, pool-24, branch `feat/p9-cov-20261003`)

Stopped by OWNER STOP (PM relay) during the reading phase, before any code. Contract: `resume4-dispatch.md`,
`resume6-addendum.md`, `r6-wave3-common.md`, `briefs/brief-COV-MV.md`, `wave3-carry.md` (COV-MV, Q9 / Q10 / Q17,
"Added after drafting"), ledger rulings COV-4/5/7/9/11/12/13, W3-9/10/17, PRE-9/10/14/24a/29, C3-1/4/9, E2-8,
OD5-7, ALCOMB-1, W1-I1 (all read).

## State

- Head: `2efdcfbc` = merge of `0dbfa459` (C3 part A APPROVE head) on top of root `5c57c1ef` (fast-forward from
  `0145a834`). Both merges clean, no conflicts. Root has since moved to `438f0481`: merge it first.
- No code, no build, no test run. Tree clean. No WIP commit (nothing to commit except this file).
- Build trees: `build-equity/` (COV / COVR's earlier tree) kept, untouched by this lane. No tag `p9-covmv-*` used.
- No permission refusal.

## Required items of the dispatch (1-8)

| item | status |
|---|---|
| 1 rule `mv-aim-v1`, kind target | not started (design below) |
| 2 COV-13 (c) singular-F form + rank-deficient-F test | not started (form chosen below) |
| 3 PRE-9 row with `risk_model` / `risk_model_sha256` params, append-only | not started (shape below) |
| 4 COV-11 a = 2 over-correction control; per-row group check | not started |
| 5 print-only `mv_aim` summary (priced count, spec-vol ratio, eigen 00-02 share) | not started |
| 6 flag-absent identity, realised-gross cap statement | not started |
| 7 tests on synthetic containers | not started |
| 8 registration + template | not started |

## Findings from the reading (for the fresh agent)

Where things are (at `2efdcfbc`):
- Target slot: `atx-impl/src/book/nav_rules.cpp:96-98` (`constexpr std::span<const Row> targets{};`). Replace with
  a size-deduced `constexpr Row targets[] = {...}`; then `rules_json()` (`:170-171`) must wrap it as
  `std::span<const Row>(targets)` (an array in the braced list does not deduce with the two spans).
- Parser entry: `atx-impl/src/strategy_nav_v7.cpp:1100-1121` (`registry_flag`; today any non-`cost-sqrt-v2` id
  throws "has no handler in this build"; add the `mv-aim-v1` handler there). Help text `:1009-1010` says
  "none registered in this build" (stale once the row exists; a help-only byte change, list it).
  NavSpec already has `target_rule` (`book/nav_spec.cpp:75`, Text) and `risk_model` / `risk_model_sha256` keys.
- `--risk-model` today is accepted only with an spo rule or `--risk-target` / `--vol-target`
  (`strategy_nav_v7.cpp:1228-1235`) and the store is opened only then (`:1365-1372`): extend both to the target
  rule. Reuse `o.spo_risk` (one store, one `RiskStore::open` check; it refuses role, sessions, geometry, root, seal).
- Hook point: `form_desired_target` in `atx-impl/src/book/nav_decide.cpp:23-44` (C3's "stable extension point",
  called once per rebalance decision on the calling thread from `decide_construction`, `run_books`
  `book/nav_session.cpp:347-349`; the v7 extension is thread-local and reachable there). Proposed one-call edit:
  `ATX_TRY(const bool r, detail::form_desired(...)); if (r) ATX_TRY_VOID(v7::apply_target_rule(x, d,
  shared.desired)); return co::Ok(r);` with `apply_target_rule` the identity without an extension / rule. The
  C2 calibrate tape (`record_construction` / `restore_construction`) then records the post-rule target, so replay
  passes restore it unchanged. The decide path (`book/nav_deploy.cpp:118`) also goes through it: refuse the rule
  there or prove it.
- Repeated constructions of one decision (grid leverage groups, ADV-cap multiples, `book_groups` in
  `book/nav_lockstep.cpp:144-161`): record summary stats only for a decision index above the last recorded one
  (first construction of each decision), so counts are per decision.
- Store read: `RiskStore::read(d, slice)` exactly as `BookScaler::estimate` (`strategy_risk_target.cpp:141`);
  priced = `slot < 50 && isfinite(specific) && specific > 0` (`:33-36`); group column = `1 + slot`; Unavailable
  (no forecast at d) -> empty priced set -> pass-through, counted. COV-11: check per-row group (slot) values
  yourself.
- Kernel (new `atx-engine/include/atx/engine/book/mv_target.hpp`, `src/book/mv_target.cpp`, one source line in
  `atx-engine/CMakeLists.txt` beside `src/book/risk_target.cpp`): do NOT use `risk::FactorModel::create` /
  `apply_inverse`: it needs F SPD and forms F^-1 (`factor_model.hpp:176-215`), which COV-13 (c) forbids. Use
  V^-1 b = D^-1 b - D^-1 X y with (I + F G) y = F X'D^-1 b, G = X'D^-1X (sparse accumulation: market + one
  group + S styles per row), solved by a hand-written partial-pivot LU in fixed loop order (eigenvalues of I + FG
  are >= 1 for PSD F, so it is defined for singular F; no Eigen, deterministic). Then w = P V^-1 P alpha,
  alpha_i = sqrt(D_i) desired_i (the SD washes out), gross-normalised; impl rescales to the desired gross on the
  priced set (W3-17), unpriced keep weights. Do not reuse `book::factor_variance`; if reused, guard its
  `k * k` (`atx-engine/src/book/risk_target.cpp:34-35`) by a division check.
- Eigen ranks for the print: COV's convention is rank 00 = the smallest eigenvalue of F (COV report :556-562,
  root's M2i finding); check `strategy_risk_model.cpp`'s eigen family for which F (kept factors, floor) before
  defining "ranks 00-02"; share = sum_{k<3} lambda_k (u_k'e)^2 / e'Fe, e = X'w of the MV target.
- Incompatible (code reading): spo-v1 / spo-v2 / spo-v3 (required); `two-speed-v1` (its fast sleeve F lives in
  the shared construction state, `book/nav_lockstep.cpp:100-102`, `strategy_risk_target.cpp:184-202`);
  `--adv-hold-q` (the MV target would undo the parent's registered ADV cap; refuse, with a test); recommend
  requiring `--rule aim-partial-v5` (refuses aim-partial-v6 too). Y-F0's NAV flags (`scripts/specs/v8/
  lib-v8ysb-gm.json`): aim-partial-v5, cadence 1, price-risk-v1, `--locate-in-aim`, warm start 60,
  `--capacity-curve`: all must stay compatible. Under `--locate-in-aim` the MV target can put a negative weight
  on a special-tier name (desired 0 there); the books' locate block still applies at the plan: no clip (no extra
  constant), print a count and state it in the report.
- Row shape: copy `strategy_risk_target.cpp:331-357` (`property`, `store_properties`): params `risk_model`
  (`--risk-model`), `risk_model_sha256` (`--risk-model-sha256`, pattern `^[0-9a-f]{64}$`), both required;
  x-flag `--target-rule mv-aim-v1`; E2's `nav_flags` (`scripts/wave_kinds.py:384-401`) derives the flags.
- Test fixtures: `strategy_spo_fixture.hpp` (`Role`, `write_risk_model`, `nav_config`), `strategy_spo_cli_fixture.hpp`
  (`write_run_inputs` for CLI runs), `strategy_cov_fixture.hpp` `add_container` (container store from a legacy
  dir, used by `CovStore.VolAndRiskTargetRecordsUnchanged`), C3's CLI helpers in `strategy_nav_spec_test.cpp`.
- COV-11 band test: `CovBias.EigenFamilyInBandOnAPlantedModel` (impl COV tests); add `eigen_amplification = 2.0`
  beside the a = 0.5 control, must miss the band.
- Design note for the report (one line, not a stop): P V^-1 P alpha is the registered form; it is not the
  constrained optimum V^-1 (alpha - c 1) of a dollar-neutral max-Sharpe problem.

## Next step for a fresh agent

1. `git merge feat/platform-p9-20261003` (root now `438f0481`); check whether AL-CLOCK has merged (append-only
   conflict in `nav_rules.cpp` / `strategy_nav_v7.cpp`, keep both, W3-9).
2. Implement per the brief's tasks 1-3 (kernel + gtests, row / parser / hook / summary + impl gtests,
   registration + template), using the findings above; then build `p9-covmv-a..` (equity-dev, then equity-rel),
   run the named filters, write `task-COV-MV-report.md`.
