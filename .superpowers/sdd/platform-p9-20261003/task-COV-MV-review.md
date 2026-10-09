# Lane COV-MV: first review (RESUME 9)

- **Verdict: APPROVE**
- Lane `COV-MV`, pool-31, branch `feat/p9-covmv2-20261007`. HEAD `bda51b98` (records); code head `846739c1`.
- BASE `685e238b` = `git merge-base bda51b98 feat/platform-p9-20261003` (REV-BASE). It is root's head today.
- Reviewed: `git diff 685e238b bda51b98`. 18 files, +2364 / -20, `.superpowers/` excluded.
- The lane's files are byte-equal at `846739c1` and `bda51b98` (`git diff 846739c1 bda51b98 -- <the 18 files>` is
  empty). All three root merges in the range are plain: `10259f38`, `db33c6ca` and `a8baba2a` each have the tree
  that `git merge-tree --write-tree ^1 ^2` recomputes.
- Contract: `r7-review-dispatch.md`, `resume8-addendum.md`, `resume9-addendum.md` (TEST-1), `r8-review-notes.md`
  "COV-MV", `briefs/brief-COV-MV.md`, and the "COV-MV" sections of `r7-dispatch.md`, `r8-dispatch.md` and
  `r9-dispatch.md`.
- Rulings grepped: COVMV-1 (`status-8.md:159`), COV-4, COV-5, COV-11, COV-13, COV-14, PRE-9, W3-9, W3-17, CRT-2,
  REV-BASE, TEST-1, TEETH-RESTORE, TEETH-POOL, EXE-CUR, SPDLOG-RELINK, PERM-1.
- Pool-24 was not read for evidence and nothing was written there.

## 1. Spec verdict, item by item

### `r7-dispatch.md` COV-MV

| # | item | verdict | evidence |
|---|---|---|---|
| 1 | merge root | done | `10259f38` (`1b64ce45`), `db33c6ca` (`fd084e78`), `a8baba2a` (`685e238b`). All plain (above). |
| 2 | items 1-8 as designed | done | Section 2 below, item by item. |
| 3 | synthetic only; the store is never opened; the pin is named | done | The only matches for `p9-m2i-dcov-covv1` in the lane's logs are the path string printed by `validate_specs.py`. Neither pool-24 nor pool-31 has such a directory. Every test builds its store with `write_risk_model` / `add_container`. |
| 4 | registration and template | done | `scripts/specs/p9/rules/mv-aim-v1.json` and `scripts/specs/p9/templates/mv-aim.json`. Section 4 covers K-P9-11, `source_sample_end` (verified by me), the pin and ALCOMB-1. |
| 5 | W3-9 keep-both with AL-CLOCK | pending by rule | AL-CLOCK is not in root at `685e238b`. The keep-both is root's (section 7). |
| 6 | Debug, then Release, named, whole, canary | Debug done; the rest pending by rule | Debug `p9-covmv-f` on `a8baba2a`: MvTarget 11/11, MvAimRule 15/15, named filter 99/99. Release, the whole target-tests on the merged head and both canaries are deferred to the gate (TEST-1). |
| 7 | report with "For PRE" and the COV-4 trace | done | `task-COV-MV-report.md`, sections "For PRE: the P9-MV block" and "What the reviewer traces for COV-4". |

### `r8-dispatch.md` COV-MV

| # | item | verdict |
|---|---|---|
| 1 | merge, configure, implement items 1-8, one commit per group, tags | done. Commits `756035ed`, `f6b05cbd`, `26b67bd5`, `d44e05f5`, `628a9ffb`, `846739c1`; builds `p9-covmv-a` .. `-f`. |
| 2 | registration and template | done (section 4). |
| 3 | W3-9 | pending by rule (AL-CLOCK is not in root). |
| 4 | tests | `MvTarget.*`, `MvAimRule.*`, the refusal teeth (`p9-covmv-e`: 3/3 fail with the refusals off), the `Cov*` suites (inside the named 99), the in-binary flag-absent tests: done. Whole target-tests: 405/405 at `846739c1` before the merge; on the merged head, deferred (TEST-1). Canary: deferred (TEST-1). `scripts/tests` and the stage chain: not needed, no Python changed. |
| 5 | report; delete the object trees | done. When I started, pool-31 `build-equity/` held only the tracked `audits/` and `v8-interim3-pitch-render-run2/` plus the `p9-covmv-f` receipt and logs. |

### `r9-dispatch.md` COV-MV

| # | item | verdict |
|---|---|---|
| 1 | handoff note "continued in pool-31" | done (`6eea0c54`). |
| 2 | Debug `p9-covmv-f`; Release `p9-covmv-g`; merge root `685e238b`; report; delete trees | Debug done; Release deferred (TEST-1); merge done (`a8baba2a`); report done (`bda51b98`); trees deleted. |

### Brief, "What the reviewer must check"

| # | check | verdict |
|---|---|---|
| 1 | as-of (COV-4) | holds: section 3 (b). |
| 2 | rule absent = byte-identical | holds by code reading and the in-binary tests. Root's real-data X-5 / Y-F0 identity is deferred to the gate. |
| 3 | the arithmetic is the registered rule | holds. P is applied on both sides (`mv_target.cpp:185-190`, `:238-243`). alpha = sigma z (`:181`). The priced set is `strategy_mv_aim.cpp:150-166`. The gross rule is `:181-183` (W3-17). Unpriced names pass through. No dense V, no constant, no D floor. |
| 4 | determinism | holds. Fixed loop order, one call per decision on the calling thread. `BookWorkersParity` is real: rule on, `--capacity-curve`, `--book-workers` 1 (the default) vs 4, every file byte-compared. |
| 5 | bounds and no wrap | holds. `check_view` multiplies with checks (`mv_target.cpp:51-75`): K, K x K, n x styles. `mv_aim_working_bytes` and `workspace_bytes` saturate. `factor_variance` is not reused (its `k*k` wrap is avoided). The scratch is counted in `--max-bytes` through the COVMV-1 term. |
| 6 | registration | holds (section 4). |
| 7 | no NAV / IC / Sharpe output opened; nothing dated 2024-01-01 or later | holds. The logs hold synthetic gtest output, a `--list-rules` table and the spec validator only. |
| 8 | scope, `/W4 /WX`, 100 columns | holds. COVMV-1 below. 0 first-party warnings (`p9-covmv-f` log; my build `p9-covmv-h`: 0 first-party). No added line over 100 columns (awk over every `+` line of the diff, JSON excluded). |

### Rulings

- **COVMV-1: holds.** Exactly two edits are outside the brief's list.
  - The new impl file `strategy_mv_aim.{hpp,cpp}`, with its `target_sources` line as the last line of
    `atx-impl/CMakeLists.txt` (:302-304).
  - One added term, `+ v7::extension_workspace_bytes(names)`, in `nav_workspace_reserve_bytes`
    (`book/nav_lockstep.cpp:74-75`, +2 / -1), with a flag-absent test
    (`MvAimRule.WorkspaceReserveTermOnlyWithTheRule`: no extension, an extension without the rule, and after the
    scope ends, the reserve is unchanged; with the rule it is `+ workspace_bytes(names)`).
  - Every other touched file is named by the brief, r7 item 2 or the R7 design that r8 fixed:
    - the kernel, its test and `atx-engine/CMakeLists.txt:174-175`;
    - the impl test and the `atx-impl/tests/CMakeLists.txt` tail;
    - the registry row in `nav_rules.cpp`, plus the row's comment in `nav_rules.hpp:16-17`;
    - the hook in `nav_decide.cpp:42-48`;
    - the parser entry, refusals and summary glue in `strategy_nav_v7.{hpp,cpp}`;
    - the COV-11 control in `strategy_cov_model_test.cpp:647-657`;
    - the registration and the template.
  - No forbidden file is touched: COV's container, risk model and RiskStore code, `eigen_adjust.*`,
    `factor_model.*`, `vcpkg.json`, `CMakePresets.json` and `scripts/*.py` are all unchanged.
- **COV-11: holds.**
  - The a = 2 control sits beside the a = .5 control, with the same band test
    (`strategy_cov_model_test.cpp:647-657`). In `f-named-debug.log` (planted synthetic panel) a = 2 prints
    eigen-00..02 = .909 / .890 / .938 and fails the band, as required.
  - The per-row group check: every slot of row d must be < 50 or 255, else the run is refused, naming the row, the
    instrument and the slot (`strategy_mv_aim.cpp:132-140`). Test: `MvAimRule.RefusesAnIndustrySlotOutsideTheTable`
    (Rule level and CLI; 255 is accepted as unpriced).
- **COV-13 (c): holds** (trace in section 3 (a)). F is never inverted. The form is defined for a singular F. Test:
  `MvTarget.RankDeficientFEqualsDenseSolve` with F = v v' (rank 1 of 6, an exactly zero row and column), and F = 0.
  The target is finite and equals Eigen's dense `partialPivLu` solve of V = X F X' + D to 1e-12.
- **COV-14: holds.** The P9-MV summary prints `eigen_00_02_share {mv, desired}` (`strategy_mv_aim.cpp:212-220`,
  `:258-264`): the share of e' F_d e on the three smallest eigenvalues of F_d's forecast block, ascending (rank 00
  = the smallest, `symmetric_eig` returns ascending values), e = X'w. It is print only and never in the target.
- **PRE-9: holds.**
  - The row's `params_schema` is closed, with `risk_model` (`x-flag --risk-model`) and `risk_model_sha256`
    (`x-flag --risk-model-sha256`, pattern `^[0-9a-f]{64}$`), both required. This is risk-target's
    `store_properties` shape (`nav_rules.cpp:100-105`).
  - The parser refuses the rule without both flags (`strategy_nav_v7.cpp:1296-1298`; `RefusesWithoutAStore`).
  - E2 derives exactly the three flags from the row (my run of `validate_specs.py`, below).
- **W3-17: holds** (`strategy_mv_aim.cpp:181-183`; `UnpricedMembersPassThrough`): priced gross = g, priced net =
  0, book net = the unpriced members' net.
- **PRE-24a: holds.** `specific_vol_ratio {pooled, per_decision}` is printed.
- **W3-10 / PRE-29: holds.** The pin is `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3` in the
  registration's `store.manifest_sha256`, in `manifest_fragment.rules[0].params.risk_model_sha256` and in the
  template text. It equals the dispatch's SHA.
- **CRT-2 / COV-5: holds.** The lane only reads stores (`RiskStore::open` / `read`). It writes and rebuilds none
  and moves no `atx-cov-v1` parameter.
- **SQL2-CLS: holds.** No new schema literal. The four `atx.<name>/v<n>` literals on `+` lines
  (`atx.nav-construction-grid/v1`, `atx.p9-rule-registration/v1`, `atx.research-cycle-template/v1`,
  `atx.research-wave/v2`) are all in `atx-engine/schemas/research_store/classes.json` at `685e238b`. The summary
  key `mv_aim` is a block, not a schema.

## 2. Items 1-8 (the handoff's design)

| item | verdict | where |
|---|---|---|
| 1 kernel | done | `atx-engine/include/atx/engine/book/mv_target.hpp`, `atx-engine/src/book/mv_target.cpp`. The kernel knows nothing of the NAV loop: it depends only on `FactorRiskView` and core error / types / safe_math, plus linalg in the print helper. |
| 2 COV-13 (c) form and its test | done | section 3 (a). |
| 3 row (PRE-9), append-only | done | `nav_rules.cpp:97-118`. The size-deduced `targets[]`; `rules_json` wraps it as a span (:191). `ListRulesRowIsAClosedTargetRow` checks the row is the last row and the only target row. |
| 4 parser entry, hook, refusals | done | Handler `strategy_nav_v7.cpp:1146-1149`; refusals :1271-1299; store flags allowed :1303; store opened only with spo, risk-target or this rule :1437; hook `nav_decide.cpp:42-48`. |
| 5 print-only `mv_aim` block | done | `strategy_mv_aim.cpp:197-269`. `add_mv_aim` (`strategy_nav_v7.cpp:323-328`) is called from summarize (:607, counts on the main pass only), the recipe (:1001) and the holdings manifest (:1029). It returns first thing without the rule. |
| 6 flag-absent identity; workspace term | done | `apply_target_rule` (:948-952) returns Ok before touching `desired` when no rule is installed; `extension_workspace_bytes` (:955-958) is then 0. Tests: `FlagAbsentIdentity`, `WorkspaceReserveTermOnlyWithTheRule`, and `SummaryBlockOnlyWhenOn` (no byte of an off run names `mv_aim` / `mv-aim-v1`). The `form_desired_target` rewrite (`ATX_TRY`, then `Ok(proceed)`) has the old semantics. |
| 7 tests on synthetic containers | done | `MvAimRule.ContainerAndDirectoryStoresGiveTheSameBook`: every file byte-equal except the store pin and the `recipe_sha256` that binds it. |
| 8 registration and template | done | section 4. |

Optional items done: the warm-up count, a decision counted once across passes, the special-tier negative count,
and `--calibrate-gross` compatibility.

Counting once is real. `fresh = !formed_ || d > last_` (`strategy_mv_aim.cpp:111-115`). Pass 1 of
`--calibrate-gross` counts. A published pass that restores the tape never calls the rule. A pass that forms the
constructions again (`construction_reusable` false) reaches only d <= last_, so it rewrites the target identically
and counts nothing. `begin_run` is called once per run (`strategy_nav_v7.cpp:1453`); `CapacityPublication` does not
reset the counts. Tests: `DecisionCountedOnceWarmUpAndLocateNegatives` and `CalibrateGrossCountsEachDecisionOnce`
(the published `mv_aim` block equals the plain run's).

## 3. Traces

### (a) COV-13 (c): every solve in the kernel (`mv_target.cpp:155-250`)

1. `check_view` (:60-75) checks the shape with checked products and refuses a non-finite exposure or covariance
   entry. `RiskStore::read` already maps a non-finite F entry, and the whole row of a factor without a forecast, to
   0 (`strategy_spo.cpp:822`). So an F with an exactly singular block is the normal case, not an edge.
2. Active = finite score (NaN = no opinion). a = sigma x score, then demeaned: P alpha (:177-190). An active name
   needs a finite sigma > 0 and a finite D > 0 (:169-170). The impl passes only priced names, so this always holds.
3. u = D^-1 a. G = X' D^-1 X and r = X' u are accumulated per name over its nonzero columns: intercept, group,
   styles (:194-212). No m x m matrix exists.
4. M = I + F G and rhs = F r (:213-226). The only products with F are F G and F r. F is never factored or
   inverted, and `risk::FactorModel` is not used.
5. `solve_pivoted(M, rhs)` (:98-126): Gaussian elimination with partial pivoting (first maximum, fixed order) on
   I + F G, never on F.
   - For a PSD F the nonzero eigenvalues of F G are those of F^1/2 G F^1/2 >= 0, so every eigenvalue of I + F G
     is >= 1 even when F is singular. (The header's word "similar" is loose when F is singular; the conclusion
     holds.)
   - A zero or non-finite pivot refuses with InvalidArgument ("F not PSD"); test: F = -1/4, G = 4.
   - A non-finite solution is refused.
6. V^-1 P alpha = u - D^-1 X y (:229-237). This is push-through Woodbury. I checked it by hand:
   (X F X' + D)(D^-1 - D^-1 X (I + F G)^-1 F X' D^-1) = I + X F X' D^-1 - X (I + F G)(I + F G)^-1 F X' D^-1 = I.
   No step assumes F is invertible.
7. P again, then the gross normalisation (:238-248). `out` is written only after every check passes;
   `RefusalsAndUnavailable` checks that `out` is untouched on every refusal.
8. Rank-deficient test: F = v v' with dyadic v, `v[2] = 0`, rank 1 of K = 6. The target is finite and equal to
   the dense solve to 1e-12; F = 0 is also checked. A form that inverted F (`FactorModel::create` needs F SPD)
   would refuse or blow up here.
9. Cost per decision is O(m (2 + styles)^2) for G (about 0.95 M multiply-adds at m = 5,600 and 13 columns), K^3
   for I + F G and about K^3 / 3 for the elimination (K = 62: 0.24 M and 0.08 M). About 1.3 M flops in all,
   stated in the header (:26-27).
10. The print helper `factor_eigen` (Eigen `SelfAdjointEigenSolver` on F's forecast block) is diagnostics only.
    It never feeds the target.

### (b) COV-4: the store row read at decision d

1. `book/nav_session.cpp:324`: session t runs MARK, then EXECUTE (`t > start && t + 2 <= end`: orders decided at
   t - 1 fill at t's close), then DECIDE (`t + 2 < end`). This matches the timing contract
   `strategy_nav_replay.hpp:23-27`: decide at d after its mark, fill at d + 1's close, first return row d + 2.
2. `nav_session.cpp:348` calls `decide_construction(x, ..., t, ...)` on the calling thread, outside both
   `for_each_lane` phases. `nav_decide.cpp:129` calls `form_desired_target(x, cfg, d = t)`. `detail::form_desired`
   reads rows <= t. Then `nav_decide.cpp:47` calls `v7::apply_target_rule(x, t, ...)`, which runs only when the
   target proceeds.
3. `strategy_mv_aim.cpp:108`: `check_axes(x.session_keys, n)`. The store's session axis must equal the replay's,
   element by element (`strategy_spo.cpp:774-780`), so store index d is replay session d.
4. `strategy_mv_aim.cpp:127`: `store_->read(d, slice_)`, the only store access in the rule.
   - The legacy path reads at byte offset `date * cells`.
   - The container path reads block d and refuses unless `view.as_of == sessions_[d]` (`strategy_spo.cpp:804`).
   - A missing forecast at d is Unavailable (:786); the rule passes the target through and counts it as
     `no_forecast`.
   - Nothing else is read: no d + 1, no `at_or_before`, no statistic over the whole axis. sigma_i = sqrt(D_i)
     comes from the same slice (:164). This is the index `BookScaler::estimate` (`strategy_risk_target.cpp:141`)
     and spo read.
5. Row d's stamp is as_of = cutoff = d (K-P9-12; COV-4: block t uses sessions <= t; COV's review traced the
   estimator). So row d holds nothing after session d. The rule's output is first traded at d + 1's close.
6. `nav_session.cpp:350`: `record_construction` records `shared.desired` after the hook. A restored calibrate pass
   (`restore_construction`, :191-226) copies the post-rule target back, so it replays the same book.
7. Tests:
   - `TargetAtDReadsStoreRowDOnly`: every specific variance of the rows after T is perturbed, and every target at
     d <= T, and every NAV daily row through T, stays bit-equal. Teeth: perturbing row T itself moves the target at
     T, and a later NAV row moves.
   - `FutureReturnDoesNotChangePast`: the role and the store are cut after t, so the cut store has no later row at
     all (covariance and styles included). The targets at d <= t and the rows through t - 2 are bit-equal. The two
     together cover every field of the later rows.

## 4. Registration and template

- K-P9-11 keys are present. `predicted_mechanism` is the brief's sentence; `data_class` is `H`; `constants` is
  `[]`. The claim quotes only the paper's own figures (1994-2010); there is no house statistic.
- **`source_sample_end` = 2010: verified by me** against the primary source, source [2], the PDF at the URL the
  registration cites.
  - I extracted its text with pypdf. It reads: "we select the 50 largest US stocks as of July 30, 2010 that have
    full daily return histories dating back to July 1, 1993 (a total of 4,303 trading days)", and "the testing
    window is 4,103 trading days (from April 15, 1994 to July 30, 2010)".
  - The claim's "13.98 vs 14.64 percent" is also in the text (section "Out-of-Sample Volatilities").
  - The year is before 2015, so it is not flagged under PRE-19.
- The pin equals the required SHA (section 1). The registration pins the template's SHA-256 `83391f2c...d870`.
  That is the file's SHA at `bda51b98` (LF, 0 CR bytes); the registration's own SHA is `4b4af6e2...80b4`. The
  lane's report matches both.
- ALCOMB-1 template shape: the same key set as AL-COMB's `mom-volman.json`. NAV-only: `change.set` renames only
  `nav.output` and `monitor.output`. No `rules` key and no `change.flags`. `parent: null`; the nominal parent is
  Y-F0. `gross_match: "pm6-6-engine"` is in the manifest fragment, where AL-COMB's shape keeps the cell's keys.
- The id `mv-aim-v1` (9 characters) and the template name `p9-mv-aim` (9) are both within the limits.
- My own run of `validate_specs.py` (in pool-31, against the lane's list-rules JSON) exited 0:
  - `rule_cell_problems: []`, `registration_problems: []`;
  - `load_spec: p9-mv-aim`;
  - derived NAV flags exactly `--risk-model`, `--risk-model-sha256 ea58691f...`, `--target-rule mv-aim-v1`.

## 5. Teeth (TEST-1: one C++ teeth build; TEETH-RESTORE)

Tool: my own script in the session scratchpad, `covmv_review_teeth.py`, run once. It is not committed; its log
lines are below.

- **Mutation** (code under test, not a test). `atx-impl/src/strategy_nav_v7.cpp:1283`: `if (o.aim_v6)` became
  `if (o.aim_v6 && false)`, which removes the mv-aim-v1 refusal of aim-partial-v6.
- **Why this guard.** The token scan rewrites `--rule aim-partial-v6` to `--rule aim-partial-v5`, so the later
  "needs --rule aim-partial-v5" check cannot catch it. This single guard is load-bearing. The lane's teeth
  disabled the whole block at once; this mutation tests the one guard on its own.
- **Procedure (TEETH-RESTORE).**
  - Before: `git status --porcelain` empty; `git hash-object` = HEAD's blob `71855f9a`; raw SHA-256 `5a5d18b4...2938`.
  - The tool kept the original bytes in memory and in a backup copy, then waited for an idle host (HOST-3).
  - It mutated at 19:54:44, built, and restored in a `finally` block at 20:00:36.
  - After: raw SHA-256 equal; `hash-object` = HEAD's blob; porcelain empty.
  - It asserted the same again after the tests (20:00:50). No `git checkout` / `restore` / `stash`.
- **Build.** `scripts/research-build.ps1 -Tag p9-covmv-h -Targets
  atx-impl-strategy-target-tests,atx-engine-book-tests` (equity-dev). This followed a fresh
  `powershell scriptstx-build.ps1 configure -Preset equity-dev` in pool-31.
  - Receipt `build-equity/mega-p9-covmv-h-receipt.json`: Source `bda51b98`, DirtyEntries 1 (the mutation), exit 0,
    378 TUs, 349 s.
  - Exes, re-hashed against the receipt (EXE-CUR, equal): `atx-impl-strategy-target-tests.exe` `13846cb9...685c`,
    `atx-engine-book-tests.exe` `6c81b489...066d`.
  - 0 first-party warnings. The log's 11 warning lines are clang-cl's `/MP` driver note and databento-cpp's two
    `getenv` deprecations, all third-party or driver.
- **Result: CAUGHT.**
  - `atx-impl-strategy-target-tests.exe --gtest_filter=MvAimRule.*` -> exit 1, `[  PASSED  ] 14 tests`,
    `[  FAILED  ] MvAimRule.RefusesIncompatibleRules`. Its 4 failed assertions are all the aim-partial-v6 case:
    - `strategy_mv_aim_rule_test.cpp:313`: exit 0 instead of 2, so the run published;
    - :314 and :315: no refusal message;
    - :317: the output directory exists.
  - Every other refusal case of the test still passed, so the mutation is isolated.
  - `atx-engine-book-tests.exe --gtest_filter=MvTarget.*` -> exit 0, `[  PASSED  ] 11 tests`. The kernel is
    unaffected by the impl mutation, so this is a clean-equivalent kernel run.
- **Object tree** deleted by name afterwards. `build-equity/` keeps only the tracked `audits/`,
  `v8-interim3-pitch-render-run2/` and the `mega-p9-covmv-f-*` / `mega-p9-covmv-h-*` receipts and logs. Freed
  about 2.8 GB (C: 48.4 -> 51.2 GB free).

The lane's own teeth (`covmv-logs/e-teeth-refusals.log`, build `p9-covmv-e`, receipt DirtyEntries 2): with the
whole mv-aim-v1 refusal block and the S11 check disabled at once, `MvAimRule.Refuses*` gave 0 passed and 3 FAILED
(35 failed assertions). That build was in pool-24, before the freeze; it is not re-run.

## 6. Quality verdict: good

- **Modular.**
  - The kernel is one engine unit with no NAV-loop knowledge (spans in, `Result` out, caller-owned scratch).
  - `Rule` (impl) is the only part that knows the store's layout (50 slots, 11 styles, `1 + slot` groups, the
    same priced predicate as `BookScaler`).
  - The hook is thin: two lines in `nav_decide.cpp` and a three-line forwarder in v7.
  - The v7 glue follows the file's own `add_risk_target` pattern.
- **Performant.**
  - The kernel allocates nothing after the first call (exact `reserve`, bounded by `mv_aim_working_bytes`; tested).
  - The impl's per-instrument vectors are reserved once; `out_` and `weights_` are re-assigned within capacity.
  - Two allocations remain per decision. `RiskStore::read`'s `narrow` buffer is COV's code. The eigen print is S1.
  - The solve's cost is stated (section 3 (a) 9).
- **`/W4 /WX`.** 0 first-party warnings on `p9-covmv-f`; 0 on my `p9-covmv-h`. No sign-conversion or
  shadowing hazard found by reading. `static_cast` is used only at Eigen index and ptrdiff boundaries.
- **100 columns.** Held.
- **Determinism.** No RNG, clock, map or thread in the rule; fixed reduction order. The 1-vs-4 workers byte test
  runs with the rule on.
- **Errors.** Every refusal names the rule; the output is untouched on refusal. Pass-throughs are counted and
  never silent.
- **Lifetime.** `FactorRiskView` spans borrow members of `Rule` for the length of a call only. The store is held
  by `shared_ptr<const>`.
- **Tests test the contract.** They compare against a dense reference (Eigen LU on the formed V), a hand closed
  form, bit-equality against the model without the NaN names, real 1-vs-4 bytes and real truncation. No test pins
  the implementation's own numbers.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `atx-impl/src/strategy_mv_aim.cpp:215`; `atx-engine/src/book/mv_target.cpp:286-290` | Suggested | The eigen print allocates an Eigen `MatX` block and a `SelfAdjointEigenSolver` (plus its result) on every applied, scored decision: heap traffic outside the reserved scratch, in the decision loop. The bytes are counted in `workspace_bytes`, so `--max-bytes` admission is honest. It is also the dominant cost per decision in Debug (the lane says so). | Keep a solver and a K x K block in `FactorEigen`, sized once (Eigen's `compute` reuses storage), or accept it and state it in the header's allocation note. |
| `atx-impl/src/strategy_mv_aim.cpp:215` | Suggested | A print-only figure can stop a cell. `factor_eigen` returns InvalidArgument when F_d's forecast block is asymmetric beyond 1e-9 relative, and Internal on non-convergence, and `record` propagates it. Loud by design (report "Deviations"), and the risk is small for a symmetric estimator. | PM's choice: keep it (a store defect is loud), or record NaN and count the decisions. Either way, the registration's `fixed_choices` should say so. |
| `atx-impl/src/strategy_nav_v7.hpp:117` | Suggested | The v7 header includes `strategy_mv_aim.hpp`, so every includer gets the engine kernel header. The header needs only `mv_aim::Rule` as an incomplete type (the accessor returns `const mv_aim::Rule*`; `State` lives in the .cpp). | Forward-declare `namespace mv_aim { class Rule; }` and include the header in `strategy_nav_v7.cpp` (house style section 6). |
| `atx-impl/src/strategy_mv_aim.cpp:101-195` | Suggested | `Rule::apply` is about 95 lines (house style section 3 aims at about 60). It mixes geometry, counting, the S11 check, building the priced view, the kernel call and the gross rescale. | Extract `priced_view(x, d, desired)` (the loop at :141-166) and the count updates. |
| `atx-impl/src/book/nav_rules.cpp:106-108` | Suggested | `incompatible[]` names `two-speed-nav-v1` (the NAV row id), while `per-name-v1`'s list names the composition rule `two-speed-v1`. `adv-hold-v1` and `hold-band-v1` are not rows of the table. No consumer reads the NAV `incompatible[]` today (grep of `scripts/`), and the exe refuses the flags itself (tested), so nothing breaks. | Pick one convention for the registry (rule names, as `rules:` blocks name them) and state it in `nav_rules.hpp`; P10 is fine. |
| `atx-impl/src/strategy_mv_aim.cpp:108` | Suggested | `check_axes` compares the whole session axis on every decision: O(dates) per decision. `BookScaler` checks once (`axes_checked_`). Negligible, but different from the R7 design's "once per input". | Check once per `begin_run` / input pointer, or keep it and leave it (cheap). |
| `task-COV-MV-report.md` (R9 evidence) | Suggested | SPDLOG-RELINK: the report says the exe SHA-256s were re-checked against the `p9-covmv-f` receipt before the runs, but no log records that check, and the exes are deleted. The receipt SHAs match the report's prefixes (`25abbffc...`, `9e38654c...`; the receipt copy is byte-equal to `build-equity/mega-p9-covmv-f-receipt.json`). | Next time, log the file hash beside each gtest run. |

No Required finding.

Design notes for the PM (not findings; the registered form, read before the cell):

- **The parent's neutralisation is not preserved.** The rewrite of the priced block replaces the parent's
  price-risk-v1-neutralised desired weights with P V^-1 P alpha. V's factors penalise exposure but do not
  neutralise it.
- **Locate-in-aim can be undone.** Under `--locate-in-aim`, names that `form_desired` zeroed can be put short
  again. This is counted (`special_tier_negative`), and the books' locate block applies at the plan.
- **AL-CLOCK combination.** `mv-aim-v1` with AL-CLOCK's `info-clock-rate-v1` is neither refused nor tested
  together. P9-MV's parent does not use that rate.
- **The decide path.** `strategy_live` installs no extension. A deploy manifest's `nav.rule` accepts only plain
  ids, so `aim-partial-v5+mv-aim-v1` is refused (loud). A hand-written `aim-partial-v5` would drop the rule: a P10
  live-parity note.

## 7. What root must check at merge

1. **Merge order (W3-9).** AL-CLOCK merges first, then `feat/p9-covmv2-20261007`. I ran
   `git merge-tree --write-tree --name-only bda51b98 feat/p9-alclock2-20261007` (`7ebd41ff`).
   - ONE textual conflict, at the tail of `atx-impl/tests/CMakeLists.txt`. Keep both: AL-CLOCK's block first (its
     blank line, its comment, `target_sources(atx-impl-strategy-target-tests PRIVATE
     strategy_nav_info_clock_test.cpp)`), then COV-MV's (`strategy_mv_aim_rule_test.cpp`).
   - These auto-merge: `nav_decide.cpp`, `nav_lockstep.cpp`, `nav_rules.{cpp,hpp}`, `strategy_nav_v7.cpp`.
2. **`nav_workspace_reserve_bytes`** (`book/nav_lockstep.cpp`).
   - The auto-merged body holds both terms: AL-CLOCK's per-name `clock` term inside the names product, and
     COV-MV's trailing `+ v7::extension_workspace_bytes(names)`.
   - Check that the body has exactly those two additions over `685e238b`.
   - Re-run `MvAimRule.WorkspaceReserveTermOnlyWithTheRule` and AL-CLOCK's reserve test on the merged build.
3. **The rule table.** The merged exe's `nav --list-rules --json`, against the pre-merge exe:
   - only `info-clock-rate-v1` (trade-rate) and `mv-aim-v1` (target) are added;
   - `mv-aim-v1` is the last row and the only target row (`ListRulesRowIsAClosedTargetRow` asserts this).
4. **SQL2-CLS.** COV-MV adds no new `atx.<name>/v<n>` literal (the four in its diff are in `classes.json`
   already). Nothing to register.
5. **1 vs 4 workers with the rule on.** `MvAimRule.BookWorkersParity` is in ROOT-1a's filter. The gold-run 1/4
   workers check is the gate's (TEST-1).
6. **The pin is unchanged:** `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`, in
   `mv-aim-v1.json` (`store`, `manifest_fragment.rules[0].params`) and in the template text.
   - Registration SHA-256: `4b4af6e238b529d5d7eabccd2699022dc653da52fa45801eb3f61896bd6680b4`.
   - Template SHA-256: `83391f2c526b28008d48e250f0e7d26e7c3f8661786d43a8f3d76439f664d870`.
   - When root sets the template's `parent`, `template_sha256` is re-pinned, and so the registration's SHA
     changes. Both go into the prereg (PRE-29).
7. **ROOT-1a at the merge.**
   - A Debug build of `atx-engine-book-tests` and `atx-impl-strategy-target-tests`.
   - `MvTarget.*`; `MvAimRule.*`; the lane's named filter
     `Cov*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavAnnMeanGross.*`;
     AL-CLOCK's `InfoClock*`.
   - Deferred to the gate (TEST-1): Release, the whole target-tests, both canaries, the X-5 / Y-F0 rule-absent
     identity (`summary.json /producer` substituted), and the `--list-rules` diff on root's own exe.
8. **The merged branch.** Root merges `feat/p9-covmv2-20261007` (pool-31), never pool-24's branch. The two
   `// TEETH` lines in frozen pool-24 are the owner's.

## Evidence: my own runs vs the lane's logs

- **Mine, in pool-31.**
  - `validate_specs.py` against `covmv-logs/d-nav-list-rules.json`: exit 0 (section 4).
  - T1's guards, `scripts/tests/test_no_python_mirror.py` and `test_no_versioned_scripts.py`, with
    `-p no:cacheprovider`, my own `--basetemp` and `PYTHONDONTWRITEBYTECODE=1`: exit 0, 11 passed.
  - The `source_sample_end` check against the primary source's text.
  - `git merge-tree` against AL-CLOCK's head; the plain-merge checks; the 100-column scan; the SQL2-CLS grep.
  - The teeth build `p9-covmv-h` and its gtests (section 5).
- **From the lane's logs only.** Clean-tree `MvAimRule.*` 15/15, `MvTarget.*` 11/11 and the named filter 99/99
  come from `covmv-logs/f-*`. The build is `p9-covmv-f` on `a8baba2a`; receipt `f-receipt.json`, test exe
  `25abbffc...`, book-tests `9e38654c...`, DirtyEntries 0. The whole target-tests 405/405 come from
  `d-whole-target-tests-debug.log` (`846739c1`, before the root merges).
- **Blind.** I opened no NAV / IC / Sharpe output, no 2020-2023 statistic, nothing dated 2024-01-01 or later, and
  no byte of the pinned store.
