# P9-C prerequisite closure and exact registration proposal

Preparation only under R15-C-PREP, HEAD `6065767a2864117e525f7d1b4e267d6c82aacac4`,
branch `feat/platform-p9-20261003`. PM rejected M; accepted parent remains H.
Ledger136 SHA `c98dd0c4d59bada990fb34c7bf5efc3ae59873acc313de771a2d025a449013b7`,
first135 SHA `7b87d61ccb3285e9ea94aefb52222e610f74287b5559988339eccf09aed07e00`;
N_c65, N_hand126, M110, N_tot236, P9 admission additions0 (PM-verified M handback).
No C spec mutation, diagnostic, probe, test, measurement or trial write. Existing mandatory
contracts remain read; only this amendment/scratch21 and applicable C evidence were inspected.

## Five exact template prerequisites and existing proof

1. **Exact:** "the PM has ruled info-clock-rate-v1 distinct from Y-5 (two-speed-v1) before any Y read (plan DEC-16; lit section 7 restatement risk)"

   **Proof:** progress.md:79 DEC-16 precedes Y reads; Y-5 completion is :147 (R0-9,
   trialcc150c210a3f98e4, report74184cce). B.4 and the registration restatement preserve the
   distinction: theme sleeves/aims/netting versus one book's event-clock rate per name-day.
   No Y statistic was needed for this prerequisite. Proposed satisfied.

2. **Exact:** "P9-C registered by the PM in the P9 prereg (plan section 5.2); source_sample_end 2019 FLAGGED under PRE-19 / PRE-31 (Ruling ALCLOCK-2); P9-C lapses at 0 if lane AL-CLOCK is not merged and APPROVE when the P9 cells start (PRE-14)"

   **Proof:** p9-prereg.md B.4:933-1066 registers this cell, constants, acceptance and prints;
   PRE-2BII-RULED :1034 and CELLS-4 :1038 retain its RUNS status. AL-CLOCK APPROVE :744,
   AL-CLOCK fix APPROVE :860; merges858a4fca/1d003241 are ancestors of current HEAD (exit0).
   source_sample_end2019 remains FLAGGED, including clean-source OD-3 exclusion. No lapse.
   Proposed satisfied as scientific registration; operational parent/pin closure awaits PM.

3. **Exact:** "lane AL-CLOCK reviewed and merged by root; root repeats the C3-5 probe with --rate info-clock-rate-v1 at that merge (Ruling C3-13)"

   **Proof:** first reviewe9b3a323/APPROVE7ebd41ff, merge858a4fca; fix re-review/APPROVE
   eed512bd, merge1d003241. Both reviews and merges are ancestors (exit0). Existing root
   root-wave3-merge-report.md:538 records clk3 preflight through spec, exit0, no nav.rule
   override, --rule aim-partial-v5 once, --rate info-clock-rate-v1 once, --trade-fraction .05
   once; m4-logs/c3-5-probe.txt and probe-wave-clk3.log preserve it. No NAV was run because
   that tiny world lacks ea_days_since. :1028-1043 preserves the fix's row/fixture evidence.
   Current nav_rules.cpp:75 row is trade-rate, x-flag --rate info-clock-rate-v1, no params;
   wave_kinds.py:483-554 derives this append-route flag without replacing nav.rule.
   No probe/test repeated. Proposed satisfied.

4. **Exact:** "root re-pins the lineage parents to the wave-3 P9-B0 references before the first P9 cell (W1-I2: the NAV exe changed)"

   **Proof:** re-pin of record2078f877, progress.md:998; REPIN-REV :1003 and completed
   restore/verify :1004 retain that re-pin. FLAGS-ROUTE f8d852f3 adds the two print flags
   by inheritance and preserves the pinned reference identity; B.4 records its evidence.
   2078f877/f8d852f3 are ancestors (exit0). Frozen54 exes/CRT/six-reference pin evidence
   remains the completed6065767a M handback, independently verified by PM; no new build
   or unchanged-tool test. Proposed satisfied.

5. **Exact:** "root sets parent to the last accepted cell whose NAV runs --rule aim-partial-v5 at --trade-fraction .05 with the fixed rate and no --two-speed (Y-F0 at registration; its --capacity-curve is accepted with the rule, Ruling ALCLOCK-3), whose fields manifest lists ea_days_since point in time, and whose NAV flags name --info-clock-print when the wave-3 P9-B0 printed it on that reference (Ruling ALCLOCK-1); and runs the cell as an atx.research-wave/v2 kind rule wave whose rules block is [{name: info-clock-rate-v1, params: {}}] with gross_match pm6-6-engine (Ruling ALCOMB-1)"

   **Proof:** PM scratch21 retains accepted H after rejecting M. Parent
   scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json atbd837403, current SHA
   `1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f`, NAV
   build-equity/mega-nav-p9-theme-cov-hedge-gm, L1.1674, libraryv8ysb. Read-only resolver
   selected nav.rule=aim-partial-v5 and nav.flags: cadence1/trade-fraction.05 once,
   no --rate/--two-speed/aim-partial-v6/spo; --capacity-curve, --info-clock-print and
   --ann-mean-gross once. Fixed rate is the absent --rate default. Fields.output is v15;
   manifest SHA `26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09`.
   Only fields[name=ea_days_since].{name,file,sha256,point_in_time} establishes availability:
   ea_days_since.f64, SHA `fb6742f084c04b14302d0d26a91c740e2e08be1e6704856d815a62053a6d709b`,
   point_in_time=true. No field payload/return/IC read. Proposed satisfied conditional on
   the exact parent/v2 rules registration below; the existing textual guard is unchanged.

## Closed delta proposed; not applied

Original template SHA `95d4104a421c32f91877649f1ec9dfbdb409c9a121d6f6e2377e5e25e19b42d5`,
Git blob `31da5b6fc02ceb7674735a350794ee51d830141f`.
Original registration SHA `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152`,
Git blob `f858e08ca3c967b028cd8b3aeb0eb0e6c7c39dd2`. Both preserved at starting HEAD.

Only existing JSON leaves proposed:
- templates/info-clock.json /parent: null -> "theme-cov-hedge-v1-attempt2-gm.json".
- Same template /requires: the five strings above -> [], on explicit PM closure.
- rules/info-clock-rate-v1.json /manifest_fragment/rule_cell/template_sha256: old SHA ->
  SHA of the exact registered LF-byte template.

No description, nominal_parent, manifest_note, historical rule requires, constants, predictions,
capacity statement, field mapping or source flag correction. Native run_refusal remains intact.
New scripts/specs/p9/waves/p9c-info-clock.json would use v2, kind rule, wave p9c-info-clock,
rules [{name:info-clock-rate-v1,params:{}}], H parent/libraryv8ysb, pinned v15, pm7-34,
pm6-6-engine, expect.n_before65, construction_cap71, native timing/lock/receipt settings and
registered budgets. Printed native criteria only capacity-4x-higher and cost-bps-lower;
these are nondeciding. out_dir build-equity/waves/p9c-info-clock, record.copy_to cells/waves.
NAV-only parent phases are reused by the supported driver. Registration commit first, then
adjacent PRE-40 B.4/Appendix IV16 pins plus precommitted explicit read plan/filter before launch.
No existing C native manifest or generated info-clock-rate-v1 / -gm template was present.

## Proposed scalar read and source plan

Source/pin/void/input/generated-commit checks and six mechanics precede rho-only reading from
the existing hash-bound native eval bundle: sessions, rho, one_minus_rho, degenerate only;
R15-V2-RHO omission encoding/C.11 apply. Then deciding dSR/mechanics and individual registered
Memmel SE, parent/cell net-Sharpe, two-sided/one-sided p leaves. No whole lw/cbb/stats object.
Native pm7-34 alone decides/writes one trial; no DERIV. Keep Appendix A/identity bridge flags.

C-specific prints for parent and gross-matched cell, each produced scenario k, are explicit
summary.json scalar leaves scenarios[k].info_clock.{fast_share,name_days,fast_name_days,
missing_name_days,planned_turnover,fast_planned_turnover,turnover_share}; provenance leaves
rate,field.name,field.sha256,window_sessions,theta_fast,theta_slow,definition individually.
Also scenarios[k].exposure.max_gross_leverage and mean_gross_leverage beside the parent's
(C3-4/ALCLOCK-4, OD5-7 uses realised figures). No new deciding criterion. Capacity CSV selects
primary scenario multiple1 cost_bps_per_traded_dollar and multiple4 net_sharpe, same producer.
Expected +.02 to +.06 net Sharpe, book gross +~5%, capacity down remain registered estimates,
nondeciding; TRAIN mean gross is matched by construction. No M sleeve or H-only vol/gross-alpha
criterion. Existing fixed-rate-only x1=S2 test limitation remains disclosed; rule-on P10 is
untested, and this sentence is not an acceptance input. No test or imputed print proposed.

Unchanged sealsrc.py SHA `fd1aa3e60bf63c0988f74fa3fc0994d4dd9523904bd715079dbf5c7ef3eea551`:
invoke its CLI with the explicit C manifest, never import. Exact anticipated native suffix:

```text
--seal-allow 20261003=R15-SEAL-C --seal-allow 2026-10-02=SEAL-ALLOW --seal-allow 2026-10-03=R15-SEAL-D --seal-allow 2026-10-06=R15-SEAL-D --seal-allow 2026-10-17=SEAL-ALLOW
```

Actual per-hit source checks still required: class C exact standalone mandated exclusion token
in numbered commit-header logs; A exact owner PNG filename02; D exact03/06 owner PNG entries
in native dirty-header/parsed receipt array; B own started_utc17 only. OTHER stops. A different
actual UTC day needs PM ruling. M's exact-file sleeve adjudication grants no C exception.
M's inherited H weights exposure and accidental sealsrc Y-S metadata scan remain preserved in
the M record/handoff; no scientific choice changes. No extra source/test allowance requested.

After separate release: immediate bounded native wave run, report free RAM/disk, no RAM wait,
no --admission-wait or --host-budget argument, inherited w3584MiB/300s, floor512MiB, C:>=15GB.
Preserve one actual stop without retry. Native trailer/exclusion environment remains prescribed.
Await PM five-closure/exact-delta/source/read ruling; no C computation or next-cell work yet.
