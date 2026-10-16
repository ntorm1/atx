# Immutable common-model storage

Lane: `feat/vol-markov-storage-20261016`, frozen base
`182ee25c561abaa1cbe16bc4836ed83587293b37`. Implemented before new checks, as
requested by the user. No SR prices or vols, parquet-specific behavior, changes
to default numerical grids, or changes to the finite-model assumptions.

## Representation and arithmetic

`AmericanMarkovLayer::states()` and `AmericanMarkovTransition::rows()` replace
the public vectors with const spans backed by private shared ownership of const
vectors. Independent layer/transition copies retain their buffers after the
original model is destroyed. Default moves leave those views empty. Model copies
share only immutable grids/rows; existing interval metadata still owns its data.

Diffusion layers share the same state grid until a cash event produces a new
shifted grid. A compilation-local ordered map caches diffusion row blocks by the
exact binary64 bits of `variance_step`, within one interval only. It never
quantizes time or shares across different local-volatility intervals. A new
represented step uses the original row-construction arithmetic. Calendar time,
variance time, growth, discount, forward mass propagation, and rollback retain
their original expression and accumulation order. Views are hoisted outside
scalar, batch, and PWL state/option loops.

`diagnostics.stored_rows` remains the expanded logical count used by compilation
and fit rollback budgets. Transition, state, volatility, and row hard caps are
unchanged. Additional diagnostics report unique represented element counts:
`physical_state_values`, `state_grid_blocks`, `physical_transition_rows`, and
`transition_row_blocks`. These are neither total allocated bytes nor reduced
pricing work. Cash transitions remain separately owned. The cache is released
after its interval; the immutable model retains the row buffers.

## Postimplementation validation

Release owning targets: 49/49 pricing/grid/batch/PWL tests and 14/14 fitting tests.
The two new tests cover exact-step sharing, different-step/interval isolation,
cash grid shifts and complete wipe, independent physical count reconciliation,
unchanged logical budget refusal, input/model/view lifetime, copy/move behavior,
and price bit equality after model copies/moves. Existing moment, positivity,
strike/calendar no-arbitrage, reference, and fit tests remain green.

Owning PCH-off object checks passed for the four changed pricing/fitting sources,
the three changed test sources, and the independent storage probe. The wrapper
also compiled the duplicate focused/monolithic objects for those test sources.
The first check attempt found a stale hygiene configure missing the grid test;
reconfiguring `vol-hygiene` then running the same focused check passed. No full
library hygiene or all-target build was run.

The new standalone `tools/american_markov_storage_bench_main.cpp` runs seven
synthetic input models and records explicit-byte operator fingerprints, every
scalar price's binary64 bits, independent physical block counts, and diagnostic
compile times. The baseline executable was built before the implementation.
All seven expanded operator fingerprints and all 616 scalar price bits match.
The existing `--full --pricing-only` audit matches on all 143 cases, including
69,426 binary64 values after excluding 397 timing fields. Both versions compile
127 cases and explicitly refuse 16 cases; every refusal and logical diagnostic
is unchanged. This verifies storage parity, not continuous-American accuracy.

Prepared successful scalar and batch queries recorded zero ordinary and aligned
C++ allocations across the seven fixtures. The standalone probe's thread-local
counter self-calibrates via explicit replacement allocator calls. It excludes
direct C allocation, OS allocation, and other threads. JSON and receipt work run
outside the armed windows. Source review confirms that the changed serving paths
only borrow spans and reuse the existing workspace.

Scalar and batch kernels already differed at the bit level in the baseline:
321 of 616 values differ, with the same per-fixture counts in the candidate.
Candidate maximum absolute difference is 8.5265128291212022e-14. Cross-kernel
bitwise equality is not claimed; baseline/candidate scalar bitwise equality and
full-audit scalar/batch metrics are preserved.

| Fixture | Physical rows before | Physical rows after | State values before | State values after |
| --- | ---: | ---: | ---: | ---: |
| martingale | 47,343 | 1,290 | 47,472 | 129 |
| short_low_vol | 9,417 | 129 | 9,546 | 129 |
| high_vol | 401,577 | 1,548 | 401,706 | 129 |
| dated_cash | 63,210 | 2,451 | 63,339 | 387 |
| local_vol | 21,543 | 1,677 | 21,672 | 258 |
| clock_pause | 29,928 | 1,677 | 30,057 | 129 |
| deterministic_cash | 4,386 | 1,161 | 4,434 | 306 |

## Receipts and reproduction

Artifacts are under `C:/atx-cache/quote-benchmark/`, all with prefix
`markov-storage-20261016-`:

- `baseline.json`: SHA256 `3B6059A07B26CE4F6E0D75EE449498665E41DF0BC521C905572174CB5D744E05`
- `candidate.json`: SHA256 `36B40B87852500D15BA679ABC36EC84FF1E8091996B4A258EDD9B5C2A6263F78`
- `comparison.json`: SHA256 `E1FEACEF2FA9D792B8230851A72AE3261771EE0A987EC9150F6C15300D281076`
- `baseline-audit.json`: SHA256 `DF6AB3FB09221D693795EB70E318A3A7E6C67737EA846AE069E35F4C0F7AADE7`
- `candidate-audit.json`: SHA256 `EC2052F501A022B0E347100815E86B1288C5D18AA754A745545662873DE64B03`
- `audit-comparison.json`: SHA256 `7D6A0289544AAE25EDAB20C8B79A2DF9E6B797DB93C4D22003F95AF45FE3ABBB`
- `tests.xml` and `fit-tests.xml`: focused GoogleTest receipts.
- `bin/` (directory name `markov-storage-20261016-bin`): preserved baseline and
  candidate standalone executables, each accepting `--repetitions 1..100`.
  Baseline binary SHA256 `6CAA3433B9FB5CDD0EE96EF3AAE0DC982E5A43FB91E75B14832BA2716ED972BD`;
  candidate binary SHA256 `11C4E046805C4ED6DD3B4720A6C903418AAE8B0C1CDE1BC620CEF58043956EA2`.

Toolchain: clang-cl 18.1.8, Windows 11 Home 10.0.26200 x64, Release
`/O2 /Ob2 /DNDEBUG`, AVX2,
ThinLTO, precise floating-point defaults; host Intel i7-1260P, 12 cores/16 logical
processors. The three-repetition timing samples ran on an unpinned host with
concurrent agent work. They are diagnostic data only, not a performance claim.
Parent should obtain a quiet pinned lease before comparing latency distributions.

The probe target was temporarily registered for checks, then CMake was restored
before committing. Parent owns permanent build registration. Required wiring:

```cmake
add_executable(atx-vol-american-markov-storage-bench tools/american_markov_storage_bench_main.cpp)
target_link_libraries(atx-vol-american-markov-storage-bench PRIVATE
    atx::vol atx::core atx_warnings nlohmann_json::nlohmann_json)
```

Use the leased tree's absolute `scripts/atx-build.ps1` with `-Preset
vol-rel-avx2` to build that target and the focused tests. Audit parity uses the
existing `atx-vol-american-markov-audit --full --pricing-only` executable. The
parent audit can expose the four new physical diagnostics without changing its
numerical model or resource policy. Exact-SHA independent review is required
before integration.
