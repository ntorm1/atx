# Common-model stage timing and durable experiment progress

The native common-model benchmark now measures market preparation, liquidity
policy resolution, grid construction, fitting, and equivalent American IV
separately. Every completed snapshot, including identity and numerical refusals,
has all five durations and explicit attempted flags. Unvisited stages have zero
duration. Stage sums are reported for the cohort. Successful fits disclose their
objective evaluation counts and termination; a failed fitter Result currently
contains no evaluation ledger, so that count is null and the cohort records the
number of unavailable counts. No failed fit is represented as zero evaluations.

These are wall-clock diagnostics. Stage calls exclude most report construction;
the existing snapshot duration includes orchestration, and the cohort duration
now includes enabled progress writes. Neither figures nor new disk flush costs
are HFT latency measurements. No model configuration, objective, resource limit,
market input, numerical algorithm, or prediction value was changed.

## Progress protocol

`--model common-markov --progress <fresh-path.jsonl>` enables an optional native
journal. Quote-slice use, existing files, input/output/report/policy aliases, and
hardlink aliases are refused. The file is created exclusively; records are
bounded to 4 MiB including their newline. Each complete JSON line is flushed and
committed through `_commit` on Windows or `fsync` on POSIX before processing the
next snapshot. The Windows handle denies concurrent writers. This is an offline
experiment facility, outside production pricing APIs.

The first record has sequence zero, state `incomplete`, expected snapshot count,
total input rows, excluded input-status rows, expanded numerical configuration,
configuration SHA-256, and a run-binding digest. The binding includes raw input
CSV SHA-256, policy byte SHA-256 (null for auto), directly resolvable invoked
executable SHA-256, model and dividend convention. Native input is hashed before
and after CSV parsing; custom policy bytes are checked around configuration
loading. The wrapper always invokes and hashes an absolute executable path.

Each subsequent snapshot record has increasing sequence, the run-binding digest,
exact underlier/root/snapshot identity, profile, row/chain counts, status and
failure stage, row/IV status counts, stage durations, evaluation count and fit
termination. These records contain neither raw quotes nor evaluation labels.
The configuration and input are bound without duplicating all market diagnostics
or per-option prices into the progress file.

Only after every eligible snapshot is processed does a final `complete` record
declare `numerical_processing_complete`, with cumulative row/snapshot/status
counts and excluded input-status rows. This completion covers numerical
processing, including explicit refusals. It does not claim final prediction,
report, or wrapper run-receipt publication succeeded. The normal final report
and predictions still cover the full original cohort.

An interrupted journal is incomplete if the final completion record is absent.
Only full newline-terminated records constitute its usable prefix; an in-flight
trailing fragment must be ignored. The current snapshot is not checkpointed.
There is deliberately no resume support or silent file reuse. Flush/commit and
the real-process interruption test support process-interruption durability;
there is no filesystem/power-loss failure certificate.

## Wrapper provenance

`quote_benchmark.py run --progress <fresh-path>` binds that option and forbids
passthrough replacements. Its existing alias protection now also covers the
journal and immutable `<predictions>.start.json` receipt.

Before native launch the wrapper exclusively creates and fsyncs the start
receipt. It contains the exact command, timestamp, wrapper import-time hash,
executable/input/manifest hashes, policy bytes/hash, dividend/model selection,
and every pending output path. This evidence survives native or wrapper
interruption. A start receipt always says `started_incomplete`; it is never
rewritten to suggest completion. Existing start receipts also prevent accidental
reuse of an interrupted experiment output name.

On success the wrapper verifies its original start receipt and usual input,
policy, binary hashes, streams and validates the bounded native journal's
sequence/bindings/completion counts, and binds both journal and start-receipt
SHA-256 into the exclusive, fsynced final `.run.json`. A partial or corrupt
journal cannot become a final successful receipt. Execution still opens no
evaluation labels. Price and volatility scoring masks are unchanged.

## Validation and integration

Implementation preceded tests, per the user's instruction. Focused Release AVX2
CLI checks cover prediction-byte invariance with/without journaling, independent
price/IV controls, hashes, aliases, exclusive reuse refusal, all expected failure
stages, no eligible snapshots, a real child killed after completed snapshots,
and a real native-to-wrapper journal/receipt run. Python checks cover prelaunch
receipt survival, protected paths/options, journal corruption and incomplete
records, final hashes, and existing scorer behavior.

Commands use the leased pool-36 wrapper from frozen
`a8b0e4199186a148d2ea3051ebc1df617e49e1b3`:

```powershell
& C:/atx-wt/pool-36/scripts/atx-build.ps1 -Preset vol-rel-avx2 configure
& C:/atx-wt/pool-36/scripts/atx-build.ps1 -Preset vol-rel-avx2 check `
  atx-vol/tools/quote_bench_progress.cpp atx-vol/tools/quote_bench_common.cpp `
  atx-vol/tools/quote_bench_main.cpp
& C:/atx-wt/pool-36/scripts/atx-build.ps1 -Preset vol-rel-avx2 build atx-vol-quote-bench
$env:ATX_QUOTE_BENCH_BINARY='C:/atx-wt/pool-36/build-vol-rel-avx2/bin/atx-vol-quote-bench.exe'
$env:PATH='C:/atx-wt/pool-36/build-vol-rel-avx2/bin;C:/atx-cache/vcpkg_installed/x64-windows/bin;' + $env:PATH
python -m pytest --noconftest atx-vol/python/tests/test_common_markov_bench_cli.py `
  atx-vol/python/tests/test_quote_bench_cli.py -q `
  --junitxml=C:/atx-cache/quote-benchmark/common-progress-20261016-native-tests.xml
python -m pytest --noconftest atx-vol/python/tests/test_quote_benchmark.py -q `
  --junitxml=C:/atx-cache/quote-benchmark/common-progress-20261016-wrapper-tests.xml
& C:/atx-wt/pool-36/scripts/atx-build.ps1 -Preset vol-hygiene configure
& C:/atx-wt/pool-36/scripts/atx-build.ps1 -Preset vol-hygiene check `
  atx-vol/tools/quote_bench_progress.cpp atx-vol/tools/quote_bench_common.cpp `
  atx-vol/tools/quote_bench_main.cpp
```

Final checks: **75 native CLI tests passed in 5.31 seconds**, zero skips, and
**82 wrapper/scoring tests passed in 16.48 seconds**. Native XML SHA-256:
`26c89723e0b52002739669928e6ff3379e03656790c882769ded59f61a973275`.
Wrapper XML SHA-256:
`91e998ede74f008af50c4cdad6a9019de0f16d234fa9b8304944c3b7002d11f5`.
PCH-off checks passed for the owning TUs of both changed private headers; Ruff
and diff checks passed. No full market-data panel, dev200, untouched
validation, reference panel, broad test labels, or performance benchmark is run
by this lane. Temporary CMake registration is restored before committing;
parent must add `tools/quote_bench_progress.cpp` to `atx-vol-quote-bench`, in
addition to the previously integrated common runner and common policy TUs.
No shared ledger, CMake, README or library API edits belong to this commit.
