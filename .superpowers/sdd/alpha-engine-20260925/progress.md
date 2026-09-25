# Alpha-engine DAG continuation — 2026-09-25

## Resume point

User requested completion of the 2026-09-24 DAG series with sub-agent-driven development,
implementation before tests, and real engine and tradeable alpha work. The active goal remains
open. Authoritative scope: docs/plans/2026-09-24-alpha-engine-production-swarm.md and findings.
The companion goal prompt supplies the pre-registered defaults and data discipline. The current
request authorizes continuation; do not repeat the old W0 permission stop. The owner-controlled
2020+ unseal remains closed.

## Worktrees and ownership

- C:\atx is the dirty feat/tier1-parity checkout: read-only for this work.
- Prior integration: C:\atx-wt\pool-1, feat/w0-integration @ bc5cc646. Its heartbeat is alive;
  leave that lease and worktree untouched.
- Current integration: C:\atx-wt\pool-2, feat/aes-codex-integration-20260925,
  frozen base bc5cc646b46f6a7c23a60e87d28dfa9b972671ec.
- Lease run aes-codex-integ-20260925, heartbeat aes-codex-integ-20260925T2130.
  Lease auto-configure fails on the deleted optional modules, after publishing a valid lease.
  Build through the wrapper with equity-dev/equity-rel/equity-bench, never bare dev.
- No pushes. No writes/migrations to warehouse.duckdb. No reads dated >= 2020-01-01.
- Existing graph MCP tools are unavailable in this session: targeted rg/source reads are the fallback.
- Four agent slots total; root orchestrates and three agents handle bounded tasks. Serialize
  compilation and heavy real-data runs as required by RAM (about 4 GB free at resume).

## Recovered state

All ten W0 lanes and FIXUP are merged in bc5cc646. Their reports/reviews live in
.superpowers/sdd/w0/. The integrated gate, quiet-host performance gate and G0 remain pending.
Main still points to 2e0d738f. Do not advance it until gate evidence is complete.
The old progress file lists provisional P1-P11: these are not owner waivers or acceptance proof.

## Active work

| Task | Agent | State |
|---|---|---|
| G0 old-vs-new reruns | g0_evidence | Own leased pool; first Release build slot, then serial heavy slot granted. Fresh outputs C:\atx-wt\g0-data. |
| Independent W0 gate audit | w0_gate_audit | Read-only audit of acceptance, commands, and data-safe test selection. |
| Replay integration correction | w0_replay_integration | Own pool-4. Confirmed replay_config silently pins Abort; implementing corrected TerminalReturn default plus explicit legacy selector. Waiting for build slot. |
| Integration/docs | root | I-24 guard-run headline corrected from source artifact; no alpha profitability claim. |

### Follow-up assignments

- g0_evidence: pool-3 feat/w0-g0-codex-20260925, Release build ongoing. Frozen input
  manifest preflight confirms all 13 context date axes end before 2020. Do not rebuild solely
  to relocate deps if successful binaries are valid; isolate before any later rebuild.
- w0_replay_integration: pool-4, implementing/configuring corrected first-missing terminal
  fallback and explicit ex-post legacy mode. As-of terminal-event handling required.
  Granted isolated Debug configure/build with env CMAKE_BUILD_PARALLEL_LEVEL=1 if free RAM >4GB.
- w0_gate_audit: finished independent audit, now implements D-12 in pool-5 on
  feat/w0-membership-integration-codex-20260925. Narrow VM CS eligibility mask preserves
  temporal history; post-masking alone is insufficient. Builds held pending slot.
- root: implemented E0a public versioned inference/return-treatment config and declared
  horizon-aware t-statistics, tests added after implementation. See
  .superpowers/sdd/w0/lane-inference-integration-report.md. Root configure in progress,
  compilation waits replay build slot. Every repair still needs fresh adversarial review.

### Later progress

- Root inference code committed 9caf9e85, then fresh review found undersampled HH
  cancellation and unequal-weight HAC normalization issues. Fixes implemented, post-fix TU
  check passed; combine/eval target build running (session33388). Root deps verified isolated.
- Membership branch code af40186d/report39347e28, fresh static review by g0_evidence found
  no blockers. Runtime approval pending. Pool5 may configure/build after G0 frozen Release
  finishes, with one worker, freeRAM>4GB and <=3 total compiler workers.
- Replay config + future-evidence fix expanded to avoid artificial short gains from unknown
  price holes. Unknown-price haircut is adverse to each position, source5
  AssumedMissingPriceAdverse, with assumed PnL/counts and unusable-for-alpha-evidence flag.
  This explicitly deviates from the literal old B0 negative-return-on-every-missing-short
  clause, whose economic flaw the independent reviewer found. Do not mark that literal
  item met or imply an owner waiver. True due+available events retain table/Shumway returns.
- G0 build reached263/266 with no errors. L7/L10/L9 can use archived bc5cc646 binaries,
  since current repairs do not affect those paths. Baseline2013 needs replay fix rerun.
  cp21 MUST wait for reviewed membership merge before the 13 expensive cells; no redundant
  pre-mask run. Pin original26 families in the diagnostic branch only; never merge the pin.

## Acceptance gaps to resolve

- Replay consumer forces Abort despite B0 engine TerminalReturn default. G0 baseline cannot
  satisfy B-04 until the adapter is corrected and actual run completes.
- E0a combiner public V1 configuration is absent; horizon-blind NW rejects 13.3% under MA(20).
  Correct the real API/wiring rather than marking a documented limitation as completed work.
- E0a published HAC interval passes 94.5% coverage. The bootstrap interval is a distinct
  estimator; retain both evidence and definitions without conflating them.
- R0 sanitizer acceptance was replaced by checked iterators; neither is equivalent evidence.
- Quiet-host benchmarks, complete touched-target gate, G0 metrics and all W1-W5 work pending.
- Fresh audit found B0 last_print scanning future closes; current missing-close NAV changes
  when future prices change. Replay agent owns repair and future mutation proof.
- D-12 only admission was masked; family cross-sectional operators saw year-union names.
  Membership agent owns repair inside the VM, keeping rolling warmup data separate.
- A-18 search-driver/fitness cache remains hash-only (W2-A4 production follow-through).
- R-06 stage_riskmodel still uses static groups/no PIT cap (W3-R4 production follow-through).
- Full gate must explicitly exclude DataRealPanel and corporate-action real-data smoke tests
  using 2024/2026 inputs, even with environment variables cleared. No sanitizer proof yet.
- For performance, compare identical equity-bench runs at O1 head 3ccf012c and current head
  on quiet host; checked-in alpha_throughput.json is a busy-host legacy sample. No false
  pass via bench-gate -Update/-AllowMissing. Filters: BM_Kernel, Wq101_, BM_Search,
  BM_OptimizerProduction M=1000/3000/5000 modes 4/6/7; repetitions=3.

## Evidence recorded this continuation

- Source gate_report.json family_blend.validation: sharpe_net=-1.218738142997244,
  p_one_sided=0.9503335211679915; family_blend.holdout.sharpe_net=1.729687486711181;
  holdout.prior_reads=3. Corrected QUANT_PLATFORM_SWARM_STATUS.md (I-24).
- No engine or economics acceptance claimed yet by this continuation.

### G0 measured results so far (agent receipts; no final G0 gate yet)

- Frozen baseline2013 @bc5cc646: exit1, 2.031s, peak0.146GiB, missing-price abort at
  period6 / 2013-04-12 / security150340. Corrected replay rerun still required.
- L7 @bc5cc646: exit0, 61.625s, peak0.105GiB. Of151 fields, only path separator
  metadata and three tiny numeric changes (~1e-9/1e-10); headline verdict unchanged.
- L10 @bc5cc646: exit0, harness1/1, 302.079s, peak1.201GiB. NO CANDIDATE remains:
  no non-reference expression with positive nonoverlap t>2 in either universe.
  qual_gpa t1000 IC .01652 -> .0166986 and t1.670 ->1.76392; t3000 IC .02324 ->
  .0226574 and t1.816 ->1.65685. Full pooled deltas1098 cells,758 changed.
- L9 is running under the shared heavy lock. Data dated2020+ remains unread.
- G0 harness/progress/static-membership review commit1e048eac in pool3. Later import
  reviewed replay+D12 repairs before corrected baseline; then archive unpinned binary and
  pin cp21's original26 families only in its diagnostic branch. No tuning.

## Next sequence

### Current live jobs and repair status

- Root pool2 session60207: inference bandwidth rebuild plus focused26 tests. Earlier
  sync build passed, first test run25/26: MA20 rejection141/2000=7.05% above unchanged7%
  ceiling. Fixed actual kernel mismatch by matching existing cross_section_ic HH
  max(h-1, rule-of-thumb) rule, with paired obsolete-rule diagnostics; result pending.
- Replay production8ba15b0efeaff53587cc0ac36493cd159247b126 passed wholebook128/128,
  focusedimpl32/32. Test-fixture correction4f257729d2949f082c818f53b42c2c7aca3b9cf9
  expands holding-window fixture to4days. Independent reviewer verified logs; PCH-off
  check/report pending. Replay agent will then scope native ASan risk acceptance gate.
- D12 report39347e28 build near completion inpool5; focusedimpl/wholealpha pending.
- G0 pool3 merged D12+replay provisionally at8ee78be46c0cfc01d0c892e77fd4a2671ce942f7,
  isolated Release rebuild underway, worker1. Corrected baseline authorized after D12
  runtime gate; replay gate passed. Frozen L9 still runs from archivedbc5 binary.
- Final wholeimpl belongs only to integrated root correctness gate (not duplicated across
  repair lanes). Data exclusions and environment clearing remain mandatory.
- Benchmark follow-up assigned to audit agent after current tasks: oldO1 3ccf012c vs
  final integrated equity-bench, identical filtered repetitions3, quiet host. Not started.

Finish G0/build audit and integration repairs; run focused post-implementation validation,
fresh adversarial review, and the W0 integrated gates. Record honest old/new metrics and
benchmark evidence. Then scaffold W1, begin D1/D5 data foundations with new modules avoiding
tier1-parity ownership, and schedule A1/E1/R1/B1 followed by the remaining DAG dependencies.
Keep this file current before handoff/context compaction.
