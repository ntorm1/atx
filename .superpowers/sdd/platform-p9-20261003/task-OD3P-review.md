# Lane OD3P review (first review, RESUME 8)

## Verdict

**CHANGES_REQUIRED**: 1 Required, 6 Suggested. The code, the contract and ALCLOCK-2 are sound. The one Required
finding is in the runbook: its B6 spec recipe, followed literally, cannot carry the weights, so the single OD-3 read
would HARD-STOP at `w:E1` after the history u passes had already run.

## Reviewed SHA

- HEAD `d78d23ce` (pool-21, `feat/p9-od3p-20261004`). BASE `1b64ce45` = `git merge-base d78d23ce
  feat/platform-p9-20261003` (REV-BASE). This is also root's current head (`git -C C:/atx-wt/pool-2 log --format=%h -1`).
- Lane range `937865f0`..`d78d23ce`: 10 commits, 4 of them root merges. Every merge is plain, meaning its tree equals
  `git merge-tree --write-tree <^1> <^2>`: `70439eef`, `e9bedcc7`, `e809271f`, `09e2350a` all PLAIN.
- The code at HEAD is the code the lane's suites ran on: `git diff e9bedcc7 d78d23ce -- . ':(exclude).superpowers'`
  is empty.
- Files in the range: `atx-impl/tools/carry_weights.py` (new), `atx-impl/tools/test_od3_tooling.py` (new),
  `scripts/research_roles.py`, `scripts/cycle/cli.py` (+3 lines; E2's file, listed as a cross-lane edit),
  `scripts/specs/p9/od3/flagged_members.json` (new), `scripts/tests/test_research_cycle_od3.py` (new), and sprint-dir
  records. There is no C++, no kind file, no file of B2's, no IC runner file and no `classes.json` edit.

## Spec verdict (r7-dispatch "OD3P" 1-7, r8-dispatch "OD3P" 0-1, r8-review-notes "OD3P")

| item | verdict | evidence |
|---|---|---|
| 1 merge root, re-run the two files | done | All 4 merges are plain. My run on HEAD: 61 passed. |
| 2 OD3P-1, carry tool | done | The carry zeroes nothing: every era file is W's document with only `train_manifest_sha256` swapped and `carried_from` added, and the anchor file is W's bytes (`carry_weights.py:196-208`). With a list, `carry.json` only gains the `drop` record. The `theme_standardise` refusal is gone, and the theme-erc-v1 / ic-shrink-v1 bases are tested as accepted. My mutation V2 (strip the listed members' signs) was CAUGHT. |
| 2 OD3P-1, kept refusals | done | Unknown, repeated or malformed id; empty list; pending rules; bad schema or source; pin; a base applying a listed rule; non-v1/v2 weights; no `--era` (`carry_weights.py:104-171`, `:192-195`). Each refusal publishes nothing (tested). "Unknown" means not a key of W's `weights`. The fitter writes a weight row for every library candidate (`fit_composition_weights.py:2163-2177`, `:2213`, `:2547`), so "unknown" equals "not a library candidate", which is consistent with the contract's exe refusal. |
| 2 OD3P-1, cycle | done | With `fit.carry.drop`, the u and w passes of every `is_history` role get `--bind <list>` as the runner's last bind before `--`, and the exe argv ends with `--absent-members <list> --absent-members-sha256 <pin>` (`research_roles.py:389-409`). TRAIN roles, NAV, fields and summ get nothing; the carry gets `--drop-list`. The cycle refuses (exit 2, at plan) an exe without `absent-members-v1`. This fails closed: a missing exe, a failing exe or a non-envelope output offers nothing (`scripts/cycle/phases.py:45-60`). My mutation V1 (probe the wrong capability) was CAUGHT. It also refuses a drop with no history role, and a nav / ref flag or `nav.rule` naming a listed rule. |
| 2 contract `od3p-absent-contract.md` | done | Complete and consistent with OD3P-1: list schema `atx.od3-flagged-members/v1`; the exe reads only `schema` and `members`; flag spelling, both-or-neither, at most once; capability in the `capabilities` array of `--list-rules --json` (exactly what `phases.py:45-60` reads); the defining equivalence in OD3P-1's own words; where the cycle puts the flags, which matches `research_roles.py:389-409` line for line, including the synthetic argv. I spot-checked its C++ citations, and each is present within one line: `strategy_ic_rules.cpp:545-546` (`exe_capabilities`), `:387-393`, `strategy_ic_composition.cpp:314-333`, `strategy_ic_runner.cpp:118-119`. Lane ABS's brief takes sections 2-5 as its spec. |
| 3 ALCLOCK-2 | done | `rules` = [theme-cov-hedge-v1, mom-volman-v1, info-clock-rate-v1], with `rule_source_sample_end` 2017 / 2016 / 2019. `rules_pending` = [mv-aim-v1]. The test pins both, and checks that the list as committed is refused until root resolves it. |
| 4 runbook | done, one defect (R-1) | Every required line is present. Four history lines (section 1). G-B8 on E1+E2, with 2015-2023 printed only (R4). The era fields argv rule (B7, with self-check). `holdout_gate.py` never runs (0.2). The identity-bridge flag on every print, verbatim equal to prereg C.1 / ALD-9 (0.3). History NAVs only through the cycle (0.4). "not read: no producer" (sections 1, P0, 4.1, 5). Every value not yet known is marked `[UNKNOWN: ...]`. |
| 5 suites | done | From the lane's logs on `e9bedcc7`; the code is unchanged since. |
| 6 T2 | pending by rule | B2's redo is not in root at `1b64ce45`. Not a finding. |
| 7 report, handoff | done | "For PRE" (Appendix IV item 32) is at `task-OD3P-report.md:106-115`. "For root" (the identity procedure) is at `:117-134`. |
| R8 0 DIRTY-1 | done | `52805177` "docs(od3p): handoff at the owner stop (R7 round)" commits the handoff, the report and the 6 R7 logs. These are sprint-dir records only. |
| R8 1 merge | done | `70439eef` (root `1b64ce45`, docs only, not a no-op): plain, lane files re-run with 61 passed. The contract and the runbook are tracked. |

Further trace items:

- **Flagged rules reach a refusal.** theme-cov-hedge-v1 and mom-volman-v1 are fit-side rules
  (`fit --theme-hedge` / `--mom-volman`). Their writers record `theme_hedge.rule` (`composition_theme_hedge.py:10`, `:22`) and
  `theme_schedule.rule` (`composition_mom_volman.py:96`), and the carry tool's `applied_rules` finds both. The test's
  `flagged_rule_block` case has the real shape. NAV-side rules (`--rate info-clock-rate-v1`) are caught by the
  cycle's `check_drop_rules`.
- **Records.** `v8-od3-protocol.md` (blob `294d66b1`, sha256 `640cd91e...c9bb`) and `task-XPRE-report.md` (blob
  `a40be229`) equal the `a7671d1a` blobs.
- **Blind.** The runbook and the report hold no 2020-2023 statistic; I read both in full. The disclosure that the
  protocol record quotes two v8 TRAIN figures of R-2 (none used) survives only in the round-1 handoff (S-5). I read
  those lines with the numbers masked.
- **Drop list.** All 12 ids are candidates of Y-F0's library (67 candidates, sha256 prefix `41010b0b`). The ids equal
  prereg Appendix III, the file is `eol=lf`, and its current sha256 is
  `9aa6fb596a4edd0b2a8028a337678161efdfa50535d61dbe9c18fdff9f56f5ff`.

## Quality verdict

Good, apart from R-1:

- **Flag-absent identity.** Proven by tests against pinned blobs:
  - `research_roles.py` `313b23b3`, which is also root `1b64ce45`'s file: plan, status, argv and lock, on four specs.
  - The round-1 module `8081a6d0`.
  - The branch tool `186655d9`: every byte and the refusal text.
- **Data safety.** There is no look-ahead and no seal exposure: no data is read and no return is opened. No pinned
  value or golden was edited. No Python copy of a C++ rule exists (G-P5: `test_no_python_mirror.py` 7 passed).
- **Determinism.** `publish` writes in sorted order, and the record's members and rules are sorted. Sets are used for
  membership only.
- **Error paths.** Every carry refusal is raised before `publish`. Publication is pending-then-rename, with rmtree on
  failure.
- **Weaknesses.**
  - The drop-list content checks run only at the fit step (S-1).
  - "history only" is untested against a non-anchor TRAIN role (S-2).
  - One silent skip (S-3).
- **Observation, pre-existing (T1), not a finding.** In a carry cycle the u passes still run on the history eras
  before the carry and feed nothing; they print nothing.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `.superpowers/sdd/platform-p9-20261003/p9-od3-runbook.md:126` (B6 `fit` bullet), `:139-143` (P7) | **Required** | B6 keeps "the book's own [fit] section", so `fit.output` stays the book's TRAIN fit dir. That is the dir holding the carried `composition_weights.json`; for Y-F0, `scripts/specs/v8/lib-v8ysb-gm.json` has `fit.output` = `build-equity/mega-weights-v8x-theme-erc-v8ysb`, with no `out_root`. `RolesCycle.carry_step` marks the carry `done` when `<fit.output>/composition_weights.json` exists (`scripts/research_roles.py:381`). The carry therefore never runs: no `composition_weights.E1/E2.json` are written, and on line 4 the carry tool's drop-list checks never run. `run` then HARD-STOPs at `w:E1`, after the fields and the history u passes have already run. I reproduced this on the lane's own fakes (probe below: `fit [bounded; done]`, then `HARD-STOP [w:E1]: an upstream pin is unresolved`). P7 does not catch it, because the plan's "fit = carry" line just shows `done`. The text is inherited from the v8 protocol's B6, but this runbook wins over that record. | In B6, set `fit.output` to a fresh dir per book (`build-equity/od3-fit-<book>`), never the TRAIN fit dir. Lines 2 and 4 must get distinct dirs even when both carry Y-F0's TRAIN fit; otherwise line 4's carry is "done" from line 2 and the drop list is never checked or recorded. In P7, STOP unless the plan shows phase `fit` as `pending`, with the carry argv's `--output` equal to that dir and, on line 4, ending in `--drop-list ... --drop-list-sha256 ...`. |
| `scripts/research_roles.py:317-345` (`check_carry`) | Suggested | The drop-list content checks (pending rules, schema / members, ids that are not keys of W, a flagged rule in W) run only inside the carry tool. That means at the fit step, after the history u passes. The cycle already reads W and the list at plan time. A `fit.output` that already holds `composition_weights.json` is also accepted, which is R-1's mechanism. | Call `carry_weights.drop_list` / `check_drop` from `check_carry` (import them; no copied logic). Refuse a carry whose `fit.output` already exists. Every refusal then lands at plan / P7. |
| `scripts/tests/test_research_cycle_od3.py:281` | Suggested | Test gap: "history roles" is never tested against "non-anchor roles". Mutating `research_roles.py:396` to `history = list(self.eras[:-1])` passed all 12 drop tests (probe P1, NOT CAUGHT). The code is right; the OD-3 specs' non-anchor eras are all history. | Add a three-role case (E1 history, E2 inside TRAIN and not the anchor, E3 anchor), asserting that E2's u / w argv carry no flag and no bind. |
| `scripts/research_roles.py:406` | Suggested | `absent_members` silently skips an IC step that has no runner `--`. That would fail open if a step's shape ever changed. Today every u / w step is runner-wrapped (`scripts/cycle/phases.py:571-607`). | Raise `CycleError` instead of skipping. |
| `.superpowers/sdd/platform-p9-20261003/p9-od3-runbook.md:197` | Suggested | The prediction reads "P9-F0's history S2 net Sharpe vs its TRAIN value + P9-W's dSR". PRE-39 registers it on P9-F0h. The runbook defers to the ledger, so the read would not go wrong, but the print text should match. | Change to "P9-F0h's history S2 net Sharpe = its TRAIN S2 net Sharpe + P9-W's dSR (PRE-39)". |
| `.superpowers/sdd/platform-p9-20261003/task-OD3P-report.md:148-160` | Suggested | The round-1 blind disclosure exists only in the overwritten round-1 handoff (`80484f0b:.../handoff-OD3P.md:50-51`). It says that the committed `v8-od3-protocol.md` record quotes two v8 TRAIN figures of R-2, none used, and that a root subject carried Q5 q-values, none used. | Restate it in the report, under "Deviations" or a "Blind" line. |
| `.superpowers/sdd/platform-p9-20261003/task-OD3P-report.md:91-93` | Suggested | The proposed class for `atx.od3-flagged-members/v1` is `cycle-spec`, whose ingest is `spec_doc` with `spec_kind` `spec`. The catalog would therefore ingest the drop list as a cycle spec: `spec_pins` / `spec_follow` run on it (`atx-engine/src/research/store/catalog/ingest_spec.cpp:104-115`). This is harmless (no output, name or inputs keys), but the row is mislabelled. The guard passes either way (checked in memory below). | Root's choice: keep it as proposed, or give it its own class before `other` (globs `**/od3/*.json`, ingest `spec_doc`, `spec_kind` `other`, like `diagnostic-spec`). |

## Evidence

**My own runs** on HEAD `d78d23ce` in POOL. All used `C:/Program Files/Python312/python.exe`,
`PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider` and my own `--basetemp`. No ENV-1 exit occurred and nothing was
refused by the permission system.

```
$ python -m pytest scripts/tests/test_research_cycle_od3.py atx-impl/tools/test_od3_tooling.py -p no:cacheprovider --basetemp <mine>/bt-lane -q -rs
61 passed in 48.37s            (exit 0)
$ python -m pytest scripts/tests/test_no_python_mirror.py -p no:cacheprovider --basetemp <mine>/bt-mirror -q
G-P5: 34 mirrored rule function(s) in 13 row(s) left for P9 lanes
7 passed in 8.59s              (exit 0)
$ python -m pytest atx-engine/tools/test_research_store_classes.py -p no:cacheprovider --basetemp <mine>/bt-cls -q   (read-only, extra)
- {'atx.carry-weights/v1': ['atx-impl/tools/carry_weights.py'],
-  'atx.od3-flagged-members/v1': ['atx-impl/tools/carry_weights.py']}
1 failed, 4 passed, 96 subtests passed   (the expected SQL2-CLS red: exactly the two lane literals)
in memory, classes.json + the two proposed rows: unregistered {} ; shared_literal_problems [] ;
classify scripts/specs/p9/od3/flagged_members.json -> cycle-spec ; <fit.output>/carry.json -> fit-output
```

**Teeth.** These are working-tree mutations of the CODE under test, run by my own script, with each file restored
byte-identically. The SHA-256 of both files was equal before and after, and `git status --porcelain` showed 0 lines
afterwards.

```
V1 capability check reads the wrong capability ("list-rules" for ABSENT_CAPABILITY, research_roles.py:397):
   CAUGHT (exit 1; 2 failed, 2 passed) [injected], [probe_without]; restored byte-identical sha256 f55248b6f4b0
V2 carry strips the listed members' signs in the era files (carry_weights.py:205):
   CAUGHT (exit 1; 3 failed) ew-theme-std-v1 / ic-shrink-v1 / theme-erc-v1; restored byte-identical sha256 e1facc6de92b
P1 probe: non-anchor eras instead of history eras (research_roles.py:396 -> list(self.eras[:-1])):
   NOT CAUGHT (exit 0; 12 passed, -k drop); restored byte-identical   -> S-2
```

**Probe for R-1** (synthetic). I used the lane's `drop_root` fake world from my scratch script; no repo file was
changed. The `same_dir` case sets `fit.output` to the dir holding the carried `composition_weights.json`.

```
== distinct (fit.output 'out/W'): fit pending; run --stop-after w: exit 0; calls fields, check, U-E1, U-E2, W-run1, WT-E1, WT-E2
== same_dir (fit.output 'fit'):
   fit  None  state=done     ['fit/composition_weights.json']
   w    E1    state=pending  ['fit/composition_weights.E1.json']
   # phase fit [bounded; done; fit.carry: TRAIN admission and weights carried to every era, ...]
   run: cycle.spec.CycleError: HARD-STOP [w:E1]: an upstream pin is unresolved (upstream output missing)
        (after fields F-E1, F-E2, check, U-E1, U-E2 had run)
```

**From the lane's logs only:**

- Whole `scripts/tests` under seeds 0 and 1: 543 passed, 4 skipped each.
- `test_stage_chain.py`: 9 passed.
- The lane's 22 teeth mutations (`od3p-evidence/teeth-r7.log`).

All of these ran on `e9bedcc7`, whose code equals HEAD's.

## What root must check at merge

1. **R-1 closed by a re-review.** Root checks that B6 names a fresh `fit.output` per book (lines 2 and 4 distinct)
   and that P7 stops unless `fit` is `pending`. Optionally S-1's plan-time guard is in place too.
2. **SQL2-CLS rows in `atx-engine/schemas/research_store/classes.json`.** I checked both against the file at HEAD.
   Both classes exist, with globs `**/*.json`, a `schemas` array and format `json`. Neither literal is in any class,
   `legacy_allow` row or `domains` row. Adding both makes the guard green, with no shared-literal problem.
   - `atx.carry-weights/v1` goes into class `fit-output` `schemas`. `carry.json` is published in `fit.output` beside
     the carried `composition_weights*.json`, and its ingest is `artifact-only`. Extend the class's `writer` with
     `atx-impl/tools/carry_weights.py`.
   - `atx.od3-flagged-members/v1` goes into class `cycle-spec` `schemas`, as the lane proposes. The file is
     `scripts/specs/p9/od3/flagged_members.json`, pinned by `fit.carry.drop.sha256`. Alternatively give it its own
     `spec_kind: other` class (S-6).
   - Until the rows land, `test_every_schema_literal_is_registered` is red on exactly these two literals.
3. **Identity procedure** (report "For root").
   - Re-run the lane files, then the whole `scripts/tests` under seeds 0 and 1, then `test_stage_chain.py`
     separately. There is no build and no exe.
   - The identity tests pin root's `research_roles.py` blob `313b23b3`. If another lane changes root's
     `research_roles.py` before this merge, that test must be re-derived by the lane; root never edits the pin.
4. **Before the read (runbook P0).**
   - Resolve `mv-aim-v1` from COV-MV's registration (into `rules` with its year if that year is 2015 or later, else
     removed) and re-pin the list. Its current sha256 is `9aa6fb59...f5ff`. `rules_pending` must be empty.
   - T2 is pending on B2's redo, and OD-3 waits for it (PRE-21).
5. **After ABS merges.**
   - `<frozen ic exe> --list-rules --json` lists `absent-members-v1`.
   - The od3 cycle's plan of a `fit.carry.drop` spec shows the E1 / E2 u and w argv ending in the two flags, and E3
     without them.
   - Without ABS, line 4 prints "not read: no producer".
6. **Lane open question 1 (u and w passes).** The r8 trace and the ABS brief both state "u and w". No change is
   needed unless the PM rules otherwise.

## Checked

- [x] `.agents/cpp/agent.md` section 10: not applicable (no C++ in the lane).
- [x] Diff stays inside the lane's scope. The cross-lane `scripts/cycle/cli.py` hunk (3 lines) is listed. D2R-2 (a)
  correctly left: the stale text is in `scripts/cycle/spec.py`, which is B2's file and was not edited.
- [x] Evidence in the report matches its claims. I re-ran the lane files, the mirror guard and the classes guard,
  re-did two teeth, and checked the records' blobs and the list's ids and pins.
