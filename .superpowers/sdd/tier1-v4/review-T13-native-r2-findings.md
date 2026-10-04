# Task13 R2 scoped independent review

Commit: cb318dd9. Reviewer: /root/t16_release_foundation, independent of the author.
Scope: remaining I1 and I6 from review-T13-native-r1-findings.md, plus regressions in the R2 diff. Prior I2-I5 and unchanged areas were not reopened.

## Spec Compliance

**I1 addressed.** `atx-db/src/atx_db/alpha_panel/distributions.py:57-69` now requires a locally attached per-share unit for the selected amount. The narrow explicit from/to branch permits the old value's omitted unit only when the new value has a local per-share unit; intervening scales/comma totals cannot satisfy that branch. Unresolved per-share amounts are rejected before emitting amount or declared_regular_rate. The prior USD600-million reproduction cannot pass either the unresolved-value exclusion or the local-unit check. Direct covering fixtures additionally reject aggregate amounts without relying solely on the final annual-rate NULL.

**I6 addressed by honest deferral.** `atx-db/src/atx_db/alpha_panel/events_buyback.py:399-402` and `:461-465` independently construct authorization first_announcement as typed BOOLEAN NULL, with unknown_incomplete_global_history. Executions retain false with not_applicable_execution. The published projection includes the basis at `:488`; producer and public full-row replay agree. Schema/extractor and the registry/documentation move consistently to v3. Neither an isolated later filing nor adding an earlier matching filing certifies complete global history.

Full prior-history admission remains missing, explicitly documented rather than inferred from partial partitions. Source/class/clock semantics and actual rates remain unqualified; current indicated annual rates remain NULL. Original actual gates remain 0 PASS / 3 FAIL, with declaration/vendor agreement unmeasured and native buyback handchecks 0/200. This is engineering approval, not production/source acceptance or Task13 completion.

## Strengths

The amount fix restores local monetary-unit association rather than merely clearing annual rates. The history fix preserves unknown classification and an explicit reason throughout publication and independent replay. Changed tests cover both isolated and combined authorization histories and preserve tampered-join rejection.

## Issues - Critical / Important / Minor

No new Critical, Important, or Minor findings in the scoped changed code.

## Assessment

**Task quality: Approved for this scoped R2 engineering checkpoint.** I1 and I6 are addressed, and no new changed-code regression was identified. Full source qualification, complete prior-history proof, original gate measurement and downstream Task4 acceptance remain open.

Reviewed the supplied full diff, prior findings, task brief and R2 report. Did not rerun author tests or unchanged suites; no additional reproduction was necessary. No actual source/data/hash/HTTP jobs, producer edits, commits, or subagents. This findings report is the only written file; no live jobs remain.
