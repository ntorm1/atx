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

(Session R7, second stop: still all 8 not started in code; designed in full, see "Session R7" below.)

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

## Session R7 (second owner stop, 2026-10-05)

Dispatch: `r7-dispatch.md` section COV-MV (steps 1-7). Reading redone in full (resume4 / resume6 / r6-wave3-common /
resume7, r7 COV-MV, brief, wave3-carry COV-MV + W3-9/10/17 + COV-11, cov-design 5.2, ledger rulings incl. COV-14,
PRE-24a, PRE-29, PRE-9, COV-13, the RESUME 7 block; `.agents/cpp/agent.md`; prereg B.5).

### State at the stop

- Step 1 DONE: merged root `a15ad887` -> head `338e0e07` (clean, no conflict). AL-CLOCK (pool-8) had NOT merged in
  root and had no code committed (its head `8a6ebe5b`, handoff only): the W3-9 keep-both append is still ahead.
- Steps 2-7 NOT STARTED: no code, no build, no test run, no tag `p9-covmv-*` used (next tag `p9-covmv-a`).
- `build-equity/` holds only COV / COVR receipts and logs (3.3 MB, object trees already gone): the first build is a
  fresh configure (`powershell scripts\atx-build.ps1 configure -Preset equity-dev`). C: 85 GB free at the stop.
- No permission refusal. Tree clean after this commit.

### Design fixed in this session (use it; file:line at `338e0e07`)

Engine kernel (new `atx-engine/include/atx/engine/book/mv_target.hpp`, `src/book/mv_target.cpp`; one source line
after `src/book/risk_target.cpp` at `atx-engine/CMakeLists.txt:173`; test `atx-engine/tests/book/book_mv_target_test.cpp`
is globbed into `atx-engine-book-tests`):
- `mv_aim(const FactorRiskView& m, span<const f64> score, span<const f64> sigma, span<f64> out, MvAimScratch&)`
  -> `Result<usize>` (active names). Active = finite score (NaN = no opinion: out 0, outside every sum). a = sigma *
  score, demeaned over active (P alpha); u = a / D; G = X'D^-1X and r = X'u accumulated per name over its nonzero
  columns (0, group, 1 + groups + s); M = I + F G, rhs = F r; partial-pivot elimination (first max, fixed order;
  zero / non-finite pivot -> InvalidArgument "I + F G singular: F not PSD"); b = u - (X y) / D; w = b - mean(b);
  out = w / sum|w|. Unavailable: < 2 active, P alpha == 0, sum|w| == 0. InvalidArgument: sizes (checked
  multiplication for n * styles and K * K, R2b class), group > groups, non-finite covariance / exposure, sigma or D
  not finite > 0 on an active name. Never F^-1 (COV-13 (c)); valid for singular F since eig(I + F G) >= 1 for PSD F.
  Push-through identity checked by hand: V (D^-1 - D^-1 X (I + FG)^-1 F X' D^-1) = I.
- `MvAimScratch` (vectors active, pivot, a, u, g, m, r, y, columns, values; `bytes()` = sum capacity * sizeof);
  grow only by exact `reserve`, so `mv_aim_working_bytes(names, groups, styles)` (saturating u64) bounds `bytes()`.
- Print helpers: `factor_exposure(view, w, e)` (e = X'w, name order, as `factor_variance` `risk_target.cpp:74-104`);
  `factor_eigen(covariance, k, FactorEigen&)` = `atx::core::linalg::symmetric_eig` (`atx-core/include/atx/core/
  linalg/decompose.hpp:127`) of the forecast block {j : F_jj > 0} (the store's RiskSlice sets non-forecast NaN
  entries to 0, `strategy_spo.hpp:232`; COV's eigen family uses the finite-diagonal block, ascending,
  `strategy_risk_model.cpp:1414-1437`); `low_eigen_share(eig, e, 3)` = sum_{r<3} lambda_r (u_r'e)^2 / sum_r
  lambda_r (u_r'e)^2, NaN when not > 0.
- Gtests `MvTarget.*`: ThreeNamesDiagonalClosedForm (K = 1, F = [0], dyadic sigma / score so the comparison is
  exact), EqualsDenseInverseOnSmallN (N 40, K 6 = 1 + 3 groups + 2 styles, realistic D ~ 1e-4, F ~ 1e-4 so
  kappa(V) ~ 40; |w - w_dense| <= 1e-12 at gross 1), RankDeficientFEqualsDenseSolve (F = v v' with dyadic v plus a
  zero row / column: exactly singular), GrossNormalised, PositiveScalarWashesOut (c in {1e-3, 1, 1e3}, 1e-14),
  NanAlphaIsNoOpinion (bits equal to the model without those names), DollarNeutralOnThePricedSet,
  WorkingBytesBound (+ saturation), refusals with teeth, LowEigenShare on a diagonal F with one zero factor.

Impl rule (proposed new `atx-impl/src/strategy_mv_aim.{hpp,cpp}`, tail `target_sources(atx-impl-strategy PRIVATE
src/strategy_mv_aim.cpp)` at the end of `atx-impl/CMakeLists.txt` (cross-lane CMake tail); keeps the edits in C3 /
L4 files to glue):
- `apply(x, d, no_short, desired)` on the calling thread: `RiskStore::check_axes` once per input
  (`strategy_risk_target.cpp:104-107` idiom); `read(d, slice)`: Unavailable -> pass-through `no_forecast`; COV-11
  S11: every slot of row d must be < 50 or == 255 (`risk_no_row`), else refuse naming instrument and slot; priced =
  member && finite desired && slot < 50 && D finite > 0 (`strategy_risk_target.cpp:33-36`); < 2 -> pass-through;
  kernel with score = desired, sigma = sqrt(D), view groups 50 / styles 11 / group 1 + slot; Unavailable -> `flat`;
  priced desired_i = g * out_k, g = sum_P |desired_i| (W3-17); unpriced members and nonmembers untouched.
- Stats only for scored decisions (d >= `x.decision_begin`) with d above the last recorded index (calibrate pass 2
  that forms the construction again, `book/nav_run.cpp:249-262`, must not double count); warm-up counted apart.
  Block `mv_aim` in `summary.json` (main pass; capacity pass: parameters only, as `add_risk_target`
  `strategy_nav_v7.cpp:321-331`): rule, declaration, risk_model {directory, manifest_sha256}, scored decisions,
  applied, pass_through {no_forecast, fewer_than_two_priced, flat}, warm_up, priced_names / unpriced_members
  {n, mean, min, max} over applied, specific_vol_ratio {pooled = sum |w| sigma / sum |d| sigma, per_decision},
  eigen_00_02_share {mv, desired}, special_tier_negative {locate_in_aim, name_decisions, decisions} (no_short[i] &&
  w_i < 0). recipe.json and the holdings manifest: rule, declaration, risk_model. Rule id suffix `+mv-aim-v1` before
  `add_risk_target`'s suffix (no Python parses the suffix: grep of `scripts/*.py` done).
- `v7::extension_workspace_bytes(names)` (0 without the rule) added as ONE term in `nav_workspace_reserve_bytes`
  (`book/nav_lockstep.cpp:65-75`, cross-lane one-term edit) so the scratch counts in `--max-bytes` (reviewer item 5).

Glue in C3 / L4 files (list each in the report):
- `book/nav_rules.cpp:96-98`: `constexpr Row targets[] = {{"mv-aim-v1", Kind::Target, "--target-rule mv-aim-v1",
  mv_aim_params, mv_required, mv_incompatible, "<description>"}};` with params `risk_model` (`--risk-model`) and
  `risk_model_sha256` (`--risk-model-sha256`, pattern `^[0-9a-f]{64}$`), both required (PRE-9), incompatible
  {aim-partial-v6, spo-v1, spo-v2, spo-v3, two-speed-nav-v1, adv-hold-v1, hold-band-v1}; `rules_json()` `:170-171`
  wraps `std::span<const Row>(targets)`. `--target-rule mv-aim-v1` passes `wave_kinds.nav_flags`
  (`scripts/wave_kinds.py:384-401`; `PLACEHOLDER_RE` :99 does not match the lowercase id). NavSpec already has
  `target_rule` (`book/nav_spec.cpp:75`): no NavSpec edit.
- `strategy_nav_v7.cpp`: `NavV7Options::mv_aim`; State member + `begin_run` reset; parser `:1100-1121` handler sets
  `o.mv_aim` (keep the "no handler" throw for other ids); `:1230-1232` spo_only lets the store flags through when
  `o.mv_aim`; refusal block inserted after the spo_values loop (`:1246-1277`) and BEFORE `if (o.spo_v1)` (so the
  rule's own message wins): spo rule, aim-partial-v6, `--rule` not aim-partial-v5, `--two-speed`, `--adv-hold-q`,
  `--hold-band`, `--construction-grid`, missing store flags; store open `:1365` adds `|| o.mv_aim`; help `:1009-1010`
  and header `strategy_nav_v7.hpp:15` text (help-only byte change); `summarize` / `extend_recipe` /
  `extend_holdings` add the block; new `v7::apply_target_rule(x, d, no_short, desired)` (identity without an
  extension or the rule).
- Hook `book/nav_decide.cpp:42-43`: `ATX_TRY(const bool proceed, detail::form_desired(...)); if (proceed)
  ATX_TRY_VOID(v7::apply_target_rule(x, d, shared.no_short, shared.desired)); return co::Ok(proceed);`. Runs on the
  calling thread (`book/nav_session.cpp:347-350`, outside `for_each_lane`), before `record_construction`, so a
  restored calibrate pass replays the post-rule target. One construction per decision because adv-hold / two-speed
  (the only `book_groups` splitters, `book/nav_lockstep.cpp:144-161`) are refused. Decide path: the extension is
  installed only by `dispatch_nav_v7` (`strategy_nav_v7.cpp:1380`); `strategy_live.cpp:1377` runs without one, so
  the hook is the identity there (state it; optional test).

Impl gtests `atx-impl/tests/strategy_mv_aim_rule_test.cpp` (globbed into `atx-impl-tests`; add a
`target_sources(atx-impl-strategy-target-tests PRIVATE ...)` tail in `atx-impl/tests/CMakeLists.txt`): fixtures
`strategy_spo_fixture.hpp` (Role, write_risk_model with specific_overrides, nav_config), `strategy_spo_cli_fixture.hpp`
(write_run_inputs), `strategy_cov_fixture.hpp` `add_container` (role pin must be 64 hex), CLI pattern
`strategy_vol_target_test.cpp:619-687`, truncation pattern `:694-774`. Truncated-role note: `Role(d)`'s member
exit stretch depends on d (`strategy_spo_fixture.hpp:166-168`): build the "future deleted" role by copying a prefix
of the full role's vectors, and the store by `write_risk_model` on the prefix sessions (its draws are date-ordered,
so the prefix rows are bit-equal). Teeth plan: one extra build with every new refusal disabled at once, run the
refusal tests, record the failing assertions, revert.

COV-11 a = 2 control: beside the a = 0.5 control in `CovBias.EigenFamilyInBandOnAPlantedModel`
(`atx-impl/tests/strategy_cov_model_test.cpp:644-646` per re-review 2 S10; a COV file: the smallest append).

Registration / template shape: `scripts/specs/p9/rules/mom-volman-v1.json` and `templates/mom-volman.json` (read);
the registration's store pin is `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3` (prereg B.5,
`build-equity/p9-m2i-dcov-covv1`, never opened); `source_sample_end` still to be taken from a cov-design section 8
primary source (Menchero-Wang-Orr 2011 / 2012) or written "unverified" (PRE-19).

## Session R8 (2026-10-06; dispatch r8-dispatch.md COV-MV)

### State at hand-back

- Merged root `1b64ce45` (merge `10259f38`, clean), then root `fd084e78` (SQL3; merge `db33c6ca`, clean; Debug
  suites not yet re-run on it); AL-CLOCK not merged in root
  (`git merge-tree` with its head `c3f3f1f8`: clean, keep-both is textual-conflict free today).
- Items 1-8 DONE and committed (code / spec head `846739c1`): kernel `756035ed`; row, parser entry, hook, refusals,
  summary block, workspace term, impl gtests `f6b05cbd` (+ slip `d44e05f5`, test slip `846739c1`); COV-11 a = 2
  control `26b67bd5`; registration and template `628a9ffb`. Report: `task-COV-MV-report.md` (evidence in
  `covmv-logs/`).
- Debug evidence on the clean head (tags p9-covmv-a, c, d): MvTarget 11/11, MvAimRule 15/15, named suites 99/99,
  whole atx-impl-strategy-target-tests 405/405 (1 DISABLED by design); teeth (p9-covmv-e): the 3 refusal tests fail
  with the refusals disabled.
- PERMISSION REFUSAL (PERM-1, not retried): the command that reverts the teeth mutation (`git checkout --
  atx-impl/src/strategy_nav_v7.cpp atx-impl/src/strategy_mv_aim.cpp`, inside a compound command quoted in the
  report). The working tree therefore still carries two uncommitted `// TEETH` lines:
  `atx-impl/src/strategy_nav_v7.cpp:1271` (`if (o.mv_aim && false) {`) and `atx-impl/src/strategy_mv_aim.cpp:135`
  (`if (true || slot < ...) continue;`). Never stage them. `build-equity/bin/atx-impl-strategy-target-tests.exe` is
  the teeth binary.

### Next step for a fresh agent (after the PM / owner rules on the revert)

1. Revert the two `// TEETH` lines (`git checkout --` of the two files, once approved); `git status` must be clean
   except git-ignored sprint files.
2. Debug canary: `scripts/research-build.ps1 -Tag p9-covmv-f -Targets atx-equity-strategy-ic,
   atx-equity-strategy-targets,atx-research-eval,atx-impl-strategy-target-tests -Canary` (PYTHONDONTWRITEBYTECODE=1);
   re-run `MvAimRule.*` on it.
3. Release once: `powershell scripts\atx-build.ps1 configure -Preset equity-rel`, then `-Preset equity-rel -Tag
   p9-covmv-g -Targets atx-engine-book-tests,atx-impl-strategy-target-tests,atx-equity-strategy-ic,
   atx-equity-strategy-targets,atx-research-eval -Canary` (stale-PCH ccache trap: CCACHE_RECACHE=1); run MvTarget.*,
   MvAimRule.*, the named filter and the whole target-tests in Release.
4. `git -C C:/atx-wt/pool-2 log --format=%s -30`: if root moved (AL-CLOCK), merge it (keep both rows / terms) and
   re-run the Debug filters; update the report's evidence and "Pending"; commit the report (`git add -f`).
5. Delete `build-equity` / `build-equity-rel` object trees by name (keep `mega-*` receipts and logs).

## Session R9 (2026-10-07; dispatch r9-dispatch.md COV-MV; TEETH-POOL)

- Continued in pool-31 on `feat/p9-covmv2-20261007` (TEETH-POOL); the two `// TEETH` lines stay uncommitted in
  pool-24 for the owner (`atx-impl/src/strategy_nav_v7.cpp:1271`, `atx-impl/src/strategy_mv_aim.cpp:135`; pool-24
  is read-only for this lane: no build, no edit, no git write; nothing under its `build-equity*/bin` is evidence).
- Start: pool-31 head `b6e304ee`, tree clean, no `// TEETH` line in either file. Pool-24's lane records
  (`handoff-COV-MV.md`, `task-COV-MV-report.md`, `covmv-logs/**`) compared with pool-31's: identical modulo the CRLF
  checkout; nothing copied.
- Step 1 of "Session R8 / Next step" is void here (the committed tree holds no teeth line); steps 2-5 run in this
  pool per the R9 dispatch.

### State at hand-back (R9; ruling TEST-1 amended the dispatch mid-round)

- Root `685e238b` merged (merge `a8baba2a`, clean, docs only); AL-CLOCK not in root. A dry merge with
  `feat/p9-alclock2-20261007` (`7ebd41ff`) shows one keep-both conflict for root: the tail of
  `atx-impl/tests/CMakeLists.txt` (report, W3-9 note).
- One Debug build `p9-covmv-f` on `a8baba2a` (`atx-impl-strategy-target-tests` + `atx-engine-book-tests`, which
  `MvTarget.*` needs; no canary): exit 0, 0 first-party warnings. `MvTarget.*` 11/11, `MvAimRule.*` 15/15,
  named filter 99/99 (logs `covmv-logs/f-*`).
- Deferred to the gate (TEST-1): the whole target-tests, the Release build and suites, both canaries.
- No teeth run in R9. No permission refusal in R9. The `build-equity` object tree is deleted by name after the
  report commit (receipt / logs kept). Next step: the first review (the PM dispatches it).

## Fix round 1 (COVMV-S; R9; stopped by OWNER STOP 9)

- Dispatch: "Dispatch COV-MV fix round 1" (ledger), Ruling COVMV-S, review `task-COV-MV-review.md` (the seven
  Suggested). Pool-31, branch `feat/p9-covmv2-20261007`, start head `e9e794b3` (the review commit on `bda51b98`;
  code `846739c1`).
- STOP 9 arrived while single-TU checks ran. Everything below is ONE commit `cc63c38f`
  (`WIP(cov-mv-fix1): ...`), because the stop came before the slicing step; history is never rewritten, so the
  report lists the hunks by item instead of by commit.
- No research build ran in this round (tag `p9-covmv-i` is still unused). No test ran. No teeth run. No permission
  refusal. Configure `equity-dev` done (`covmv-logs/i-configure-dev.log`, exit 0).
- Single-TU checks (`atx-build.ps1 -Preset equity-dev check`): `atx-engine/src/book/mv_target.cpp` and
  `atx-impl/src/strategy_mv_aim.cpp` compile clean, 0 warnings (`covmv-logs/i-check-1.log`). Second check
  (`strategy_nav_v7.cpp`, `book/nav_rules.cpp`, `book/nav_decide.cpp`, `book_mv_target_test.cpp`,
  `strategy_mv_aim_rule_test.cpp`): all five compile clean, 0 warnings, exit 0 (`covmv-logs/i-check-2.log`; the test TUs pulled about 224 engine
  objects through their order-only deps).

### Items: done in the working tree, committed in `cc63c38f`, untested

| item | where (file) | category | test |
|---|---|---|---|
| S-1 (REQUIRED) | `atx-engine/include/atx/engine/book/mv_target.hpp` (`FactorEigen` gets a PIMPL `Workspace`, move-only, the vectors reserved once at k / k x k); `atx-engine/src/book/mv_target.cpp` (`FactorEigen::Workspace` = the block `MatX` + `Eigen::SelfAdjointEigenSolver<MatX>`, kept across calls; `factor_eigen` runs `symmetric_eig`'s guard `linalg::detail::is_symmetric_within(kSymmetryTol)` and `solver.compute` with the default options, the same messages and codes; `sized` reuses a new `reserve_exact`); `atx-impl/src/strategy_mv_aim.cpp` `workspace_bytes` (comment only: the value is UNCHANGED, so `nav_workspace_reserve_bytes` with and without the flag is unchanged; `book/nav_lockstep.cpp` not edited) | allocation | new `MvTarget.FactorEigenOnKeptStorageIsAFreshSolve` (62-factor blocks: same, smaller, larger, 1, after a refusal; every value and vector bit-equal to a fresh `linalg::symmetric_eig`); existing `MvAimRule.WorkspaceReserveTermOnlyWithTheRule` |
| S-2 (REQUIRED) | `strategy_mv_aim.hpp` (`Counts::share_unavailable`, `share_error`; header text); `strategy_mv_aim.cpp` `Rule::apply` tail: `if (auto printed = add_eigen_shares(); !printed) note_unavailable(printed.error());`, `record` now `void` (ratio, negatives), `add_eigen_shares` (both shares or neither), `note_unavailable`; `summary_json`: no failure -> the `eigen_00_02_share` block byte-identical to before; any failure -> `"eigen_00_02_share": null` plus `"eigen_00_02_share_unavailable": {"decisions": N, "first_error": "..."}` (a key that exists only on failure) | failure handling | new `MvAimRule.PrintFailureLeavesTheBookAndWritesNull` (helper `plant_asymmetric_f`: F_{0,8} moved 1e-4 from 0 with its sign kept, F_{8,0} left; factor 8 = slot 7, which no fixture name has, so the kernel reads F bit for bit as before and the print's symmetry guard fails at every decision; in-process targets bit-equal to the clean store's, counts, null; CLI: every file but recipe.json / summary.json byte-equal, those equal after the pin, `recipe_sha256` and the two eigen keys are removed) |
| S-6 (REQUIRED) | `strategy_mv_aim.cpp` `Rule::check_input`: `check_axes` once per run (`axes_checked_`, reset by `begin_run`), then `assert(store_->check_axes(...) && "...")` (Debug only); `strategy_mv_aim.hpp` (`axes_checked_`, docs). Deviation: "at store open" is not possible as such, because `RiskStore::open` (dispatch_nav_v7) runs before the role is loaded; the first decision of the run is the first point the replay's session axis exists (the `BookScaler::estimate` / `spo::Engine::prepare` pattern). | check timing | existing `MvAimRule.*` (no test changes) |
| S-4 | `strategy_mv_aim.cpp` / `.hpp`: `apply` (now ~25 lines) calls `check_input`, `begin_decision` (fresh / scored, `Visit`), `priced_view` (S11 + the view; returns the unpriced count), `tilt` (kernel + gross rescale), `pass_through`, `count_applied`, `record`, `add_eigen_shares`, `note_unavailable`; `invalid` returns `auto` (the Err proxy). Same order of every side effect as before. | structure | existing `MvAimRule.*` |
| S-3 | `strategy_nav_v7.hpp`: the `#include "strategy_mv_aim.hpp"` replaced by `namespace atx::impl::strategy::mv_aim { class Rule; }`; `strategy_nav_v7.cpp` includes `strategy_mv_aim.hpp` (grep: no other includer of v7.hpp uses mv_aim; `book/nav_rules.cpp` and the rule test include it directly) | structure | compile only |
| S-5 | `book/nav_rules.cpp` `mv_aim_refuses`: `two-speed-nav-v1` -> `two-speed-v1` (as per-name-v1's and AL-CLOCK's rows name it) + one comment line; `MvAimRule.ListRulesRowIsAClosedTargetRow`'s expected list updated to match. Changes `nav --list-rules --json` bytes of the mv-aim-v1 row only (no Python consumer of `incompatible[]`; no registration or template byte changes). | structure (registry naming) | `MvAimRule.ListRulesRowIsAClosedTargetRow` |
| S-7 | record only: the R9 runs' exe SHA check was not logged; the receipt SHAs match (reviewer). Next build: hash each exe into the log beside its gtest run. | record | - |

Residual allocation (S-1, stated in `mv_target.hpp`): Eigen 3.4.0's `SelfAdjointEigenSolver::compute` makes one
block-sized vector per call inside `HouseholderSequence::evalTo` (its API takes no workspace), and the block /
solver storage is resized when the forecast block's size changes from the previous decision. Everything the print
owns is kept. `RiskStore::read`'s per-call `narrow` buffer is COV's code (not this lane's).

Byte identity (the PM's bar): no arithmetic changed. Every hunk is allocation (S-1), check timing (S-6), failure
handling (S-2: bytes differ only when a print fails, which aborted the run before), or structure (S-3, S-4; S-5
changes the list-rules row text only). The proof rests on the tests below plus the new S-1 bit-identity test; no
before/after byte compare is possible within TEST-1's one build.

### Next step for a fresh agent (exact)

1. In pool-31, tree clean at `cc63c38f`. Re-run the single-TU checks that did not finish or failed (see
   `i-check-2.log`); fix slips in place (a slip is a normal commit, `fix(cov-mv): ...`).
2. ONE Debug build (HOST-3: fifth; wait while another build runs; `covmv-logs/tools/wait-build.ps1 -Tag p9-covmv-i
   -Targets atx-impl-strategy-target-tests,atx-engine-book-tests`). Log the two exe SHA-256s beside the runs (S-7).
3. Run `MvTarget.*` (expect 12: 11 + the S-1 test), `MvAimRule.*` (expect 16: 15 + the S-2 test), the named filter
   `Cov*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavAnnMeanGross.*`
   (expect 99).
4. Teeth (the only other build, skip if the host is loaded): with a tool that saves and restores the bytes itself
   (TEETH-RESTORE), mutate in `atx-impl/src/strategy_mv_aim.cpp` the line
   `  if (auto printed = add_eigen_shares(); !printed) note_unavailable(printed.error());` to
   `  if (auto printed = add_eigen_shares(); !printed) return printed;` (the pre-round behaviour: the print stops the
   run); build `p9-covmv-j` (`atx-impl-strategy-target-tests`), run `MvAimRule.PrintFailureLeavesTheBookAndWritesNull`,
   expect FAILED; restore; assert `git status --porcelain` empty and `git hash-object` = `git rev-parse HEAD:<path>`
   (CRLF checkout: compare git's blob ids, plus the raw SHA-256 against the saved pre-mutation bytes).
5. Append "Fix round 1 (COVMV-S)" to `task-COV-MV-report.md` from the table above plus the evidence; one line to
   this handoff; commit (`git add -f`); delete `build-equity`'s object tree by name (keep `audits/`,
   `v8-interim3-pitch-render-run2/`, the `mega-p9-covmv-*` receipts and logs).
6. Root re-proves at merge: ROOT-1a's filters (unchanged) plus the two new tests; `nav --list-rules` shows the
   mv-aim-v1 row with `two-speed-v1` (S-5); flag-absent identity unchanged (the reserve term's value did not move).

Disk: `build-equity` now holds the configured tree and about 230 check objects (about 0.6 GB); NOT deleted at the
stop (the report is not final and the next agent reuses the configured tree).

- R10 (2026-10-08, pool-31): steps 1-3 and 5 done, step 4 deferred (HOST-4: SIGN and the D3W-PB reviewer were
  waiting to build); roots `b182dfdd`, `08f04e07` (plain) and `094bcd3b` (one keep-both at the tail of
  `atx-impl/CMakeLists.txt`, ABS's row first) merged; Debug `p9-covmv-i` on `19e967d0`: `MvTarget.*`
  12/12, `MvAimRule.*` 16/16, named 100/100 (99 + C3b's new NavSpec test), exe SHAs logged beside each run (S-7);
  report section "Fix round 1 (COVMV-S)"; the `build-equity` object tree (about 2.8 GB) is deleted by name after
  this commit. Next: re-review 1 (BASE `e9e794b3`), whose one build may run `covmv-logs/tools/teeth-s2.py`.
