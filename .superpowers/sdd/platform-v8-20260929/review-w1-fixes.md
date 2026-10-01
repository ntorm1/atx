# Review W1 fixes (scoped re-review): FIX-C, FIX-AB, R45 session 3, A2 follow-up

Snapshot: `C:/atx-wt/pool-2` at `b44774d6`, range `d22c8e99..b44774d6`. Every file was read with `git show b44774d6:<path>`
or `git diff d22c8e99 b44774d6`. The working copy was not read, because a mining-verb merge was in flight. This review
is read-only: nothing was built or run, and nothing dated 2024-01-01 or later was opened.

Severity follows part 1: **I** important, **M** medium, **m** minor. "Verified" means the claim follows from code I
read. Anything else is marked unverified, with what would settle it. Findings use the ids F-1..F-14 in one sequence:
- F-1..F-7 are residuals or side effects of the fix lanes (part 1).
- F-8..F-14 come from the first read of R45 session 3 and the A2 follow-up (part 2).

---

## Part 1: the Wave 1 I and M findings and Ruling E-33

Line numbers are at `b44774d6`. "Rule" means the binding rule in `task-FIX-C-brief.md` / `task-FIX-AB-brief.md`.

| id | sev | verdict | where the fix is | rule | note |
|---|---|---|---|---|---|
| C-1 | I | ADDRESSED | `scripts/research_cycle.py:1003-1038` (summ step: a verdict spec passes `--dsr-ledger <ledger>`; without a ledger it is refused at plan time, EXIT_USAGE); `scripts/cycle_verdict.py:77-96` (`dsr_block` reads only `deflated_ledger`, else `VerdictError` -> HARD-STOP `research_cycle.py:1648-1654`); N and V[SR] from `backtest_integrity.ledger_n` / `dsr_variance` (`:1047-1069`) | matches | `scripts/tests/test_cycle_scoring.py` reproduces the review's example (SR 1.0, T 1,006, N 41): ledger DSR .91 / SR0 .33 vs Lo .42 / 1.10. `--ledger` is appended before `--dsr-ledger` is read (`nav_summ.py:905-941` before `:982-993`), so the cell is in N and V (C-16 is moot inside the cycle) |
| C-2 | M | ADDRESSED | `research_cycle.py:405-427` (`summ_protocol`, `validate_summ_protocol`: a verdict spec may not ask for another protocol; `summ.origin` or `--origin`, not both), `:1040-1052` (`v8_summ_flags`: `--protocol v8`, `--origin summ.origin`, refusal without it); `research_add_alpha.py:206-211` (child: verdict true, `summ.origin` = wave origin, the parent's `--origin` dropped) | matches | argv pinned in `test_v8_summ_step_carries_the_protocol_and_the_origin` and `test_add_alpha_from_a_v8_parent_inherits_the_v8_protocol`. Residual F-2 (seed / draws can still be overridden) |
| C-9 | M | ADDRESSED | `atx-impl/tools/compare_window_overlap.py:250-266` (`reason`; `bit_identical` needs at least one compared cell), totals (`keys_without_cells`, reason) | matches | `test_zero_cells_compared_is_never_identical` covers both no-session and no-instrument |
| C-10 | M | ADDRESSED | `:234-247` (`max_rel_diff`), `:515-527` (`w0a_class`: any NaN mismatch, missing old cell or key without cells is a stop), stdout adds `nan_mismatch_cells`, `w0a_class`, `reason` | matches | `test_nan_against_value_is_a_mismatch_and_nan_against_nan_a_match`: NaN/NaN equal, NaN/value a stop even when max_abs_diff < 1e-9 |
| C-3 | M | ADDRESSED | `backtest_integrity.py:787-813` (`check_line`: flags that would be dropped on a ledgered trial_id raise, so nav_summ exits non-zero), `:734-747` (`defect_line`), `invalid_ids`; `research_ledger.py` `defect_main`; verb `research_cycle.py ledger-defect` | matches | Residual F-5 |
| C-4 | M | ADDRESSED | `backtest_integrity.py:815-828` (`check_rerun`: target must be an earlier cell line of the same kind, not an event or era line; a window re-run's target must be on another window_id or have none) | matches. Declared deviation: a window re-run needs no defect (prereg item 2) | Residual F-1: the review's own example still adds 0 when it names a legacy trial_id |
| C-5 | M | ADDRESSED | `:829-833` (a blind or returns re-run needs a defect of its target on an earlier line); `trial_counts` `:939-960` (the replaced invalid cell counts 1, the blind re-run 0, a returns re-run is a new trial) | matches ("a rerun never lowers N") | totals on consistent ledgers equal the old rule's (test). Residual F-1 |
| C-6 | M | ADDRESSED | `backtest_integrity.py:845-905` (`fold_head`, `chain_head`, `_walk`: legacy lines fold from 64 zeros in stored form, the first chained line pins them, an unchained line after a chained one is refused); `research_cycle.py` `ledger_state` (verdict `ledger{path, head, lines}`), `copy_ledger` logs the head | matches | The root ledger has 0 chained lines, so no v1-chained ledger breaks. Residual F-3 |
| C-7 | M | ADDRESSED | `scripts/cycle_admission.py:51-86` (one chained count-1 admission line per gate-listed candidate with an admission row; trial_id = candidate, DSL, role, window, so a resumed gate or full run adds nothing); `research_cycle.py:1636-1646` (before the gate, v8 cycles with a ledger); `appendix_a_v8` `backtest_integrity.py:1030-1044` counts them | matches | Interaction with A2's re-screens: F-11 |
| C-13 | M | ADDRESSED | `scripts/cycle_resume.py:47-83` (`cycle_binding.json` written after a NAV run; on resume the spec digest, else the argv digest from the binding or the completed receipt; a mismatch is EXIT_PIN naming both digests); `research_cycle.py` `run_cycle` (nav phase) | matches | Residuals: F-6 (ref phase unbound); F-9 (template specs: the digest is the template file only) |
| C-11 | M | ADDRESSED | `atx-impl/tools/holdout_gate.py:209-292` (`--ledger` required; a chained `validation` line {ruling path, ruling SHA-256, thresholds SHA, deploy SHA} is appended before any NAV series is opened; a ruling whose bytes differ from a cited SHA for the same path is refused, exit 2) | matches | Bisection is visible (one line per ruling), not blocked; FIX-C states this. Residual F-4 |
| C-12 | M | ADDRESSED | `scripts/research_gc.py:37-41` (`path_key`: abspath under root, normpath, normcase), `:104-115` (`users` compares by key, store-base rule merged from A2) | matches | Junctions and symlinks are not resolved (disclosed) |
| E-33 | ruling | ADDRESSED | `backtest_integrity.py:80-81` (`MINING_CAMPAIGN` in `LEDGER_KINDS`); `campaign_line` (count 0, origin mined, `registry{path, chain_head, count}`); `trial_counts` / `ledger_counts` / `appendix_a` skip it; `check_record_fields` refuses it as a NAV line | matches | `test_a_mining_campaign_line_adds_nothing_to_n_and_carries_its_registry_count`. No writer yet (H3, OD-7) |
| B-2 | M | ADDRESSED | `atx-impl/src/strategy_marginal_ic.cpp:203` (`ic_weights_themes`: theme_standardise or theme_redistribution, the runner's own block checks from `strategy_ic_admission.cpp`), `:457-473` (`rerank_theme_rows`: centred tied re-rank inside the theme with the composition's kernels), method and input keys only under theme_standardise | matches | Unit scale is correct: the residual is invariant to W_theme. Test `MarginalIc.StandardisedThemesComeFromTheWeightsBlock` passed at v8-6 (integration log) |
| B-3 | M | ADDRESSED | `atx-engine/src/data/strategy_data.cpp:174-206` (`role_delisting_returns_applied`, `refuse_delisting_returns_signal_role`, naming path, flag and field); callers `strategy_ic_admission.cpp:216` (`admit`, before any payload), `strategy_marginal_ic.cpp:410` and `:619`; integration `strategy_target_replay.cpp:825` (`--role` only) | matches | `returns_applied` was introduced with the delisting block itself (`47757cb4`), so no older v2/v3 role is newly refused. Residual F-7 |
| B-4 | M | ADDRESSED | `atx-impl/src/strategy_ic_runner.hpp:15` (`ic_fields_manifest_max_bytes` 16 MiB, reason in comment); `strategy_ic_admission.cpp:333` (`bind_fields`, refusal names both bounds); `strategy_ic_signal_cache.cpp` legacy manifests; marginal `read_fields` | matches | The NAV verb's own fields bound is already 16 MiB (`strategy_nav_replay.cpp:82`) |
| A-2 | M | ADDRESSED | `atx-impl/src/strategy_spo.cpp:1052-1066` (`warm_up_step`: aim-partial-v5 move of book and shadow, no risk read, no solve, no row), `:1254` (branch before `prepare` / calibration, the only risk reads: `:1261`, `:1481`); `warm_up` key only when a warm-up ran | matches (option 1, the choice and reason stated) | Changes warm-start spo outputs only; no accepted warm-start spo cell exists |
| A-3 | M | ADDRESSED | `strategy_nav_replay.cpp:1303-1308` (refusal when every book has gross 0 after EXECUTE on `score_begin`; message names K, the warm-up rows, date, session_ns and row); `warm_start_record` + `score_begin_gross` (summary); holdings manifest `warm_start` | matches, with a declared deviation: the row index is in summary.json, not recipe.json (recipe hash stability for the deploy pin) | Integration needed the `PinBench(140)` fixture for `NavLabelRole.SameRoleIsIdentity` (`229f8e78`, test only) |
| A-4 | M | ADDRESSED | `strategy_spo.cpp:1444-1452` (`aim_correlation_traded` over names either side holds); `strategy_spo_v3.cpp:88-97` (`e14_criterion` reads its mean against .9; NaN rows from a flat book skipped by `Stats`) | matches (both written, criterion on the traded one) | The traded book is the book DECIDE reads at d (one decision behind the aim). Declared; it biases the criterion low, never high |
| A-1 | M | ADDRESSED | `strategy_nav_replay.cpp:2229-2245` (`leverage_groups`: with `adv_hold_q > 0`, one group per distinct aim leverage), `:2332-2353` (one `run_books` per group, results back in grid order), `:3254-3263` (`leverage_groups` in the grid manifest only when > 1) | matches (each variant at its own leverage; grouping, not refusal) | Uncapped grids take the old single path |
| B-1 | M | ADDRESSED | `atx-engine/tools/research_fields_price.py:74` (`CALENDAR_GROUPS`), `:203-215` (`session_calendar` digest over rule id + sessions 1970..seal-1), `:694-708` (`reuse_inputs` / `entry_inputs`), entries record it | matches | First `--reuse` recomputes 4 fields once (PM3-5 records the expected 45/18) |

C-8 was skipped by Ruling E-34, as the brief says.

**Counts: 21 ADDRESSED (20 findings + E-33), 0 NOT ADDRESSED.**

### The fix lanes' verification verdicts

- **A-2 part 2, NOT A DEFECT (FIX-AB): agree, and moot.**
  - FIX-AB cites the risk verb, which writes every role row with forecast set once 252 sessions of structural history exist.
  - For B0c's 4-year role (base from 2018-06-01, TRAIN from 2020-01-02) the warm-up rows of K = 60 also lie after that point. This is my inference from the dates; the v8 risk store is not built.
  - In any case, `f1a928f9` reads no risk row before `decision_begin`.
- **B-2, "partly NOT A DEFECT" (the grouping mismatch): agree.**
  - `generate_library.py:285-288` at `81abfa12` writes `family = theme = registry theme`.
  - The fitter takes its themes from the same rows (`fit_composition_weights.py:462-478`).
  - The real defect was the missing re-rank, and it is fixed.
- **CONFIRMED verdicts: agree.** FIX-C confirmed C-2 and C-7; FIX-AB confirmed B-4 (605,601 B for 73 rows; 1,024 rows unreachable under 1 MiB).

### Integration fixes (`7c77fac2`, `d833b25f`, `229f8e78`)

All three are correct as described.
- **`7c77fac2`:**
  - Sets `summ.origin "prior"` on base-lo1 and base-lo3.
  - Neither base spec has `--origin` in its extra, and both have `summ.ledger`.
- **`d833b25f`:**
  - Applies the B-3 refusal to `role` only, after the JSON-object test and before the seal and membership rules.
  - The label role is never checked.
- **`229f8e78`:** changes a test fixture only.

### New breakage and residuals of the fix diff

No regression of severity I was found. The flag-off paths I read are unchanged: summ argv for v7 specs, v7 Appendix A, NAV marks without a label, the uncapped grid, spo without a warm start, and marginal without a theme_standardise file.

**F-1 (M) `rerun_of` is checked for existence, kind and window only: no uniqueness, no identity of the re-run cell**

- **Where:** `atx-impl/tools/backtest_integrity.py:815-833` (`check_rerun`), `:939-960` (`trial_counts`), `:1047-1069` (`dsr_variance`).
- **Window re-runs:**
  - A window re-run of any of the 37 legacy lines passes, because they have no window_id. Nothing refuses a second window re-run of the same target, and nothing ties the new line to the target's cell.
  - So C-4's own example still adds 0 when it names a legacy trial_id instead of B0a's: a new construction cell ledgered with `--rerun-of <a v7 trial_id> --rerun-basis window`.
  - A window re-run line without a window_id (nav_summ without `--protocol v8`) also passes, because `:826` compares only when the target has one. It then sits in the legacy variance.
- **Blind re-runs:** one defect line admits any number of sibling blind re-runs of the same target.
  - Each sibling adds 0, so N counts 1 for k + 1 results read.
  - Each sibling's SR enters V[SR], because only the target is excluded.
- **Registration rules broken:**
  - prereg item 2: each v7 cell is re-scored once on the longer window.
  - item 5: no retry.
  - item 7: the re-run replaces the invalid cell, singular.
- **What it blocks:** the N, and so the DSR, of every later cell.
- **Fix:**
  - Refuse a second `rerun_of` of the same target with the same basis (and window).
  - Require a window_id on a window re-run.
  - Bind the re-run to the target's cell (cell name or spec digest), or require an owner-ruling id per re-run line.
- **Verified** by reading. The C-5 test exercises one re-run only.

**F-2 (m) A verdict spec can still override the pre-registered bootstrap seed and draws**
- **Where:** `scripts/research_cycle.py:415-427`.
- **The defect:** only another `--protocol` is refused. `summ.extra` may carry `--seed`, `--draws` or `--block`, which nav_summ honours over the v8 defaults (`nav_summ.py:1054-1056`). add-alpha keeps the parent's extra (`research_add_alpha.py:206`).
- **Evidence:** `test_cycle_scoring.py` scores its verdict cell with `--draws 99`.
- **Current specs:** none of the ten v8 specs carries these flags today.
- **Fix:** refuse those three flags in `summ.extra` when `summ_protocol` is v8.
- **Verified.**

**F-3 (m) Recorded ledger heads are never checked**
- **Where:** `research_cycle.py` (`ledger_state`, `copy_ledger`).
- **The defect:** the verdict records `{head, lines}`, but no code recomputes `chain_head` of the first `lines` lines of the current ledger against it. A tail edit is therefore detectable only by hand.
- **Fix:** a check in `ledger_state` / `research_ledger` against every earlier `cycle_verdict.json` head.
- **Verified** (no reader exists).

**F-4 (m) `holdout_gate --ledger` is any file**
- **Where:** `holdout_gate.py:226-232`, `:300`.
- **The defect:** the validation line goes to whatever path the caller passes. A scratch file satisfies it, and so does a wrong path such as a sprint copy. The owner ruling does not name the ledger.
- **Fix:** the ruling names the ledger path and the gate refuses any other, or the gate defaults to the ledger of record.
- **Verified.**

**F-5 (m) A defect line needs no ruling**
- **Where:** `backtest_integrity.py:734-747`, `research_ledger.defect_main`.
- **The defect:**
  - The verb takes a reason only.
  - On a ledgered cell it is necessarily written after the summ printed the cell's returns.
  - It removes the cell from N and from V[SR].
- **Why it matters:** prereg item 7 and E-31 require the invalidity to be decided without seeing returns. Appendix A v8 does not print defect lines.
- **Fix:** require an owner-ruling reference and date on the defect line, and print the count of defect lines in the v8 block.
- **Verified.**

**F-6 (m) C-13 binds the nav phase only**
- **Where:** `run_cycle` checks the binding for the `nav` phase only.
- **The defect:** the `ref` phase is the same NAV verb (output `<nav>-ref`), and it resumes on `summary.json` alone. A ref made before the fields or flags changed then feeds the byte identity `ref-s2-daily` as stale.
- **Fix:** write and check the binding for `ref` too.
- **Verified.**

**F-7 (m) The B-3 refusal lives at each caller, not in the loader**
- **Where:** `read_strategy_role` (`strategy_data.cpp:73-172`).
- **The defect:** the loader still loads a delisting-returns role for any other caller. FIX-AB lists the legacy `strategy_runner.cpp` and the Python builders as uncovered.
- **Cross-lane:** the H3 mining verb's "shared research-role loader" is being merged now and is not in `b44774d6`.
  - Unverified whether it calls the refusal.
  - Settle it by grepping the merged H3 head for `refuse_delisting_returns_signal_role`.

---

## Part 2: R45 session 3 (E-25 declared clearing) and the A2 follow-up

### Findings

**F-8 (M) The `ref` phase of an add-alpha child of a labelled parent runs without `--label-role`, so R-2 and R-7 hard-stop**

- **Where:**
  - `scripts/research_cycle.py:1093`: the label is added only for `phase == "nav"`.
  - `:934-944`: `ref_step` calls `nav_step("ref", ...)` with the parent's NAV flags.
  - `:177-179`: the INPUT_KEYS comment calls ref "an identity against the unlabelled parent".
  - `scripts/research_add_alpha.py:156-157`: the child keeps `label_role`.
  - `research_add_alpha.py:198-201`: the child gets a ref phase and the file-mode compare `ref-s2-daily` against the parent's S2 daily whenever the parent ran.
- **The defect:**
  - Once B0c is accepted, every parent is labelled.
  - The child's ref reproduces the parent's NAV with the same flags (`--warm-start-sessions 60`, `--capacity-curve`) but without the label marks.
  - It is then compared byte for byte with the parent's labelled daily CSV.
- **Failing scenario:**
  - A2's own R-2 sequence (task-A2-report step 6, "ref first ... must reproduce P's S2 daily") with P = B0c, using fields with `grp_ff12f49`.
  - R-7 with fields v11.
  - Both differ from the parent's fields, so ref is not skipped, and the compare is a hard stop.
- **What it blocks:** the R-2 and R-7 cycles. It is not silent, but the obvious workaround (dropping the compare) removes the fields identity.
- **Fix:** pass `--label-role` / `--label-role-sha256` to the ref phase whenever the spec has `inputs.label_role`, since the parent it reproduces was labelled. Add a test with a label role whose marks differ.
- **Verified** by reading.
- **Unverified (data):** at least one held name has an applied termination in the TRAIN rows. B0c's `label_only_present_cells_scored > 0`, together with any held termination, settles it.

**F-9 (M) Template cells: the C-13 binding and the verdict's `spec_sha256` hash the template file only**

- **Where:**
  - `scripts/cycle_resume.py:39-46`: the digest is the file at `cycle.spec_path`.
  - `cycle_resume.py:63-67`: a spec-digest match returns before the argv check.
  - `scripts/research_spec.py:64-81`: the parent is a path, never pinned.
  - `lock_template`: the parent's pins stay in the parent's file.
  - `research_cycle.py:1650`: the verdict's `spec_sha256` is computed the same way.
- **The defect:** a template resolves through its parent chain, so its NAV argv depends on files whose bytes the digest does not cover.
- **Failing scenario:**
  - R-4 (a template on B0c) has run its NAV.
  - Root then edits `base-b0c.json`: re-points `label_role`, changes a nav flag, or relocks a pin.
  - R-4's resolved NAV argv changes, but `r4-hold-band.json` does not.
  - Resume scores the old NAV dir, and the verdict binds an unchanged `spec_sha256` to it. This is review C-13's scenario moved one link up the chain.
- **What it blocks:** the provenance of every R-cell verdict written as a template (B0c, R-1, R-3..R-6).
- **Fix:**
  - Bind and record a digest of the resolved spec (canonical JSON), or of the whole template chain's files.
  - Or always compare the argv digest, not only when no spec digest exists.
- **Verified.**

**F-10 (M; the ruling interpretation is unverified) Ruling E-27's "otherwise" branch: on an ew-theme-v1 parent, R-3 runs a rule whose theme masses move with the gains**

- **Where:**
  - `scripts/specs/v8/r3-aim-gain.json`: maps `ew-theme-v1 -> ew-theme-aim-v1`.
  - `atx-impl/tools/composition_rules.py:41`: "On an ew-theme-v1 parent R-3 stays ew-theme-aim-v1 (its bytes unchanged)".
  - `fit_composition_weights.py:1520-1529` (`ew_theme_aim_weights`): `w_k ∝ g_k / (T n_theme(k))`, normalised globally, no member cap.
  - `fit_composition_weights.py:2129`: the dispatch.
- **The conflict:** E-27 reads "the persistence gains g_k multiply the member weights of the parent's composition ..., renormalised inside the theme so each theme keeps 1 / T; the member cap 1 / (2T) is applied after; id ew-theme-std-aim-v1 when the parent is R-1, ew-theme-aim-v1 otherwise".
  - The std branch implements this (`composition_rules.py:231`: score x gain inside the theme, then the cap).
  - The v1 branch keeps the v7 rule. There, a theme's mass is `sum_theme g / sum_all g`, not 1/T, and no 1/(2T) cap applies.
  - Either the ruling meant the v7 rule under the old id, or R-3 on a v1 parent needs a within-theme variant.
- **What it blocks:** the registration of R-3 whenever R-1 is rejected. The ruling's cost-if-wrong ("gains shift weight across themes") is exactly what the v1 branch does.
- **Fix:** a PM clarification of E-27 before R-3 runs on a v1 parent. If the ruling's mechanics govern, add `ew-theme-v1` with the within-theme gains (`tier_weights` with equal scores x gains, then `member_cap`).
- **Verified (code).**

**F-11 (m) A wave of re-screens only is ledgered as admission trials**

- **Where:**
  - `research_add_alpha.py:186`: `"admitted": trials or list(rescreens)`.
  - `scripts/cycle_admission.py:55-75`: writes one count-1 admission line per `gate.admitted` row.
- **The defect:** with no trial in the wave, every re-screen becomes an admission trial line. This contradicts Ruling R2-e (0 admission trials) and the slim recipe's `admission_trials = new - rescreens` (`generate_library.py` `build_recipe`).
- **Reachability:** only by a re-screen-only wave. A2's R-2 sequence adds the 7 trials first. This is an A2 x FIX-C interaction: A2 was written without C-7.
- **Fix:** `cycle_admission` skips the library's `rescreens` (read the slim recipe's `trials.rescreens`).
- **Verified.**

**F-12 (m) Label-only presence is not tied to the declared terminations**

- **Where:**
  - `atx-impl/src/strategy_target_replay.cpp:1552-1569` (`load_label_role`).
  - `:922-957` (`check_declared_clearing`).
- **The defect:**
  - A label role may add presence on any `--role`-absent cell, at any finite positive price, provided its effective membership there is 0.
  - `label_only_present_cells` is reported but never compared with the declared `universe.delisting.applied.terminations` (`prepare_recent_research.py:918`).
  - It is also never checked that each such cell is the session after a last present session, with no later presence.
  - The declared-clearing check covers `member.u8` only.
- **Impact:** E-25's "anything else refused" holds for the membership, not for presence. The impact needs a builder defect, because every other payload SHA and the universe pins must match.
- **Fix:** require `label_only_cells == applied.terminations`, and check each label-only cell against the termination rule.
- **Verified.**

**F-13 (m) S3 double-counts the terminal loss on labelled runs**

- **Where:** `strategy_nav_replay.cpp:593-606` (`carry_absent`).
- **The defect:** a label-priced termination realizes r at T. It then takes the adverse haircut eta K sessions later (S3, K = 1), at `h(1+r)(1+eta)`.
- **Status:** the E25 report states this under "Risks", but no ruling covers it.
- **Effect:** S3 of B0c and of every R cell is not comparable with B0a, B0b or v7 S3.
  - R-5's "S3 not lower" compares two labelled cells, so it is consistent.
  - A scorecard row that sets B0c's S3 against B0a's is not.
- **Fix:** a ruling (keep, or eta 0 on a label-realized termination), and a label on the S3 rows.
- **Verified.**

**F-14 (m) `r6-spo-v3.json` removes `--capacity-curve` against Ruling E-37**

- **The defect:** the template sets `"--capacity-curve": false` and its text says "spo refuses it".
- **The conflict:**
  - E-37, declared after A2, says spo-v3 accepts it as a report-only pass.
  - E-29 needs the 4x report on every R cell.
- **Ownership:** the code side is N-2 (fix lane 2), and the template must flip with it.
- **Verified** (template text against progress.md).

### Checked, no finding

**E-25 (R45 session 3, `strategy_target_replay.cpp:818-957`, `:1528-1580`; `strategy_nav_replay.cpp` MARK paths)**

- **Flag off.** MARK reads the role's own spans (`:2285`), and the recipe and summary keys are written only when the flag is on.
- **The label role marks the books only:**
  - MARK (`mark_flat`, `carry_absent`, `realize`) reads `c.mark`.
  - EXECUTE, the session ring, the tiers and the liquidity windows read `x`.
  - A label-only cell is role-absent, so no order fills there.
- **Manifest rule.** The checks are:
  - every key except `files`, `universe` and (when cleared) `score_member_counts` is equal;
  - extents are equal, and SHAs are equal except the four patched payloads;
  - the universe id and the base-role, bridge, SIC and delisting pins are equal.
  - The builder's role manifest has no argv, time or other run-varying top-level key (`prepare_recent_research.py:549-561`), so a real label role passes.
- **Declared clearing.**
  - The cell rule `kept 1 / label member 0 / role absent / label present` and the count equal the builder's `cleared += kept[tt, j] != 0` (`:906-907`).
  - The blend's `member` is effective membership (`strategy_target_replay.cpp:755-756`). Together with the per-cell membership check, this forces the label member to 0 on label-only cells.
- **B-3 wiring** is applied to `--role` only, and its message is kept.
- **Seal.** It is checked on `score_end_ns` and on every role session before any label payload is opened.
- **Deploy.** The deploy recipe is computed without the label, so a labelled run is never a deploy pin.
- **Capacity pass.** It forwards every flag except `--emit-holdings` (`strategy_nav_v7.cpp`), so its books are labelled too.
- **Risk store.** It is bound to `--role`'s SHA (`strategy_spo.cpp:669`), so a label role cannot reach spo through the risk store.

**add-alpha on a v8 parent**
- A template parent is refused while its parent is null, a `requires` is open, or a fill remains.
- The name rule substitutes the parent token, or appends `-NAME`.
- Shared stores are passed on only when `fit.work_dir == <base>/fit-work`.
- The kept inputs include `role` and `label_role`; every reference input is re-derived from the parent's complete IC attempts (`ic_attempt`).
- `runner.phases` and `ic.w_flags` are inherited, and the OD-2 caps are written only when the parent has no `runner.phases`.
- `--removes`, `--replaces` and `--rescreen` edit the library through `generate_library.py` (exceptions inherited, re-screens out of `admission_trials`).

**cache gc.** A derived child of a named store base is kept, compared by `path_key`. A direct-child store is never kept by the base rule unless a spec names `build-equity` itself.

**ew-theme-std-aim-v1 (std branch)**
- `w_k = (1/T) score_k g_k / sum_theme score g`, then the member cap 1/(2T).
- Gains of 1 give ew-theme-std-v1 bit for bit.
- The runner block stays `ew-theme-std-v1`, so the C++ needs no change.
- Aim records are computed for both aim rules (`AIM_RULES`).
- No other tool dispatches on the composition id (grep of `atx-impl/tools`).

**E-28 caps and templates**
- R-1 sets `runner.phases.w.max_rss_mib 3072` and `ic.w_flags --max-memory-mib 3072` (w pass only).
- `W_BUILT` options cannot be overridden.
- The caps are inherited down the template chain and by add-alpha children.
- 300 s comes from the base `runner.phases.w`; the card is 300 s / 2,560 MiB on base-lo1 and base-lo3.
- NAV-only templates reuse the parent's monitor. Correct: `book_monitor --baseline` reads the u pass, admission, sleeve and fit work, never the NAV (`research_cycle.py:1214-1235`).
- Known and disclosed by A2: derived `reference_combined` assumes w attempt 1 (`research_spec.py:92`). A missing attempt-1 file fails `lock` loudly.

**Part 2 totals: I 0, M 3 (F-8, F-9, F-10), m 4 (F-11..F-14).**

---

## Coverage

| file (at `b44774d6`) | read | result |
|---|---|---|
| review-w1-A/B/C.md, review-w1-part2-brief.md, task-FIX-C/FIX-AB brief + report, task-E25-report.md, task-A2-report.md (follow-up and risks), integration-log "integration 5 part A", progress.md 300-526 | whole (progress from line 300) | context |
| `scripts/cycle_verdict.py` | diff | C-1 ok |
| `scripts/research_cycle.py` | diff (all hunks), 700-740, 934-944, 1214-1236 | C-1, C-2, C-7, C-13 ok; F-2, F-3, F-6, F-8 |
| `scripts/cycle_admission.py`, `scripts/cycle_resume.py` | whole | C-7, C-13 ok; F-9, F-11 |
| `scripts/research_add_alpha.py` | diff (all), 104-122, 148-222 | C-2 ok; F-8, F-11 |
| `scripts/research_gc.py` | whole | C-12, A2 gc ok |
| `scripts/research_ledger.py` | diff, 60-130 | C-3, C-7 ok; F-5 |
| `scripts/research_spec.py` | whole | F-9; templates otherwise clean |
| `scripts/specs/v8/*.json` (10) | whole | F-10, F-14; 7c77fac2 ok |
| `atx-impl/tools/backtest_integrity.py` | diff (all), 700-730, 1025-1072 | C-3..C-7, E-33 ok; F-1, F-5 |
| `atx-impl/tools/nav_summ.py` (unchanged in range) | 585-620, 884-1000, 1050-1130 | ordering of `--ledger` / `--dsr-ledger`; F-2 |
| `atx-impl/tools/compare_window_overlap.py` | diff | C-9, C-10 ok |
| `atx-impl/tools/holdout_gate.py` | diff | C-11 ok; F-4 |
| `atx-impl/tools/composition_rules.py`, `fit_composition_weights.py` | diff; 1520-1530; 462-482 at 81abfa12 | E-27 std branch ok; F-10; B-2 verdict |
| `atx-impl/strategies/generate_library.py` | diff; 278-292 at 81abfa12 | ok; F-11 |
| `atx-engine/tools/research_fields_price.py` | diff | B-1 ok |
| `atx-engine/tools/prepare_recent_research.py` | 545-575, 770-930 | E-25 manifest keys, clearing count; F-12 |
| `atx-engine/include/.../strategy_data.hpp`, `src/data/strategy_data.cpp` | diff | B-3 ok; F-7 |
| `atx-impl/src/strategy_target_replay.cpp`, `_detail.hpp` | diff (all), 735-765 | E-25 clean except F-12; B-3 wiring ok |
| `atx-impl/src/strategy_nav_replay.cpp` | diff (all), 590-640, 1226-1330 | A-1, A-3, E-25 MARK ok; F-13 |
| `atx-impl/src/strategy_nav_v7.cpp` (unchanged) | 400-470, grep | capacity pass forwards flags |
| `atx-impl/src/strategy_spo.cpp`, `.hpp`; `strategy_spo_v3.cpp`, `.hpp` | diff; 30-60; grep of call sites | A-2, A-4 ok |
| `atx-impl/src/strategy_marginal_ic.cpp`, `.hpp` | diff | B-2, B-3 ok |
| `atx-impl/src/strategy_ic_admission.cpp`, `strategy_ic_library.cpp`, `strategy_ic_signal_cache.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_runner.hpp` | diff | B-3, B-4 ok |
| `scripts/tests/test_cycle_scoring.py` | whole | C-1 example, C-2 argv pinned |
| `atx-impl/tools/test_compare_window_overlap.py` | diff | C-9 / C-10 three cases |
| `atx-impl/tools/test_trial_ledger_rules.py` | test list, 204-240 | C-5 one re-run only (F-1) |

**Not read:**
- C++ tests: `strategy_live_test.cpp` (+673), `strategy_spo_v3_test.cpp`, `strategy_marginal_ic_test.cpp`, `strategy_nav_replay_test.cpp`, `strategy_ic_runner_test.cpp`, `strategy_data_test.cpp`. The integration log records them passing at v8-6 / v8-6a.
- Python tests: `test_research_spec.py` (+571), `test_research_cycle.py`, `test_research_ledger.py`, `test_holdout_gate.py`, `test_research_cycle_label_role.py`, `test_cycle_resume.py`, `test_composition_rules.py`, `test_research_fields_price.py`, `test_nav_summ_v8.py`.
- Other: `strategy_nav_replay.hpp` (doc hunks), and the H3 mining verb, which is not in `b44774d6` (F-7).
- Hunt "tests that cannot fail": covered only for the three test files read.
