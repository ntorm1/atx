# Lane E2 re-review 3 (fix round 3)

## Verdict
**CHANGES_REQUIRED** (1 Required, 4 Suggested).

Spec verdict:
- **E2-10 / R2c is met for `nav.leverage` itself.** Every route that sets or changes the key in a non-leverage kind
  stops at preflight (exit 3, before register), naming the key, the value and the route. Probed with 201 + 8
  probes: `constants.set`, the template's `change.set` / `change.unset`, a whole `nav` section, and `gross_match`
  `none` and `pm6-6`.
- **Every flag-list spelling of `--aim-leverage` is refused** under E2-8: `=value`, case, single dash,
  abbreviation, value slot and parent-value map.
- **The three Suggested of re-review 2 are fixed, with teeth.** E2-12 is met. E2-13 (a) holds. P9-C's shape works
  on Python pm6-6.
- **No regression.** v1 plans are byte-identical, no committed spec is newly refused, and the suites are green
  under both seeds.

One route is left, and it comes from C2, which is already merged into root. `--calibrate-gross` makes root's NAV
exe move L itself. It is in no rule table, so E2's guard lets it through on every flag route of every kind (R3a).
The fix is a few lines.

## Reviewed SHA
- Head `ea7d9dde`. Scope: `d0054b32..ea7d9dde`. Over `scripts/` that is 4 files: `wave_kinds.py`,
  `wave_stage_kinds.py`, `tests/test_wave_kinds.py` and `tests/test_wave_driver.py`. The diff was written to a
  scratch file and read from there.
- In between sit WIP `daaa483c` and handoff `42d7401e`. `daaa483c..ea7d9dde` is exactly the two unreviewed
  additions (item G).
- Lane base `1239a5ff`; branch `feat/p9-e2-20261003`; pool-17. Root head for the cross-lane reads: pool-2
  `3e283275` (C2 is merged there).

## Evidence
Run in `C:/atx-wt/pool-17` at `ea7d9dde`, tree clean. Explicit paths, `PYTHONDONTWRITEBYTECODE=1`,
`-p no:cacheprovider`, and each `--basetemp` its own dir in this session's scratchpad. Separate invocations, one after
the other:
```
PYTHONHASHSEED=0 python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/bt-s0 scripts/tests
-> 458 passed, 4 skipped in 593.34s; exit 0
PYTHONHASHSEED=0 ... --basetemp=<scratch>/bt-c0 atx-engine/tools/test_stage_chain.py   -> 9 passed in 0.38s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/bt-s1 scripts/tests
-> 458 passed, 4 skipped in 700.26s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/bt-c1 atx-engine/tools/test_stage_chain.py   -> 9 passed in 0.33s; exit 0
```
The 4 skips are the known live-input gates: `ATX_EQUITY_BIN` x1 and `RESEARCH_CYCLE_LIVE_ROOT` x3.

**Probes.** A scratch script (not committed) drove the lane's fake-world helpers (`test_wave_kinds.world / wave /
KindsFake`) with `run --until preflight`, or `--until spec` / `--until match` where stated. Where an argv is quoted,
it is research_cycle's own `Cycle.nav_step` on the written cell, with placeholder pins.

**Teeth.** The head's `scripts`, `atx-engine/tools` and `atx-impl/{tools,strategies,tests/fixtures}` were extracted
with `git archive` into the scratchpad, so pool-17's tree was never edited. On that copy, the 74
`test_wave_kinds.py` tests plus `test_cache_path_bound_w1_i1` all pass (75). Then one edit at a time, the named
tests, and the file restored byte for byte:

| mutation | result |
|---|---|
| `rule_flag_guard` no longer calls `leverage_refusal` | 7 failed (all `E2_10_PROBES`) |
| the 2.0 cap disabled | cap test failed |
| the no-fixed-v1 refusal disabled | cap test failed |
| N3's line reverted to `cell = spec` | `test_cache_path_bound_w1_i1` failed at `:1011`, the new block's `== 3` (`assert 0 == 3`) |
| `norm_option` made the identity | 3 failed (the three S2 rows) |
| `touching` back to key-only | 1 failed (the S3 row) |
| `unmapped_ic_rules` returns `[]` | 2 failed (both E2-12 tests) |
| MR-4's two rows added to `COMPOSITION_FLAGS` | **74 passed**: no E2 assertion depends on them being unmapped |

**v1 plan bytes.** I ran `research_wave.py plan` on the 5 committed v1 manifests (`y-2`, `y-3`, `y-5`, `y-s`,
`y-s.head`, `--root` pool-17) from the head copy and from a `d0054b32` copy (`git archive`). The output is
byte-identical: 4 at exit 0, and `y-s.head` at exit 2 in both. pool-17's `build-equity` holds only audit scripts, so
the plans read no data.
- Every round-3 helper (`norm_option`, `occurrences`, `touching`, `leverage_refusal`, the cap) is reached only
  through `preflight_kind` / `rule_flag_guard`. Both return early for v1 (`git grep` of the callers).

**Committed specs.**
- The 10 P9 kinds fixtures were run to preflight. The 8 runnable ones exit 0. `new-cost-model` (in neither table) and
  `walk-forward` (schema only) exit 3, as before.
- A scan of all 101 JSON files under `scripts/specs/` found no flag token that `norm_option` would read differently.
- None of the 45 committed templates sets `ref`, `compare` or `static_check` (relevant to S1's fix).

## Item by item

**A. R2c / E2-10: every route, every v2 kind.**

| route | rule (NAV / composition) | role | horizon | add / replace / field / walk-forward |
|---|---|---|---|---|
| `constants.set nav.leverage` (`"5.0"`, `5.0`, the parent's own `"1.1474"`, `nav.leverage.x`), gm `none` and `pm6-6` | 3, E2-10 | 3, E2-10 | 3, E2-10 | 2: `rule_cell` refused by the schema |
| template `change.set nav.leverage` / `change.unset nav.leverage` | 3, E2-10 | 3, E2-10 | 3, E2-10 | n/a |
| whole `nav` section (constants or template, L 5.0) | 3, E2-10 (`rule_cell.constants.set nav: the cell's nav.leverage would be '5.0'`) | 3 | 3 | n/a |
| `nav.flags` list: literal `5.0`, `=5.0`, `--AIM-LEVERAGE`, `-aim-leverage`, `--aim-lev`, `--Aim-Leverage=5.0`, `-AIM-leverage=5.0`, `-aim-lev=5.0` | 3, E2-8, token and route named | 3 | 3 | n/a |
| flag map (constants / template / `horizon.flags`): the same spellings, value slot (`--cadence --aim-leverage`, `--cadence -AIM-LEVERAGE=5`), parent-value map | 3, E2-8, map entry named | 3 | 3; single dash in `horizon.flags`: exit 2 (horizon schema) | n/a |
| a `leverage` block in another kind | 2 (`leverage is the block of kind leverage`) | 2 | 2 | 2 |
| **`--calibrate-gross` in any flag route** | **0 (R3a)** | **0** | **0** | n/a |
| `ref` section with its own leverage / flags | 0 (S1) | - | 0 (S1) | n/a |

- **Passes that are not routes.** Some raw values are overridden by the kind's own derived flags, so the resolved
  cell equals the derived one:
  - in a rule wave naming fixed-v1, `set nav.flags [--aim-leverage, 5.0]` is overridden by the derived
    `--aim-leverage 1.5`;
  - in a horizon wave, `{--cadence: --aim-leverage}` is overridden by the horizon's `--cadence 5`.

  Tokens the exes do not read as options also pass: `"--aim-leverage 5.0"` as one token, or a stray `{leverage}`
  positional. The NAV exe refuses unknown tokens at run, as in round 2.
- **Kind leverage values.**
  - `2.0000001` -> 3, with both the table-range message and the cap message.
  - `5.0` -> 3, with both messages.
  - `0.5` -> 3, table range.
  - `abc`, `""`, `1e0`, `" 1.5"`, `1.5.0` and the number `1.5` -> 2, the decimal-string schema
    (`wave_kinds.py:182-184`).
  - `2`, `2.0`, `1`, `1.0`, `02.0` and `2.00000000000000001` -> 0. The last parses as 2.0 in Python and in the
    C++ `stod`.
  - A leverage wave with a raw `nav.leverage` -> 2, the schema. With a raw `--aim-leverage` flag map -> 3.
- **One pass on purpose.** In a leverage wave, a whole `nav` section carrying L 1.5 passes. The kind's own
  `nav.leverage` is merged after it (`merged_constants` order), so the cell runs at `leverage.value`, and the guard
  confirms that by comparing the resolved cell.

**B. The three Suggested of re-review 2.**
- **S1 (N3 test with teeth): fixed.** The new block in `test_cache_path_bound_w1_i1` (`test_wave_driver.py:991-1017`)
  fails at `:1011` with N3's line reverted (above). It gives the parent its own shorter `ic.cache`, off the shared
  stores, and sets the bound between the two depths.
- **S2 (case and single dash at preflight): fixed.** All spellings in the table stop at exit 3, before register.
  `norm_option` is applied to both sides of every comparison, and no committed token changes under it.
- **S3 (the route message): fixed.** The refusal now reads `rule_cell.constants.flags nav --cadence --vol-target:
  the cell's nav.flags adds token(s) ['--vol-target'] ...`. The route is clear in every exit-3 probe. A dict-typed
  `change.unset` is the one exception (S4).

**C. E2-12: met.**
- `UNMAPPED = "zz-unmapped-rule-v0"` occurs only in `test_wave_kinds.py:63` (`git grep` of pool-17 and of root).
- `assert UNMAPPED not in WK.COMPOSITION_FLAGS` pins it unmapped. No `theme-cov-hedge` or `mom-volman` literal is
  left under E2's `scripts/`.
- With MR-4's rows added, all 74 pass. Nothing can make the id "registered" except a lane choosing it, and the `zz-`
  prefix and the assert make that visible.

**D. Ruling E2-13.**
- **(a) Holds.** On a kind rule wave naming fixed-v1:
  - 3 for `aim_leverage` 2.5 (`rules.fixed-v1.aim_leverage: 2.5 violates maximum 2`), 2.0000001, 5, 0.99, `"2.5"`
    (type), `true` and `Infinity`;
  - 0 for 2.0 and 1.0;
  - vol-target-v1 and risk-target-v1 at 2.5 -> 3;
  - on a table that allows 3, 2.5 passes (ruled: no second check).
  - **Gap:** `NaN` passes preflight (S2).
- **(b) P9-C's shape works here on Python pm6-6.** This branch has no `pm6-6-engine`: `GROSS_MATCH` is `pm6-6` /
  `none`. The probe:
  - **Setup.** A kind rule wave names a non-leverage NAV row. C3's trade-rate rows exist nowhere yet, so it is the
    synthetic `zz-trade-rate-v0` with x-flag `--trade-rate zz-trade-rate-v0` and no params. It uses the fixture
    template `rule-nav.json`, with `gross_match: "pm6-6"` and gross mismatched, run `--until match`.
  - **Preflight passes.** The guard is silent because `nav.leverage` is unchanged.
  - **The calibration cell's argv** is `--rule aim-partial-v5 --aim-leverage 1.1474 --trade-rate zz-trade-rate-v0`.
  - **Match corrects to L' 1.1782**, and the `-gm` argv is `--aim-leverage 1.1782 --trade-rate zz-trade-rate-v0`.
    **L' reaches the matched run's argv.**
  - **E2-10 does not refuse the match stage's own write.** The guard runs at preflight only, on the manifest's rule
    cell, never on a `-gm` copy. `preflight_kind` re-run after match returns `[]`.
  - **The documented limit is wider than "fixed-v1" (S3).** A named rule whose params give `aim_leverage` writes a
    literal into the argv: fixed-v1 always, vol-target-v1 / risk-target-v1 when given. The `-gm` copy then has
    `nav.leverage` 1.1782 while its argv keeps `--aim-leverage 1.5`. Without `aim_leverage` (C1's live table makes it
    optional), L' reaches the argv.
- **Cannot be probed here:**
  - C2's engine stage (`match_engine`, `calibrated_doc`, `record_doc`: pool-2 `wave_stage_cell.py:287-401`);
  - the exe's calibration;
  - the cell of record through verify / judge / record;
  - C2's two record lines.
- **Read in root:** the engine `-cal` recipe keeps `nav.leverage` at the cell's L and adds
  `change.flags.nav --calibrate-gross / --calibrate-tolerance`. The cell of record sets `nav.leverage = L'` through
  `gm_doc`. Neither passes through E2's guard. For P9-C's shape the argv L is `{leverage}`, the cell's L, so
  `:372`'s `leverage_requested` check passes. For a literal-`aim_leverage` rule, `:372` refuses at match (EXIT_PIN),
  so it fails closed. Root's checks are in "What root must check at merge" item 5.

**E. No regression.**
- **Sampled re-review 2 probes**, on a C1-faithful NAV table (risk-target-v1 with bias / cadence), each 3 and
  naming token and route:
  - `set nav.flags +--vol-target`;
  - split `--vol` / `-target`;
  - value slot;
  - `--risk-target-bias=2`;
  - leverage + `--risk-target-cadence`;
  - role `--vol-target`;
  - a whole nav section with `--vol-target`;
  - N1 with AL-COMB's two ids, on 4 kinds (`lists ['mom-volman-v1', 'theme-cov-hedge-v1'] ... add their rows (e.g.
    MR-4) first`).
- **Earlier than preflight.** The `--composition` parent-value map now stops at exit 2 (F-8 in `rules_problems`).
- **The control** `--cadence` passes.
- **v1 plans and committed specs:** unchanged (Evidence).

**F. Suites:** 458 / 4 / 0 and 9/9 under both seeds (Evidence). This matches the lane's report.

**G. The two unreviewed additions: no behaviour change.**
- `daaa483c..ea7d9dde` is only:
  - the cap test asserting `code == 3` before reading the failed receipt (same expected text);
  - `raw_routes` reading `change.unset` through a local (`isinstance(..., list)`, as in the WIP).
- Round 3 itself, the WIP compared with `d0054b32`, did change `change.unset` from `list(... or [])` to lists only
  (S4, a nit).

## Findings
`path:line | severity | problem | fix`

**Required**

- **R3a** `scripts/wave_kinds.py:431-456` (`table_flags` / `owned_flags`), `scripts/wave_stage_kinds.py:172-236`
  (`rule_flag_guard`) | minor (fail-open; an E2-10 route, the R2c class) |
  - **Problem.** C2 is merged in root. Root's NAV exe accepts `--calibrate-gross G_P [--calibrate-tolerance T]` in
    any NAV argv (pool-2 `strategy_nav_replay.cpp:3982-3983`; usage `:3910-3914`). Pass 1 runs at
    `--aim-leverage L`; then the exe publishes the plain run at L' = L x G_P / G, within [1, 2]. So the exe itself
    changes the leverage the cell's NAV runs at. Neither flag is a rule-table x-flag, so E2's guard treats both as
    free.
  - **Probe** (`--until spec`, all exit 0; register ran):
    - `--calibrate-gross 0.95` through `rule_cell.constants.flags nav` (horizon, `gross_match: "none"`), the
      template's `change.flags` (composition rule), `constants.set nav.flags` (role) and `horizon.flags`;
    - a leverage wave with a `rule_cell` flag map;
    - a NAV rule wave on pm6-6.

    Each cell's NAV argv carries `--aim-leverage <L> --calibrate-gross 0.95 ...`.
  - **Consequence on root's exe.** A horizon / rule / role wave publishes at an unregistered L'. A leverage wave
    publishes at L' instead of `leverage.value`. In both, the wave records `nav.leverage` with `gross_match: "none"`,
    a mode that never reads `summary.json` `calibration`. No Python code in root refuses the flag outside
    `match_engine` (`git grep`).
  - **Fix.**
    - Reserve both as the match stage's own flags, e.g. `MATCH_FLAGS = {"nav": {"--calibrate-gross",
      "--calibrate-tolerance"}}`, unioned into `owned_flags`. Then the map, `=value`, abbreviation,
      case / single-dash and resolved-list checks all apply.
    - Word the refusal for E2-10: only the gross-match stage (pm6-6-engine) calibrates.
    - C2's own write is never checked: `calibrated_doc` writes the `-cal` recipe at match, after preflight, and the
      guard reads only the manifest's rule cell.
  - **Tests.** The four flag routes on a non-leverage kind, and the leverage-wave case, refused at exit 3 naming
    token and route; the 10 fixtures unchanged.
  - **Owner.** The flag name is C2's, but naming two strings needs no C2 code. If the PM prefers, root lands it at
    the C2 / E2 merge instead. Either way it must land before any v2 wave runs on root's exe.

**Suggested**

- **S1** `scripts/wave_stage_kinds.py:136-144, 239-258` (with `scripts/research_spec.py:179-190`,
  `scripts/cycle/phases.py:644`, `scripts/wave_stage_record.py:81-91, 106-117`) | minor | The `ref` section is an
  unguarded NAV run in a v2 rule cell.
  - **The route.** `resolve` drops the parent's `ref`, but `change.set` can add one. `ref_step` lets it override
    `rule`, `leverage` and `flags` for the reference NAV.
  - **Probe.** Horizon (`constants.set ref`) and composition rule (template `change.set ref`), with `{output,
    combined: library, leverage: "5.0", flags: [--aim-leverage, {leverage}, --vol-target, vol-target-v1]}`. Both pass
    preflight and spec. The ref argv is `--rule aim-partial-v5 --aim-leverage 5.0 --vol-target vol-target-v1`, while
    the cell's NAV argv is clean.
  - **Not R2c.** The ref NAV is not the cell of record and is not ledgered, and the exe refuses `aim_leverage`
    outside [1, 2] at run.
  - **By reading, not probed.** A rule cell's own `ref` turns `nav_exe`'s `ref` from `"none"` to `"ran"`. `exe_problem`
    then takes the library branch and W1-I2 (a)'s fail-closed refusal no longer applies. `resolve` also drops
    `compare`, so `"ran"` proves no identity.
  - **Fix.** Refuse raw set / unset keys `ref`, `ref.*`, `compare*` and `static_check*` (`IDENTITY_SECTIONS`) in a
    v2 rule cell; none of the 45 committed templates sets them. Or guard `ref.flags` / `ref.leverage` / `ref.rule`
    like `nav`'s.
  - **PM ruling item:** do E2-8 / E2-10 / W1-I2 (a) cover this route? C2's `calibrated_doc` already anticipates "a
    template that sets its own ref" (pool-2 `wave_stage_cell.py:304-309`).
- **S2** `scripts/wave_kinds.py:292-296` (with `scripts/wave_manifest.py:372`) | minor | `NaN` passes a param's
  `minimum` / `maximum`. `json.loads` accepts `NaN` in a manifest.
  - **Probe.** `rules: [{name: fixed-v1, params: {aim_leverage: NaN}}]` exits 0 at preflight and derives
    `--aim-leverage NaN`.
  - **Where it stops.** The exe refuses it only at run, after register (pool-2 `strategy_target_replay.cpp:126-134`,
    the negated range). E2-13 (a)'s [1, 2] bound therefore holds at preflight for every value but NaN.
  - **Fix.** In `schema_problems`, refuse a non-finite number (`math.isfinite`) for type number / integer. Or make
    `WM.load` refuse `NaN` / `Infinity` (`parse_constant`).
- **S3** `task-E2-report.md:533-538` | doc | The E2-13 (b) limit is stated for "a kind rule wave naming fixed-v1".
  - **It is wider.** Any named NAV rule whose params give `aim_leverage` writes a literal over `{leverage}`:
    vol-target-v1 and risk-target-v1 too (probe above).
  - **Both modes fail closed.** In Python mode the matched run repeats the calibration's argv, so its G is unchanged
    and the stage stops after its one correction. The fake world cannot show this: `FakeCycle` derives gross from
    `nav.leverage`, not from the argv. Root's engine mode refuses at match (pool-2 `wave_stage_cell.py:372`).
  - **Fix.** Widen the report line. Note for the P9 templates: a NAV rule under gross matching omits
    `aim_leverage`.
- **S4** `scripts/wave_stage_kinds.py:127-131` | nit | Round 3 reads `change.unset` only when it is a list.
  - **The route.** `research_spec.resolve` iterates a dict-typed `unset` by key (`research_spec.py:184`), so
    `change.unset: {"nav.leverage": true}` still unsets.
  - **Probe.** It is still refused (E2-10), but the route reads `the resolved cell (no raw route names it)`. Round 2's
    `list(...)` named it.
  - **Fix.** Name a dict's keys, or refuse a non-list `change.unset` in the guard.

Not re-raised (ruled or earlier observations):
- N5, the fitter's `allow_abbrev` (D3), S8 / S6's role seal (E2-7), the stale v1 verify line (E2-6), E2-11 (SPO).
- `nav.rule` riding along a whole `nav` section (`aim-partial-v6` passes on every template kind) is outside the owned
  set, as noted in round 2.

## What root must check at merge
1. **MR-4, in this merge.**
   - Root's IC table already lists `theme-cov-hedge-v1` and `mom-volman-v1` (AL-COMB is merged; pool-2
     `strategy_ic_rules.cpp`).
   - Add `"theme-cov-hedge-v1": {"fit": {"--theme-hedge": "theme-cov-hedge-v1"}}` and
     `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}` to `wave_kinds.COMPOSITION_FLAGS`. Checked here:
     with both rows, `test_wave_kinds.py` stays 74/74.
   - Then `WK.unmapped_ic_rules` on root's live `--list-rules --json` must be `[]`. Otherwise every v2 wave stops
     by name (N1).
2. **The positive live-table check** (it replaces D2's old flip item):
   - a pytest that `WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": <live table>})` gives
     `({'fit': {'--composition': 'ew-theme-std-v1'}}, [])`;
   - `new-combination-rule.json` to the spec stage on the live table;
   - with E2-10: `leverage-change.json` (2.0) and `new-nav-rule.json` to preflight on C1's live NAV table. A table
     with no fixed-v1 `aim_leverage` range refuses every leverage wave.
3. **C2's two lines in `wave_stage_record.py`** (C2 R4). E2's `verify_inputs` is `:283-290` at `ea7d9dde`; round 3
   did not touch the file. Insert root's
   `if mt.get("record_spec"): now["record_spec_digest"] = w.spec_digest(mt["record_spec"]) if
   w.exists(mt["record_spec"]) else None` (root `:263-264`) just before `return pinned("match", mt, now)` (`:290`).
   It places cleanly.
4. **C2's S13** (`task-C2-rereview-2.md:84-88`). In the merged `wave_stage_cell.py` `run_stage` engine branch:
   - replace the old exe-notes `log(...)` text with E2's two-line wording (E2's Python branch,
     `wave_stage_cell.py:48-52` at `ea7d9dde`);
   - add E2's `# ruling E1-REUSE-a2: outputs made by another exe than on disk now` comment.
5. **From D (b), once C2 and E2 are both in root:**
   - (a) **P9-C's shape in a fake world on `pm6-6-engine`.** A kind rule wave naming a non-leverage NAV row, with
     the template keeping `--aim-leverage {leverage}`. Check that:
     - preflight passes;
     - the `-cal` argv carries the cell's L and `--calibrate-gross`;
     - `:372`'s L check passes;
     - the `-gm` cell of record has `nav.leverage` = L' to 4 dp;
     - verify pins `record_spec_digest` (item 3).
   - (b) **R3a's reservation**, whoever lands it, still lets that engine wave through preflight and match, and
     refuses a manifest that writes `--calibrate-gross` itself.
   - (c) **A named NAV rule with `aim_leverage` on `pm6-6-engine`** stops at match (EXIT_PIN, `:372`). It fails
     closed; no P9 template should take that shape (S3).
   - (d) **`wave_manifest.py`'s doc line** `"gross_match": "pm6-6" | "none"` gains `pm6-6-engine`. C2 left it to E2.
   - (e) **C3's real trade-rate rows.** The probe here used a synthetic x-flag. Re-run P9-C's shape on C3's real
     rows. If a row's x-flag is `--rule <id>`, it duplicates `nav_step`'s own `--rule nav.rule`; decide which wins.
6. **Re-review 2's "What root must check" items 3-12 still hold:**
   - B2's post-identity merge (B2-12);
   - the `atx.research-wave/v2` literal in `classes.json`;
   - hash-only plan identity in pool-2 (W1-I1 margins);
   - PM7-35's Python copy;
   - no merge while a wave runs;
   - W1-I2 sequencing;
   - the separate pytest invocations;
   - the `test_cycle_e2e` margin;
   - `allow_abbrev` to D3.
7. Re-review the fix SHA of R3a (and of S1 / S2 if taken).

## Checked
- [x] Python lane: `.agents/cpp/agent.md` section 10 does not apply. C++ was read only to classify what the exes
  accept: `strategy_nav_replay.cpp` (argv, `--calibrate-gross`), `strategy_target_replay.cpp` (the [1, 2] range) and
  `strategy_risk_target.cpp` (C1's live rows).
- [x] **Nothing dated 2024 or later was opened, and no 2020-2023 output was read.** Probes used synthetic fake worlds,
  committed specs and fixtures only.
- [x] **pool-17 was never edited.** Mutations were done on a `git archive` copy. No source file changed, no branch
  switched, nothing built; nothing in `C:/atx` or `atx-db/` touched.
- [x] **One slip, fixed.** A fixture-scan script of mine set `PYTHONDONTWRITEBYTECODE` only after start-up, so it
  wrote 5 git-ignored `__pycache__` dirs into pool-17 at 14:06. They are `scripts/`, `scripts/tests/`,
  `scripts/cycle/`, `atx-engine/tools/` and `atx-impl/tools/`; all their files were created then, and no other dir
  was touched. I deleted them by name at 14:13.
  - The seed-1 suite ran partly while they existed. They were compiled from the identical sources, so the results
    are unaffected.
  - `git status` is clean.
- [x] No permission refusal this session (`git restore` was not needed: a scratch copy instead).
- [x] The report's claims match the code and my re-runs: counts, M1-M7 and the fixtures.
