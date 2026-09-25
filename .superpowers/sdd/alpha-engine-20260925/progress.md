# Alpha-engine DAG continuation - 2026-09-25

## Objective and boundaries

Complete the 2026-09-24 DAG series with sub-agent development, implementation before tests,
and real engine/alpha improvements. The goal remains ACTIVE and unlimited; do not create
another goal or claim series completion. W0's original ten lanes and FIXUP were merged at
bc5cc646b46f6a7c23a60e87d28dfa9b972671ec, but its integrated gates and G0 are still open.
W1-W5 implementation has not started.

The current user request authorizes continuation beyond the old W0 stop. Owner-controlled
2020+ data remains sealed. Only 2013-2019 development data may be evaluated. Do not write,
build, switch, stash, commit, or run migrations in C:/atx, which belongs to tier1-parity.
The sole permitted coordination write there is the token-owned data/.heavy-run.lock.
No pushes, raw worktree creation, warehouse writes, or killing others' processes.

Authoritative scope: docs/plans/2026-09-24-alpha-engine-production-swarm.md, its review-findings
companion, and the swarm-goal-prompt. Read CLAUDE.md, .agents/cpp/agent.md and harness contracts.
No graph MCP is available, so targeted rg is the discovery fallback. The deleted atx-vol
ledger is continued in atx-engine/docs/LEDGER.md. Only root appends ledger facts.

## Ownership

- Root: pool-2, feat/aes-codex-integration-20260925; lease aes-codex-integ-20260925,
  heartbeat aes-codex-integ-20260925T2130. Frozen original base bc5cc646.
- g0_evidence: pool-3, feat/w0-g0-codex-20260925; lease aes-w0-g0-codex-20260925.
- w0_replay_integration: pool-4, feat/w0-replay-integration-codex-20260925;
  lease aes-w0-replay-codex-20260925. Now also owns scoped ASan and summary disclosure fixes.
- w0_gate_audit: pool-5, feat/w0-membership-integration-codex-20260925;
  lease aes-w0-membership-codex-20260925. Also leases pool-6 for the benchmark baseline:
  feat/w0-bench-baseline-codex-20260925, run aes-w0-bench-baseline-codex-20260925,
  keeper23348, base3ccf012c40ef42c49ed21aaec96476455e06e049.
- Prior pool-1 feat/w0-integration at bc5cc646 has a live old heartbeat; leave it untouched.
- Main remains 2e0d738f. Local fast-forward is authorized only after actual wave gates.

All commands need explicit workdir/path. Lease acquisition can fail in automatic dev
configuration AFTER publishing a valid lease; inspect the record, then use equity presets.
Reuse existing pools, -MaxPool11; do not create more. Four agent slots including root.

## Live jobs (latest checkpoint)

Root session56041 runs .superpowers/sdd/w0/run-integrated-gate.ps1. Configure passed with
isolated pool-2/deps/equity-dev, PCH ON and groups alpha/factory/learn/data/eval/combine/risk/book.
The 394-step build was at102/394 at last inspection. It then automatically runs nine whole
test executables sequentially, with data exclusions and cleared opt-ins. Source compiled is
b185d056440704e7ebcfe2b9395601d7e5264269; later root commits change documentation only.
Do not modify root production sources during the build. Logs/hashes are under
build-equity/w0-integrated-gate/ (configure.log, build.log, results.txt, per-target logs).

Root's first launch correctly stopped before configuration at2.39GiB free. A membership
hygiene check accidentally expanded through a test-object worker dependency to182 actions;
direct Ninja ignored CMAKE_BUILD_PARALLEL_LEVEL and spawned many workers. Its owner stopped
ONLY the verified pool-5/build-hygiene Ninja824 process tree. No other pool was killed.
Memory recovered to7.8GiB and regular root/G0/ASan one-worker builds resumed.

Use CMAKE_BUILD_PARALLEL_LEVEL=1 for build (Jobs controls ctest). Wrapper check invokes Ninja
directly, so use one production TU per check and inspect dependency closure; never test
objects that pull worker/link dependencies. New builds require >4GiB free and <=3 total
compiler workers. Existing jobs may continue above2GiB. No quiet performance claims while
other builds, tests, or G0 runs are active.

## Repairs merged and reviewed

- Inference52b8c6ea: public versioned inference/raw-return config and declared label horizon.
  V3 overlapping labels use HH Uniform max(h-1, rule-of-thumb lag), matching existing eval;
  NW1994 plug-in is Bartlett-specific. Weighted V3 uses the exact sandwich variance; full-lag
  cancellation guarded. V1/V2 remain reproducible. Callers still must declare horizon;
  irregular finite-row compaction is an approximation. Focused26/26, independent6/6,
  scoped PCH-off production3TUs pass. Fixed2000-stream MA20 null:6.30% versus obsolete7.05%,
  unchanged3%-7% bounds and seed, zero old fallbacks, mean old lag30.98 versus HH20.
- Replay47e5ef8e (production8ba15b0e, fixture4f257729) merged33c9f194. Correct default and
  explicit Abort reach baseline/book consumers. No future last-print or unpublished-event
  evidence enters default prefix NAV. Whole book128/128, focused impl32/32, PCH-off6TUs pass;
  independent review APPROVE. Unevidenced shorts now receive adverse stress losses, not
  fabricated gains. Assumptions are distinct from actual terminal events and invalidate
  alpha evidence. This deliberately corrects the literal old all-sides-negative B0 clause;
  do not claim that literal clause passed or that the owner waived it.
- Membership productionaf40186d/report39347e28, reviews9e6b9f27 mergedf390f6d2. VM owns a
  validated CS eligibility mask across all16 CS opcodes, fused/date-parallel included;
  raw time-series history is preserved. Masked panels bypass the old subtree cache.
  Family masks cover warmup and evaluation. Focused27/27 (96.01s), whole alpha706/706
  (33.190s), independent physical reduced-universe oracle1/1 (14ms) pass. G0 reviewer APPROVE.
  Two production-only hygiene checks remain; their dependency closure is exactly2 compiles.
  Reports d988f7b8 and benchmark preparation224be5f7 are docs-only and not yet imported.

## Active ASan and reporting follow-up (pool-4)

The concrete R04 out-of-bounds defect triggers cpp instruction section8's explicit reversal
of its earlier sanitizer decline. Native LLVM18 ASan is installed. Debug CRT /MDd fails;
scoped equity-asan uses Release CRT /MD, /Od, NDEBUG, PCH OFF, static libs, isolated deps.
The dedicated target instruments actual factor_model.cpp and the existing sector fixture.
Root's independent static review has no blocker so far; actual runtime proof remains pending.

Files: CMakePresets.json, atx-engine/tests/CMakeLists.txt, sector-columns fixture,
and scripts/test-risk-sector-asan.ps1. Build preset defaults to the scoped target. Runtime
components and symbolizer are discovered and validated. Exact old beta[2] read must emit
ASan heap-buffer-overflow in a positive detection control; corrected five-test suite must pass.
No UBSan or whole-engine instrumentation claim. Support/vcpkg dependencies are uninstrumented.
Mixed STL annotations initially failed linking; target-private string/vector annotation
suppression is allowed, with explicit exclusion of logical size-within-capacity checks.
Raw allocation bounds remain instrumented and must be proven by the negative-control read.
A getenv warning was fixed with getenv_s, without weakening /WX.

G0 found an additional real presentation defect: nested replay summary flags assumptions,
but top-level baseline summary copied performance without eligibility fields. Replay owner
is authorized to propagate eligibility, reasons, counts and PnL into BOTH baseline/book
summaries, implement first then extend focused tests. Keep this production commit separate
from ASan wiring so G0 can import it for a tiny Release rebuild and seconds-long rerun.
Restore pool-4 equity-dev PCH ON before its regular impl target build; use sequential checks.
Root will review the new summary patch; no metric/math changes are intended.

## G0 evidence (all pre-2020, no tuning)

Immutable original binaries at C:/atx-wt/g0-data/bc5cc646_20260925/bin/unpinned, with hashes.
Corrected unpinned source8ee78be46c0cfc01d0c892e77fd4a2671ce942f7 includes D12+replay.
L7/L9/L10 do not use the later repair paths; no duplicate rerun solely for those changes.

- Frozen2013 baseline: exit1,2.031s,peak0.146GiB, abort security150340 at period6/2013-04-12.
- Corrected2013 baseline: exit0,2.266s,peak0.146GiB; Abort control exit1 at same point,1.766s.
  INELIGIBLE as alpha evidence:32 assumed liquidations,0 evidenced, stress PnL-$1,957,929.80;
  7 flagged shorts lost$154,981.83. Diagnostic SR1.333373/return5.91% are not alpha evidence.
  Nested report has flags; top-level propagation fix above must be retested/rerun.
- L7: exit0,61.625s,peak0.105GiB;151 fields differ only in path separators and3 tiny numeric
  values (~1e-9/1e-10), unchanged headline verdict.
- L10: exit0,302.079s,peak1.201GiB, NO CANDIDATE. No positive non-reference nonoverlap t>2.
  qual_gpa t1000 IC .01652->.0166986,t1.670->1.76392; t3000 IC .02324->.0226574,
  t1.816->1.65685. Full1098-cell comparison has758 changes.
- L9: exit0,987.109s,peak1.007GiB,zero admitted. Candidates2243->2504,scored2065->2306,
  families52->56,n_eff5.714->6.7503. Validation netSR-1.218738->-.535778,
  p.950334->.770325, still BY/RW1. New digestcdf326b6f5d6e3a8.
- I24 old report correction is committed: old saved validation netSR was-1.218738142997244,
  p.9503335211679915, not the previously claimed positive figure; reused2019 netSR1.7296874867
  with3 prior reads. Source SHA256cd8ac431e797d902fd93a558184a26d919f809336c95b6f906b1ed7e27b15824.

G0 is finishing the7-step diagnostic26-family rebuild, then13 serial cp21 cells (~12min
historically) and frozen Python scorecard (perhaps15min). Pin is NEVER committed: archive
unpinned source/exe, apply local pin, archive pin patch/exe, restore both source files, then
run immutable diagnostic binary. Final report/harness commit must contain no pin sources.
Heavy lock is atomic token/PID-owned and removed only by its matching owner. Publish final
hash-bound manifest last. G0 will release its compiler slot to the audit agent after pin build.

## Benchmark preparation (audit agent)

Pool-6 baseline at O1 configured equity-bench successfully: Release, equity-only, bench ON,
groups all, isolated deps/equity-bench. No baseline build or measurements yet. Current may
freeze b185d056 production, provided later ASan edits change no engine/benchmark behavior;
top-level reporting follow-up changes must be disclosed and inspected for benchmark relevance.
Build bench+worker serially. Complete66-case filter:
^BM_Kernel|^Wq101_|^BM_Search|^BM_OptimizerProduction/M:(1000|3000|5000)/mode:(4|6|7)/
Verify actual registration names, same keys, repetitions3, no AllowMissing or Update bypass.
Windows CPU-set API verified i7-1260P P-core logical0-7, affinity0xFF; topology archived.
Measure only after G0 and root tests/builds are quiet. Gate threshold20% stays fixed.

## Remaining wave gate and next sequence

Finish the active combined correctness run, scoped ASan, membership hygiene, top-level
summary repair, G0 comparison/hash manifest and fair quiet performance gate. Merge only
reviewed code and report commits (never diagnostic pin). Qualify newly changed target
closures after these follow-ups. Current data gate excludes actual external2024/2026
fixtures even when env vars are cleared; DataUniverse doc-only test stays included.

Acceptance limits stay explicit: E0a HAC interval coverage94.5% passes; bootstrap92.5%
is a distinct estimator. E0b MonteCarloMaxV2 gate FPR5.45% is calibrated; conservative
ClusterMcFloorV2 default <=1.41% is a different rule. D0 VWAP is an adjusted typical-price
proxy, not dollar turnover. No quiet benchmark or profitable-alpha claim yet.

Then freeze W0 gate SHA, fast-forward local main only, and scaffold W1. Start D1 PIT identity
and D5 data foundations on the critical path, with new atx-db modules avoiding tier1-parity
ownership. Follow DAG through W4 and W5 development evidence; owner-only2020+ unseal stays
closed. A18 production hash-only caches belong to W2-A4, R06 production PIT exposures to
W3-R4; these are not already closed. Keep this checkpoint current before compaction.