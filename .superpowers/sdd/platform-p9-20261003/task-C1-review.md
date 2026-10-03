# Lane C1 review

## Verdict
BLOCK

## Reviewed SHA
f6cd387d48e35d92597ef40ffdc86f566bb1b0a9 (feat/p9-c1-20261003, base d7c1c520). Commits: dd925b7f (sqrt),
ef75dfa0 (per-book rule, capacity lockstep, summary binding, adv-hold, nav-rules), f6cd387d (report).
Worktree C:/atx-wt/pool-15. HEAD matched and the tree was clean before and after the review.

## Evidence
1. C1 is C++ only and its report names no pytest, so I ran the nearest NAV tools pytest inside the lane's pool:
```
cd C:/atx-wt/pool-15 && PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q \
  -p no:cacheprovider atx-impl/tools/test_nav_summ.py atx-impl/tools/test_nav_summ_v8.py
....................s.........                                           [100%]
29 passed, 1 skipped in 4.86s
exit_code=0
```
`git -C C:/atx-wt/pool-15 status --short` returned 0 lines after the run.
2. I recomputed every pinned probe constant independently, exit_code=0. Method: Python binary64 with math.sqrt, in the C++
   operation order of replay_cost.cpp:92-102 and strategy_cost_v2.cpp:199-204. Every constant matched:
   - the 5 `ReplayCostSqrt.CostFractionBitsArePinnedOnFixedInputs` cases;
   - the capped cost 0x404be66666666667 and the unrationed cost 0x405bd54a245c0bbf;
   - the linear-law constant 0x3f441601c96ed8c2;
   - the 5 capacity impact_y and 4 marginal_cost_s2 constants.

   The CRT gives pow(m, .5) == sqrt(m) for m in {.5, 1, 2, 4, 8}. So the impact_y values in capacity/recipe.json
   are unchanged, as the report claims.
3. Added diff lines over 100 columns: 0 (exit_code=0), which confirms the report.

## Findings
path:line | severity | problem | required fix

- `.superpowers/sdd/platform-p9-20261003/task-C1-report.md:111` | major | §4 says "X-5 identity run (Release, at
  the merged SHA)". That contradicts brief-C1 and plan §2.4 C1, which say: "X-5's NAV re-run under the new Debug
  build ... then a Release NAV build compared to Debug bit for bit". The report's own "Release expectation"
  (:138) lists the CRT calls that remain. guarded_move's std::log runs in every book's MARK, so under Release
  even the S1 CSVs can differ from the reference. The byte-identical claims (:132-133) then cannot be checked, and
  a regression would look the same as build noise. | Restate §4 as two runs:
  (a) the new Debug (equity-dev) build against the old X-5 output, under the substitution list;
  (b) Release against that Debug output, with its own expected-difference statement (drop summary.producer).
  Say that the probe covers the sqrt path only.
- `.superpowers/sdd/platform-p9-20261003/task-C1-report.md:134` | major | The substitution list excuses whole
  files "plus whatever follows from it": the daily/events CSVs of 4 of the 5 main books, every capacity/ file,
  capacity_curve.csv and v7_transfer_coefficient.csv. The X-5 run therefore cannot detect a defect in the
  structural change on 9 of 10 books: per-book BookState/BookLeverage, the capacity lockstep, book_groups and
  the record merge. That is the DEC-20 risk of a list that hides a real change. The commit split makes a
  discriminating check free. | Add a two-stage identity:
  (1) dd925b7f (sqrt only) against the old X-5: only the files excused for sqrt may differ;
  (2) f6cd387d against the dd925b7f output: every daily/events CSV (main and capacity/), recipe.json,
  capacity/recipe.json, capacity/summary.json, capacity_curve.csv and v7_transfer_coefficient.csv are
  byte-identical; summary.json differs only at v7.extras, v7.files and producer; v7_extras.json differs only at
  files["capacity/summary.json"].
  Also name the summary.json JSON paths that sqrt may move (scenarios[S2-family], v7.books).
- `atx-impl/src/strategy_nav_v7.cpp:208` | minor | capacity_spo_v3_declaration was reworded unconditionally.
  declarations() puts it in recipe.json's v7 block (:273, via extend_recipe :940). Every spo-v3 +
  --capacity-curve run therefore changes recipe.json and its recipe_sha256 even with --adv-hold-q absent. This
  flag-absent byte change is missing from the re-pin list (X-5 is not affected). | Keep the old sentence (the
  recipe's adv_hold_rule override already states the new cap under adv-hold), or list spo-v3 capacity recipes in
  the DEC-20 re-pin list.
- `atx-impl/src/strategy_nav_replay.cpp:334` | minor | validate_nav_config checks only the law and the rule. The
  header contract (strategy_nav_replay.hpp:153) says "Not Fixed requires aim-partial-v5 and the store
  (InvalidArgument)". But a null leverage.risk passes here and is refused only at the first plan, by
  BookScaler. RiskTargetParams ranges are never validated for library callers: sigma_star 0 silently clips every
  L_t to .8 L. | Refuse `!cfg.leverage.risk`, and call `bk::validate_risk_target(cfg.leverage.params)` for
  RiskTargetV1, both in validate_nav_config. Add both refusals to
  NavBookRule.RefusedOutsideAimPartialV5AndOnTheDecidePath.
- `atx-impl/src/strategy_nav_replay.cpp:2368` | minor | With --adv-hold-q and --capacity-curve, book_groups gives
  each multiple its own sequential lockstep (main+x1, x.5, x2, x4, x8). That is 5 construction/price-exposure
  passes where the old flow ran 2. The per-group warm-start inert check can also refuse a run that the old
  capacity pass accepted (deviation 5). The report gives no wall-time estimate for adv-hold capacity cells
  against the 600 s bound. | Add the extra passes and the per-group refusal to the report's open risks, with an
  expected wall-time factor. No code change is required.
- `.superpowers/sdd/platform-p9-20261003/task-C1-report.md:147` | minor | Deviations 1-2 move exe identity from
  recipe.json to summary.json, and drop the argv SHA-256 from the published bytes. This goes against brief
  deliverable (4) and plan §2.4 C1. The reasons are sound: the decide path recomputes the recipe SHA, and the
  directory-identity tests vary argv. But no PM ruling covers the move. | Record a PM ruling in progress.md
  that accepts the move, with the argv SHA-256 going to the K-P9-10 receipt (E1).
- `atx-impl/src/strategy_risk_target.cpp:362` | minor | The K-P9-7 rows list aim_leverage as "required", yet its
  schema carries `"default": 1`. The flag is optional (TargetReplayConfig default 1.0), so a K-P9-9 validator
  built from params_schema would refuse a rule entry that omits it. | Remove aim_leverage from "required" in all
  three rows, or remove the default.

## Checked
- [x] .agents/cpp/agent.md §10 checklist applied to the diff:
  - UB and lifetime: spans and pointers into Book and NavRun outlive their use. The pointer from
    BookLeverage::plan is used before the next call.
  - Error paths: every new Result goes through ATX_TRY or ATX_TRY_VOID.
  - Bounds: the merge loops are bounded by the record totals.
  - Determinism: records are merged by (session, book index), and lanes write only book-owned state.
  - Compile scan for clang-cl 18 /W4 /permissive- /WX found no definite error:
    - ATX_TRY into existing lvalues expands to `decl = *std::move(tmp)` (error.hpp:157).
    - Observer{sink, book} field order matches.
    - ScopedNavExtension::State is public.
    - The spo::RiskStore forward declaration matches `class`.
    - build_engine_git_sha is in atx::impl, in the same library (atx-impl-core).
    - Every aggregate init is complete (NavRun 17, PlanInputs 13, TcRecord 9, BookDecision 11), so
      -Wmissing-field-initializers stays quiet.
    - The unique_ptr members point to complete types.
    - No helper or constant is left unused.
    - Every fixture helper the new tests call exists (7-arg write_risk_model, Role fields, CapBench, v61_book,
      write_run_inputs).
    - The nav_replay/cost_v2/nav_v7 TUs skip the PCH (atx-impl/CMakeLists.txt:122), and the new uses carry
      their includes.
  - The first compile at root remains the gate.
- [x] The diff stays inside the brief's files-in-scope: the 12 code/test files plus the report. There are no
  CMake, scripts/** or atx-db edits, and strategy_target_replay.cpp is untouched.
- [x] Evidence in the report matches its claims (cheap spot checks re-run):
  - 0 added lines over 100 columns;
  - every probe bit is correct;
  - no test pins a summary.json SHA. The only pinned SHAs are recipe SHAs (strategy_nav_replay_test.cpp:1645,
    1665, 1999, 2150, 2861), and these are unchanged because recipes do not move.

## Notes (verified, no finding)
- Flag-absent behaviour, with no v7 flag:
  - lockstep_scenarios returns the matrix, the rule is Fixed and make_book_state returns null.
  - plan_weights takes the old v7::plan else-branch.
  - With one variant, book_groups equals the old leverage_groups.
  - recipe.json is unchanged, and the only addition is summary.producer.
  - Under v7 without --capacity-curve, v7_extras.json is unchanged.
- Per-book L: BookLeverage::plan takes base = the book's own cfg.target.aim_leverage (risk_target.cpp:270).
  v7::plan_book takes base = p.cfg's L. configure() runs per config after the variant copy, and same_leverage
  gates grids. A capacity book's L comes from its own config. There is no path that applies main L to capacity
  books or the reverse.
- Lockstep order:
  - The main books are indices [0, capacity_begin), and run_books iterates in index order.
  - flat[group[j]] restores that order, and NavRun takes first(main_books).
  - Capacity books keep no TC or leverage records, so the merged order equals the old sequential seam.
  - The shared construction depends on the scenario only through nav_multiple, and only under the cap
    (form_desired_target:822). fill_liquidity stores window values only.
- summary.json is written last. The order is recipe, main CSVs, capacity/ (publish_capacity), the v7 files,
  v7_extras.json, then summary.json. Its v7.files binds the SHAs of v7_extras.json and capacity/summary.json.
  CapacityPublication restores the pass before the main summary is built.
- adv-hold fix: the cap NAV is nav_multiple × initial_nav. nav_multiple is 1 unless the book is a capacity book:
  --capacity-curve with a finite multiple, or the Capacity pass. A capacity id without the curve (the spo-v3
  shortcut) stays a main book. x1 joins the main group.
- --book-workers: plan_rule only reads run state and writes the BookState. The calling thread forms the decision
  liquidity (run_books:1396), and lanes only read it. RiskStore::read opens files per call. The spo engines are
  refused (shared_plan_state).
- Look-ahead: VolTarget.TruncationInvariant replaces the future after T (prices, volume, signals, store rows). It
  then checks every leverage record and daily row up to T bit for bit, under both laws. Seal: no new date handling.
- --max-bytes 1 GiB: the reserve now charges 10 books instead of 5. Each extra book adds about 22 MB:
  1 MiB + 192 B × names + days + 262,144 × sizeof(NavEvent). The X-5 receipt (open) shows sampled peak RSS
  585 MiB, a 1536 MiB / 180 s limit and 146 s wall. The headroom is plausible; root reads the admission line
  first.
- Blindness: I opened only those receipt fields plus its executable path and SHA. I opened no NAV, return or IC
  output. The lane's report shows only git commands.
