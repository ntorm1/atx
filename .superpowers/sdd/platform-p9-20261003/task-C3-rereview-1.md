# Task re-review 1: lane C3, part A, fix round 1 (P9 wave 3)

Reviewer: fresh independent delta re-review (Opus 5.5), read-only on code. Pool-15, branch `feat/p9-c3-20261004`,
head `7345e057`. Scope `99b676f7..7345e057`: code commit `9a7c490f` (12 files under `atx-impl/`), docs `5911a63e` and
`7345e057` (`git diff 9a7c490f 7345e057` touches no code). The parts the first review (`99b676f7`) approved are not
re-reviewed except where this diff touches them.

Contract read: `resume4-dispatch.md`, `resume6-addendum.md`, `wave2-review-dispatch.md`, `briefs/review-template.md`,
`.agents/cpp/agent.md` section 10, `task-C3-review.md`, `task-C3-report.md` "Fix round 1", `handoff-C3.md`,
`c3-logs/fix1-*`, ledger lines C3-1..C3-6, PRE-7, PRE-7a, OD5-7, E2-8, ALCOMB-9. No build (the lane's object trees are
deleted). No pytest: the lane changed no Python, and its tiny-world driver needs exes that no longer exist; I only
parsed `c3-logs/fix1-nav-identity.py` with `ast` (no file written): it parses. No data opened; no 2020-2023 NAV dir or
statistic read (the only numbers seen are the synthetic gtest-fixture / tiny-world values inside the lane's logs).
No file mutated for teeth: two mutations repeated by reading. No permission refusal.

## Verdict: APPROVE

R-1 and R-2 are fixed as the first review asked, each with a test that fails with its check removed. C3-3 adds the
alias key to `cost-sqrt-v2` only; C1's rows cannot move (built by the untouched `risk_target`), and no committed
capture of `nav --list-rules --json` exists to move. PRE-7 / PRE-7a: `--ann-mean-gross` / `options.ann_mean_gross` is
a print-only opt-in; from the code I find no path by which a flag-absent byte of a NAV run directory or its console
moves; the value is defined, stated, and is what `summarize_nav` sums. No golden, digest or pinned value was edited;
the only relaxed assertions are the ones S-1 asked for. Counts in the report match the logs. Nothing Required; six
Suggested (two need a PM ruling, none blocks the merge). The flag-absent before / after comparison is root's at merge
(ruling C3-6).

## Required

None.

## Suggested

| id | where | problem | fix |
|---|---|---|---|
| S-9 | `atx-impl/src/book/nav_run.cpp:127` (`read_grid`, v8 code, not in this diff) | The construction grid file is parsed with `Json::parse(text, nullptr, false)`: last value wins, the R-1 hole one file over. `{"flags": {"--aim-leverage": "1.2", "--aim-leverage": "1.9"}}` runs at 1.9 while a reader of the file sees 1.2 first, and `grid_manifest.json` `grid_sha256` pins bytes the exe reads differently. A NavSpec names such a file through `construction_grid`. | Reuse `parse_spec`'s callback (one key set per open object) in `read_grid`; refusal only, no flag-absent byte moves. PM: C3b or a later lane. |
| S-10 | `nav_cli.cpp:147-152` (`--holdings-format` without `--emit-holdings`, "accepted (and ignored)"); `book/nav_write.cpp:83`, `:194` (`--monthly-budget` under aim-partial-v5: recorded in `recipe.json`, declared "monthly_budget unused"); `strategy_nav_v7.cpp:1272-1273` (`--spo-alpha implied-aim`, a no-op on spo-v3) | Pre-existing argv behaviour the NavSpec inherits by design (spec = argv): three option keys are accepted and change no trade. The first two are what C3-1's rationale calls "accepted and ignored" (though C3-1 rules the constants block); `monthly_budget` also moves `recipe.json` / `recipe_sha256` for an identical book. No P9 template names any of them (`scripts/specs/p9` in pool-2 and pool-17). | PM ruling: either C3-1 extends to options (then `to_args` refuses them, accepting that `--print-spec` of such an argv line is then refused), or record them as accepted no-ops. |
| S-11 | `task-C3-report.md` "Ledger candidates (fix round 1)"; `handoff-C3.md:12` | The candidate ids collide with rulings already in the ledger: the report's "C3-5" (the `--ann-mean-gross` producer) and "C3-6" (S-3: the decide path accepts a seal-less manifest without the P13 log line) vs PM's Ruling C3-5 (M2h probe) and Ruling C3-6 (the comparison that did not run). The S-3 deviation, which the first review accepted only if recorded, is not yet a ledger line. | PM records both under fresh ids (e.g. C3-7, C3-8). Docs only. |
| S-12 | `atx-impl/tests/strategy_nav_spec_test.cpp` (S-4) | S-4 is fixed in code (`nav_spec.cpp:347-354`, `:474`: the printed digest is `sha256_hex` of the bytes parsed), but no test asserts the `nav spec: atx.nav-spec/v1 <path> sha256 <hex>` line's value, which root item 3 and a receipt's stdout digest read. | One assertion in `SpecRunPublishesTheFlagRunBytes`: stdout holds `"sha256 " + sha256_file(file)`. |
| S-13 | `atx-impl/src/book/nav_spec.hpp:30-34` (S-7 comment) | The comment lists vol-target-v1 and risk-target-v1 as rules whose realised gross can leave L; `--spo-gross G` (the spo rules' planned-gross cap, default L, up to 1.5 L: `strategy_nav_v7.cpp:1025-1026`, `:1297-1303`) is a third, also under a narrower `leverage_range`. Not "accepted and not honoured": the range bounds the parameter L, as the header says; C3-4's registration rule covers it, and no P9 cell runs spo. | Name `--spo-gross` in the same comment on the next touch. |
| S-14 | `book/nav_spec.hpp:36-40`, `strategy_nav_replay.hpp:518-521`, help `nav_cli.cpp:116-118` (definition text) | Correct and stated, but the prereg needs two more words than the code comment gives: the figure is gross of trade costs, short borrow AND long financing (write-offs stay in), an arithmetic daily mean on each book's own previous pre-trade NAV (the net path) times 252 (not a CAGR), and one value per scenario. It is the mean of the same series `gross_sharpe` already annualises. | PM / PRE: confirm "gross of financing" is the gross-alpha figure P9-H, P9-MV and G-B3 mean (ALCOMB-9 b), and name the scenario (S2 primary) in each line. No code change. |

First-review Suggested items (S-1..S-8), checked against the code at `9a7c490f`:

| id | status |
|---|---|
| S-1 | fixed. `nav_rules.cpp:57`, `:76` size-deduced `constexpr Row trade_rates[]` / `cost_laws[]`; the target slot stays `std::span<const Row> targets{}` (`:98`, a zero-length array is ill-formed; COV-MV replaces that one line, which only COV-MV touches). Tests check C3's rows by lookup / per-kind prefix and still check kind order (`strategy_nav_spec_test.cpp:446-464`); `TargetRegistry.UnregisteredIdIsRefusedBeforeOutput` uses `zz-no-such-target-v0`. The only assertions relaxed in the round are these (exact sizes / id lists -> prefixes), as S-1 asked. |
| S-2 | C3's half done by C3-3 (the row lists `--cost-v2`); the guard is D3's carry. |
| S-3 | not logged; recorded as a deviation (acceptable per the first review's second option) but the ledger line is missing and its id collides (S-11). |
| S-4 | fixed in code (`nav_spec.cpp:305-354`: file read once, digest of those bytes); no test of the line (S-12). |
| S-5 | fixed: `CostLawRegistry.CostSqrtV2IsTheCostV2RunByteForByte` (`:575-583`) checks every recipe `cost_rule` is a cost-law row id and the set is the four laws. |
| S-6 | partly: `atx-impl/CMakeLists.txt:297-300` fixed; the other lanes' comments left "on the next touch" (fine: comments only). |
| S-7 | fixed: `nav_spec.hpp:30-34` (C3-4); S-13 adds spo. |
| S-8 | note only; nothing to do. |

None of them should be Required.

## Checks (evidence)

**(a) R-1 duplicate keys: PASS.** `book/nav_spec.cpp:320-340` `parse_spec` is the house idiom of
`atx-engine/src/eval/trial_epoch.cpp:76-87`: a `std::vector<std::set<std::string>>` stack, push on `object_start`,
insert on `key` and flag a failed insert, pop on `object_end`, callback always returns `true` (so the document is the
DOM parser's). Differences, all harmless: trial_epoch throws from the callback and caps depth at 12; C3 records the
first duplicate and refuses after the parse (`allow_exceptions = false`), and is bounded by the 1 MiB file cap
(`read_text`, `:306-316`). Because the stack holds one set per open object, every object level is checked: the spec's
top level, `options` and `constants` (its arrays hold numbers, no objects). The refusal names the key
(`duplicate key "K" in PATH ...`). It runs in `load` (`:347-354`), the only NavSpec reader (`ns::read` and `nav --spec`
both call it; no other C++ or Python reader of `atx.nav-spec/v1` exists in the tree), before `to_args`, before the
`nav spec:` line and before any directory (`dispatch` `:462-474`). Test `NavSpec.RefusesADuplicateKey`
(`strategy_nav_spec_test.cpp:339-375`, raw text): repeated `options` and `schema` (top level), `aim_leverage` and
`cadence` with equal values (inside `options`), `leverage_range` (inside `constants`); each `ns::read` fails naming
the key, `nav --spec F --output D` exits 2 with empty stdout and no `D`; controls: the clean file reads, `schema`
inside `options` is not a repetition. A callback that checked only the top level would let case 2
(`options.aim_leverage`) through and fail `:358`.

**(b) R-2 grid + narrower range: PASS.** `nav_spec.cpp:281-283` refuses `construction_grid` with a narrower
`leverage_range`, after the aim-in-range check and beside the `calibrate_gross` refusal; the full range [1, 2] still
returns early (`:272`), so a grid is accepted then (test `:319-320`). Test lines `:312-313` (`aim 1.2` + grid with
[1, 1.5]) and `:321-324` (grid alone, refused naming `construction_grid`). Other keys that move the aim leverage after
the check: none left. `--aim-leverage` other than 1 is refused under every non-aim-partial rule
(`strategy_target_replay.cpp:135-137`); vol-target / risk-target move the realised L_t, not the parameter (C3-4,
`nav_spec.hpp:30-34`); `--spo-gross` caps planned gross up to 1.5 L (S-13, a doc point, same class as C3-4).
Accepted-but-unconsumed keys after this round: none new (`ann_mean_gross` is consumed, (d)); three pre-existing argv
no-ops inherited by the spec (S-10).

**(c) C3-3 `x-flag-aliases`: PASS.** `nav_rules.hpp:55` `Row::x_flag_aliases{}` (trailing, default empty, so every
existing aggregate initializer is unchanged); `nav_rules.cpp:74` the alias array, `:93` on `cost-sqrt-v2` only;
`row_json` (`:100-121`) builds the same object as before and adds `"x-flag-aliases"` only when non-empty, so every
other C3 row dumps to the same bytes (nlohmann `json` sorts keys: `x-flag` < `x-flag-aliases`, the new key lands last).
C1's rows: `rules_json` (`:168-174`) still starts with `risk_target::leverage_rules_json()`; they never pass through
`row_json`; `strategy_risk_target.{hpp,cpp}` have an empty diff since base `e1c16051`; `NavRules.LegacyAliasesOnTheirRowsOnly`
(`:475-525`) compares them by compact dump and checks no alias key on them, the exact part-A key set on C3's rows, the
alias is a NavSpec Switch flag and differs from the x-flag, and C3's seven other rows carry none. Fixtures: no committed
capture of `nav --list-rules --json` exists in pool-15, root pool-2 (`5afb5447`) or pool-17 (`dd857198`):
`scripts/tests/test_wave_kinds.py` holds a synthetic table of C1-shaped leverage rows (not a capture),
`classes.json` only the schema literal, root's `m2h-logs/e2-live-table-check.py` compares `x-flag`, `incompatible`,
`kind` of live rows; `wave_kinds.nav_flags` / `table_flags` read rows with `.get`, so the optional key is tolerated.
The text listing (`kind id` per row) is unchanged. `strategy_vol_target_test.cpp` (C1's tests) is untouched by the
round.

**(d) PRE-7 / PRE-7a opt-in: PASS.** Flow: `nav_cli.cpp:40-47` (switch, repeated = exit 2) ->
`NavEmitOptions::ann_mean_gross` (`strategy_nav_replay.hpp:700`) -> `NavRun::ann_mean_gross`
(`nav_replay_internal.hpp:644-647`, trailing member) set at `nav_run.cpp:414` (single and calibrated run) and `:521`
(each grid variant) -> `nav_publish.cpp:79`, the only output change: `if (run.ann_mean_gross)
entry["ann_mean_gross"] = summary.ann_mean_gross;`.
- Flag absent, every writer the diff touches: `publish_scenario` adds nothing; `scenario_summary`
  (`nav_write.cpp:363-380`), `turnover_json`, `risk_json`, the v7 capacity rows (`strategy_nav_v7.cpp:362-379`) name
  `NavSummary` fields one by one and never the new one; `publish_capacity` (`nav_publish.cpp:201-217`) builds its
  `NavRun` with 17 initializers, so `ann_mean_gross` is default false there flag present or not; daily / events CSVs
  (`write_daily`, `write_events`) take no `NavRun` flag; `recipe.json` comes from `nav_recipe(cfg, base, scenarios,
  limits, fields, label)` (no emit options), as do the holdings manifest's `nav_recipe_sha256` and the decide path's
  recipe pin (`nav_deploy.cpp:57`); `producer_json` reads the build only; no new console line. `summarize_nav`
  (`nav_summarize.cpp:159`) stores one more number in a field; the mid-struct insertion in `NavSummary` shifts no
  initializer: repo-wide its only constructions are value-initializations (`strategy_nav_v7.hpp:141`,
  `strategy_cost_v2_test.cpp:543`) and named fills; no `sizeof`, memcpy or hash of the struct. The three `NavRun`
  constructions are positional and I counted all 19 / 19 / 17 slots: correct. Key order / float formatting: nlohmann
  `json` sorts keys, so the key would sit between `ann_mean` and `ann_vol`, and only when present; doubles print
  shortest round-trip (the test reads the printed value back to the same bits).
- Not a rule flag: no run config, construction, recipe, cost or NAV value reads it (`run_base` and
  `construction_reusable` take `cfg` / `rate` / `execution`, not `emit`); it is in no NAV table row, so the E2-8 guard
  passes it, and it is neither equal to nor a prefix of a rule flag. Absent, nothing about identity changes. Present:
  `recipe.json`, `recipe_sha256`, CSVs, `capacity/`, `v7_extras.json`, the grid's `recipe_file_sha256` and
  `grid_sha256` stay; `summary.json` gains the key per scenario, so its bytes, a grid variant's `summary_file_sha256`,
  the NavSpec's SHA-256 (`"ann_mean_gross": true`) and the bounded-run receipt's `argv_sha256` move. `cycle_resume.py`
  has no NAV `REUSE_NEUTRAL` entry (`:49-50`), so a flag-absent output is never reused for a flag-present step or the
  reverse (pin stop, not silent reuse). Right, since the key changes `summary.json`.
- Definition: rows = `return_observation && k > 0`, the rows of `ann_mean`, `net_sharpe` and `observations`
  (`nav_summarize.cpp:140-159`); series = daily `gross_return` = (mark P&L + write-off) / NAVpre_{t-1}
  (`nav_mark.cpp:142`), where `net_return = gross - trade_cost - borrow - long_financing` is checked each session within
  `identity_tolerance` (`:144-149`); annualisation = running arithmetic mean x 252, the same `Moments` accumulator
  `gross_sharpe` uses. Stated in `nav_spec.hpp:36-40`, `strategy_nav_replay.hpp:518-521`, `:689-695` and the help
  text, and the test recomputes it bit for bit from the published CSV (`:719-729`). Cell vs reference: the same exe and
  the flag on both give the same function of each run's own rows, per scenario, so the figures are like for like; a
  pre-C3 reference has no key, hence PRE-7a's "both name it at the wave-3 P9-B0". Definitional note for the prereg:
  S-14.
- One parser: the NavSpec key (`nav_spec.cpp:92`, Switch) renders `--ann-mean-gross`, which `nav --spec` feeds to the
  same `dispatch_nav_replay`; the v7 seam's token scan passes unknown tokens through in order
  (`strategy_nav_v7.cpp:1091-1186`, `args.push_back(key)`), so under the seam the replay parser reads it too.
  `NavSpec.FlagsAndSpecSameConfig` line 3 (spo-v3 under the seam) checks flags -> spec -> flags and the seam's
  pass-through equal; `NavAnnMeanGross` checks `--print-spec` carries the key only when named and the spec run publishes
  the opt-in run's bytes, on the v7 `v6_shape` and plain v5, plus a one-variant grid equal to its standalone opt-in
  run.
- Flag-absent byte paths outside the NAV run (not PRE-7a's pinned outputs, listed for completeness): `nav --help`
  gains one clause (tests only `find` substrings of it: `strategy_live_test.cpp:2269`, `:2647`, `:3243`,
  `strategy_vol_target_test.cpp:586`; none spans the edit); `nav --list-rules --json` gains the alias key (C3-3, not
  under the opt-in); spec files with a repeated key or a narrower range + grid, accepted before, are now refused (none
  committed).

**(e) Goldens / pins / weakened tests: PASS.** The round's diff touches 12 code files, none a golden, fixture, digest or
expected-value file (`tiny_world_goldens.json`, `strategy_nav_replay_test.cpp` with its pinned recipe digests,
`strategy_vol_target_test.cpp`, `strategy_risk_target.*`: empty diff since base). No `DISABLED_`, `GTEST_SKIP`,
pragma, NOLINT or `/wd` added; no added line over 100 columns. Relaxed assertions: only S-1's (above).

**(f) Suggested:** see the two tables. Nothing should be Required: S-9 and S-10 are pre-existing behaviour outside this
diff, S-11 / S-14 are docs and PM rulings, S-12 / S-13 are a test line and a comment.

**(g) Teeth: PASS (log + two by reading).** `fix1-teeth-k-named.log`: 28 pass, 5 fail, each at the mutated check's
own line, which I matched to the test file at head (`:315`, `:323` M2; `:358` M1; `:508` M3; `:579`, `:583` M6;
`:706`, `:715` M4; `:724`, `:728`, `:729` M5). Repeated by reading: M2 (drop `nav_spec.cpp:281-283`): the grid spec in
the bad list is accepted (`:315`), `grid_narrow` succeeds (`:323`). M4 (guard always on): the absent summary holds the
key (`:706`), and the present file minus the key no longer equals it (`:715`). Wiring not mutated by the lane but
covered by reading: `nav_run.cpp:414` passing `false` fails `:712`; `:521` passing `false` fails the grid tree compare;
dropping the table row `nav_spec.cpp:92` fails `--print-spec` at `:736`.

**(h) Counts vs logs: PASS.** `fix1-gtest-{j,l}-named.log`: filter as reported, 33 tests from 11 suites, 33 passed;
`fix1-gtest-{j,l}-whole.log`: 390 tests from 73 suites, 390 passed, 390 OK lines, no DISABLED; the three new tests
and the renamed `TargetRegistry` test are OK in all four. 390 = 387 + 3. Canary: `mega-p9-c3-j-canary.log`
(Release) and `mega-p9-c3-l-canary.log` (Debug): `15 passed`. Receipts: j `Source 9a7c490f`, 0 dirty, configured
at `9a7c490f`, equity-rel; l `Source 9a7c490f`, 2 dirty (the docs), configured at `99b676f7-dirty` (provenance only:
`summary.json` `producer`). The exe-level check `fix1-nav-optin-l.log` ran both phases on the same post-round exe
(as the log says): flag-absent twice identical (12 files, console equal) and the opt-in differs only in
`summary.json` by 5 lines for 5 scenarios. The pre-round vs post-round exe comparison was not run (command refused);
C3-6 moves it to root. From the code (d): no path by which a flag-absent NAV byte moves.

## What root must check at merge

1. Builds through `research-build.ps1`, both presets:
   `-Targets "atx-impl-strategy-target-tests,atx-impl-tests,atx-equity-strategy-targets,atx-equity-strategy-ic,atx-equity-strategy,atx-equity-strategy-risk,atx-equity-strategy-mine,atx-research-eval,atx-impl" -Canary`.
   Expected: named filter
   `NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavFieldsSeal.*:NavBookWorkers.*:NavBookRule.*:NavAnnMeanGross.*:CalibrateGross.*:CovStore.*`
   33 / 33; whole target-tests 390, Debug and Release; `atx-impl-tests` whole once (its base + C3's 13: part A's 10 +
   this round's 3; never built by the lane); canary 15 on the unchanged Debug and Release goldens. On "modified since
   the precompiled header": delete that tree's `cmake_pch.cxx.{pch,obj}` by name, rebuild with `CCACHE_RECACHE=1`.
2. `atx-impl-core` is INTERFACE: any root list or script naming it as a build target names `atx-impl-strategy` /
   `atx-impl-pipeline`; a root-side append since `ab189471` that writes `atx-impl-core` is retargeted at the merge.
3. `--spec` = argv on X-5 and Y-F0 (same exe, argv verbatim from the cells' receipts, no statistic read):
   `$EXE nav <argv> --print-spec > x.navspec.json` through a byte-preserving redirect (Git Bash `>` or `cmd /c`; never
   PowerShell 5.1 `>`), then the argv run and `nav --spec x.navspec.json --output NEW/spec` into two new dirs: every
   file byte-identical. The spec run's stdout gains `nav spec: atx.nav-spec/v1 <path> sha256 <SHA-256 of the file>`.
4. Flag-absent identity against the P9-B0 references (ruling C3-6): X-5 and Y-F0, through `--spec` and through argv,
   without `--ann-mean-gross`, each byte-identical to its P9-B0 reference outside `summary.json` `producer`; plus the
   tiny-world goldens on Debug and Release (item 1). Check the cells' `--fields` manifests for a `seal` block first:
   without one the NAV stdout gains the P13 legacy line, so the wrapper's `stdout.log` digest moves (keep it on the
   volatile list then). A miss returns the lane (PRE-7a). Optional tiny-world before / after with both bins:
   `c3-logs/fix1-nav-identity.py old --bin <pre-merge bin> --work W`, then `new --bin <merged bin> --work W`,
   `RESULT PASS`.
5. The opt-in on X-5: the same argv plus `--ann-mean-gross` changes only `summary.json`, by one `ann_mean_gross` key in
   each main scenario object (erase it, re-dump with `dump(2)` + newline, or delete those lines: equals the flag-absent
   file); `recipe.json`, every CSV, `capacity/` (its `summary.json` has no such key) and `v7_extras.json` byte-identical.
6. Release = Debug NAV on X-5 (G-P3 NAV half): equal except `summary.json` `producer` (`engine_git_sha`,
   `build_type`); with the opt-in the `ann_mean_gross` values are equal too.
7. Wall time: the five research exes' build time on a fresh tag against the pre-C3 receipt (F-10); X-5 NAV wall time
   before / after the 14-TU split, Debug and Release (cross-TU calls are no longer inlined, C-3).
8. SQL2-CLS: add `atx.nav-spec/v1` to `atx-engine/schemas/research_store/classes.json` (C3's only new schema literal;
   `x-flag-aliases` and `ann_mean_gross` are optional keys inside existing schemas).
9. W3-8 kind-fixture flip (root post-merge; D3 owns the kind files): drop `new-cost-model.json` from
   `SCHEMA_ONLY_FIXTURES` and its "SCHEMA ONLY" description; run the 10 tiny-world kinds against the real exe's
   `nav --list-rules --json` (E2's tests use a frozen synthetic table). Under E2-8 the real table owns `--rule`,
   `--trade-fraction`, `--dust-multiple`, `--exit-rate`, `--rate`, `--rate-rra`, `--rate-lambda`, `--rate-min`,
   `--rate-max`, `--cost-law`; under C3-3 D3's guard must also treat `--cost-v2` (the row's `x-flag-aliases`) as
   `--cost-law cost-sqrt-v2`. Check no P9 template's `change.flags`, `rule_cell.constants.flags` or `horizon.flags`
   names one. `--ann-mean-gross` is no rule flag and a prefix of none.
10. C3-5 probe: re-run the M2h tiny-world probe (`m2h-logs/m2h2-engine-o1.py`) with a wave naming one of C3's real
    trade-rate rows; report which `--rule` token reaches the NAV argv; a duplicate or conflicting `--rule` is a stop
    for the PM.
11. PRE-7a at the wave-3 P9-B0: every P9 cell and its reference name `--ann-mean-gross` alike (NavSpec
    `"ann_mean_gross": true`); the pre-C3 references carry no key, so they are re-run with the flag; `argv_sha256`
    then differs from the flag-absent run (no cross-reuse). W1-I2: re-pin the lineage parents to those references
    before the first P9 cell.
12. Ledger (PM): record the S-3 deviation (decide path, seal-less manifest, no log line) and the C3 producer under
    fresh ids (S-11); rule S-9, S-10, S-14.
13. Part B (C2-11, C2-9, C2-10, W3-6) in the C3b round after E2 is in root (C3-2).

## Open questions (PM)

- S-10: does C3-1's "no accepted-and-ignored key" extend to options (`holdings_format` without `emit_holdings`,
  `monthly_budget` under aim-partial-v5, `spo_alpha implied-aim`)?
- S-14: is the gross-alpha figure meant gross of financing (borrow + long financing) as well as of trade costs? The
  key is that (consistent with `gross_sharpe`); a "gross of trade costs only" figure would need another producer.
- S-9: who adds the duplicate-key refusal to the construction-grid reader (C3b or later)?
