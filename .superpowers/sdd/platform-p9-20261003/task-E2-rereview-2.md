# Lane E2 re-review 2 (fix round 2)

## Verdict
**CHANGES_REQUIRED** (1 Required, 3 Suggested). Spec verdict: E2-8 is met for every flag-list route, and E2-9 is met
(N4 fixed, N3 one line, N5 and `allow_abbrev` deferred as ruled).
- Every round-1 R2b probe and the N1 / N2 probes are now refused at preflight (exit 3, before register), with the
  token and the route named.
- Spot-checked tests have teeth: 4 of the 12 new ones fail on `assert 0 == 3` with the three source files put back
  to `05a0fcd1`.
- No committed spec or fixture is newly refused, and v1 plans are byte-identical.

One route is left: `nav.leverage`, the value `research_cycle` substitutes into `--aim-leverage {leverage}`, which is
`fixed-v1`'s bounded param. The guard compares the flag lists before that substitution. So any non-leverage template
kind can set `nav.leverage` and run `fixed-v1` unnamed, outside its `[1, 2]` schema (R2c). The fix is a few lines.

## Reviewed SHA
- Head `d0054b32`; this round's base `05a0fcd1` (the review commit `867f40f4` sits between them, docs only).
- Lane base `1239a5ff`; branch `feat/p9-e2-20261003`; pool-17.
- Diff read: `867f40f4..d0054b32` over `scripts/`, 6 files, 608 diff lines, written to a scratch file and read from
  there. Every fix needs a new review.

## Evidence
Run in `C:/atx-wt/pool-17` at `d0054b32`, tree clean. Explicit paths, `PYTHONDONTWRITEBYTECODE=1`,
`-p no:cacheprovider`, `--basetemp` in this session's scratchpad (removed after). No `atx-engine/tools` test in the
same session.
```
python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/pt-k scripts/tests/test_wave_kinds.py
-> 62 passed in 64.17s; exit 0
python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/pt-d scripts/tests/test_wave_driver.py -k "cache_path_bound or parent_nav or lock_exes or claims_lock or judge_records or launch_waits or two_launches or adopt_never or k_p9_10"
-> 13 passed, 15 deselected in 35.57s; exit 0
```
- The lane's logs `logs/e2f2-*` match its report: 446 passed / 4 skipped under seeds 0 and 1, and stage chain 9/9
  twice, as separate invocations.
- The added lines contain no skip / xfail mark and no ledger or trial code.

**Teeth (item 3).** The tree at `d0054b32` was extracted with `git archive` into the scratchpad (no worktree). A
second copy had `wave_kinds.py`, `wave_stage_kinds.py` and `wave_stage_preflight.py` restored from `05a0fcd1`.
Four tests ran in both copies:
- `[set nav.flags --vol-target]`;
- `[horizon.flags nav --vol-target]`;
- `[template change.unset nav.flags]`;
- `test_raw_rule_flag_guard_allows_free_flags_and_names_unmapped_ic_rules`.

Head copy: 4 passed. Restored copy: 4 failed, each on `assert 0 == 3`, i.e. preflight passed with the old code.

**Probes.** Scratch pytest using the lane's fake-world helpers. The NAV table is the C1-faithful one: risk-target-v1
also carries the `--risk-target-bias` and `--risk-target-cadence` params (pool-2 `strategy_risk_target.cpp`). All
results are preflight exits.
```
round-1 R2b   set nav.flags +--vol-target (rule)             3  "rule_cell.constants.set nav.flags: the cell's nav.flags adds token(s) ['--vol-target'] ..."
              set fit.flags --theme-erc (NAV-only template)  3  "rule_cell.constants.set fit.flags: ... adds token(s) ['--theme-erc']"
              fit --theme-erc=V                              3  "rule_cell.constants.flags sets ['fit --theme-erc=theme-erc-v1'] ..."
              fit --theme-er                                 3  "... [\"fit --theme-er (abbreviates ['--theme-erc'])\"]"
              fit --composition=V                            3  at preflight now (was spec exit 4)
N1            AL-COMB table (pool-19), new-signal / new-nav-rule / leverage-change
                                                             3  "the IC exe's rule table lists ['mom-volman-v1', 'theme-cov-hedge-v1'], which ... COMPOSITION_FLAGS does not map ... (e.g. MR-4)"
N2            horizon.flags nav --vol-target                 3  "horizon.flags sets ['nav --vol-target'] ..."
              role constants.flags nav --vol-target          3
              leverage+rule_cell constants.flags --risk-target-cadence   3
new routes    set whole nav section (+--vol-target)          3  "rule_cell.constants.set nav: the cell's nav.flags adds token(s) ['--vol-target'] ..."
              split tokens --vol / -target                   3  "... adds token(s) ['--vol'], read as --vol-target ['-target']"
              owned flag in a value slot (--cadence --vol-target)  3  (refused; route text says "the resolved cell (no raw route names it)")
              --composition parent-value map                 3
              --risk-target-bias=2                           3
              set nav.flags.1 (list index)                   0  _walk turns the list into a dict: resolved nav.flags = ['1', '--aim-leverage', '1.5'];
                                                                no owned token added; the NAV exe refuses "unknown flag: 1" at run
              case variant --Vol-Target                      0  the NAV exe refuses unknown flags at run (strategy_nav_replay.cpp:3458)
              single dash -theme-erc in set fit.flags        0  the fitter refuses it (parse_args exit 2); via a flag map it is refused at preflight
              --composition in the ic section                0  the IC exe refuses unknown options (strategy_ic_runner.cpp:912)
              set nav.rule aim-partial-v6                    0  not a table rule (outside E2-8's owned set; see Observations)
              set nav.leverage 5.0, gross_match none, on new-horizon / new-combination-rule / new-universe
                                                             0  spec stage 0, the cell's change.set nav.leverage = 5.0  -> R2c
controls      set nav.flags +--cadence                       0  (free)
              committed new-horizon (--cadence)              0
fixtures      all 10 committed P9 kinds fixtures to preflight: 8 runnable -> 0; new-cost-model and walk-forward -> 3,
              for their schema-only reasons, as before
```
`env` from a spec is PATH-only (`cycle/phases.py:243-248`). The NAV sources read no environment variable. The fitter
has no `fromfile_prefix_chars`, so no response files are read. `--theme-resid-parent` (from `inputs`) without
`--theme-resid` is refused by the fitter (`composition_resid.py:230`).

**False refusals (item 2).** I scanned all 101 JSON files under `scripts/specs/`. The owned set used: `fit` =
`COMPOSITION_FLAGS` plus MR-4's `--theme-hedge` and `--mom-volman`; `nav` = C1's 7 rule and param x-flags plus
`--two-speed`.
- No spec flag token and no template `change.flags` key is a proper prefix of an owned flag, so the abbreviation rule
  refuses nothing committed.
- 36 v8 template entries set an owned flag (`--composition`, `--theme-resid`, `--risk-model`, ...). Those templates
  cannot be reused as P9 rule-cell templates, as E2-8 intends: name the rule.
- The committed P9 fixtures pass (above).
- v1 manifests never reach the guard (`kind` is None).

**v1 plan bytes (item 5).** `research_wave.py plan` on the 5 committed v1 manifests (`scripts/specs/v8/waves/y-2`,
`y-3`, `y-5`, `y-s`, `y-s.head`, `--root` pool-17) gave byte-identical output from the head copy and from the copy
with the three files at `05a0fcd1`. That is 4 at exit 0 and `y-s.head` exit 2 in both, and pool-17 stayed clean.
Round 1's AST check of the v1 verify line still holds (`wave_stage_record.py` is untouched this round).

## Item by item
1. **The guard (E2-8):** compliant for flag lists, with one gap (R2c).
   - `rule_flag_guard` (`wave_stage_kinds.py:154-215`) runs for every v2 kind:
     - the map check, with `=value` stripped and abbreviations refused;
     - the resolved-cell comparison, using `argparse_values` semantics against the parent with only the derived
       flags applied;
     - N1 by name;
     - both tables required.
   - It resolves the cell the way the spec stage does (`kind_constants` is now shared with `rule_cell`).
   - Every refusal names the token and the route.
   - Library kinds have no raw route into the cell's flags.
2. **False refusals:** none found (above).
3. **Teeth:** confirmed on 4 of 12 (above).
4. **N3 and N4:**
   - **N4 is correct and weakens nothing.** The environment-dependent `assert deep > RC.CACHE_PATH_MAX` became a
     bound set from the computed path. The test keeps the refusal at `deep - 1` and the message text, and gains the
     message number (`"{deep} characters (> {deep - 1})"`) plus positive cases: fits at `deep`, audit-exact refused at
     `deep` and admitted at `deep + 6`, and the wave admitted at `wdeep`. The real 250 bound is still pinned (the
     `226 + len(id)`, `256 (> 250)` and `at most 24` asserts run before any monkeypatch).
   - **N3's line is correct** (`wave_stage_preflight.py:52`). It mirrors `research_add_alpha.py:186-187` by importing
     `shared_stores`, not copying it.
   - **N3 is untested.** The fake parent has no `ic.cache` (keys `flags`, `u_output`, `w_output`), so the cell's
     dir equals the parent's. With N3's line reverted to `cell = spec` in a scratch copy,
     `test_cache_path_bound_w1_i1` still passes (S1).
5. **v1 plans:** unchanged (above).

## Findings
`path:line | severity | problem | required fix`

**Required**

- **R2c** `scripts/wave_stage_kinds.py:127-135, 188-191` (with `scripts/cycle/phases.py:821`) | minor (fail-open; an
  E2-8 route left) |
  - **Problem.** All 18 v8 specs carry `--aim-leverage {leverage}` in `nav.flags`, and `nav_step` substitutes
    `nav.leverage` into it. `--aim-leverage` is `fixed-v1`'s x-flag, with `aim_leverage` bounded to `[1, 2]`.
  - `rule_cell.constants.set` / the template's `change.set` / `change.unset` of `nav.leverage` changes that owned
    flag's value. The guard compares the lists before the substitution, so it sees `{leverage}` on both sides.
  - Probe: on horizon, a composition-rule wave and role, with `gross_match: "none"` (valid for any kind) and
    `set nav.leverage 5.0`, preflight and spec both exit 0 and the cell carries `nav.leverage = 5.0`: `fixed-v1`
    unnamed at 5x.
  - Kind leverage already refuses this route for itself (`wave_kinds.py:183-184`). With `pm6-6` the matched leverage
    is rescaled, so only the calibration run uses the set value; with `"none"` it is the judged cell.
  - **Fix.**
    - In `flag_lists`, read `nav` lists as `nav_step` does: `{leverage}` -> `str(sec.get("leverage"))`.
    - Build `base` from the parent with the kind's own sets applied as well (`kind_constants`' `sets`, so kind
      leverage's `nav.leverage` is the expected value).
    - Add `nav.leverage` to `touching`'s keys for `nav`, so the route is named.
    - Alternatively, refuse a `nav.leverage` set / unset key in every kind but leverage.
  - **Tests.** Add the probe (horizon or composition rule, `gross_match: "none"`, `set nav.leverage 5.0`) as a
    refusal, and check that the leverage fixture still passes.

**Suggested**

- **S1** `scripts/tests/test_wave_driver.py:972-976` | minor (test teeth) | N3 is untested: the fake parent has no
  `ic.cache`, so the derived dir equals the parent's, and the test passes with N3's line reverted. | Give the wave half
  a parent with an explicit, deeper-or-shorter `ic.cache` that is not on the shared stores, set `CACHE_PATH_MAX`
  between the two depths, and assert that the cell's (derived) dir decides.
- **S2** `scripts/wave_stage_kinds.py:138-141` | minor | Case variants (`--Vol-Target`) and single-dash forms in a flag
  list pass the guard. The NAV / IC exes and the fitter refuse them, but only at run, after register. | Count a token as
  an occurrence of an owned option when it matches after `casefold()` and after normalising a leading single `-` to
  `--`, so the stop comes at preflight.
- **S3** `scripts/wave_stage_kinds.py:200-215` | nit | A token reached only as the value of a free flag (`--cadence
  --vol-target`) is refused, which is correct and conservative. But the route text says "the resolved cell (no raw route
  names it)" although `rule_cell.constants.flags nav --cadence` put it there. | Let `touching` also name a map entry
  whose value is the token.

Not re-raised (ruled): N5 (P10 note), the fitter's `allow_abbrev=False` (D3 carry), S8 and the S6 role-seal part (E2-7),
the stale v1 verify plan line (E2-6), E2-4.

**Observations (no finding, outside E2-8's owned set).**
- `nav.rule` (`--rule aim-partial-v6` / `spo-v*`), `fit.script`, the `ic` section, and a section replaced through a
  list-index key (`nav.flags.1`) still reach a template cell untouched by the guard. Only the table rows' flags are
  "owned".
- The trading-rule and SPO choices therefore stay template-only routes, as in v8.
- **PM ruling item.** `--risk-model` / `--risk-model-sha256` are owned through the vol-target-v1 / risk-target-v1
  params, but the SPO rules (`--rule spo-v*`, not table rows) read the same flags (`w04-v61-spo1-lo1.json`,
  `r6-spo-v3-gm.json`). So a v2 wave cannot express an SPO cell with a new risk model: raw is refused, and SPO cannot
  be named. That is fine if no P9 wave needs it. Otherwise the NAV table needs SPO rows (a C lane).

## What root must check at merge
1. **Positive live-table check, replacing D2's old flip item.** D2's merge request 1 (flip `test_wave_kinds.py`
   `:247-251, :311-312` at `5ca50ef5`) is superseded: those asserts read the frozen `d1_table()` and stay green.
   - After D2: `WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": ic_table(False)})` must give
     `({'fit': {'--composition': 'ew-theme-std-v1'}}, [])`. Add it as a pytest, plus `new-combination-rule.json` to the
     spec stage on the live table.
   - The guard now also requires D2's table to list only mapped ids: today the 8 `COMPOSITION_FLAGS` ids.
2. **MR-4 in the same merge as AL-COMB's C++ table.** Add
   `"theme-cov-hedge-v1": {"fit": {"--theme-hedge": "theme-cov-hedge-v1"}}` and
   `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}` to `wave_kinds.COMPOSITION_FLAGS`. Without them
   every v2 wave stops by name (N1, intended). No committed fit token abbreviates either flag (scan above).
3. **B2 (Ruling B2-12).** B2's post-identity part (deletion, B2-11, its re-review R1 / R2) lands after E2. The B2 lane
   merges root's head (with E2) into its branch, resolves its own hunks onto `scripts/cycle/**` and
   `wave_stage_record.py`, and is re-reviewed. Root does not hand-place those hunks. SQL3 waits for it (W3-12).
4. **NAV-rule and horizon fixtures (C1 is merged).**
   - Run the report's post-merge command with the built exes: expect `new-nav-rule.json ({'nav': {'--aim-leverage':
     '1.5'}}, [])`.
   - Every v2 wave now needs both `--list-rules --json` listings: IC, and NAV `nav --list-rules --json`. A pre-C1 NAV
     exe refuses every v2 kind.
5. **The literal `atx.research-wave/v2`** goes into `classes.json` (SQL2-CLS).
6. **Base-vs-head hash-only plan identity, with the real IC exe on disk, in pool-2.**
   - W1-I1 runs at plan. `v70-lo3` is 249 at pool-2 (a 1-char margin), so never run it from a longer root.
   - Compare the 130 original keys.
   - v1 wave plans are byte-identical across this round (checked here on the 5 committed v8 waves).
7. **PM7-35's Python copy** (`wave_rules.py:50-51`) retires only after root's identity against B1's C++ predicate.
8. **Do not merge while a wave runs.**
   - Not under a `receipt_digest: "content"` wave (E1-STALE).
   - Not under a v1 template-cell wave between nav and verify whose parent NAV came from another exe (W1-I2 (a)).
9. **W1-I2 sequencing.** (a) is keyed on the exe hash. The order is: the last NAV-exe relink, then the P9-B0 re-base,
   then the (b) re-pin, then the first wave. Any `exes_sha256` that (b) adds must name the exe the cells run.
10. **pytest.** Keep `scripts/tests` and `atx-engine/tools` tests as separate invocations. The W1-I1 test no longer
    depends on `--basetemp` (N4).
11. **`test_cycle_e2e` with `ATX_EQUITY_BIN`.** The Release canary under `%TEMP%` passes through W1-I1 with about 8
    chars of margin (about 2 with audit-exact). A refusal there is exit 2 at plan.
12. **The fitter's `allow_abbrev=False`** is D3's required carry (E2-9). Until then, an abbreviation typed outside a
    wave reaches the fitter.
13. After R2c, re-review the fix SHA.

## Checked
- [x] Python lane: `.agents/cpp/agent.md` section 10 does not apply. The C++ argv parsers (NAV, IC) and the fitter's
  argparse were read to classify which spellings fail loudly.
- [x] The diff stays inside E2's files. Nothing was opened that is dated 2024 or later, and no 2020-2023 output was
  read (synthetic fake worlds, committed specs and fixtures only).
- [x] The report's claims match the code and the re-runs. The lane's "all 12 fail when reverted" was spot-checked
  on 4.
