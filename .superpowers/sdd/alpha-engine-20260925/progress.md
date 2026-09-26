# Alpha-engine DAG continuation - 2026-09-25

## Objective and boundaries

Complete the 2026-09-24 DAG series with sub-agent development, implementation before tests,
and real engine/alpha improvements. The goal remains ACTIVE and unlimited; do not create
another goal or claim series completion. W0's original ten lanes and FIXUP were merged at
bc5cc646b46f6a7c23a60e87d28dfa9b972671ec, but its integrated gates and G0 are still open.
W1-W5 are not complete; the owner pulled forward the IC-screen subset of W2-A4.

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

## Active owner steering and implementation (2026-09-26)

LATEST OWNER INSTRUCTION supersedes the long performance hold below: no25-45minute
runs at this stage; focus building. The81-case benchmark is DEFERRED, NEVER passed.
Do not launch it or automatically rerun mining. User prioritized vectorized rough
forward-return IC across several horizons before full backtests, retaining true alphas.
This explicitly authorizes proceeding with the scoped W2-A4 subset before that long gate.

LATEST COMPLETED PACKET: 123163e2 qualifies production7e9f6f4a, clean build6a401f56,
with test-only fixture correctione2aaea7e. W1 foundation83/83 (19.904s); IC63/64
then corrected fixture1/1 (17.362s+0.011s); impl42/42 (35.752s). 189distinct
checks pass, zero skips/unresolved failures. Initial assertion omitted decision-price
eligibility; production unchanged. Initial build324.500s stopped on missing JSON
include; resumed source-local fix375.350s; test-only rebuild8.531s. No active
compiler/runtime remains. Full wave/performance/RSS/harness gates remain open;
V3 practical noisy pruning/general recall still unqualified. Logs/XML/receipts
and binary hashes in pool2/build-equity, report w1/foundation-v3-qualification-report.md.

Next source is NOT imported yet: B1 core a3ee26f0, tests7cad09c9, report3a9ae917
independently source-approved by root; calibration63ea1ef4 awaiting source review
and postimplementation fixtures. A5 first slice d9925eec under review: persist corr
recipe on reopen and use bounded top16 exact refinement for PoolView approximate
continuous redundancy; compact records/segments/resolver still in progress.
D1 pure synthetic source work continues, including narrow PitRecord/decoder expiry
wiring. Do not build imported subsets separately: batch the next coherent release.

Earlier detail (superseded by completed packet above where it says pending):IC V2 source through abe8fec6 passed 102 distinct focused checks, zero failures/skips:
engine59, impl42, additional widecohort1; final discovery8 repeated after provenance
refresh. First build402.686s, completion78.394s, wide/provenance43.617s. Small noisy
cohorts rejected0/18 nulls; planted effects retained but no population recall claim.
Report .superpowers/sdd/w1/priority-ic-report.md remains historical V2 evidence.

Independent source review approved explicit EquivalenceV3/.002/3.5: kernelb9e55206,
mined7b11bc5, global8ccf0982. V3 removes only the arbitrary one-SE veto; every
horizon/metric still needs enough evidence and its bound below the protected floor.
Quarter safeguard is heuristic. Underpowered candidates pass; practical pruning
and production throughput remain unqualified. V2 remains explicit with its recipe.

W1 A1 imports aeea9b55 through5a64fa2d, reporte7fa0e3c; E1 imports0d7a45ec,
4e49679e,e2ea5e6d,43d5ad98; scaffold95d0773d. Source reviews accepted. Combined
build8ccf0982 compiled32productionTUs then failed cube JSON include (324.500s).
Root7e9f6f4a adds source-local JSON includes to cube/test without global PCH churn,
and independently reviewed date-outer IC rank reuse with a same-count/different-ID
fixture. Remaining closure42C++objects+5links; no PCH/dependency/worker rebuild.
Runtime pending. Only root builds, Jobs1; actual head/provenance frozen at configure.
Logs pool2/build-equity/w1-v3-*. No long benchmark/mining run authorized.

New source lanes active in existing leases: pool3 D1 dated security links plus
minimum exporter/PitRecord/decoder validity wiring; pool4 A5 compact library,
absolute-correlation retrieval and lifecycle; pool5 B1 cost-surface core/adapters.
No source imported yet. Only synthetic short Python checks allowed to D1; no agent
C++ builds. No warehouse writes/migrations/data downloads/actual2020+ payload reads.
Root owns CMake/DAG/ledger. Full B1 estimators/calibration/borrow remain separate.
Original DAG index records existing source SHAs; W1 and all later gates stay open.
Scaffold adds factory/ic_screen.cpp to CMake. Root branch remains integration;
main has NOT been fast-forwarded. Existing production correctness evidence remains valid
for frozenfcbcc; new implementation will get bounded owning checks after implementation.
Kernel owner /root/w0_replay_integration inpool4: newfactory/ic_screen.hpp/.cpp + tests.
Search owner /root/w0_gate_audit inpool5: search_driver,search_state,search_progress,
factory.cpp admission paths and tests. Mine owner /root/g0_evidence inpool3 (after concise
L9report): stage_equity_mine.cpp/.hpp +mine tests. Root: CMake, globalconfig,discover/
resumeidentity/progresssink/trialledger bindings, integration and review. Coordinate API;
no overlapping edits. Existing leases may be reused for this priority fix; merge scaffold.
No agent compiles until root grants sole Jobs1 slot. No TDD, no whole-suite ceremony.

ICscreen must reuse already-materialized VMsignals, immutablecachedforwardlabels and
worker-local scratch; realSIMD acrossinstruments, no stripped/stridedVMhistory. Training
labelmaturity cutoff, delay, masks and ReturnGuard remain explicit. Conservative reject
only if adequatelypowered bounds exclude a configured practical absIC in EVERY horizon;
short/sparse/uncertain evidence passes. Retain weak/inverse/longhorizon alpha. Explicit
DisabledV1 preserves legacy. Every screened trial remains counted and durable; nofakePnL.
SearchResult.all_scored includes every trial; existingFactory andmine currently rescore
all_scored, so explicit persistedrejectionidentity MUSTfilter everyfullrescore/admission
path, including alreadydedupedseeds. Merelyreturningraw=-inf withouttypedresumeorigin is
insufficient. Auditsfound fiveFactoryrescorepaths. Onlyfinalgenerationadmitted_candidates
is NOTa replacement forall_scored because earlier validcandidates woulddisappear.

L9 existingrun COMPLETE native0,818.734s,peak0.964GiB; heavylockreleased. Output
C:/atx-wt/g0-data/w0-vwap-v2_e464000f_20260926. Old->V2:candidates2504->2523,
scored2306->2293,family56->47,admitted0->0;validationblendSR-.535778->-.106037,
BY/RW1. ActualDSRclusters3(distinctfromfamily47),Nraw2293/Neff6.0949568. Manifest
SHAa1bae0951662f08298ae2483011059957ee0079dd7dab04aec0fa5dbb10651ea,31files,
oldparent3e2fd328...preserved. Final report9889f2a imported5c933ba6; root independently
verified all31file sizes/hashes/pathbounds and unchanged parent. Numerical closure done.

Finalbenchbuild/registryreport source3fdb60ce imported151b92cb. Root independently
rehashedfinalbench/worker/runner/protocol and verified81exactnames/order, receipt at
build-equity/w0-vwap-closure/final-benchmark-prerequisites-verification.json. Currentbench
e708b192...,workerdbc05f35...,runner63bba16c...,protocolf29b4a0f.... Actualunchangednoop
3.869s/zerocachecalls. Ownedjobsmokepassed but noeligibletimingcomparison exists.

## Current checkpoint (2026-09-26; supersedes historical notes below)

Latest user concern: too much compiling and insufficient visible engine progress. Required
D0 correctness and Release builds are now COMPLETE. Do not rebuild or repeat passing
correctness suites absent an actual changed source, failure, or unresolved concern.
W0 remains open only for corrected L9 evidence and quiet performance qualification.
W1-W5 implementation has not started. The goal remains active.

Root HEAD before this checkpoint: 00a9328d, clean. Frozen production/build source is
fcbcc9d1a351ccd339687389b118aca65e2812b5; subsequent root commits are reports/preparation.
Original DAG completion index is docs/plans/2026-09-24-alpha-engine-production-swarm.md.
Final D0 report 932c1061 and independent approval b131c3f4 are integrated. All four
owning targets: 1,683 passed, six known skips, zero failures. Alpha 711/711, data239/240,
book128/128, impl605/610. Independent N128 fixture oracle passed (all19 fields, masks,
every f64 bit, digest75b4a957ff30e9c3), plus nine independent focused checks.

D0 raw daily-close VWAP defaults to V2 with known raw basis, finite positive raw price/
volume, geometry validation, context/move policy propagation, and persisted CLI/recipe/
report/discovery identity. Explicit adjusted-typical V1 reproduces old behavior. Imports:
f54b55e5,2a7194d5,0f74a357,c1011024; capacity resume identity0037c515; tests58df3e29,
c68b78a4,1e4ee053,efe57fd9; frozen benchmark pins9c2ffd1f. This daily proxy is NOT true
intraday VWAP. Adjusted OHLC/mixed-basis lint remains W2-A3.

Completed build/runtime receipts (never restart these sessions):
- Root Debug session99434: native0,991.797s,163 compiler actions/6 links, Jobs1;
  exact unchanged repeat5.047s with no work/zero cache calls. First-use common/data PCH
  carriers and changed shared-header consumers explain the one-time closure. Peak owned
  RSS1607.44MiB; host physical minimum1.078GiB, commit3.210GiB; no pressure stop.
- Root runtime session73678: all data/book/impl results above passed. Known external-data
  exclusions and opt-in clearing preserved; no actual external data opened by suites.
- Alpha report033b89c2, scope auditf1620990: exact warm repeat3.106s/no cache calls.
  No additional affected default-policy group was found; do not broaden suites.
- Hygiene report63d28b5c: pool4 integrated context PCH-off native0,16.473s,84/84 first-party
  inputs equal root; real_panel prior source+34 headers unchanged. Five actual root impl
  commands already PCH-off and compiled successfully. Pool2 has NO equity-hygiene tree;
  its earlier attempted standalone check failed before compiler. Do not configure one
  just to duplicate verified owner checks.
- G0 Release session47187: native0,46 steps,327.542s,42 cache calls (12 preprocessed hits,
  30 misses), zero preprocessing errors, only atx-impl/Jobs1. Sourcee464000f entire tree
  equals fcbcc (treeb008adc830408a31a7871770673bacbaa561a811). Archived binary SHA256
  0ffc7848babbe279492eb3e3316afc72e24f22f4fc82a731c4da00d036ff9261. Archive at
  pool3/build-equity-rel/g0-vwap-build-e464000f_20260925; root independently rehashed all27
  file/size bindings, receipt in build-equity/w0-vwap-closure/g0-release-archive-verification.json.
  Archive manifest86ee554876a59ae262c1d12481a1b0cafb0d63e69042de38a681f0a2ab8891bb.
- Current benchmark Release session75401: native0,52.469s,8 compiler actions/3 links,
  Jobs1,sourceb7afdec8; peak ownedRSS1006.93MiB/no pressure stop. All161 normalized
  production/benchmark compiler commands match baseline flags. No more compile needed.
  Owner is completing hashes/frozen81-case registry/no-op/cache evidence.

G0 L9 has explicit root authorization to START after token-owned atomic heavy lock,
fresh runtime admission, six pre-2020 input checks and exact fixture hash. On resumption
at2026-09-26 14:26UTC there were no ATX/compiler processes, no heavy lock, and no new
w0-vwap-v2 output directory. Agent must confirm interruption left no run before launching.
Both existing G0 and audit agents were resumed; former replay owner has completed work.
Do not treat interruption alone as permission to rerun an already finished process.

Only full L9 needs corrected numerical evidence (impact report35c68fe6). Reuse L7/L10,
all13 cp21 baseline+IC, native replay and all14 unaugmented contexts unchanged. Original
frozen G0 root C:/atx-wt/g0-data/bc5cc646_20260925 remains immutable; manifest SHA256
3e2fd328a15c6671d81aff9aa2012388aad924e995b6b8185591259f117da679. New L9 uses exact old
receipt argv/environment except verified new executable, fresh output and explicit
--vwap-rule raw-daily-close-v2: pop192,gens15,threads2,seed20260923,train2013-16,
val2016-18,holdoutoff,seal2020-01-01. Fresh registry; no old numerical state. Bind exact
fixture checkout bytes1a04d5e7182e331e1f20c87c17528ae12156036e195fdbcf47995fa92aae1e9b;
LF blob265c7babc099a17ca242a00ae5299d9b8f407f6d7bf168e16006189f89312385 differs by EOL.
Original manifest omitted fixture bytes; retain that disclosure. Publish new manifest last.
Prior L9 987s/~1.01GiB is scheduling evidence only. No tradeable alpha has been demonstrated.

Quiet benchmark remains fixed: N128,81 cases,three reps,20% regression threshold;
WQ2520dates/70alphas/workers1,2,4,8,16; search756x500x6; optimizerM1000/3000/5000.
Five isolated family processes, all baseline first then all current. Baseline pool6 source
3ccf012c40ef42c49ed21aaec96476455e06e049, built; current final Release sourceb7afdec8 built.
The first baseline kernel process completed but was INVALID (host409MiB/externalCPU/
505741pagefaults/CV44%); invalidity receipt blocks aggregation, no kill occurred. Root's
metadata loop may have contributed. No valid timing yet. Never change frozen dimensions
or threshold after seeing results. Required fresh five-second native CPU/RAM preflight,
roughly3GiB launch floor, externalCPU <=1 logical core and sustained-contention guard.
ALL agent writers and root repository discovery must quiesce for actual timing. L9 and
benchmark serialize; audit holds timing until explicit release after L9 and quiet state.
Reduced N128 cache pressure does not qualify later production-scale gates.

Compiler policy: wrapper only, normal PCHon/static/equity presets, isolated hygiene PCHoff,
one native worker. Actual compiler admission preferably3GiB physical, allowed2GiBphysical
AND3GiBcommit with known closure/zero competing compiler. Metadata-only configure may
use1.5/3GiB but no package/project compilation. Sustained severe pressure guard stops only
verified owned tree; preserve objects/logs. Never kill other workers. Compiler improvements
e54602fe and d3d04510 are measured; source SHA only invalidates stage_discover out of385
commands. No-op receipts prove incrementality, not a controlled speedup ratio.

Next: finish corrected L9/manifest/comparison, finish quiet81-case performance gate;
record actual W0 gate and localmainFF only when all required gates pass. Then freeze W1
Lane0 and start D1 pacing lane plus disjoint production implementation. Prepared briefs
A1/B1/D1-D5/I1/Lane0/X1 and source/provenance investigations remain preparation only.
Do not repeat them in place of implementation. D1 historical coverage and unrecovered
82-case list are real unresolved items, not reasons to fabricate evidence or backdate
availability. Root alone appends atx-engine/docs/LEDGER.md.

## Historical live-job notes

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

At the time of the original broad build, Jobs controlled only ctest; this is FIXED above.
Inspect check dependency closures before running: test objects can pull worker/link
dependencies. Current single-worker memory policy is in the checkpoint above. No quiet
performance claims while other builds, tests, or G0 runs are active.

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

## Newest source staging (supersedes older pending-import notes)
B1 core e5f8ea8d, tests a3aed92c, report86e50ae8; scaffold f6704d55. Calibration416961a3/168cb34c/b0ddb715, report11268bb2, source independently approved after rank normalization fix. Focused cost target six TUs now registered. No new configure/build or runtime active; prior compiled binaries still qualify production7e9f6f4a/clean6a401f56 and test correctione2aaea7e only. A5 d9925eec/10f0d46e not imported; awaiting G0 review and fixtures. D1 source commit forthcoming; audit owner ready to review. Packet123163e2 independently approved by audit owner (all four XMLs and three binary hashes verified;189 unique checks). Batch next source releases into one warm Jobs1 build; no long benchmark/mining run.


## Current W1 qualification batch (2026-09-26, supersedes older pending-source notes)
Root pool2: source through bee493a6; original DAG indexes all imports. A5 through5d37aa27 + fixture9c4a2fd8; B1 core/calibration as previously recorded; D1 through592aa045 with independent reviewa5610cf0; E6 breadtha5bb66ac, PBOkernel20f22155/bcd96bfb andcallerbb86d9de, combinedsourceb17356ad/a53dbb63/dce7312c, regimes6c4c9006/7c919551. All source reviews approved; runtime remains pending. Integrated D1 synthetic Python15+7 passed (0.025s+0.154s native test time), logs build-equity/w1-d1-integrated-*-tests.log.
The first stable-object batch atdce7312c used Jobs1 and existing PCH/deps, stopped after88.563s on11 incomplete Provenance aggregate initializers in the new library fixture under -Werror. Test-only9c4a2fd8 supplies explicit fields; no production algorithm fix. Successful objects are retained. Log/start/receipt build-equity/w1-stable-objects-*. No compiler currently active after that stop.
Next final bounded targets: atx-engine-w1-{cost,eval,library,data}-tests, atx-engine-ic-screen-tests, atx-impl-ic-screen-tests. Focused eval now includes PBO,breadth,regimes,combined source,NNgate and factory caller/consumer checks; impl adds discover/provenance for rule binding. Only root compiles, Jobs1. 2020+ remains sealed; no warehouse writes, long benchmark or mining run. New source-only next lanes: pool4 R1 sparseconstraints, pool5 remaining B1 spread/FIM/borrow, pool3 D5 qa/universe; no imports during this qualification batch.


## Current W1 qualification result and next source batch (supersedes pending runtime)
Clean compiled source d9031039; report16c96293: 283 distinct C++ checks pass, zero unresolved failures/skips, plus D1Python22/22. Cost35, data19, eval72, IC64, selectedimpl56; library36/37 then corrected fixture0f3ace26 passes1/1. Fixture binds index recipe before first admission, no production change. Integrated build646.203s Jobs1, no PCH/dependency/worker rebuild; fixture rebuild16.989s oneobject+link. All runtime/build sessions finished. Binary/XML SHA bindings and caveats in engine-contracts-qualification-report.md. No rerun needed absent a new source change.
Next unimported source: pool3D5a5250e25 under pool5review; pool5B1modelsf9f73d26+fixtures3f64bdaa+reportc4253be0 under pool3review; pool4R1 slices96aadfb8/42f781a0 with relative feasibility/application wiring still in progress. Root reviewsR1, owns globalCMake/ledger and may implement remainingE6CPCV. No long benchmark/data/mining run; all compiler work rootJobs1 only. Original DAG updated with source and evidence SHAs.


## Current source/build handoff and owner worker change
Prior report16c96293 independently approved at487e8734 (source1b364cdd),283 uniqueC++ plus22Python. Root imports B1 modeled f60b085e/86bcfcb7/f4674eac with reports7954afd5/23c867fe; D5abb466be/587808d1, reviewda4d1575; root dateCPCVcoref7b51798/tests65834fa6/review3f977e75; R1throughaf7814d4 with fixtures613cdc26,report84342f42,rootreviewb11c6337. Original DAG indexes exact source/imports. Integrated D5Python7/7 in0.268s. C++ runtime for this batch pending.
Six small production objects (CPCVdate+costsurface+fourmodels) compiled successfully atb7a90377 in9.969s Jobs1 with no PCH/dependency rebuild. No compiler currently active. Configure succeeded atb7a90377 after quoting the Windows -D path correctly; initial unquoted configure failed before changes. Advisory Ninja copied manifest is DRY-RUN ONLY, never execute; graph was55objects7links before six successful objects.
User newest instruction explicitly allows faster compilation and says do not use just one worker. This SUPERSEDES all old Jobs1-only notes. Next bounded batch Jobs2, adapt to measured memory; onlyroot owns compilation so agents never add uncontrolled fanout. Host commit headroom recently below1GiB with no compiler active; gate the Jobs2 launch rather than kill other users processes. Keep PCH/deps warm. Next targets w1cost/data/eval/risk and impl-w1-contract; helper w1-run-focused.ps1 accepts them. No long benchmark/mining/data run.
Next agents: pool3 D6core98ad63d5 awaits rootreview while actualstageconsumer continues; pool5 E6CPCV versionedcaller/cache/config/identity migration; pool4 R2 estimator source design while available for R1runtime fixes. No newsourceimports during next build.

## Latest qualified batch and next source work (supersedes pending runtime)
Root clean production6debcc10; reportdbb16af3 records262 distinct C++ passes plus7D5Python. Cost46/data60/CPCV13/risk74/application68+corrected1, zero unresolved failures/skips. Fixture-onlyc42eb524 repairs assertion compile issues;402af294 supplies required type path so date cutoff guard is reached. No algorithm changes after6debcc10. Current root binaries qualify that production identity, not agent source below. All sessions finished; no compiler or runtime currently active. Jobs2 mainresume209.852s/final48.009s/cutoff17.575s, warmPCH/deps/objects retained, no long runs. The original DAG references reportdbb16af3 and exact sources. Root owns compilation with multiple workers and measured memory, agents source-only.
Next D6freeze98ad63d5+97a00229+6e44a6da pendingimports; root reviewed core/boundedmapping/masks, now reviews actualstage/history adapter. Root must add RunConfig.panel_storage_rule defaultlegacy-f64-v1/parser and CMake core+test. Independent D6 review assignedpool5 afterCPCV fixes/fixtures. CPCVcaller63352bc4 pendingimports, root reviewed most; sparse date-axis checkedfold workspace correction inprogress. R2slices01d28812+5e228f45 pendingimports; root reviewed kernels, requested covariance symmetry/PD validation before disabledsimulationreturn and locale-independent recipe. Hybrid/modelconsumer inprogress. Pool3 assigned reportdbb audit then W1-L1dataset source design; coordinate learnedconsumer ownership withpool5. No sourceimports/build until reviewed batch is ready. All actual2020+/mixedpayloads sealed, no warehouse writes or alpha promotion; W0longcomparison deferred, notpassed.

## Current source integration and bounded build state
Root pool2 through ef2d900f. D6 core/adapter imported through a100160c; E6 CPCV callers through bb32edcf; R2 through727535b9, fixturesfce0c442/8385e784, report977a93ee. Original DAG now records exact imports. Root final source review in review-d6-r2-integration-source.md resolves D6 adapter hold and R2 MRAD naming/window correction. New runtime qualification pending; prior262-check dbb16af3 evidence remains unchanged.
Jobs2 leaf build atd4185238 completed36.581s exit1: six production objects passed, panel_store failed deprecated _wfopen under /WX. Rootef2d900f uses _wfopen_s preserving exclusive wbx. Warm objects/PCH/deps retained. Session71170 finished; no build currently active. Latest physical memory ~950MiB, commit headroom~4GiB; allow host recovery before broader Jobs2 launch. No agent builds or long simulations.
Pool3g0 owns L1 streamed dataset and bounded FeatureMatrix adapter, uses rootd4185238 base. Pool5audit owns I1runtime prereg bcfa5f65 review + postimplementation fixtures; root sharedflags/guard already present. Pool4replay owns pulled-forward R3 costed factor-space solver on integrated977a93ee R1/B1 base, explicit route/costs/pins/unsupported geometry; minimal X1optimizer adapter afterward. Only root global CMake and LEDGER. Actual2020+/mixedpayloads remain sealed; no warehouse writes/push/main FF/alpha promotion.

## Compiler/private implementation priority, current handoff
Root source through492ed134; source reviews6defe305/58bedfe7 and PCH audit40f82415. QP008fcdb9 and factor492ed134 preserve exact numerical bodies in CPP; exposuresd340caa0 similarly; lightweight recipes7ee0b04b remove algorithm header fan-out; stable application/test PCH6ea53ffd. One combined configure/build next, root only, Jobs3 or2 according to measured RAM; no flag/PCH toggling. No-op and actual localized CPP cache-miss measurements required. No active build/runtime at this checkpoint.
Clean2e398101 prior six XMLs: data86/eval98/risk34/IC65/applicationcontracts97/search85passes+one opt-in skip. Runtime prereg fixture aborted on moved CSV vector; test-only70bfef58 fixes it, three-case rerun pending. Final resume55.927s Jobs3; unchanged no-op3.562s zero cachecalls. New refactor runtime not yet qualified.
L1 frozen603e7e9c/e672bcd8/6cf937f8/342563f6/3d199892 independently approved2be2645a, not imported. R3 frozen65bdf3da/ec6719b2/db36df91/fc80a5ee/6be7f67f report09c05301 revieweff75785, not imported; root legacy-only factor extraction means integration needs careful resolution. Compile iteration qualification comes first. All scale/empirical/wave/alpha gates remain open; sealed data and long-run deferral unchanged.

## Private boundary qualification complete; next source batch integrated
Root throughc1fa14aa. Compiler reporta850d48f independently audited63e1ee26 (sourcede3e7264). Actualbuildfa5 failed515.544s on source-local SHA/PCH macro mismatch;56df1587 isolates SHA in tiny generated CPP(.489s action). Resume73.009s found local CRT policy afterforcedPCH;d59f4abd skipsPCH onlytrial_ledger and5existingfixtures. Finalwarmresume177.817s Jobs2 passed; unchangedrepeat3.8043152s zero compiler/cache calls. PrivateQP namedlocaledit817bd584 measured21.7589912s Jobs3, exactly1CPPcachemiss+4links, no caller/PCH/deps compile. Totaladoption766.370s, notcoldspeedup.
Refactor runtimeclean d59: risk67/67(native21.967s), actualI1runtime3/3(22.604s), provenance/config12/12(10.764s),82distinct. I1moved-rowfixture abortclosed. Leaf817QPanalytic/determinism2/2(.017s)repeatnames. Artifacts w1-private-*; prior2e465+1 remainsseparate. No activecompiler/runtime sessions. Lastbinaries built817 withconfiguredSHA d59; they doNOTqualify newerfeaturebatch.
L1 source603/e672/6cf/342/3d199 nowimportedf0309cc1/d4715797/ffc010f4/f4ca20bf/0a734575; review2be2645a. R3finalreviewedeightfilesat6be7f67f integrated77a81833 preservingprivatefactorCPP; sources65bdf3da/ec6719b2/db36df91/fc80a5ee/6be7f67f, report567a1275 review2e88f2b7. Rootc1fa14aa registersdatasetCPP/evalfixture andriskcostlegacy/newV2fixtures. No configure/build ofthisbatch yet; combinewithforthcomingA4sourcefreeze toavoidrepeatedbroadheaderbuilds.
Agents source-only: pool4replay coreA4executionobjective343e4a2b/3251e057/1448c7e0 +fitness/searchconsumerinprogress; pool5audit mine184ee024 awaitingfinalcoredeclarations, thenpostimplementationfixtures; pool3g0 independentcoreaudit/handledgerfixtures. Bothreviewers found absent-but-finite entry/heldmark defect1448c7e0; ownerrepairpending. V2programmaticpreparedcontextonly,noCLIloader; missing/unboundcost orpoolrecipefailsclosed. FullA4residual/HAC/half-life andalllargedata/wave/alphagatesopen. RootownsCMake/ledger/compiler Jobs>=2 bymeasuredmemory.

## L1/R3/A4 runtime qualified; E2/L2 source work continues

Root through4f27a8f9. Reportffa53664 qualifies clean compiled/configured2a3d5c2b: six disjoint XMLs249passes, no failures/skips; all28newchecks pass. Core14(.044s), dataset5(.198s), costed37(19.400s), mine30(32.751s), search65(19.378s), eval98(15.929s). Last excludes only the existing CPCV timed benchmark and separately run dataset cases. Source2a3 binaries and all XML/log hashes in committed companionreceipt; complete local w2-execution-qualification-index.json SHA fbd13874219e0653c285a7b6cef61e7646c0263270d495d095ee59c69d448905. Prior reports' counts overlap and are not additive.

Both current builds passed firstattempt: w2-execution-leaf-build82.065s Jobs2 includes22.1sconfigure/eightCPPactions; w2-execution-build359.857s Jobs3 has75CPPactions/sixlinks. PCH/dependencies stayed warm. Total441.922s broad API integration is not acceptable routine edit latency or speedup evidence; private-CPP edit21.759s/no-op3.804s remains independently qualified. Sessions85988/28357/83439/20295/60847/39504 FINISHED. No build/runtime active. Do not redo this batch without a new relevant source change.

A4 source imported throughc61a4e17; mine699197cf/1e7d8665; final independentfixturebcdfd208/review5acae159; registration2a3d5c2b. Presence, negative-cash funding and signed-DSR overlay findings closed. Programmatic opt-in shared delayed target/fill/cost/borrow/cash/maturity kernel is qualified, not CLIloader/defaultswitch/resume/residualHAC/half-life or tradeable alpha. L1/R3 source/import audit4238cf3b and runtimeffa53664 now close the bounded pending checks.

Next source not compiled: root E2 fields/parser/shared validate_ic_epoch_flags a8d41b36, two postimplementation parser/file/direct guard cases b5df42d0; contractreview17917fd6 importedfda7466b. Tiny exact-body learning helper extraction source de52e7c2 imported4f27a8f9 (43lines, no numerical change; no extra CMake). Pool5 E2 catalog/prereg/stage productionec918266 under pool3review; fixturesinprogress; root must register src/eval/trial_epoch.cpp and owningfixtures after review. Pool4 L2 gbt.hpp/.cpp production2ca57542 under pool3review, fixturesinprogress; augmentation-identity seam under clarification. No broad merge, no imports during compilation, rootonlycompiler withJobs>=2 adapted to RAM.

Pool3g0 audits current runtime bindings then E2 source, with L2 review ongoing. Pool5 owns E2; pool4 owns L2; root owns CMake/config/LEDGER/DAG and integration. W0longcomparison deferred NOTPASSED, practical V3 noisy-null pruning/general recall UNQUALIFIED, real2020+/mixedpayloads sealed, no warehouse write/push/mainFF/alpha promotion. FullW1-W5goal remains ACTIVE.
