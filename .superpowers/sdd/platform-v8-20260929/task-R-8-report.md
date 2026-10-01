# Task R-8 report: ex-ante risk target risk-target-v1 (lane RISK, pool 9)

Branch `feat/platform-v8-risk-20260930` from `28051c4c`. Never compiled (lane rule 2); Python tests run.
Commits: `9d607b0e` engine scaler + tests, `f5a8eefe` NAV flag, spo-v3 seam, adapter, impl tests, `855b1e9b` cell
template `r8.json` + spec test, plus this report.

## 1. Registration (fixed blind, before any read; E-30 precedent, Ruling E-40)

Rule verbatim (plan R-8): `L_t = clip(sigma_star / (b x sigma_hat_t), .8 L, 1.25 L)`.

| item | value as coded | where |
|---|---|---|
| sigma_star S | `.05` annualised (`--risk-target .05`) | `scripts/specs/v8/r8.json` |
| bias b | `1.15` (`risk_target_default_bias`; `--risk-target-bias`, written explicitly in r8) | `atx/engine/book/risk_target.hpp` |
| cadence C | `21` sessions (`risk_target_default_cadence`; `--risk-target-cadence`, explicit in r8) | same |
| clip | `[.8, 1.25] x L` (`risk_target_clip_lo/hi`, no flag) | same |
| annualisation | `252` (`risk_target_periods_per_year`) | same |
| L | the book's `--aim-leverage` (1.247 in the v8 specs) | |
| sigma_hat_t | `sqrt(252 x book_variance(r_d, c / sum_i abs(c_i)))`: c = the book's current weights at its DECIDE d (after the session's fills; spo's `exante_vol_current` book), scaled to gross 1 over the WHOLE book; variance `(B'w)'F(B'w) + sum d_i w_i^2` over the names with a risk row (slot < 50, finite specific > 0), as `book_variance` prices them; no specific ceiling | engine `gross_one_vol`, impl `Scaler::estimate` |
| risk model | row d of the pinned atx-risk-v1 store (`--risk-model` / `--risk-model-sha256`, the spo rules' flags, open and role check; the v1.1 store of the 4-year role, filled by root) | |
| estimate timing | first estimable decision (forecast at d, book not flat, variance > 0), then the first estimable decision at least 21 sessions after the previous estimate; held between; L before the first; a due decision that cannot be estimated keeps the clock | `risk_target_update` |
| scope | every decision the replay plans (warm-up included), every book on its own state (S1/S2/S3/stress/capacity books), each pass from a clean state; records = the main pass's scored decisions | `strategy_nav_v7.cpp` `State::plan` |
| aim-partial-v5 | moves toward `L_t x desired` (L_t replaces `--aim-leverage` in the book's config for `update_weights`) | |
| spo-v3 | tracks `L_t x desired`; its gamma (one calibration, S_prior / sigma_aim) is calibrated on `L x desired` (the parent's gamma) and its gross sanity bound stays `2 x L` | `BookDecision::base_leverage`, `plan_tracking` |
| unchanged | the shared desired target and its ADV cap `Q ADV / (L NAV)` (read at L, not L_t) | |
| refused | aim-partial-v6, spo-v1/v2, every non-aim-partial-v5 target rule, missing store | parser |
| rule id | `<parent rule>+risk-target-0.05` (`-bias-b` / `-cadence-C` appended only off 1.15 / 21) | `rule_suffix` |

## 2. What was built

Engine (reusable, `atx-engine/include/atx/engine/book/risk_target.hpp`, `src/book/risk_target.cpp`):
`RiskTargetParams{sigma_star, bias, cadence}`, `validate_risk_target` (S in (0, 1], b in (0, 10], C in [1, 1e4]);
`FactorRiskView{groups, styles, group, exposures, covariance, specific}` (target-tracking layout: intercept, one-hot
groups, styles); `factor_variance(m, w)`; `gross_one_vol(m, w, gross, P = 252)` (Unavailable: flat book or variance <= 0);
`risk_target_leverage(p, sigma_hat, L) -> {raw, leverage, clip}`; `RiskTargetState`, `risk_target_due`,
`risk_target_in_force`, `risk_target_update(p, L, session, m, w, gross, state) -> Result<bool>`.

Impl adapter (`atx-impl/src/strategy_risk_target.hpp/.cpp`, namespace `risk_target`): `Options{on, params}`;
`Scaler(options, store)` with `begin_run()`, `leverage(x, d, rebalance, book, L, current, record) -> Result<f64>`,
`records()`; `records_csv`, `declaration`, `parameters_json`, `summary_json`, `rule_suffix`.

NAV hook (`strategy_nav_v7.hpp/.cpp`): `NavV7Options::risk_target`; flags `--risk-target S`, `--risk-target-bias b`,
`--risk-target-cadence C` (claimed by `claims_nav_args`); `--risk-model(-sha256)` now also allowed for the risk target
without an spo rule; dispatch opens the store for spo or risk target (one store); `State::plan` = scaler then
`plan_rule` (the old body, `cfg` with `aim_leverage = L_t`, `base_leverage = L`); `ScopedNavExtension::risk_target_scaler()`.
Outputs only with the flag: `recipe.json`, `summary.json`, holdings manifest get `"risk_target"` (rule, declaration,
sigma_star, bias, cadence_sessions, clip, periods_per_year, series; summary and `v7_extras.json` also `books`: per book
decisions, estimates, base_leverage, L_t and multiplier n/mean/min/max, decisions/estimates at each clip,
decisions_before_first_estimate, sigma_hat over estimates); `<output>/risk_target.csv` (columns
`session,book,rebalance,updated,gross,priced_share,sigma_hat,raw,L_t,multiplier,clip`, one row per scored decision and
book of the main pass; hashed in `v7_extras.json` files; written on a void run too, it holds no NAV or return).

Cell template `scripts/specs/v8/r8.json` (A2 mechanism; nominal parent base-b0c, parent null for root): nav flags
`--risk-target .05 --risk-target-bias 1.15 --risk-target-cadence 21 --risk-model <fill> --risk-model-sha256 <fill>
--capacity-curve` (E-29; a no-op on B0c's chain), output `build-equity/mega-nav-v8-r8-rt.05-b1.15-c21-L1.247`. Year
table: inherited `nav_summ --protocol v8` (per-year vol). The description carries the registration and acceptance.

## 3. How root verifies

Single-TU checks: `powershell scripts\atx-build.ps1 check atx-engine\src\book\risk_target.cpp`, the same for
`atx-impl\src\strategy_risk_target.cpp`, `atx-impl\src\strategy_nav_v7.cpp`, `atx-impl\src\strategy_spo.cpp`.
Build: `atx-impl-strategy-target-tests` (now lists `strategy_risk_target_test.cpp` and the engine
`book_risk_target_test.cpp`); optionally `atx-engine-book-tests` (auto-glob) and `atx-impl-tests` (auto-glob).

gtest filters (atx-impl-strategy-target-tests):
- new: `RiskTarget.*:BookRiskTarget.*` (7 + 7 tests: closed form on a synthetic covariance, clip, cadence, refusals;
  dense recomputation on the fixture store, replay cadence every 21 sessions, aim-partial-v5 bit for bit at L_t,
  spo-v3 aim / gamma / bound, capacity x1 = main, CLI, blocks).
- identity: `RiskTarget.FlagAbsentKeepsThePinnedBenchDigests:SpoPin.*:SpoV3.*:SpoHook.*:NavV7Hook.*` (SpoPin pins
  0xda6b6871e7e267c5 / 0xaabdbb72f99a6e13 asserted again under the new code; every R-6 test exercises the
  `plan_tracking` edit with `base_leverage` NaN; `RiskTarget.SpoV3TracksTheScaledAim...` shows base = L equals NaN
  bit for bit).

Identity argv (flag absent, byte-identical): re-run the parent cell's NAV argv exactly as `research_cycle.py plan`
prints it for the parent (B0c, or R-6 / the last accepted cell) on the merged binary into a fresh `--output`, and
compare sha256 of every file with the parent's existing NAV dir (daily_*.csv, events_*.csv, recipe.json, summary.json,
v7_transfer_coefficient.csv, v7_extras.json, capacity/*). No new key or file may appear.
R-8 cell argv: the same + `--risk-target .05 --risk-target-bias 1.15 --risk-target-cadence 21 --risk-model <dir>
--risk-model-sha256 <sha>` (r8.json resolves it; `plan scripts/specs/v8/r8.json` after setting parent).
Mechanics before reading returns: `summary.json` `risk_target.books.<S2>`: `decisions_before_first_estimate` (0 if the
store forecasts the warm-up rows), `estimates` ~ scored sessions / 21, clip counts; `risk_target.csv` `priced_share`
near 1. Acceptance (report only here): `nav_summ --protocol v8` year table S2 vol in [.04, .06] each year AND paired
dSR >= -1 SE.

Python run here: `scripts/tests/test_research_spec.py` 32 passed (r8 added to NULL_PINS, FILLS, EXPECTED_CHANGES, the
flag delta); `test_cycle_scoring.py` + `test_research_cycle.py` 95 passed, 3 skipped.

## 4. Deviations and interpretations (with reasons)

1. "updated every 21 sessions" read as: the first estimable decision, then the first estimable one >= 21 sessions later
   (sessions = decision rows; with cadence 1 every 21st). A due decision without a forecast or with a flat book does not
   advance the clock (no estimate is invented).
2. Gross-1 over the whole book, variance over priced names (literal "book_variance on the gross-1 current book":
   book_variance prices only names with a risk row). The priced share is reported per decision.
3. spo-v3: gamma and the gross bound read L, not L_t (the tracker differs from its parent only by the aim's scale;
   keeps E-37's x1 = main and gamma_equals_main under the capacity curve).
4. Per-book L_t (each book's own current book), the plan says "the current book"; S2 is the scored one.
5. The plan's line reference `strategy_nav_v7.cpp:312-384` is the old `State::plan`; wired there (now `plan` +
   `plan_rule`).

## 5. Cross-lane edits

- `atx-impl/src/strategy_spo.hpp`: `BookDecision::base_leverage` (trailing, default NaN) -- lane R6's file.
- `atx-impl/src/strategy_spo.cpp` `Engine::Impl::plan_tracking`: base L for the bound and the calibration (value
  preserving when NaN) -- lane R6's file.
- `scripts/tests/test_research_spec.py`: r8 expectations (lanes COMB2 / ORTH add r10 / r11 to the same dicts: merge
  by union).
- CMake lists: `atx-engine/CMakeLists.txt`, `atx-impl/CMakeLists.txt`, `atx-impl/tests/CMakeLists.txt` (one line each).

## 6. Open risks

1. **Acceptance texts disagree** (for root, before any read): prereg rule 5 requires paired dSR > 0 of every cell; the
   plan's R-8 text says dSR not lower by more than one SE (and expects Sharpe -.01 to -.04, which rule 5 would reject).
2. **The rule scales the aim, not the book.** Under aim-partial-v5 (theta .05) the book carries about 1 / 1.247 of
   the aim's gross (the registered reason L is 1.247), so from first principles the book's realised vol lands near
   S x b_true / (b x 1.247), i.e. about 4.0% if b is right: the lower edge of [.04, .06]. Under spo-v3 the book tracks
   the aim's gross. Registered verbatim; not changed.
3. The clip binds by construction when the parent's gross-1 vol is far from S / (b L) (outside [.8, 1.25] of it); then
   the year criterion fails by design. Unknown without a read.
4. No specific ceiling in the scaler: a store with blown specific variances (the v1 TRAIN store had 1e0..9e12 on ~178
   names for 20 sessions from 2020-05-12) would put L_t at .8 L for 21 sessions under aim-partial-v5 (spo-v3 voids
   on any clamp). Root: check the v1.1 store's clamp count (spo-v3 `capped_specific`) before R-8.
5. The ADV cap (R-5, if accepted) reads L: at L_t = 1.25 L a position can reach 1.25 x Q ADV.
6. The daily decide path (`strategy_live`) does not take `--risk-target` (research NAV only).
7. Never compiled: clang-cl /W4 /WX risks are mine; the partial aggregate init of `BookDecision` relies on the
   default-member-initializer exemption (precedent: `Ctx` in strategy_nav_replay.cpp).
