### Spec Compliance — ❌ Needs fixes

Reviewed supplied commit a61ef6d340bfe335fc980a8082b6fb99e9dccce9 in review-T4-r1.diff, bounded code/test/doc hunks and generated evidence summaries. The local r0 defects improve as described below, but required acceptance remains unfinished. No hard gate is waived.

### Strengths

I1 CLOSED locally: ratios.py:637-646 separates history insufficiency into ratio_history_missing; ratio_undefined now contains true zero denominators only. coverage_gate retains ordinary missing/history cells in its denominator and handles empty masks. The new empty-mask fixture asserts cells=eligible=1 and share=0; the existing FY0 fixture also asserts the consumer-generated history diagnostic. The prior observed-missingness comment is corrected.

I2 CLOSED locally: ratios.py:675-699 uses strict shared cutoff for links and ASOF dated FINRA evidence, removing all-time modal classification. The fixture checks daily/monthly equality exclusion, earlier-link inclusion and a later warrant observation becoming effective only after its clock. Missing FINRA evidence is explicitly unknown under the visible common-link contract; historical class certification is not claimed.

I3 CLOSED for the local pairing defect, with producer qualification outstanding: ratios.py:510-522 enforces fiscal-quarter progression/year consistency/same fyr; :558-576 enforces annual fiscal-year progression/same fyr and an adjacent preceding year-end gap of 350–380 days. Fixtures reject calendar transitions and a 335-day annual comparative while retaining the raw annual record. These assertions check actual selected lag rows. The standardized schema exposes fyear/fyearq but no period-start/duration certification. task-4-report.md:98 and ratios.requirements.json producer_needs explicitly state that end-date screening does not certify source duration and assign dated period start/end or a comparable-full-period flag to Task5. This qualification must survive downstream use and future acceptance.

Cash source checks add reported MarketableSecuritiesCurrent and prefer explicitly reported aggregate cash/investments without double counting. Fixtures assert both arithmetic and aggregate precedence. Supplementary checks recompute two primary formulas separately from retained filing primitives, without broadening the asserted source-discovery scope.

### Issues — Critical

None established in this fix round. Package paths do not intersect foreign-dirty-files.txt or shared registries.

### Issues — Important

I4 OPEN, plan-mandated: task-4-report.md:100-111,440 and t4-r1-evidence.json retain 0/6 hard gates PASS. Member primary coverage is 24/66 (extensions 2/3; composites 0/4), correlation/range gates remain FAIL, and source certification remains incomplete. Main 40 issuer-date observations ×36 formulas give 999 matches,143 both absent,297 narrow-source-chain absences,1 ratio/history NULL and0 numerical mismatches. Supplementary80 cells give59 matches,15 source absent,6 both absent. Only35/66 primary formulas have been checked;31 primary and4 composites remain unverified. Neither both-absent nor narrow extraction absence certifies formula/source equivalence. Remaining ROE prior-TXDITC case is keyed and history-NULL, not arithmetic success or proven primitive absence. Retain Task5 source/history/duration and Task13 indicated-dividend obligations until independent recovery and rerun.

GC3/R9 OPEN acceptance dependency: overlap all-common19/66 and R3000p11/66 measurements are explicitly provisional/invalid, not final gate evidence. Require completed Task2 publication and verification of required file SHA/size against its manifest before the controller's coordinated refresh. A manifest hash alone cannot certify a stable stage snapshot. Do not relabel historical or overlap receipts as current acceptance.

No new fix-induced blocking defect established. The former minor empty-mask/comment findings are closed. The initial attribution-path failure is disclosed (receipt exit1 and logged traceback), followed by successful guarded attribution; it is not an unresolved execution blocker.

### ⚠ Cannot verify from diff

Source-backed annual duration/full-period equivalence, complete73-formula source-model certification (including Ohlson nominal-assets/OCF qualification), independent historical classification completeness, and final GC3 coverage remain uncertified. Local fiscal screens are useful safeguards, not substitutes for missing producer metadata.

### Assessment — Task quality: Needs fixes

Spec compliance: Needs fixes. Quality: Needs fixes because required source/coverage/range/correlation acceptance remains open; local I1/I2/I3 corrections are supported without establishing full-suite acceptance. No additional local fix is demanded beyond the recorded producer obligations and coordinated final-universe refresh.

Verification scope: supplied diff/evidence/receipts plus targeted standardized-schema read for the named duration/fiscal-field risk and bounded stderr inspection. Reported26 offline tests passed in19.44s; no suite, build, network, git reconstruction or real-data query rerun. Guard receipts show ≤1GiB limits, successful final build/handcheck/attribution and no cap hits; final build peak0.6282GiB. No held-out return statistics inspected. Only this assigned findings artifact was written; no staging/commit/subagents.
