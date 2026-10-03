# Task 2 fix round 1 and Task 2x review

Reviewed specific commits `d306e7768f7d72293aa600718a396814c98caba9`, `f0ab20f50a5d9fa4e4e8d0a68b23beeba90f51f4`, and `21d6c4ace3a9672b66a33cf9bcbad7f414517482` in `review-T2-r1.diff`. Binding inputs: REVIEWER.md, resume-contract.md (including R4/R5), global-constraints.md, gap-analysis.md §2 item 1, both task briefs, original findings and both implementer reports. Task 2 review is limited to original issues and regressions introduced by the fixes; Task 2x receives a full task review.

### Spec Compliance — Task 2 ❌ Needs fixes

| Original issue | Verdict | Evidence and reasoning |
|---|---|---|
| I1 incorporation snapshot | ADDRESSED | `universe.py:177`, `:241`: undated snapshot table/join removed; dated cover history is selected strictly before cutoff. Unknown earlier incorporation remains unknown. The later-foreign-snapshot and dated-cover fixtures protect earlier membership. Appending a later cover cannot replace a strictly earlier as-of observation. |
| I2 retained SIC de-SPAC | ADDRESSED | `universe.py:132`: SIC 6770 supplements SPAC only with zero/missing panel sale_ttm, matching the established panel rule. The fixture retains SIC 6770 and changes operating revenue, then checks admission. Concrete named-risk check: `panel.py:594` selects fundamental events using `issuer_cutoff > vis_clock`; revenue is inherited from that PIT panel, rather than obtained from a current issuer snapshot. |
| I3 SC/BZX index exclusion | ADDRESSED | `security_master.py:48`, `identity_v3.py:703`, `indexes.py:166`, `market_index.py:93`: source-based NULL-exchange repair is applied before codes and full index eligibility/ranking. The universe uses the same authoritative map. Source dates must precede the session and be at most 45 days old; stale, same-day and unsupported classes remain unmapped. New fixtures check market/index eligibility; published verification reports zero index/universe mismatches for every 2018–2026 year. Recomputed SC representation is 110–231 R2000 companies over the nine effective reconstitutions, rather than attaching names to old ranks. |
| I4 French accuracy | NOT ADDRESSED | `task-2-report.md:147` and `task-2x-report.md:26`: 27/60 months still fail the unchanged ±5% gate; manifest retains FAIL. Source reconciliation is substantial progress, but no controller carry-forward ruling or measured accuracy repair closes the requirement. |
| I5 future BDC withdrawals | ADDRESSED | `universe.py:183`, `:239`: per-clock status events replace global minimum election/maximum withdrawal; latest qualifying event strictly before cutoff controls the flag, with withdrawal winning ties. Withdrawal/re-election/future-withdrawal fixture asserts the entire prior history stays unchanged when a future withdrawal is appended. |

The sample-binding repair also addresses original M3: rule, seed, unique sample keys and a hash of membership/classification fields are checked against the newly generated sample before the published hand-check PASS (`universe.py:349`, `:609`). The verdict artifact contains 100 unique identities, 17 newly adjudicated rows with primary SEC evidence URLs and 83 retained judgments. The changed set consists of five all_common claims and twelve SPAC claims; evidence descriptions distinguish the sampled date from later retrospective audit documents. Reported pooled precision remains 97/100 and ADR precision remains explicitly 9/12. Original M2 missing-month/year checks are improved; this review does not broaden into unrelated old weaknesses. Actual beta/idio-vol coverage still remains a documented Task 6 dependency.

### Spec Compliance — Task 2x ✅ Approved

The canonical map contains SC→XNAS and BZX→BATS, unsupported values have no default, and the repair preserves non-NULL frozen panel exchanges. Codes, market_index and index builders consume the repair before their existing computations. Input manifests/code fingerprints include security_master; registry changes are dataset stubs, not shared registries. No foreign-dirty path is changed in the package, and no panel rebuild is reported.

SC meaning independently verified in the primary [Nasdaq report contract](https://www.nasdaqtrader.com/content/home/help/tsiqtxtkey.pdf), which identifies historical SC as Nasdaq Capital Market. The package also cites primary [FINRA Notice 19-33](https://www.finra.org/sites/default/files/2019-10/Regulatory-Notice-19-33.pdf) and the SEC exchange directory for BZX nomenclature; warehouse BATS conventions are preserved.

R5 is implemented at the actual aggregate-return source boundary: `market_index.py:85` rejects end dates beyond 2022, `:98` bounds table u before daily/monthly/factor aggregation, and `:389` passes the build end into that loader. Concrete named-risk review of existing consumers found daily/monthly and FF sorts aggregate u; historical return aggregation filters its source to before the pre-2023 splice. The added fixture supplies a 2023 extreme-return row and checks both exclusion from u and absence of published post-2022 periods. Published evidence reports 5,122 unique return keys, maximum period 2022-12-31 and zero prohibited periods; no return values after 2022 were inspected in this review. The initial inherited-builder violation is candidly recorded with its time, command and superseded row count; the missing interim SHA is explicitly acknowledged, not represented as recovered provenance.

The package supplies before/after class cells and lines, NULL counts, all_common/index coverage, nine SC R2000 reconstitution counts, unique keys, zero code cutoff violations/overlaps, all input hashes and guarded sequential command receipts. Reported combined gates are 8 PASS/1 FAIL, preserving French FAIL. Reported offline regression result is 61 passed in under ten seconds; no full suite/build was rerun for review.

### Strengths

Fixes target the original defects without altering the accuracy threshold. Source-driven exchange repair is centralized, history-dependent issuer flags use strict dated joins, and return construction is bounded before aggregation. Tests assert observable membership, clock, source-absence and output behavior. The evidence reports limitations instead of converting internal classifier consistency into an independent accuracy claim.

### Issues — Critical / Important / Minor

Critical: 0. Important: 1 retained original issue. New fix-induced blocking issues: 0. New Minor issues: 0.

**I4 — Important, NOT ADDRESSED: binding accuracy gate remains unresolved.** `.superpowers/sdd/tier1-v4/task-2-report.md:147`, `:281`; `.superpowers/sdd/tier1-v4/task-2x-report.md:26`. French FAIL now spans 27/60 observations (33 inside), with maximum absolute deviation 10.69%. The removal of non-PIT incorporation naturally exposes historical unknowns; preserving that correction is necessary, but it does not waive the accuracy gate. The 60-month exclusive-class ledger and overlapping source counters reconcile implementation counts, not benchmark constituents. June 2020's 90 missing lines in a 1,170-line neighboring-month cohort demonstrate a vendor coverage hole, but cannot establish the exact French missing constituent set. The 2018/2019 unknown-incorporation cohort comprises 1,454 CIKs; only nine CIKs have selected pre-2020 cached text containing the header literal, with original completeness/state/acceptance clocks unverified. This identifies a feasible dated-source remedy, not completed reconstruction. For the 2021/2022 excess, missing incorporation is near zero and zero same-classifier suspects does not prove correct inclusion. Fix: complete independently dated incorporation/class/status remediation and remeasure, or obtain a concrete controller carry-forward ruling that assigns the residual defect. Keep the manifest and docs FAIL until that happens; neither present hypotheses nor prospective source work close I4.

### Cannot verify from diff

Published Parquet contents, actual guard execution/peak consumption, runtime input-hash freshness and absence of unreported data commands cannot be independently reproduced from this package alone. Evidence artifacts and producer logic were checked structurally and against their reported measurements; no real-data diagnostics, builds or full suites were run. The 17 evidence links establish traceability, but this review did not independently fetch and read all 17 SEC documents. Source-code checks outside the diff were limited to named risks: panel revenue visibility and market aggregation/history consumers.

### Assessment

Task 2 spec: **Needs fixes**. Task 2 quality: **Needs fixes**, solely because original I4 remains open; I1/I2/I3/I5 are addressed and no new blocking regression was found.

Task 2x spec: **Approved**. Task 2x quality: **Approved** for its repair/rebuild scope, with the disclosed inherited interim return violation corrected and the parent French failure retained honestly. This approval does not authorize carrying forward or waiving Task 2's accuracy gate.

Review process was read-only except this assigned findings artifact; no staging/committing, git reconstruction, subagents, builds, full suites or real-data commands.
