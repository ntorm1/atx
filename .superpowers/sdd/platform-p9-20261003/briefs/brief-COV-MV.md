# P9 lane briefs (paste one section, plus "Rules for every P9 lane", into an Opus 5.5 implementer's dispatch)

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md` (cited as "plan §n"). Finding ids (F-n, P9-Rn, NV-n, OR-n, FD-n, CM-n,
DS-n) and contracts (K-P9-n) are defined in plan §0 and §2.3. Literature ids (lit §n, [n], F1..F18) refer to
`docs/plans/2026-10-02-p9-literature-review.md`. Root fills `<frozen-sha>` and the pool at dispatch.

## Rules for every P9 lane

1. Read first: `.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding: never build C++, never run real data,
   never dispatch subagents, never push, never touch `atx-db/`, files only with the Write / Edit tools because the
   shell hook breaks heredocs), then plan §0.6 and §2.2-§2.3, then the review files your brief names. For C++:
   `.agents/cpp/agent.md` first; write code that compiles first time under clang-cl 18 `/W4 /permissive- /WX`
   (no unused variables, sign conversions or shadowing; 100-column limit; copy the owning file's idiom).
2. Work only in your leased pool on your branch (`feat/p9-<id>-20261003`, base `<frozen-sha>`). Lease:
   `powershell scripts\lease-worktree.ps1 -Branch feat/p9-<id>-20261003 -Base <frozen-sha> -Agent p9-<id>
   -RunId p9-<id>-20261003 -HeartbeatId p9-<id>-hb -MaxPool 20` (root may have leased it for you; check `-Status`).
3. Blind. Do not open any return, IC, Sharpe, turnover or NAV output of 2020-2023 (`build-equity/` NAV, cards,
   marginal, admission and diagnostics files are closed; manifests, receipts and field lists are open). Nothing dated
   2024-01-01 or later is opened by you or by code you run. The numbers in status 7 and the ledger are public.
4. Identity discipline: every change is behind a flag or provably value-preserving; flag absent = byte-identical; say
   exactly how root verifies it (targets, gtest filters, argv, the expected byte-identical files, any substitution
   list). Never edit an expected hash. A Python copy of a C++ rule is deleted only in a later slice, after root's
   identity run (plan §0.6).
5. PM8-12: numerical and research logic goes in atx-engine C++ (generic) or atx-impl C++ (strategy-specific) with
   gtests; Python is orchestration, specs, receipts and reports. No new versioned copy of any script; no new
   `research_fields_*.py` builder module (plan DEC-5).
6. Stay inside "Files in scope". Touching a file another lane owns is a lane failure unless the brief names it as a
   cross-lane edit; list every such edit in the report. CMake: append one block at the end of the owning list.
7. Implement first, then the tests named in the brief (they are the acceptance contract) plus what pins behaviour.
   Run pytest yourself on synthetic data: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   <files>`. C++ tests are written, not run; name the anchored gtest filters root will run.
8. Commit per task: `git add <your files>`, conventional message, trailer
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
9. Report: `.superpowers/sdd/platform-p9-20261003/task-<ID>-report.md`, committed with `git add -f`, in the
   `.agents/harness/TEMPLATES.md` "Lane report" shape: outcome, branch / SHA, files changed, evidence (each pytest
   command with exit code 0 and output tail), how root verifies (build targets, gtest filters, identity runs with
   argv), deviations, cross-lane edits, open risks, 0-3 ledger candidates.
10. Final reply to the PM: at most 15 lines (status DONE / DONE_WITH_CONCERNS / BLOCKED, commit SHAs per task, one test
    line, concerns). An adversarial reviewer reads your exact SHA before merge; fix rounds get a new review.

---

## Lane COV-MV: `mv-aim-v1`, the max-Sharpe NAV target rule on the pinned risk store (blind; wave 3b)

**Pool / branch:** 24 (COV's warm tree, leased after COV merges and its lease is released; else a fresh lease with
`-MaxPool <its number>`), `feat/p9-covmv-20261003`, run id `p9-covmv-20261003`, heartbeat `p9-covmv-hb`.
**Wave:** 3b (ruling COV-7); base = the integration head after C3 merges (COV and C2 merged before it).
**Effort:** M. **Serves:** Sharpe (the optimal-Sharpe path of the owner's covariance directive). **Trials:** 0 (cell
P9-MV is root's). **Merge slot:** 3b, after AL-CLOCK (plan §3.2), beside SQL4; never under an in-flight wave
(E1-STALE). **Needs merged:** COV (with its fix round 2: R1b and the R2b geometry wrap, ruling COV-9), C2, C3 (typed
NavSpec and the K-P9-7 registries with a `target` kind). **Deadline (COV-7):** if this lane is not merged and reviewed
APPROVE before the P9 cells start, P9-MV is dropped at 0 trials.

**Standing contract (overrides rules 1, 2, 7 above where they differ):** `resume4-dispatch.md` (builds only through
`powershell scripts\research-build.ps1`, target-scoped, single-use tag `p9-covmv-<letter>`, one build at a time, memory
gate, root first; pytest with explicit paths; delete your own build tree after the report commit), `wave3-carry.md`
"All lanes" and "COV-MV" (it wins over this brief). Ruling W2-BUILD: you build and run your own gtests.

**Goal.** A NAV rule of kind `target` (K-P9-7) at the NAV path's single desired-target extension point, between the
shared desired target and the construction rule (cov-design §5.2): at each decision d it replaces the desired target's
priced members by the gross-normalised P V_d^{-1} P alpha_d, with V_d = X F X' + D read from row d of the store the
cell pins (`--risk-model DIR --risk-model-sha256 SHA`). Construction (aim-partial-v5 and its trade-rate rules) and the
leverage rule follow unchanged. Rule absent: every NAV byte is today's.

**Read:** `cov-design.md` §3.2 (the normative as-of invariant), §5.2, §5.3, §6 Q1 / Q8; plan §2.3 K-P9-7 and K-P9-12,
§2.4 INFRA-R "COV-MV", §5.2 row P9-MV; `progress.md` rulings COV-2, COV-4, COV-5, COV-7, COV-8, COV-9, G-P11, ALCOMB-1;
C3's report and its merged registry and NavSpec; COV's report, review and re-reviews in pool-24's sprint dir (R2b, S9);
`atx-impl/src/strategy_nav_replay.hpp:17-30` (timing: decide at session d after its mark, fill at d+1's close, first
return row d+2; the single desired-target extension point); `strategy_spo.hpp:15-30` (the Grinold-Kahn alpha:
a_i = IC sigma_i z_i / sqrt(h), sigma_i = sqrt(D_i) at d, z = the shared desired target over its cross-sectional SD)
and `:221-266` (`RiskSlice`, `RiskStore::open` / `read`); `strategy_risk_target.cpp:141` (`BookScaler::estimate` reads
`risk_->read(d, slice_)`) and `strategy_spo.cpp:1097` (spo reads `risk->read(d, date.slice)`): the two consumers
whose as-of COV's reviewer traced; `atx-engine/include/atx/engine/book/risk_target.hpp:55-62` (`FactorRiskView`);
`atx-engine/include/atx/engine/risk/README.md:70-111` (`FactorModel`, Woodbury `apply_inverse`, the D floor in
`create`) and `:143-159` (under gross normalisation only the V^{-1} tilt survives); `cov_container.hpp`
(`factor_risk_view`, `geometry_ok`); `strategy_risk_target.cpp:331-351` (a NAV rule row with `x-flag`);
`scripts/wave_kinds.py:356-373` (`nav_flags`: how E2 derives a cell's flags from your row); as the shape model,
AL-COMB's `scripts/specs/p9/rules/mom-volman-v1.json` and `scripts/specs/p9/templates/mom-volman.json`.
**Never read** an spo-v1 / spo-v2 / v7 cell outcome or any NAV, IC or Sharpe output: cov-design §5.2 asks the PM, not
the lane, to read them.

**Contracts.** Reads K-P9-12 through `spo::RiskStore` (a container-bearing store and a directory store give
bit-identical `RiskSlice`s: `CovStore.RiskSliceFromContainerEqualsDirectory`). Reads and extends K-P9-7 (C3's
registry): one row `{id "mv-aim-v1", kind "target", params_schema (a closed object with no free parameter),
x-flag (C3's spelling for target rules), incompatible[]}` in the `atx.nav-rules/v1` envelope.

**The rule, exactly (blind: no constant to choose, nothing tuned; PM-confirmed reading, wave3-carry Q17):**
- Members M_d: the names the shared desired target is defined over at decision d. A NaN desired weight is "no
  opinion": weight 0, excluded from every sum.
- Priced set P_d: members with a row in store row d and a finite specific variance D_i > 0 (K-P9-12's priced rows);
  every other member is unpriced.
- alpha_i = sigma_i z_i on P_d, with sigma_i = sqrt(D_i) from row d and z = the desired target over its
  cross-sectional SD on P_d. Any positive scalar (IC, 1 / sqrt(h), the SD) washes out under gross normalisation.
- w_P = P V_d^{-1} P alpha, P = I - (1 / |P_d|) 1 1' on P_d; V_d^{-1} by `risk::FactorModel` Woodbury
  (`apply_inverse`), never a dense N x N matrix. Cost ~ N K^2 per decision (N ~ 5,600, K = 62).
- Gross: w_P is rescaled so that sum |w_P| equals the desired target's gross on P_d; unpriced members keep their
  desired weights; so the vector's total gross equals the desired target's (PM6-6 gross matching at the cell level is
  unchanged). The priced block is dollar-neutral by construction, the unpriced block keeps its net: the report states
  the book's net against the desired target's on the fixture.
- |P_d| < 2 (the projection is undefined): the desired target passes through unchanged and the decision is counted in
  the summary block, never silent.
- `FactorModel::create` floors D (README :109-111): state the floor and show it never binds on a priced row of the
  fixtures, or list where it does.
- Refusals (before any compute): no `--risk-model` store; anything the existing `RiskStore::open` refuses (role,
  sessions, geometry, root); any rule in `incompatible[]`, at least spo-v1 / spo-v2 / spo-v3 (they already optimise
  against V); add any other by code reading, each with a refusal test.
- Output: a `mv_aim` block in `summary.json` only when the rule is on (decisions, priced / unpriced name counts,
  pass-through decisions, the store pin echoed). No return, vol or Sharpe figure is computed by the rule.

**Files in scope (COV-MV owns in wave 3b; plan §2.2 COV-MV row):**
- New: `atx-engine/include/atx/engine/book/mv_target.hpp`, `atx-engine/src/book/mv_target.cpp` (kernel
  `mv_aim(const FactorRiskView&, std::span<const f64> score, std::span<const f64> sigma, std::span<f64> out)` and its
  working-bytes function), `atx-engine/tests/book/book_mv_target_test.cpp` (globbed into the book test group),
  `atx-impl/tests/strategy_mv_aim_rule_test.cpp`, `scripts/specs/p9/rules/mv-aim-v1.json`
  (`atx.p9-rule-registration/v1`: K-P9-11 keys; `source_sample_end` from a paper of cov-design §8, never from house
  data; `predicted_mechanism`: "V^{-1} tilt lowers ex-ante and realised vol at equal gross alpha"; `data_class`),
  `scripts/specs/p9/templates/mv-aim.json` (`atx.research-cycle-template/v1`; NAV-only cell: renames the NAV and
  monitor outputs only, u / fit / card / w are the parent's; no `rules` key, no `change.flags` (ALCOMB-1); `requires`
  names COV-7's deadline, P9-MV registered in `p9-prereg.md` with its store pin, root sets the parent and runs it as an
  `atx.research-wave/v2` rule wave with `rules: [{name: mv-aim-v1, params: {}}]`, PM6-6 gross matching via C2), the
  report.
- Cross-lane (list each in the report): one registry row and one parser entry (NavSpec key and flag adapter) in C3's
  files at the desired-target extension point; one source line `src/book/mv_target.cpp` in the `atx-engine/CMakeLists.txt`
  source list (beside `src/book/risk_target.cpp`; the list is not globbed); one line in `atx-impl/tests/CMakeLists.txt`
  (`atx-impl-strategy-target-tests` sources); the re-pin of any `nav --list-rules` table fixture C3 names (the new row
  is the only rule-absent byte change; a listed contract change, not an expected-hash edit to fit).

**Forbidden:** COV's files (`cov_container.*`, `strategy_risk_model.*`, `strategy_risk_verb.cpp`,
`tools/equity_strategy_risk.cpp`, the RiskStore section of `strategy_spo.*`): read only, report a defect; `eigen_adjust.*`
and `risk/factor_model.*` (use them); every C3 file beyond the row and the parser entry; any other NAV rule's
arithmetic; any recipe or output byte with the rule absent; any registered constant; `vcpkg.json`, `CMakePresets.json`;
every `scripts/*.py` (the rule reaches cells through E2's `rules:` block); real data; any NAV / IC / Sharpe output.

**Tasks, in order (one commit each):**
1. Kernel `mv_target` (engine) and its gtests.
2. The `mv-aim-v1` row, NavSpec key and flag adapter, refusals, the `mv_aim` summary block; impl gtests.
3. Registration file and template.
4. Build (tags `p9-covmv-a..`), run the gtests below in Debug and Release, report.

**Gtests (the acceptance contract).** Engine `book/book_mv_target_test.cpp`: `MvTarget.ThreeNamesDiagonalClosedForm`
(no factor, V = diag(sigma^2), alpha with zero mean: w proportional to P(z / sigma), exact),
`.EqualsDenseInverseOnSmallN` (N <= 40, K <= 6 with one group and one style: Woodbury against a dense V^{-1}, 1e-12
relative per element after gross normalisation), `.GrossNormalised`, `.PositiveScalarWashesOut` (alpha x c for c in
{1e-3, 1, 1e3}: equal within 1e-14 relative), `.NanAlphaIsNoOpinion`, `.DollarNeutralOnThePricedSet`,
`.WorkingBytesBound`.
Impl `strategy_mv_aim_rule_test.cpp` (C1 / C3 synthetic replay fixtures, `strategy_spo_fixture.hpp` `write_risk_model`):
`MvAimRule.FlagAbsentIdentity` (rule absent: every record, daily row and summary byte-identical to the pre-change
code on the fixture), `.TargetAtDReadsStoreRowDOnly` (perturb every store row after d: targets at <= d byte-identical;
teeth: perturbing row d changes the target at d), `.FutureReturnDoesNotChangePast` (delete every session after t),
`.RefusesWithoutAStore`, `.RefusesIncompatibleRules`, `.UnpricedMembersPassThrough`, `.FewerThanTwoPricedPassesThrough`,
`.ContainerAndDirectoryStoresGiveTheSameBook` (NAV bytes equal except the recorded store pin),
`.BookWorkersParity` (1 vs 4 `--book-workers`, rule on: byte-identical; W1-MIN M-1), `.ListRulesRowIsAClosedTargetRow`
(kind `target`, closed `params_schema`, `x-flag`, `incompatible[]` as registered), `.SummaryBlockOnlyWhenOn`.
Unchanged and green: `CovStore.*`, `NavBookRule.*`, `VolTarget.*`, `Spo*`, C3's `NavSpec.*`, `CostLawRegistry.*`,
`TradeRateRegistry.*`, AL-CLOCK's `InfoClockRate.*` when merged. Python: none new; paste T1's guard sanity line
(`scripts/tests/test_no_python_mirror.py`, `test_no_versioned_scripts.py`): no Python copy of the rule (G-P5).

**Root verifies (after merge, build tag `p9-3<letter>`):**
1. equity-dev, 0 warnings: `atx-engine`, `atx-engine-book-tests`, `atx-equity-strategy-targets`,
   `atx-impl-strategy-target-tests`; then equity-rel for the targets exe and target tests. Filters
   `atx-engine-book-tests --gtest_filter=MvTarget.*` and `atx-impl-strategy-target-tests
   --gtest_filter=MvAimRule.*:CovStore.*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:CostLawRegistry.*:TradeRateRegistry.*`,
   Debug and Release.
2. Rule-absent identity, real data, 0 trials: X-5 and Y-F0 NAV (the wave-3 P9-B0 references) on the new build: every
   NAV file byte-identical; substitution list written before the run = `summary.json` `/producer` only (C1-PROD);
   Release = Debug on X-5 (G-P3 NAV half).
3. `nav --list-rules --json` of the new exe vs the pre-merge exe: the `mv-aim-v1` row is the only difference.
4. No run of the rule on TRAIN or any real role before P9-MV is registered (COV-2): that run is the cell.
5. tiny-world canary goldens unchanged (Debug and Release).
6. Before P9-MV: the role manifest has `universe.delisting.returns_applied == false` (S9, COV-9); the lineage parent is
   re-pinned to the wave-3 P9-B0 references (W1-I2); new schema literals, if any, go to `classes.json` (SQL2-CLS).

**What the reviewer must check (beside `briefs/review-template.md`):**
1. As-of (COV-4 for this consumer): for the decision at d the rule reads `RiskStore::read(d)` and nothing else: the
   same index as `BookScaler::estimate` and spo; never d+1, never `at_or_before` a later session, never a whole-axis
   statistic of the store; sigma_i from the same row. Trace in the C3-split NAV path when the decision at d is formed
   (after d's mark) and first traded (d+1's close), and show that row d holds nothing after session d (its stamp:
   as_of = cutoff = d, K-P9-12). `.TargetAtDReadsStoreRowDOnly` has teeth.
2. Rule absent is byte-identical: no default changed, no output key unconditional, no iteration order moved; the
   `--list-rules` row is the only rule-absent change.
3. The arithmetic is the registered rule: P on both sides, alpha = sigma z, the priced set and the gross rule above,
   unpriced pass-through, no dense V, no extra constant (no ridge, cap, shrinkage or clip beyond `FactorModel::create`'s
   stated floor).
4. Determinism: fixed reduction order; the book-workers parity test is real (rule on, 1 vs 4).
5. Bounds: N and K from the slice; no u32 / usize wrap (COV R2b class; `check_model` in
   `atx-engine/src/book/risk_target.cpp:33-35` compares `covariance.size()` with a wrapping `k*k`); the rule's scratch
   is counted in the NAV `--max-bytes` admission.
6. Registration: `source_sample_end` from a cited paper; one-line `predicted_mechanism`; no house statistic; the template
   follows ALCOMB-1; id `mv-aim-v1` within W1-I1's 24 characters.
7. The lane's commands opened no NAV / IC / Sharpe output and no spo cell outcome; nothing dated 2024-01-01 or later.
8. Files outside scope; `/W4 /WX` hazards; 100 columns.

**Out of scope:** costs inside the transform (spo-v2 / spo-v3 own that); any change to the estimator, the container or
`RiskStore`; choosing the store P9-MV pins (PRE / the PM, wave3-carry Q10); running P9-MV; spec keys; Python.
