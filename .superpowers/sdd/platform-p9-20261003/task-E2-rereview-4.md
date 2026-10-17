# Lane E2 re-review 4 (fix round 4)

## Verdict
**APPROVE** (0 Required, 2 Suggested, 5 observations for root / the PM).

Spec verdict:
- **R3a / E2-14 is met.** `--calibrate-gross` and `--calibrate-tolerance` are refused at preflight (exit 3, before
  register) from every route other than the gross-match stage. That holds in every v2 kind with a rule cell, in library
  kinds through the parent they inherit, and in every spelling I tried. The refusal names the token and the route. One
  route (a `rules` param value) is named only generically (S1).
- **The match stage's own use still works on both routes.** Python `pm6-6` and the merged engine stage `pm6-6-engine`
  both run P9-C's shape to `record` in the fake world.
- **E2-15 (a), (b) and (c) are met.** I probed every route; W1-I2 (a) can no longer be bypassed through a `ref`. Run
  on the pre-fix merge code, the same probes pass, so they have teeth.
- **S2 and S4 are fixed.**
- **The merge is clean.** It took root's code outside E2's files unchanged. MR-4 is AL-COMB's two rows, exactly. S13
  is placed as C2's report defines it. C2's R4 pin is intact, and its test is green and has teeth.
- **No regression.** v1 plans are byte-identical across four trees. No committed spec is newly refused. A 28-probe
  sample of re-reviews 2 and 3 still refuses.
- **Suites match the expectation:** 510 / 4 / 2 under both seeds. The 2 reds are exactly root's two e27b tests, which
  fail identically on `42f42a07`.
- **PRE-2 / PRE-11 hold.** No round-4 code is reachable from `research_cycle.py` or `scripts/cycle/`.

## Reviewed SHA
- Head `80c322ef`, branch `feat/p9-e2-20261003`, pool-17. Scope `ea7d9dde..80c322ef`:
  - merge `d2cea6a6` (parents `f525b195`, root `42f42a07`);
  - code `ce8c4f43`;
  - report commits `ebd4b542` and `80c322ef`, both docs only (`git diff --stat ce8c4f43 80c322ef` shows only
    `task-E2-report.md`).
- **The merge.** `git merge-tree --write-tree f525b195 42f42a07` gives `0f746a16`, as the lane says.
  `git diff 0f746a16 d2cea6a6` is 2 files, +5 / -2: MR-4 and S13 (item D).
- **The code commit.** `git diff d2cea6a6 ce8c4f43 -- scripts` (726 lines) was written to a scratch file and read
  from there. It touches 5 files:
  - `wave_kinds.py`
  - `wave_stage_kinds.py`
  - `wave_stage_record.py`
  - `wave_manifest.py` (doc line only)
  - `tests/test_wave_kinds.py`

## Evidence
Everything below ran in `C:/atx-wt/pool-17` at `80c322ef` with the tree clean, using `C:/Program Files/Python312/python.exe`.
- Explicit paths, `PYTHONDONTWRITEBYTECODE=1` set at launch, `-p no:cacheprovider`.
- Each run had its own `--basetemp` in this session's scratchpad.
- Separate invocations, one after the other:
```
PYTHONHASHSEED=0 python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/bt-s0 scripts/tests
-> 2 failed, 510 passed, 4 skipped in 703.98s; exit 1
PYTHONHASHSEED=0 ... --basetemp=<scratch>/bt-c0 atx-engine/tools/test_stage_chain.py   -> 9 passed in 0.50s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/bt-s1 scripts/tests
-> 2 failed, 510 passed, 4 skipped in 930.47s; exit 1
PYTHONHASHSEED=1 ... --basetemp=<scratch>/bt-c1 atx-engine/tools/test_stage_chain.py   -> 9 passed in 0.44s; exit 0
```
- **The 2 failures, in both seeds:**
  - `test_research_spec.py::test_e27b_reads_what_the_fitter_and_nav_summ_parse`;
  - `::test_the_whole_file_passes_with_a_generated_spec_present`, whose subprocess re-run fails only on e27b.
- **The cause:** `ambiguous option: --compo could match --composition, --composition-exe` (D2's fitter flag).
- **Root's own tree.** `-k e27b` on a `git archive` copy of root `42f42a07`: `1 failed, 2 passed`, with the identical
  message. On a copy of `ea7d9dde`: `3 passed`. No other test fails.
- **Skips.** The 4 are the known live-input gates: `ATX_EQUITY_BIN` x1 and `RESEARCH_CYCLE_LIVE_ROOT` x3.
- **Counts.** 516 collected = the 462 of re-review 3 + 16 of root's (AL-SIG 11, C2 5) + 38 of the lane's. This
  matches the report.

**Probes.** Scratch scripts, not committed, drove the lane's fake-world helpers (`test_wave_kinds.world / probe /
KindsFake`). For the "pre-fix" column, the same scripts ran on a `git archive` copy of the merge `d2cea6a6`, with the
head's test helpers copied in:

| set | head `80c322ef` | pre-fix `d2cea6a6` |
|---|---|---|
| R3a / E2-15 / S2 / S4 (68 probes: 66 with an expected outcome, 2 informational) | 66 / 66 as expected | 44 pass preflight (exit 0); 10 refused without the E2-14 / E2-15 text; 12 controls and schema refusals as expected |
| W1-I2 (a) through a ref (5 template kinds x 3 routes) | none: exit 4 at verify; constants: exit 3 at preflight; template: exit 4 at verify, "(ruling E2-15 (c))" | template and constants: **verify exit 0** on 4 kinds (`nav_exe {equal: false, ref: "ran"}`); leverage: exit 4 for an unrelated mechanics reason |
| re-review 2 / 3 sample (28) | all refused as before; controls pass | - |
| P9-C shape, both gross-match routes, to `record` (4) | exit 0 (item A) | - |

**Teeth (my own).** On `git archive` copies of `80c322ef`, one textual mutation at a time; pool-17 was never edited:

| mutation | named tests |
|---|---|
| `match_flag_reads` returns `[]` | 14 failed |
| `ref_refusal` returns `[]` | 11 failed |
| `exe_problem` ignores `rule_cell` | 1 failed (`w1_i2[template-a]`) |
| ref leverage not judged | 1 failed |
| non-finite check removed | 1 failed |
| C2's two R4 lines removed from `verify_inputs` | `test_engine_cell_of_record_is_pinned_after_match` failed at `verify["match.record_spec_digest"] == ...` (the unmutated copy passes) |

## Item by item

**A. R3a (ruling E2-14).**
- **Refused, exit 3 before register, token and route named** (13 probes of my own, beyond the lane's 13 `R3A_PROBES`):

  | route | probe |
  |---|---|
  | `constants.set ic.flags` | horizon |
  | `constants.set card.flags` | role, `--calibrate-tolerance` |
  | `constants.set ic.w_flags` map | composition rule |
  | `constants.flags nav` | `-CALIB` (single dash, upper case, abbreviation of both); `--calibrate-gross=0.9: true` |
  | `horizon.flags` | `--calibrate-g` |
  | leverage wave `rule_cell` | `constants.set nav.flags` |
  | template `change.set nav` whole | `-Calibrate-Tolerance=0.01` |
  | template `change.flags fit` | `--CALIBRATE-GROSS` |
  | value slot `{--cadence: --calib}` | nav-rule |
  | `[--cadence, --calibrate-gross=1]` | in a set list |
  | `change.unset nav.flags` + `constants.set nav.flags` | role |
  | a NAV rule param value `risk_model: "--calibrate-gross"` | vol-target-v1 (S1: the route is named generically) |

  - Library kinds: refused through the parent they inherit (the lane's test).
  - `ref.flags`: refused (Ba9, E2_15_SIDE).
- **Controls pass:** `--cadence 5`, and `--capacity-curve` (a `--ca` prefix that is not a match flag).
- **The match stage's own use, P9-C's shape.** A kind rule wave names the synthetic trade-rate row (`--trade-rate
  zz-trade-rate-v0`), the template keeps `--aim-leverage {leverage}`, the gross is mismatched, and the wave runs to
  `record`. Argv below is `Cycle.nav_step` on the written specs:
  - **Python `pm6-6`.** Preflight passes, with derived flags `{nav: {--trade-rate: zz-trade-rate-v0}}`. The
    calibration argv is `--rule aim-partial-v5 --aim-leverage 1.1474 --trade-rate zz-trade-rate-v0`. The `-gm`
    cell of record is at `nav.leverage` 1.1782 (4 dp), with argv `--aim-leverage 1.1782 --trade-rate ...`, and
    wave-result's `cell.spec` / `next_parent` is the `-gm`.
  - **Engine `pm6-6-engine`.** Preflight passes. The `-cal` run argv is `--aim-leverage 1.1474 --trade-rate
    zz-trade-rate-v0 --calibrate-gross 0.9863475710099681 --calibrate-tolerance 0.005`, so the stage's own write
    goes through after preflight. The `-gm` record is plain at L' 1.1782 (no `--calibrate-*`). Verify pins
    `match.record_spec_digest`, and wave-result's `cell.spec` / `next_parent` is the `-gm`.
  - **With a template's own `ref` block**, both modes reach `record`:
    - the `-cal` ref argv has no `--calibrate-*` (C2 pins `ref.flags`);
    - engine mode pins `ref.leverage` at L, while Python mode's `-gm` ref follows L' (O1).
  - **The guard never reads the stage's write.** It reads only the manifest's template and constants, never `-cal` /
    `-gm`. No later stage re-runs it.
- **Not probeable without real exes** (root does these at merge):
  - the NAV exe's actual calibration and `summary.json` `calibration` record. `FakeCycle.calibrate` synthesises it
    from `nav.leverage`, not from the argv. So `match_engine`'s `leverage_requested` check (`wave_stage_cell.py:374`)
    is not exercised against what the exe read. A named rule with a literal `aim_leverage` would pass in the fake world
    and stop (EXIT_PIN) on the real exe.
  - the mechanics reader's bit-equal G on a real NAV;
  - how the C++ parser treats `=value`, case or abbreviated `--calibrate-*` (moot: Python refuses them all);
  - C3's real trade-rate rows (a synthetic x-flag is used here);
  - the live `--list-rules --json` tables.

**B. E2-15.**
- **(a) The resolved reference side is judged as the cell side. Every probe: exit 3, naming the route.**
  - Template `change.set ref` carrying L 5.0, or an unnamed `--risk-target 0.1`, is refused on every template kind:
    rule (composition and NAV), horizon, role, and leverage-with-rule_cell (there L must equal `leverage.value`; the
    parent's 1.1474 is refused too).
  - Also refused:
    - `--vol-target=vol-target-v1`, `-VOL-TARGET`, `--vol-t` (abbreviation);
    - `--two-speed` (a composition NAV flag);
    - L `5` as a number;
    - a literal `[--aim-leverage, 5.0]`;
    - `-calibrate-gross`;
    - a ref whose flags drop the named rule's derived `--aim-leverage 1.5`.
  - The manifest routes to the same values are refused by (b).
  - **Allowed (exit 0):** a ref with no flags or leverage of its own (it inherits the judged cell side); a ref at the
    parent's L `1.1474`; a ref whose flags equal the parent's.
  - `ref.rule` is not judged; nor is `nav.rule` on the cell side (O4).
- **(b) Every route to `ref` is refused, naming key and route** (`a cell may not add, replace or edit its ref
  section ... (ruling E2-15 (b))`):

  | source | route |
  |---|---|
  | manifest | `constants.set ref` / `ref.flags` / `ref.leverage` / `ref.output` |
  | manifest | `constants.flags ref` |
  | manifest | in a leverage wave's `rule_cell`; over the template's own block |
  | template | `change.set ref.leverage` |
  | template | `change.set ref` whole + `ref.flags` |
  | template | `change.unset [ref]` / `{ref.flags: true}` |
  | template | `change.unset ref` over its own block |
  | template | `change.flags ref` |
  | template | a literal top-level `ref` |

  - A literal `ref` in `rule_cell` or in `constants` stops earlier, at the schema (exit 2).
  - `horizon.flags.ref` stops at the horizon schema (exit 2).
  - A `ref` key nested inside a whole `nav` value is no ref section and nothing reads it.
- **Which template-originated `ref` is allowed and which is refused:**
  - **Allowed:** the template file's own `change.set` key `ref`, whole (`TEMPLATE_REF`; the manifest pins the
    template's bytes), whose resolved side passes (a). It runs (spec stage, `test_the_templates_own_ref_block_is_allowed`,
    and my Ba10-Ba13).
  - **Refused at preflight:**
    - dotted `ref.*` sets (even in the template; see below);
    - `change.unset ref` / `ref.*`;
    - `change.flags.ref`;
    - a literal top-level `ref`;
    - a whole block whose L, owned flags or match flags fail (a).
  - **What "now refused" means in the lane's claim.** The verify refusal of W1-I2 (a), by (c): a v2 rule cell whose
    parent NAV exe differs from the cell's (or only one side records its exe) is refused whatever its ref did. With
    like-with-like exes it passes (`test_w1_i2_a_holds_through_a_ref[template-b]`). So a legitimate pinned ref is not
    broken; it just no longer lifts W1-I2 (a). That is exactly ruling (c).
  - **The dotted `ref.*` refusal inside the template** is stricter than the ruling's literal words "the template's own
    pinned ref block". It fails closed, its message names the fix (write the block whole), and it is documented in the
    report's Decisions (O5).
  - **No committed template or fixture is newly refused.** All 109 JSON files under `scripts/specs/` were scanned, 47
    of them templates: none sets, unsets or flags `ref` / `compare` / `static_check`, none has a literal `ref`, and no
    token reads as a match flag. The 7 P9 templates are `kinds/templates/{horizon, role, rule-composition, rule-cost,
    rule-nav}.json` and `templates/{mom-volman, theme-cov-hedge}.json`.
- **(c) The lane's pre-fix probe, re-run.** Parent NAV made by exe `a`; the cell's NAV and its ref by exe `b`;
  `gross_match` none. Run on 5 kinds (horizon, composition rule, NAV rule, role, leverage-with-rule_cell):
  - no ref: verify exit 4, as before;
  - `constants.set ref`: preflight exit 3;
  - the template's own ref: **verify exit 4**, `... (its template's own ref construction, ref ran, is not its paired
    reference: ruling E2-15 (c))`.

  On `d2cea6a6` the two ref routes reached **verify exit 0** on 4 kinds. The bypass was real, and it is closed.

**C. S2 / S4.**
- **S2.** `aim_leverage` given as NaN, `-inf`, `inf` or `1e400` (inf via JSON) is refused at preflight with
  `... is not a finite number`. So is `sigma_star` NaN on risk-target-v1.
  - The strings `"nan"` and `"Infinity"` are refused as `is not of type number`.
  - `leverage.value` `"nan"` / `"Infinity"` stops at the decimal schema (exit 2).
  - Pre-fix, NaN passed (exit 0) and `-inf` / `inf` were refused only as range violations.
- **S4.** `change.unset {nav.leverage: true}` / `{nav.flags: true}` are refused, naming `rule_cell.template
  change.unset nav.leverage` / `nav.flags`. Pre-fix, the route read `the resolved cell (no raw route names it)`.

**D. The merge.**
- **Root's code outside E2's files is unchanged.**
  - Every file that `git diff --name-only 42f42a07 d2cea6a6` lists is one the lane changed (`1239a5ff..f525b195`).
  - For the 4 both-sided code files (`wave_result.py`, `wave_stage_cell.py`, `wave_stage_record.py`,
    `tests/test_no_python_mirror.py`), the +/- lines of `42f42a07 -> 0f746a16` are identical to the lane's own
    `1239a5ff -> f525b195`.
  - `.gitattributes`: both sides added the same lines.
- **MR-4 is exact.**
  - The rows are `"theme-cov-hedge-v1": {"fit": {"--theme-hedge": "theme-cov-hedge-v1"}}` and `"mom-volman-v1": {"fit":
    {"--mom-volman": "mom-volman-v1"}}`, character for character `task-AL-COMB-report.md:87` / `:476`.
  - They are fitter-only flags (`composition_theme_hedge.py:53`, `composition_mom_volman.py:122`). No C++ or NAV parser
    reads them, so no `nav` entry is missing.
  - The committed IC table's 10 ids are all mapped (`unmapped_ic_rules == []`).
- **E2-12 holds.** `zz-unmapped-rule-v0` is only at `test_wave_kinds.py:63`. `assert UNMAPPED not in
  WK.COMPOSITION_FLAGS` passes, and N1 still stops 4 kinds by name with it.
- **S13 is placed as C2's re-review 2 defines it** (`task-C2-rereview-2.md`, item 4). The engine branch at
  `wave_stage_cell.py:74-77` carries E2's comment and E2's two-line wording, byte-equal to the Python branch
  (`:85-88`).
- **C2's R4 pin.** It sits at `wave_stage_record.py:314-315`, byte-equal to root's `:280-281`.
  `test_engine_cell_of_record_is_pinned_after_match` is green in both seeds and fails with the two lines removed.
- **The doc line.** `wave_manifest.py` names `"pm6-6" | "pm6-6-engine" | "none"` (re-review 3, item 5 (d)).
- **The one moved expectation is not a pin.** `abbreviated("--the", ...)` now also lists `--theme-hedge`: the
  committed table plus MR-4.

**E. Nothing regressed.**
- **v1 plan output.** `research_wave.py plan` ran on `y-2`, `y-3`, `y-5`, `y-s` and `y-s.head` (`--root` pool-17,
  whose `build-equity` holds only audit scripts and one receipt; no data). It ran from `git archive` copies of root
  `42f42a07`, `ea7d9dde`, `d2cea6a6` and `80c322ef`.
  - Per manifest, the outputs of all four trees are byte-identical (one SHA-256 each).
  - Exit codes: 4 at exit 0, and `y-s.head` at exit 2 in every tree.
  - pool-17 stayed clean.
- **Fixtures to preflight on the head.**
  - The 8 runnable fixtures exit 0.
  - `new-cost-model.json` exits 3 (`cost-sqrt-v2` in neither table) and `walk-forward.json` exits 3 (schema only),
    both as before.
  - AL-COMB's `templates/mom-volman.json` and `theme-cov-hedge.json` were re-rooted on the fake parent as kind rule
    waves on `pm6-6-engine`, with and without their `requires`. Both exit 0 at preflight and derive `fit --mom-volman`
    / `fit --theme-hedge`.
  - Their spec stage stops in the fake world on `monitor needs script, output`: the fake parent has no monitor
    section. This is a fixture artifact, not E2's guard.
- **Re-review 2 / 3 sample** (28 probes, on a C1-faithful NAV table with bias / cadence; every one still refuses as
  before):
  - re-review 2:
    - `set nav.flags +--vol-target`;
    - split `--vol` / `-target`;
    - value slot;
    - `--risk-target-bias=2`;
    - leverage + `--risk-target-cadence`;
    - role `--vol-target`;
    - whole `nav` section;
    - N1 with the synthetic id on 4 kinds;
  - re-review 3:
    - `nav.leverage` 5.0, and the parent's own 1.1474, by `constants.set`;
    - six `--aim-leverage` spellings;
    - `-AIM-LEVERAGE=5` in a value slot;
    - `leverage.value` 2.0000001;
    - `aim_leverage` 2.5;
    - the three R3a routes, including a `pm6-6-engine` manifest writing `--calibrate-gross` itself.

  Controls (`--cadence`, `leverage.value` 2.0) pass.

**F. Suites.** 510 / 4 / 2 under seeds 0 and 1, and the stage chain 9 / 9 under both (Evidence). The 2 are root's e27b
reds (ruling D2R-1). No other test fails.

**G. PRE-2 / PRE-11 hold.**
- **Nothing in round 4 is reachable from a v1 cycle run by hand.**
  - `research_cycle.py`, `scripts/cycle/*`, `research_spec.py` and `cycle_resume.py` import none of `wave_kinds`,
    `wave_stage_kinds`, `wave_stage_record` or `wave_manifest` (`git grep`).
  - `cycle/cli.py` imports `research_wave` lazily, for its `wave` verb only.
  - Those files are unchanged in `ea7d9dde..80c322ef`.
- **So such a spec runs as before:** a P9-L / P9-X / P9-V spec that names `--label-terminal`, a leverage rule,
  `--risk-model`, or even `--calibrate-gross` in its own `nav.flags` reaches the argv unguarded.
- **In the wave driver**, a v1 manifest returns early from `preflight_kind` (`kind` None). `exe_problem` gets
  `rule_cell=False` for v1.
- **One consequence for root (O3):** if a hand-run cycle's spec that carries `--calibrate-gross` (P9-L, PRE-2) is ever
  named as a v2 wave's lineage parent, round 4 refuses it at preflight (`the parent spec ... (the cell inherits
  it)`). Name a plain cell of record at L' instead, the shape C2's `record_doc` writes.

## Findings
`path:line | severity | problem | fix`

**Required:** none.

**Suggested**
- **S1** `scripts/wave_stage_kinds.py:384-391` (`match_flag_list_hits`) | minor (message) | A match flag that reaches
  the cell through a NAV rule's param value is refused with the token named but not its route.
  - **Example.** `rules: [{name: vol-target-v1, params: {risk_model: "--calibrate-gross", ...}}]`.
  - **This route exists on C1's live table:** `risk_model` is an unpatterned string (`strategy_risk_target.cpp:342`).
  - **The refusal reads** `the resolved cell (no raw route names it): the cell's nav.flags carries
    ['--calibrate-gross']`. E2-14 asks for token and route.
  - **Fix.** When `touching` finds no raw route and the token is a value of the derived flags (`derived`, kind rule),
    name `rules.<name>.<param>`, or at least `rules (a param's value)`. Add the probe as an `R3A_PROBES` row.
- **S2** `scripts/wave_stage_kinds.py:421` | nit | 121 columns. The house limit for `scripts/` is 120; the line is new
  in this round. **Fix:** wrap the f-string.

**Observations** (not rated against the lane; for root / the PM):
- **O1** `scripts/wave_steps.py:153-156` (`gm_doc`, template branch) vs `scripts/wave_stage_cell.py:333-335`
  (`record_doc`) — C2 / root code.
  - A template's own `ref` without its own `leverage` follows L' in Python `pm6-6`'s `-gm`. Engine mode pins it at
    the cell's L.
  - `record_doc`'s docstring (`:325`, "as gm_doc pins it, a template-set ref too") holds only for plain specs.
  - **No dSR effect:** after E2-15 (c), a rule cell's ref is not its paired reference, and no committed template sets
    `ref`. Align at root's leisure.
- **O2** `rule_cell.constants.set inputs.<k>` passes preflight. That includes the input a template's own `ref` reads as
  its `combined` (probed: exit 0 at preflight). It is no edit of the `ref` section. It is the general freedom of a rule
  cell's registered constants over `inputs.*`, which E2-8 / E2-10 / E2-14 do not cover and no ruling assigns to E2. A
  PM ruling item only if `inputs.*` in a rule cell should be confined.
- **O3** The hand-run parent note under G.
- **O4** `ref.rule` (like `nav.rule` on the cell side, noted since round 2) is not judged. E2-15 (a) names only the
  E2-8 and E2-10 guards.
- **O5** The dotted `ref.*` refusal is stricter than the ruling's words, as described under B (b). A P10 template that
  wants a dotted ref needs a ruling.
- **Not re-raised:** the v1 rule wave with a template-set ref keeps the old verify behaviour (the lane's documented
  decision: v1 is frozen by E2-6, and P9's v1 cells run through `research_cycle.py`, not the wave driver). Also N5 /
  E2-9 (now D2R-1), S8 / S6 (E2-7), E2-6, E2-11.

## What root must check at merge
1. **The positive live-table check.** E2's `D1_SCHEMAS` stays D1's frozen shape on purpose.
   - The merged IC exe's `--list-rules --json` must equal the committed `atx-impl/tests/fixtures/
     composition_rules_list.json`, the table E2's tests read.
   - On that live output: `WK.unmapped_ic_rules(...) == []`.
   - `WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": live}) == ({'fit': {'--composition': 'ew-theme-std-v1'}},
     [])`, and MR-4's two ids derive `--theme-hedge` / `--mom-volman`.
   - `new-combination-rule.json` reaches the spec stage on the live table.
   - On C1's live NAV table, `leverage-change.json` (2.0) and `new-nav-rule.json` reach preflight. A table with no
     fixed-v1 `aim_leverage` range refuses every leverage wave.
2. **The engine stage on real exes**, P9-C's shape on `pm6-6-engine` (proved here only in the fake world):
   - the `-cal` NAV calibrates and writes `summary.json` `calibration`;
   - `leverage_requested` equals the cell's L (`wave_stage_cell.py:374`);
   - the mechanics G is bit-equal;
   - the `-gm` record is plain at L' to 4 dp, and verify pins `record_spec_digest`;
   - a named NAV rule with a literal `aim_leverage` stops at match (EXIT_PIN);
   - C3's real trade-rate rows: if a row's x-flag is `--rule <id>`, it duplicates `nav_step`'s `--rule nav.rule`;
     decide which wins.
3. **ROOT-1, the full suites.**
   - `scripts/tests` under `PYTHONHASHSEED` 0 and 1, and `atx-engine/tools/test_stage_chain.py` (separate
     invocations), plus `test_research_store_identity.py`.
   - Expected red: exactly the two e27b tests until D2R (ruling D2R-1) is merged. After D2R, 0 red, and D2R's edits to
     `test_research_spec.py` are the listed cross-lane edit.
   - E2's guard is unaffected by the fitter's `allow_abbrev=False`: it refuses abbreviations at preflight either way.
4. **The merge of E2 into root** re-proves the C2 R4 lines, S13 and MR-4; they are already in the branch, so no hunk is
   placed by hand. Run `git merge-tree` first; root's commits after `42f42a07` merge on top.
5. **O1** (C2's ref-leverage asymmetry) and **O3** (a hand-run spec with `--calibrate-gross` is no v2 parent).
6. **Re-review 2's items 3-12 still hold:**
   - B2-12;
   - the `atx.research-wave/v2` literal in `classes.json`;
   - the W1-I1 margins;
   - PM7-35's Python copy;
   - no merge while a wave runs;
   - W1-I2 (b) sequencing;
   - separate pytest invocations;
   - the `test_cycle_e2e` margin.

## Checked
- [x] **Python lane:** `.agents/cpp/agent.md` section 10 does not apply. No C++ was changed; C++ was read only for C1's
  `risk_model` schema.
- [x] **Nothing dated 2024 or later was opened, and no 2020-2023 output was read.** Probes used synthetic fake worlds,
  committed specs and fixtures. The v1 plans read no data: pool-17's `build-equity` holds no data.
- [x] **pool-17 was never edited before this file.** Mutations were made on `git archive` copies in the scratchpad.
  - No branch was switched and nothing was built.
  - Nothing in `C:/atx` or `atx-db/` was touched.
  - Every Python start had `PYTHONDONTWRITEBYTECODE=1` set at launch: no `__pycache__` was created in pool-17
    (`find -mmin`), and `git status` is clean.
- [x] **No permission refusal** this session; `git restore` was not needed.
- [x] **The report's claims match the code and my re-runs:** the merge tree, the 5+2 merge lines, the counts, the
  pre-fix bypass, the fixture table and the e27b attribution.
