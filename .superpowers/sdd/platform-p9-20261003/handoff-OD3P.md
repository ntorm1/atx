# Handoff OD3P (lane, pool-21): stopped by OWNER STOP, 2026-10-04

- Branch `feat/p9-od3p-20261004`, lease run id `p9-od3p-20261004` (leases not touched). Base `5c57c1ef`.
- Code commit `937865f0` (feat(od3p)); merge of root `9c7aa654` (C3 part A in) = `09e2350a`, no conflict (root
  changed none of my files). The head is the commit that adds this handoff, the records and the evidence.
- No build, no exe, no real data, no statistic read. `holdout_gate.py` never run.

## Required items (dispatch 1-7)

1. **Carry tool, roles hunks, tests: DONE.** `atx-impl/tools/carry_weights.py` (the branch file, sha256 `5aebb3a7`,
   plus the drop list); `scripts/research_roles.py` T1 `fit.carry` + T4 `roles[].label_dir` hunks (as on the branch);
   the T1 refusal in `scripts/cycle/cli.py` `make_cycle` (3 lines after the roles branch: the listed cross-lane E2
   edit); tests `atx-impl/tools/test_od3_tooling.py` (branch T1 tests unchanged + new) and
   `scripts/tests/test_research_cycle_od3.py` (branch T1/T4 tests + new). Identity tests: no `--drop-list` = every
   output byte of the branch tool (blob `186655d9`); no new keys = plan / status / argv / lock of `research_roles.py`
   blob `313b23b3` (root's file before this lane), on four specs (TRAIN split, one role, a history pool, a risk-model
   book inside TRAIN).
2. **Drop list and clean-source base: DONE, with a design concern.** `scripts/specs/p9/od3/flagged_members.json`:
   the 12 ids in FINAL FLAG LIST order, source sha256 `7a089e52...` (the committed LF blob at `5c57c1ef`; a CRLF
   checkout hashes `4acafa73`), `rules` = theme-cov-hedge-v1, mom-volman-v1, `rules_pending` = info-clock-rate-v1,
   mv-aim-v1. The tool refuses the list while `rules_pending` is non-empty (root resolves it from the registrations).
   `--drop-list L --drop-list-sha256 S`: dropped members weigh a pinned 0 on the history eras only; kept weights,
   signs and theme blocks are TRAIN's bytes; the anchor file is TRAIN's. It refuses: an id not a key of W's
   `weights`, a repeated or bad id, a base that applies a listed rule (any `rule` string in W), a non-v1/v2 doc, no
   `--era`, and a `theme_standardise` rule other than ew-theme-std-v1. Spec key `fit.carry.drop {path, sha256}`:
   lock pins it, the plan shows `carry:drop`, and the carry argv gets `--drop-list`. The cycle refuses a nav.rule /
   nav.flags / ref.flags token naming a listed rule.
   **CONCERN for the PM (open question 1):** the IC runner needs a weight for every library candidate
   (`strategy_ic_admission.cpp:667-670`), so "absent" can only be a pinned 0, which the composer skips (`:316-318` of
   `strategy_ic_composition.cpp`). A pinned 0 also takes the member's weight out of its theme's mass (`:256-257`,
   `:263-265`). The coded present-member rule for a member with no data keeps the mass in the theme
   (`strategy_ic_rules.cpp` text "missing-or-unoriented-mass-stays-in-theme"). On top of that, theme-erc-v1 and
   ic-shrink re-derive and verify the member weights from their block (`strategy_ic_rules.cpp:200-280`; shares must
   sum to 1 per theme, `theme_erc.cpp:38-42`), so the runner refuses any zeroed member. Y-F0, and so probably the
   clean-source base, is theme-erc-v1 (`scripts/specs/v8/lib-v8ysb-gm.json` fit flags). The tool therefore refuses it
   up front. To be faithful, the drop needs a small C++ runner rule: an absent-member list that is skipped in add()
   while masses and verify keep the pinned weights. That is outside this Python lane. The PM rules.
3. **T2 on the verb: PENDING on B2.** B2 is not in root (root `9c7aa654`; B2 redo blocked / follow-up in pool-14).
   To do after B2's merge: in `nav_summ` (B2's file), `--pool-one-sided` calls `engine.paired_of(..., one_sided=True)`
   on the pooled pair plus the verdict dict (as `bundle()` does). Tests go against a fake verb, and against the real
   one if built: the branch's T2 test, the T5 test and the flag-absent identity test, all re-done on the verb. T3 is
   dropped.
4. **`--risk-model` refusal: DONE.** `research_roles.py` `check_history_risk_model`, called first in `roles_cycle`. It
   applies to a spec with any history role whose nav.flags or ref.flags carry `--risk-model*` (also in the `=` form),
   exits 2, and names PRE-10 and P9-F0h. Inside TRAIN the cycle still plans. Gap: a hand NAV run through a C3 NavSpec
   `--spec` with `options.risk_model` is not seen; the cycle never writes `--spec`.
5. **Records: DONE (uncommitted until this commit).** `LD/v8-od3-protocol.md` from `a7671d1a` (last changed in
   `846941d2`): sha256 `640cd91e...c9bb`, blob `294d66b1` (= the source blob). `LD/task-XPRE-report.md` from
   `a7671d1a`: sha256 `e778bd3d9b4e5d059fbe14235a2fbdc286513dd151a108ee04c3a663068058d3`, blob `a40be229` (= the
   source blob). Disclosure: the protocol (read as the dispatch required) quotes two v8 TRAIN figures of R-2 in
   sections 2 and 3.5; none was used. A one-line root subject seen in `git log --format=%s` carried M2g Q5 q-values;
   none was used.
6. **Runbook `LD/p9-od3-runbook.md`: NOT STARTED** (facts gathered, below).
7. **No G-P5 mirror / identity: PARTLY.** No Python statistic was added; `nav_summ` is untouched (T2 is pending).
   `test_no_python_mirror.py`, the whole `scripts/tests` (seeds 0 and 1) and the stage chain were NOT run. The
   identity procedure for root is not yet written in a report.

## Tests actually run (pre-merge tree `937865f0` content; not re-run after the root merge)

- `atx-impl/tools/test_od3_tooling.py`: 29 passed.
- `scripts/tests/test_research_cycle_od3.py`: 24 passed.
- Teeth: 14 mutations, each caught and restored byte-identically (`LD/od3p-evidence/teeth.log`,
  script `LD/od3p-evidence/mutate.py`).

## Next step for a fresh agent

1. Re-run the two files above on the merged head, then the whole `scripts/tests` under PYTHONHASHSEED 0 and 1,
   `atx-engine/tools/test_stage_chain.py` separately, and `test_no_python_mirror.py`. Root's known-red set is empty.
2. Write `LD/p9-od3-runbook.md` (dispatch item 6) from `v8-od3-protocol.md` section 5 and prereg F.1 (root's working
   copy is being edited by PRE round 2a; F.1 is now at about :798). Facts found:
   - TRAIN fields v15 receipt `build-equity/train-2020-2023-lo3-fields-v15-run/start.json`: `command` is
     `python -c "<prepare_research_fields + draft/xdata/ohlc/ydata register>"` with `--fields` of 84 names (its order
     differs from the spec's `fields.list`), `--reuse build-equity/train-2020-2023-lo3-fields-v15a`
     (+ `--reuse-sha256`, `--reuse-hardlink`) and `--mgr13f-stage`.
   - The era argv is that command with only `--role`, `--role-sha256` and `--output` changed and the `--reuse*`
     flags removed. STOP on any other diff.
   - P9-F0h's fields dir is UNKNOWN until the cells run (a cell may add a field).
   - Y-F0 fields: `train-2020-2023-lo3-fields-v15`, pin `26fee5ce...`. Role `e1c67101...`. dlret label role
     `95e16cfe...`.
   - TRAIN role receipts: `train-2020-2023-base-v1-run`, `-base-run`, `-lo3-run`, `-lo3-dlret-run`.
   - Tool flags were checked in the tree: `prepare_recent_research.py` (`project` / `role`, `--delisting-returns`),
     `era_data_audit.py` (`--era` x5, `--library`, `--plan`, `--json`).
   - The runbook must also cover: the four history lines (P9-F0h, Y-F0, R-2, the clean-source book), G-B8 on E1+E2,
     `holdout_gate.py` never, the identity-bridge flag (OD5-1 / ALD-9 text) on every print, the pre-read step "root
     resolves `rules_pending` and re-pins the list", the drop-concern stop, and every value unknown until the cells
     have run.
3. Write `LD/task-OD3P-report.md` (Lane report form). New literals: `atx.od3-flagged-members/v1`,
   `drop-flagged-members-v1`; spec key `fit.carry.drop`; carry flags `--drop-list` / `--drop-list-sha256`. D2R-2 (a)
   is not done: the stale text now sits in `scripts/cycle/spec.py` (C3b's per status-6).
4. Merge root again; do T2 if B2 is in root, else leave it pending.
5. Delete scratch `.../scratchpad/od3p` by name.
