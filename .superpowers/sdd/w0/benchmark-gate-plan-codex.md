# W0 fair benchmark gate preparation

Preparation only; no new lease, configure, build or measurement has started.
Begin only after the D-12 gate and inference review are complete and root grants
the next bounded task. Root owns integrated correctness and merges.

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

Explicitly set `ATX_WQ101_INSTRUMENTS=500`; the fixture is synthetic, 2520 dates,
70 alphas. Clear optional real-data environment variables. The 3000-name setting
uses roughly six times the panel/slot storage and is not this 16 GiB host gate.
The warm-cache case additionally permits a 2 GiB cache; reserve memory accordingly.

For baseline and current, use identical arguments and distinct output files:

```powershell
$env:ATX_WQ101_INSTRUMENTS='500'
& "$P/build-equity-bench/bin/atx-engine-bench.exe" '--benchmark_filter=^BM_Kernel|^Wq101_|^BM_Search|^BM_OptimizerProduction/M:(1000|3000|5000)/mode:(4|6|7)/' '--benchmark_repetitions=3' '--benchmark_out_format=json' "--benchmark_out=$OutputJson"
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
reserve 25-45 minutes for the quiet pair until list-tests and pilot runtime make
the estimate firmer. Runtime estimates are not acceptance evidence.

Archive both JSONs, complete stdout/stderr, exact commands, git SHAs/status,
executable SHA256, preset/cache flags, synthetic dimensions, CPU affinity/topology,
start/end times and bench-gate exit/output. Explain any intentional algorithmic
cost by defect ID without silently changing the acceptance criterion. No real
market input is needed for this gate. The optimizer cases intentionally omit
trade-cost and turnover terms; their timing must not be represented as a costed
optimizer benchmark or evidence of tradable performance.
