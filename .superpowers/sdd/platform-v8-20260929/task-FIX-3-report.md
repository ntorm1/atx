# Lane FIX-3 report: residuals of the scoped re-review (review-w1-fixes.md)

Branch `feat/platform-v8-fix3-20260930` from `fd2ff7a8`, worktree `C:/atx-wt/pool-10`. Python only; nothing built, no
real data read. One commit per finding, in the brief's order.

| finding | commit | files |
|---|---|---|
| F-8 (M) | `df42ed69` | `scripts/research_cycle.py`, `scripts/research_add_alpha.py`, tests |
| F-1 (M) | `fe5f5396` | `atx-impl/tools/backtest_integrity.py`, `scripts/research_ledger.py`, `scripts/research_cycle.py` (doc), tests |
| F-9 (M) | `124d3d11` | `scripts/research_spec.py`, `scripts/cycle_resume.py`, `scripts/research_cycle.py`, tests |
| F-10 (M, E-27a) | `c9a154c2` | `atx-impl/tools/composition_rules.py`, `fit_composition_weights.py` (aim path), `r3-aim-gain.json` (text), tests |
| F-14 (m, E-37) | `9c53fe63` | `scripts/specs/v8/r6-spo-v3.json`, test |
| F-2 (m) | `ca1db951` | `scripts/research_cycle.py`, `scripts/tests/test_cycle_scoring.py` |
| F-3 (m) | `ac59103f` | `scripts/research_cycle.py`, test |
| F-5 (m) | `baa31ce5` | `backtest_integrity.py`, `research_ledger.py`, tests |
| F-6 (m) | `f78c9a7a` | `scripts/cycle_resume.py`, `scripts/research_cycle.py`, test |

Untouched minors (the fix is not in a file this lane edits): F-4 (`holdout_gate.py`), F-7 (C++ loader
`strategy_data.cpp`), F-11 (`scripts/cycle_admission.py`: skip the slim recipe's `trials.rescreens`), F-12 (C++
`strategy_target_replay.cpp`), F-13 (needs a ruling, then C++ and the S3 row label).

## What was built

- **F-8.** `Cycle.nav_step` adds `--label-role PATH --label-role-sha256 PIN` (and binds the manifest) to every NAV
  replay of a spec with `inputs.label_role`: nav and ref. The capacity pass forwards both. A spec with both a ref
  phase and a label role can only be an add-alpha child of a labelled parent, because templates drop their parent's
  ref (`IDENTITY_SECTIONS`). add-alpha keeps the parent's own `label_role` pin (`PARENT_PINNED`), so a label role
  rebuilt since the parent ran makes `lock` refuse with exit 3.
- **F-1.** Interface changes:
  - `check_rerun` refuses a second re-run of the same target, whatever its basis.
  - A window re-run needs a `window_id`.
  - A blind or returns re-run needs a defect line on the target that carries `ruling`. A cell's own defect flag
    carries no ruling.
  - A legacy trial_id is a target only when its line is in the ledger.
  - A declared `rerun_cell` must equal the target's cell.
  - New `pin_rerun`: `ledger_append` writes `rerun_cell` (the target line's cell) into each appended re-run line, so
    the line pins both `rerun_of` and `rerun_cell`.
  - `check_line` now accepts a defect line on a cell already ledgered invalid when the line brings a ruling.
  - `defect_line(target, reason, date, ruling)`.
- **F-9.**
  - New `research_spec.spec_digest(path, repo)`. For a plain spec it is the file's SHA-256 (unchanged). For a
    template it is the SHA-256 of the compact JSON list `["atx.research-spec-chain/v1", sha(template), sha(parent),
    ..., sha(root spec)]`.
  - The binding records `spec_rule: "spec-digest-v1"`. A binding without that key, written before this fix, is still
    compared on the template file's own SHA.
  - `check_binding` now always compares the argv digest, even when the spec digest matches.
  - The verdict's `spec_sha256` is the chain digest.
  - The plan header shows the chain digest after the file SHA, for templates only.
- **F-10.**
  - New `composition_rules.theme_gain_weights(themes, scores)`: `tier_weights`, then `member_cap` at 1/(2T).
  - `ew_theme_std` (tier score x gain) now calls it; its output is unchanged.
  - New `ew_theme_aim(ids, themes, gains)` calls it with score 1 x gain.
  - The fitter's `AIM_RULE_ID` branch calls `ew_theme_aim`. The fitter's global-normalising `ew_theme_aim_weights` is
    removed.
  - The theme table gains `capped_members`; the composition text is `AIM_V1_TEXT`. The file stays schema v1.
- **F-14.** `r6-spo-v3.json` sets `"--capacity-curve": true`. On a B0c parent this is a no-op.
- **F-2.** `validate_summ_protocol` refuses `--seed`, `--draws` and `--block` (also in `=value` form) in `summ.extra`
  of a v8 scoring step.
- **F-3.** `ledger_state` calls `check_recorded_heads`. For every `<out base>/cycle-*/cycle_verdict.json` whose
  `ledger.path` resolves to this ledger, the first `lines` lines must fold to the recorded `head`. Otherwise the run
  stops with HARD-STOP [verdict].
- **F-5.**
  - `defect_line` requires `ruling` and an ISO `date`.
  - `ledger-defect` requires `--ruling` and `--date`.
  - `appendix_a_v8` ends with `; defect lines N (cells ruled invalid after scoring)` when N > 0. The text is unchanged
    otherwise.
- **F-6.** A ref run writes `cycle_binding.json` with `spec_sha256: null` and its argv digest. A done ref is checked on
  its argv before its compare runs.

## How root verifies

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_resume.py scripts/tests/test_cycle_scoring.py scripts/tests/test_research_cycle.py scripts/tests/test_research_cycle_label_role.py scripts/tests/test_research_cycle_roles.py scripts/tests/test_research_ledger.py scripts/tests/test_research_spec.py scripts/tests/test_cycle_e2e.py
cd atx-impl/tools && "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_trial_ledger_rules.py test_backtest_integrity.py test_nav_summ_v8.py test_nav_summ_pool.py test_nav_summ.py test_holdout_gate.py test_composition_rules.py test_fit_composition_weights.py test_fit_composition_weights_pool.py test_fit_composition_weights_store.py test_mega_report_v8.py
```

Results: 166 passed, 4 skipped; 248 passed, 1 skipped.

New tests:

| finding | test |
|---|---|
| F-8 | `test_add_alpha_child_of_a_labelled_parent_labels_its_ref_and_nav`; `test_label_role_spec_key_reaches_every_nav_replay` |
| F-1 | `test_a_rerun_names_a_ledgered_target_once_and_pins_it` (one refusal each) |
| F-9 | `test_a_template_cell_binds_its_parent_chain_and_always_its_argv` |
| F-10 | `test_aim_weights_within_theme_then_capped_e27a` (two-theme fixture computed by hand, bit-equal to std-aim at equal tiers); `AimEndToEnd` on a two-theme world |
| F-2 | added refusal cases in `test_v8_summ_step_carries_the_protocol_and_the_origin` |
| F-3 | `test_a_verdict_checks_every_recorded_ledger_head` |
| F-5 | assertions in `test_a_defect_found_later_is_a_defect_line` and `test_ledger_defect_marks_a_ledgered_cell_invalid` |
| F-6 | `test_a_done_ref_feeds_its_identity_only_when_made_by_the_ref_command_of_now` |

**Identity with every flag off:**
- NAV and ref argv are unchanged for specs without `label_role`.
- A plain spec's digest equals its file SHA-256, so the verdicts and bindings of B0a and B0b keep their values. The
  binding file gains the key `spec_rule`.
- ew-theme-v1, ew-theme-std-v1 and ew-theme-std-aim-v1 weight bytes are unchanged (the V6-W byte test and the std
  tests pass).
- The v8 Appendix A block is unchanged without a defect line.
- v7 summ argv and v7 specs accept `--draws` as before.

## Deviations (with reasons)

- **F-1, window re-runs.** The brief's "whose own line carries a defect line with a ruling id" is applied to blind and
  returns re-runs. A window re-run re-scores a valid cell on the longer window (prereg item 2, declared deviation of
  C-4), so it needs no defect. It now needs a window_id, at most one re-run per target, and the pin.
- **F-1, own-flag invalid cells.** A cell ledgered invalid at once (`nav_summ --ledger-defect`) needs a ruled defect
  line before its re-run. nav_summ cannot pass a ruling, and nav_summ is not this lane's file. `check_line` therefore
  admits that line when it brings the ruling.
- **F-1, rerun_cell.** `rerun_cell` is filled at append time, because nav_summ has no option for it. A caller that
  sets it must name the target's cell.
- **F-6.** The ref binding is on the argv only. A spec-digest binding would force a full `--suffix` re-run after any
  unrelated spec edit.
- **F-14.** The template sets the flag explicitly instead of only deleting `false`, so R-6 has the 4x report on any
  parent.

## Cross-lane and out-of-list edits

- `scripts/research_ledger.py`: `ledger-defect` gains `--ruling` and `--date`.
- `atx-impl/tools/composition_rules.py`: the shared implementation that F-10 requires ("no copy"). Lane COMB2 may edit
  the same module.
- Test fixtures only: `atx-impl/tools/test_nav_summ_v8.py` (defect lines carry ruling and date; re-runs of own-flag
  invalid cells get the ruled defect line) and `scripts/tests/test_cycle_scoring.py` (patches `NS.V8_DRAWS` instead of
  passing `--draws`).

## Open risks

1. **E-27a reuses the v5 id `ew-theme-aim-v1` for a new rule.**
   - v5 R4' weights can no longer be reproduced from the current fitter; they are in git at 04e9d5bc.
   - On an ew-theme-v1 parent the 1/(2T) cap binds on single-member themes, which ew-theme-v1 never capped, so R-3
     there moves weight between themes through the cap as well as through the gains.
   - When the admitted members M < 2T the cap has no fixed point and the fit is refused, as ew-theme-std-v1 is.
   - The PM may want a new id or a note in the ruling.
2. **Existing nav_summ limit (not fixed here).** `--rerun-of` / `--ledger-defect` with a multi-dir summ
   (`cells_from_ledger`) apply the flags to every listed dir, and `check_line` refuses the prior cells. Re-runs must be
   scored with the re-run cell alone.
3. **C-4's example is narrowed, not closed.** A new construction cell can still claim to be the first window re-run of
   a v7 cell that has not been re-run yet. The claim is now once-only and names its target's cell in the ledger.
4. **R-6 depends on lane FIX-2.** The R-6 template carries `--capacity-curve`; spo-v3 accepts it only after FIX-2's
   N-2 merges.
