# W0 fair benchmark gate preparation

Both isolated Release builds and identical 81-case registries are complete;
measurements have not started. The current compiled production snapshot is
`b185d056440704e7ebcfe2b9395601d7e5264269` (docs-only child `766bac4a`);
the new D0 VWAP follow-up may require an incremental current build before timing.
Root owns integration and release of the quiet measurement window. The preflight
report records actual build/cache/no-op evidence and the allocation model.

## Authorized preparation receipt

D-12 functional acceptance and both independent reviews completed. Root then
authorized lease/configuration preparation while holding compilation and all
measurements. Baseline lease is `C:/atx-wt/pool-6`, branch
`feat/w0-bench-baseline-codex-20260925`, run
`aes-w0-bench-baseline-codex-20260925`, heartbeat suffix `-ct1`.
Frozen base/HEAD: `3ccf012c40ef42c49ed21aaec96476455e06e049`.
The lease's automatic dev configure failed on removed optional modules; the lease
was explicitly verified as held. The subsequent isolated equity-bench configure
exited 0 (52.9 seconds). Its completed cache confirms Release, equity only,
benchmarks enabled, all test groups, and
`FETCHCONTENT_BASE_DIR=C:/atx-wt/pool-6/deps/equity-bench`.

At initial preparation, no target compilation or measurement had started. The Ninja command
graph contains 224 compiler commands / 234 total commands for bench plus worker;
this is the full dependency closure, not a claim of remaining cache misses.
The dry-run stopped at CMake regeneration, so an exact remaining-action count
will be recorded when the build slot opens. Baseline source remains clean.

Windows `GetSystemCpuSetInformation`, decoded against the installed SDK's
`SYSTEM_CPU_SET_INFORMATION` layout, reports the i7-1260P's higher-performance
class 1 at logical CPUs 0-7 (four SMT core pairs) and class 0 at 8-15. Verified
P-core affinity mask: `0xFF`. The raw API response is archived in
`pool-6/build-equity-bench/w0-cpu-topology.json`; CPU parking flags are transient.
Use the same inherited process affinity for both measurement launches, verify
it on the running benchmark, and archive a fresh topology receipt then.

## Comparable builds

- Baseline: frozen O1 commit `3ccf012c` in an exclusively leased existing free pool.
- Current: root's final integrated W0 commit, frozen before configure, in an owned
  pool. Do not build in another owner's pool or in `C:\atx`.
- Both use `equity-bench`: Release, no global AVX2/IPO, identical compiler/options,
  isolated `${sourceDir}/deps/equity-bench`, same targets `atx-engine-bench` and
  `atx-shm-worker` (the benchmark's static initialization checks for the worker).
- Configure/build each serially with the wrapper and `CMAKE_BUILD_PARALLEL_LEVEL=1`.
  Start only with >4 GiB free RAM and fewer than three existing compiler workers.
  No test-target build is needed for this measurement task.

Exact commands once the lease supplies `P` and the source SHA is frozen:

```powershell
Set-Location $P
$env:CMAKE_BUILD_PARALLEL_LEVEL='1'
& "$P/scripts/atx-build.ps1" configure -Preset equity-bench -Jobs 1 "-DFETCHCONTENT_BASE_DIR=$P/deps/equity-bench"
& "$P/scripts/atx-build.ps1" build -Preset equity-bench -Jobs 1 atx-engine-bench atx-shm-worker
& "$P/build-equity-bench/bin/atx-engine-bench.exe" '--benchmark_list_tests=true' '--benchmark_filter=^BM_Kernel|^Wq101_|^BM_Search|^BM_OptimizerProduction/M:(1000|3000|5000)/mode:(4|6|7)/'
```

The final command must enumerate the same complete case set at both SHAs:
raw rolling/CS kernels, four scalar/multiobjective search cases, nine production
optimizer cases (1000/3000/5000 names by modes 4/6/7), plus all 20 WQ101 cases:
StrategyA,
StrategyB and StrategyBFused at 1/2/4/8/16 workers, WarmCache at 8, and OpFamily
0/1/2/3. The WQ101 portion is the full existing alpha-throughput set, but the
broader filter also covers touched kernel, search and lane-6 optimizer paths.
Inspect emitted names before measurements and adjust literal registration syntax
if necessary, recording the exact choice. No cherry-picked subset or
`-AllowMissing` will qualify the gate.

## Quiet measurements

Coordinate an exclusive measurement window with root and the G0 owner. No build,
test suite or real-panel process may overlap. Discover the machine's P-core mask
from Windows processor topology rather than guessing CPU indices, and apply the
same process affinity to both runs. Record mask/topology and competing-process
state. If a mask cannot be verified, disclose that and resolve before claiming a
P-core benchmark.

Explicitly set `ATX_WQ101_INSTRUMENTS=128` in BOTH snapshots; the fixture remains
synthetic with 2520 dates, all 70 alphas, and every 1/2/4/8/16-worker variant. This
revises the earlier agent-selected 500-name protocol before any measurement, as
authorized by the user's RAM-workaround instruction and root. The authoritative
W0 plan sets no 500-name floor. This is a bounded-workload regression gate; larger
production-scale gates remain required by later tasks. Clear inherited ATX
environment variables and pin search dimensions to 756 dates, 500 names and six
generations. Optimizer sizes remain 1000/3000/5000. If 3 GiB is unavailable at the
first quiet launch, root approved choosing N96 with a 2.25 GiB launch floor once
before any timings, then freezing it for both snapshots. N64 would make all
subindustry groups singletons and is excluded.

Use five disjoint native process batches per snapshot: 33 raw kernels, 20 WQ101,
15 SearchThroughput, nine optimizer, and four scalar/multiobjective search cases.
Run all five baseline families first, then all five final-current families,
serially. This sequence was fixed before timings to permit current-only source
repair work while the frozen baseline runs; no compilation/tests may overlap
measurements. Every native process has
three repetitions and verified P-core affinity 0xFF. Process exit releases its
fixture/cache/scratch allocations before the next family. Keep native JSONs and
contexts; concatenate unchanged rows only for the existing comparison script.
Require the combined set to equal all 81 registered cases with no duplicates,
errors, skips or nonpositive times, exactly three iteration rows and an independently
verified median per case. No filter or threshold is weakened.

The source-only allocation model estimates WQ128's dominant cold-cache warmup
payload at 2.202 GiB. Launch with at least 3 GiB available and sample process peak
working set/private memory and host available memory once per second. A run stopped
for memory pressure is failed evidence. The 2 GiB cache holds all 549 modeled
cacheable nodes at this size (~1.319 GiB), so cache-eviction pressure at 500 names
is explicitly unmeasured; warm root-hit behavior is still exercised.

For each native family, use the prepared runner with the frozen width N and
distinct output labels; the report gives both snapshot SHAs and the complete
command form. Example for the baseline WQ family:

```powershell
python C:/atx-wt/pool-5/build-equity-bench/w0-run-bench.py C:/atx-wt/pool-6 w0-baseline-wqN-wq --registry C:/atx-wt/pool-6/build-equity-bench/w0-baseline-registry.json --compiled-source-head 3ccf012c40ef42c49ed21aaec96476455e06e049 --wq-instruments N --family wq --execute-after-quiet-release
# Require native exit 0 and every expected case/repetition present.
```

Keep all repetitions and median aggregate rows. Use the existing comparison script:

```powershell
& "$CurrentPool/scripts/bench-gate.ps1" -Baseline $FreshBaselineJson -Current $CurrentJson -Threshold 0.20
```

The production-swarm wave gate is no regression greater than **20%**, not 5%.
Do not overwrite the checked-in busy-host `alpha_throughput.json`, use `-Update`,
relax the threshold, omit missing cases, or label a contended run as a pass.
Investigate any regression before deciding whether a repeat is justified.

## Cost and evidence budget

`atx-engine-bench` requires core/TSDB/engine libraries plus all benchmark TUs,
and the sibling worker, but neither impl nor test executables. An isolated cold Release build is a
substantial serial compile; plan approximately 10-25 minutes per tree depending
on cache warmth, then report actual build counts/time rather than this estimate.
After configure, a dry-run can refine remaining compile work without starting it.

The historical 20-case, three-repetition JSON contains 467.94 seconds of measured
iteration wall time. It was contended and is unsuitable as a performance baseline,
but suggests reserving at least 15-25 minutes for the WQ101 pair alone including
setup, warmup and calibration. The full kernel/search/optimizer gate adds work;
reserve 25-45 minutes for the quiet pair. The final registry confirms 81 cases;
registration metadata supplies only 216 seconds of nominal adaptive measured
time for the pair, plus 90 complete throughput searches, 162 optimizer iterations,
calibration and setup. No extra timing pilot is required or authorized. Runtime
estimates are not acceptance evidence.

Archive both JSONs, complete stdout/stderr, exact commands, git SHAs/status,
executable SHA256, preset/cache flags, synthetic dimensions, CPU affinity/topology,
start/end times and bench-gate exit/output. Explain any intentional algorithmic
cost by defect ID without silently changing the acceptance criterion. No real
market input is needed for this gate. The optimizer cases intentionally omit
trade-cost and turnover terms; their timing must not be represented as a costed
optimizer benchmark or evidence of tradable performance.

The final current build, registry, exact executable/runner hashes and replacement
commands are recorded in the final D0 Release section of
`lane-benchmark-preflight-report.md`. Its compiled source is
`b7afdec8c54a440c8ce8659bdaf48db1fef7bb96`; use
`w0-current-vwap-registry.json` and `w0-vwap-registry-pair.json`, preserving frozen
N128 and all 81 cases. Startup ProcessExecutor workers are accounted through a
job assigned before the benchmark resumes; short-lived descendants cannot be
misclassified as external CPU. Both snapshots must use the identical final
runner hash and original dimension-protocol hash. Await L9 completion and all
writers becoming quiescent before any new timing attempt.
