# W0 disclosure, sanitizer-fixture and incremental-build closure

Outcome: PASS for the changed risk/implementation target closure. This does not close the
W0 wave: the newly identified raw-VWAP gap and quiet performance comparison remain open.

Source at build launch: `ffa6df4ef1a64d409e19dc588a168f680336f1f7`.
Configured provenance: `b1fc62b83fb5bac556d50ab668c67a4cf12f785e`; intervening changes
were documentation only. Later stable-PCH import `d3d04510` changes build ownership,
not the executable source/flags qualified here. Its separate six-output compilation,
consumer-environment comparison and no-op evidence are in the compiler report.

## Build and repeat

Exact target command, from root-owned pool2:

```powershell
& C:/atx-wt/pool-2/scripts/atx-build.ps1 build -Preset equity-dev -Jobs 1 atx-impl-tests atx-engine-risk-tests atx-shm-worker
```

Native exit 0, 158.150s including wrapper startup, launched with 3.126GiB available and
zero existing compiler workers. The transition removing target-wide Git provenance
revisited the implementation-core commands once. Per-build cache telemetry contains
36 calls: 32 preprocessed hits, four misses, no preprocessing/compile errors. Build
output contains 36 C++ actions and three links. PCH stayed enabled and dependency
build directories remained isolated under pool2/deps/equity-dev.

The identical command repeated with exit 0 in 6.722s:

```text
[0/2] Re-checking globbed directories...
ninja: no work to do.
```

Its cache-stats hash was unchanged before/after:
`d67d6bd0b19bfb4e767cc56f3b2da5e6ea216746fe81fa23eb9596601e8af8da`.
No compiler/cache calls occurred. This is a measured no-op, not a controlled speedup claim.

## Changed whole test targets

| Target | Native exit | Passed | Disclosed skips | Seconds |
|---|---:|---:|---:|---:|
| atx-engine-risk-tests | 0 | 470 | 1 | 133.852 |
| atx-impl-tests | 0 | 600 | 5 | 432.618 |

Verbatim terminal summaries:

```text
[==========] 471 tests from 62 test suites ran. (133629 ms total)
[  PASSED  ] 470 tests.
[  SKIPPED ] 1 test.
[==========] 605 tests from 123 test suites ran. (432231 ms total)
[  PASSED  ] 600 tests.
[  SKIPPED ] 5 tests.
```

Risk's skip is the explicitly opt-in nightly dense oracle. The five impl skips have the
same external-panel/fundamental-zoo or synthetic-panel prerequisite exclusions as the
previous integrated gate. External market-data and nightly environment opt-ins were
cleared. No post-2019 market inputs or warehouse were opened. Untouched groups retain
the prior nine-target integrated gate; they were not needlessly rebuilt or rerun.

| Executable | SHA-256 |
|---|---|
| Risk | `2b6694aae079e33eed5b594de1a5574216cc0f56cac82ffb1976e89c7c2ec811` |
| Impl | `1bca3e6264ff2531ae2a4b17d7cd0103983c97b8f0d580247c6ae13f4a80ead5` |

Receipts: `C:/atx-wt/pool-2/build-equity/w0-final-closure/` contains configure/build
logs, `build-start.json`, `build-result.json`, per-build ccache logs, `noop.log`,
`noop-result.json`, the exact `run-tests.ps1`, two whole-target logs and
`test-results.json` with source, times, native exits and executable/log hashes.
Unified build session8399 and test session75143 both completed with exit 0.

## Remaining integration

The original DAG execution index now carries each completed implementation/evidence SHA.
The D0 VWAP correction must preserve an explicit legacy rule and qualify its own affected
targets/recipes. Benchmark measurements use the fixed smaller synthetic width selected
before baseline, all 81 cases and three repetitions, unchanged optimizer sizes and the
20% threshold. Reduced cache-pressure measurements do not satisfy future scale gates.
