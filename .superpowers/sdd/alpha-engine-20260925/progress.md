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
- w0_gate_audit: completed membership branch at b09f45cee1f45091d8922b26763cec936031d5b4,
  released its pool-5 membership lease, then re-leased pool-5 for current benchmarks at
  b185d056 (run aes-w0-bench-current-codex-20260925, keeper460). Also leases pool-6 baseline:
  feat/w0-bench-baseline-codex-20260925, run aes-w0-bench-baseline-codex-20260925,
  keeper23348, base3ccf012c40ef42c49ed21aaec96476455e06e049.
- Prior pool-1 feat/w0-integration at bc5cc646 has a live old heartbeat; leave it untouched.
- Main remains 2e0d738f. Local fast-forward is authorized only after actual wave gates.

All commands need explicit workdir/path. Lease acquisition can fail in automatic dev
configuration AFTER publishing a valid lease; inspect the record, then use equity presets.
Reuse existing pools, -MaxPool11; do not create more. Four agent slots including root.

## Live jobs (latest checkpoint)

Root session56041 COMPLETE exit0. Integrated sourceb185d056 built394 outputs in2375.269s,
then all9 exes passed:3074 passed,7 documented skips,3081 run. Per-target counts/seconds,
source/exe/log hashes and data exclusions are recorded in build-equity/w0-integrated-gate;
report .superpowers/sdd/w0/integrated-correctness-report-codex.md committedc040476d.
No root compiler or test process remains. W0 is still NOT gated.

Root merged reviewed ASan/disclosure plus W1 prep through911071f1 at6f0e8d81 AFTER the
integrated job finished. Actual new production is stage_equity_baseline.cpp and
stage_equity_book.cpp; ASan CMake/fixture/script also merged. Root must qualify only these
affected target closures after compiler fixes, not rerun untouched8 groups. Benchmark engine
include/src/bench/core/tsdb are byte-identical to b185 (independently checked).

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
  Two production-only hygiene checks passed sequentially. Final report b09f45ce includes
  functional report d988f7b8 and benchmark preparation224be5f7; imported at1db7fdb0.

## Active ASan and reporting follow-up (pool-4)

The concrete R04 out-of-bounds defect triggers cpp instruction section8's explicit reversal
of its earlier sanitizer decline. Native LLVM18 ASan is installed. Debug CRT /MDd fails;
scoped equity-asan uses Release CRT /MD, /Od, NDEBUG, PCH OFF, static libs, isolated deps.
The dedicated target instruments actual factor_model.cpp and the existing sector fixture.
The native gate PASSED and root independently approved code370f7af4. Report3e6886ce awaits
import. Root review is .superpowers/sdd/w0/review-sector-asan-codex.md (commit1ce85d53).
Independent run5/5 passed in199ms, binary SHA256
4B4F892532390B2794B184911E8930161E2F926B51C9B39B62117B23D5B45533.
The forced old read emitted native heap-buffer-overflow at zero bytes beyond a16-byte
allocation, exit1. All corrected tests passed, with zero skipped. This is ASan, not UBSan.

Files: CMakePresets.json, atx-engine/tests/CMakeLists.txt, sector-columns fixture,
and scripts/test-risk-sector-asan.ps1. Build preset defaults to the scoped target. Runtime
components and symbolizer are discovered and validated. Exact old beta[2] read must emit
ASan heap-buffer-overflow in a positive detection control; corrected five-test suite must pass.
No UBSan or whole-engine instrumentation claim. Support/vcpkg dependencies are uninstrumented.
Bundled LLVM18 runtime failed startup; the final preset selects coherent MSVC14.42 runtime,
import library and thunk from vcvars (not an unmatched DLL substitution).
Mixed STL annotations initially failed linking; target-private string/vector annotation
suppression is allowed, with explicit exclusion of logical size-within-capacity checks.
Raw allocation bounds remain instrumented and must be proven by the negative-control read.
A getenv warning was fixed with getenv_s, without weakening /WX.

G0 found a real presentation defect: nested replay summaries flagged assumptions, but outer
baseline/book summaries copied naked performance. Production1bdeed388b8819d539175df65c202fd33b6f2d51
and reportd4b86cd23581e5d4bba92b02181001cd0dce36f7 fix both consumers, copy all10 fields,
fail qualification on assumed liquidations and hash-bind the book summary. Numerical replay.full
is unchanged. Both production TUs passed PCH-off checks. Focused32/32 passed40.32s.
Independent review766bac4a approved exact code and ran2/2 changed fixtures in1.820s.
Root imported that review as6e2a3c4e, path review-replay-disclosure-codex.md.
ASan+summary production and all three W1 prep briefs were merged at6f0e8d81 after the
root job completed. Final changed impl/risk closure is pending; original broad run stays valid
for unchanged groups. Independent ASan5/5 and disclosure32/32 plus fresh2/2 remain recorded.
G0 imported only summary production; its final disclosed binary is built already.

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

G0 completed all13 baseline+13IC cp21 processes: exit0,608.406s combined,peak0.862GiB.
All13 family/cost recipes match historical manifests: numeric checkpoint21,29 streams
(3 baselines+26 frozen families),declaredN80. Legacy iteration22 trial-ID/prose remains;
recorded as erratum alongside frozen Python scorer's stale year-union label. No numerical
rerun solely for prose. Diagnostic source pin was never committed; both files are restored.
Patch and diagnostic exe are archived. Production diff is empty.

G0 COMPLETE: report08cb4ae37381e294d6c5129b599f2e13c45881a7, imported on root9225cb30
(after metadata commits1e048eac/b289bdb9 -> rootb004d7c7/f19d522b). Python scorecard exit0,
938.703s,peak0.303GiB; R16-8 remains0/29. Intraday_mom_252 t3000 h21 netSR .676475
[.003924,1.383343] -> .492608[-.225724,1.280178]. All individual h21 pooled-cut lower
confidence limits are now <=0. No alpha promoted. E18 minimum50 plus other fixes materially
increase sparse counts; these are summed over horizons, not unique dates.

Final disclosed sourcec32df9512075879827b75f5e465f2640c579d4c8 is built and archived
under bin/corrected-disclosed; atx-impl SHA256
2edbf4f5177ca3f9a8169ff6ee8ba3e4e939ceaf98e9b450a0bad77ee2f0870e.
Final disclosed native exit0,3.782s; explicit Abort exit1,same error,2.015s. Both0.146GiB.
Top qualification failed and all10 fields match nested summary; nested replay bytes exactly
match prior8ee output (49 leaves unchanged), only12 outer-summary leaves changed.

Final g0-artifact-manifest.json published22:19:45Z, SHA256
3e2fd328a15c6671d81aff9aa2012388aad924e995b6b8185591259f117da679.
35 receipts,33 recursive manifests,597 evidence files,13 exact frozen recipes;
54 comparison tables/2,027,028 metric+metadata cells. Heavy lock clear, no active G0 process.
Root independently matched final manifest hash and5 selected comparison/receipt file hashes.
G0 owner now prepares W1-I1 sidecar/prereg docs. Pool3 CRLF ledger failure is local checkout
state; root independently verified all9 ledgers i/lf,w/lf under *.jsonl text eol=lf.

## Benchmark preparation (audit agent)

Pool-6 baseline at O1 configured equity-bench successfully: Release, equity-only, bench ON,
groups all, isolated deps/equity-bench. Build completed235/235 exit0 (~20min).
Baseline exe SHA2568bd72411065d71edbdd62eb37ffc2230aee0bf050681aa2096d85b5f04eea966.
Registry verified exactly81 cases (66 earlier was wrong:15 SearchThroughput also match).
Current pool-5 configured63.7s and completed235-action benchmark+worker build, exit0,
original session59572 revalidated terminal (do not restart). Source766bac4a is docs-only child
ofb185. All224 compiler calls cacheable:84 direct hits,140 misses,ZERO preprocessing failures.
Dedicated w0-current-ccache log/statslog/own reports are in build-equity-bench. Baseline/current
161 normalized selected production/bench commands exactly match. Registry must verify81 cases.
No timings yet. Sole compiler slot released; host now~1.3GiB free, all new compilation held.
Baseline exe SHA recorded above; current owner collecting final binary/registry receipts.

Full filter (never narrow):
^BM_Kernel|^Wq101_|^BM_Search|^BM_OptimizerProduction/M:(1000|3000|5000)/mode:(4|6|7)/
81 cases:33 kernels,20 WQ,19 search,9 optimizer. Repetitions3, exact same keys, no skips/errors,
finite positive timings; gate threshold0.20, no AllowMissing or Update bypass.
Pin ATX_WQ101_INSTRUMENTS=500 and throughput756dates x500names x6generations identically.
P-core logical0-7, affinity0xFF; CPU-set topology archived. Measure after G0/root tests/builds
and all compiler work are quiet. No performance measurements yet.

## W1 preparation only

Root19c03e3d adds lane0-preparation.md with source stubs and precision/label-clock contracts.
Pool-4 edbcb206 adds lane-b1-preparation-brief.md: one-way units, causal ADV, actual holdings
deltas, modeled borrow provenance, and primary FIM/EDGE sources. Not yet imported.
Pool-4 is preparing D1/D5 read-only: pinned tier1 already owns historical_identity,
market_owner_bridge and identity_reconstruction plus migrations0326/0327; avoid those files.
Current export load_id_bridge is dict[sr_id->cik] and main duplicates all CIK snapshots to
all mapped IDs. Correct interval joins require a narrow exporter caller/output contract
extension plus later D3 consumer interval support; no owner permission needed for routine
correctness scope. The referenced82 recycled-ticker IDs and19-date corrupt-session manifest
are evidence prerequisites not yet located. No warehouse connections or real rows opened.

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

## User steering: compiler setup and incremental compilation

User explicitly asked to optimize compiler setup/incremental compilation because builds are slow.
Keep original DAG goal active. Replay owner now implements bounded build fixes in pool4 after
read-only prep911071f186ed43bdbfc8baab110392e7fc21af02 (A1) andd3e0c59a (D1/D5).
Approved owned scope: scripts/atx-build.ps1 + script tests/docs; CMakePresets equity-hygiene;
atx-impl/CMakeLists source-local Git-SHA define; atx-engine/tests/CMakeLists stable PCH carrier
plus minimal carrier source. Implementation first. Preserve warning/FP/ISA/CRT flags.
Explicit -Jobs wins for build/check, else valid CMAKE_BUILD_PARALLEL_LEVEL, else1; ctest unchanged.
Use dedicated equity-hygiene binary/deps directory, NEVER toggle PCH off/on in equity-dev.
Stable PCH must match consumers exactly and avoid worker dependency explosion. Keep that change
separate to avoid invalidating every already-built test object in W0. Review by audit agent.
No compilation until >4GiB and coordinated slot. Benchmark flags/preset snapshots stay frozen.

Measured root build:222 engine-test outputs1367s,93 impl-test outputs542s,67 production/deps404s,
12 links61.5s. Global ccache cumulative11124/18787 cacheable,5754 hits,5370 misses,7663 uncacheable
(7581 preprocessing failures). These historical totals are NOT attributable to this build.
Audit owner will capture per-build CCACHE_STATSLOG/LOGFILE; avoid duplicate diagnostic compiles.
Confirmed avoidable triggers: Git SHA macro on all34 impl-core TUs but only stage_discover uses it;
engine-test PCH owner changes with first configured group; wrapper -Jobs absent from build/check.
No claim of measured improvement until no-op/localized-rebuild/cache diagnostics are verified.

## Recovery / current build-tools follow-up

Previous goal turn made concrete progress (completed integrated gate, merged reviewed repairs,
G0 report/import and committed build-tool improvements), not no-progress. Two agents hit a
transient authentication failure; root later recovered both through followup_task. No secrets
were inspected or changed. Original benchmark session59572 finished exit0 while the agent was
unavailable; authoritative terminal status was inspected, never restarted from old intention.

Pool4 HEAD464d0483 (440116f3 base build fixes,464d0483 rejects attached -j+4/-j=4 overrides).
Dirty pending PCH edits: atx-engine/tests/CMakeLists.txt,pch.hpp,new pch.cpp. Two minimal carriers
(common and data-specific miniz include path), exact options preserved; no worker/test
dependency. Configured344 existing commands match modulo PCH/output and deliberate SHA
localization; exactly1SHA consumer stage_discover. Common matches18book consumers; data37.
Each carrier closure exactly2 commands. No actual carrier/consumer compile proof yet.
Seven oracle-targeted-gate.ps1 raw--parallel2 call sites +four assertions being migrated to
-Jobs2 as required compatibility; no oracle workflow execution. Full old script suite depends
on removed atx-vol files, so owner validates focused pure argument-spec functions honestly.
Fresh adversarial review by audit owner remains pending final SHA/proof. Keep stable-PCH
commit separate to avoid forcing a second broad test rebuild solely for build wiring.

Scheduling refinement: original4GiB launch floor was an orchestrator precaution. Current
benchmark was allowed >3GiB, ZERO other compiler, exactly1 worker with >2GiB reserve monitoring;
launched at3.736GiB and completed. At current~1.3GiB no new compile/benchmark is allowed. Never
kill another session's Code/Chrome/Python/processes. Small existing configure/graph inspection
may proceed if no dependency/package compilation is triggered. No compiler flags changed.

G0 agent independently diagnoses current ccache misses. Fresh224/224 cacheable with zero
errors disproves a CURRENT preprocessing-failure claim; historical7581 errors remain
unattributed. Missing Clang PCH timestamp option and BASEDIR /FI rewriting are only old
hypotheses; do not change flags/config without an actual reproduced failure.
