# atx-core

A production-grade C++20 standard library for a quantitative backtesting engine.
Built to a safety-critical profile (no UB, exhaustive `// SAFETY:` rationale on
every deviation, `const`/`constexpr`/`noexcept`/`[[nodiscard]]`, Rule of Zero/Five,
warnings-as-errors `/W4 /permissive- /WX`, TDD with GoogleTest).

Umbrella header: `#include <atx/core/core.hpp>` pulls in every module. In hot
translation units prefer the specific module header to keep compile times down.

## Layout

Modules are organized in dependency layers L0 (foundation) → L9 (series).
Higher layers depend only on lower ones.

### L0 — foundation
| Module | Header | Purpose |
|--------|--------|---------|
| platform | `atx/core/platform.hpp` | Compiler/arch detection, `kCacheLineSize`, prefetch hints |
| types | `atx/core/types.hpp` | Fixed-width vocabulary types (`i8..i64`, `u8..u64`, `f32/f64`, `usize`) |
| macro | `atx/core/macro.hpp` | `ATX_ASSERT/CHECK`, `ATX_CACHE_ALIGNED`, `ATX_DISABLE_COPY_MOVE`, `ATX_LIKELY` |
| error | `atx/core/error.hpp` | `Result<T>`/`Status`, `Ok`/`Err`, `ErrorCode`, `ATX_TRY` |
| log | `atx/core/log.hpp` | spdlog-backed logging façade |
| bit | `atx/core/bit.hpp` | `popcount`/`clz`/`ctz`/`bit_width`/`byteswap`/`next_pow2`/`is_pow2` |
| util | `atx/core/util.hpp` | `ScopeGuard`, `NonNull`, `EnumFlags` |

### L1 — numeric primitives
| Module | Header | Purpose |
|--------|--------|---------|
| safe_math | `atx/core/safe_math.hpp` | Overflow-checked / saturating integer arithmetic |
| math | `atx/core/math.hpp` | `isclose`/`clamp`/`lerp`/`sign` (constexpr, float-safe) |
| hash | `atx/core/hash.hpp` | `hash_bytes`/`hash_combine` (wyhash-based) |
| decimal | `atx/core/decimal.hpp` | Exact fixed-point `Decimal` (`kScale = 1e9`), portable 128-bit math |
| random | `atx/core/random.hpp` | `Xoshiro256pp` PRNG (deterministic, KAT-verified) |

### L2 — memory
| Module | Header | Purpose |
|--------|--------|---------|
| aligned | `atx/core/aligned.hpp` | `aligned_alloc_bytes`/`aligned_free`, `AlignedBuffer<T,N>` |
| arena | `atx/core/arena.hpp` | Bump-pointer arena allocator |
| object_pool | `atx/core/object_pool.hpp` | Fixed-capacity O(1) acquire/release pool |

### L3 — containers (`atx::core::container`)
| Module | Header | Purpose |
|--------|--------|---------|
| ring_buffer | `container/ring_buffer.hpp` | Power-of-two FIFO ring, overwrite mode |
| fixed_vector | `container/fixed_vector.hpp` | Static-capacity vector (exception-safe) |
| small_vector | `container/small_vector.hpp` | Inline storage + heap spill (exception-safe) |
| hash_map | `container/hash_map.hpp` | `ankerl::unordered_dense` wrapper |
| intrusive_list | `container/intrusive_list.hpp` | Zero-allocation intrusive doubly-linked list |

### L4 — concurrency (`atx::core::concurrent`)
| Module | Header | Purpose |
|--------|--------|---------|
| spinlock | `concurrent/spinlock.hpp` | TTAS spinlock (Lockable) |
| seqlock | `concurrent/seqlock.hpp` | Single-writer / many-reader lock-free snapshot |
| spsc_queue | `concurrent/spsc_queue.hpp` | Wait-free single-producer/single-consumer ring |
| mpmc_queue | `concurrent/mpmc_queue.hpp` | Vyukov bounded lock-free MPMC queue |
| disruptor | `concurrent/disruptor.hpp` | LMAX sequenced ring (SP/MP, multi-consumer) |

### L5 — SIMD (`atx::core::simd`)
| Module | Header | Purpose |
|--------|--------|---------|
| simd | `atx/core/simd.hpp` | xsimd span reductions: `sum`/`dot`/`mean`/`min`/`max`/`scale`/`add`/`axpy` |

### L6 — statistics & algorithms (`atx::core::stats`)
| Module | Header | Purpose |
|--------|--------|---------|
| online_stats | `stats/online_stats.hpp` | `RunningMean`/`RunningVariance` (Welford ± remove), `Ewma`, `RunningMinMax<W>` |
| quantile | `stats/quantile.hpp` | `P2Quantile` streaming estimator (constant memory) |
| algo | `stats/algo.hpp` | `argsort`/`argsort_desc`/`topk`/`partial_rank` (span-based) |
| rolling | `stats/rolling.hpp` | `RollingMean/Std/ZScore/Covariance/Correlation<W>` (O(1) update) |
| cross_section | `stats/cross_section.hpp` | `rank`/`zscore`/`demean`/`winsorize`/`scale_to_unit` (alpha primitives) |

### L7 — linear algebra (`atx::core::linalg`)
| Module | Header | Purpose |
|--------|--------|---------|
| linalg | `linalg/linalg.hpp` | Eigen aliases (`Vec2..VecX`, `Mat2..MatX`) + zero-copy span bridges |
| decompose | `linalg/decompose.hpp` | `cholesky`/`qr`/`svd`/`symmetric_eig` → owned `Result<>` factors |
| solve | `linalg/solve.hpp` | `solve`/`solve_spd`/`inverse`/`pseudo_inverse`/`determinant`/`rank`/`condition_number` |
| spd | `linalg/spd.hpp` | `is_symmetric`/`is_positive_definite`/`nearest_pd` (Higham)/`regularize` |
| pca | `linalg/pca.hpp` | `pca` → `Result<PcaResult{mean, components, explained_variance, explained_ratio}>`, `transform` |
| regression | `linalg/regression.hpp` | `ols`/`ridge`/`wls` → `Result<OlsResult{beta, r2, residuals}>` |

### L8 — time & domain
| Module | Header | Purpose |
|--------|--------|---------|
| datetime | `atx/core/datetime.hpp` | `time::Timestamp`/`Duration`/`Date`, ISO-8601, NYSE `Calendar` (DST-aware sessions) |
| domain | `domain/domain.hpp` | `Price`/`Quantity`/`Notional` (over `Decimal`), `Side`, `Bar`/`Tick` |
| symbol | `domain/symbol.hpp` | `Symbol` + interning `SymbolTable` |

### L9 — series (columnar) (`atx::core::series`)
| Module | Header | Purpose |
|--------|--------|---------|
| column | `series/column.hpp` | `Column<T>`: cache-aligned growable buffer + validity bitmap |
| frame | `series/frame.hpp` | `Frame`: type-erased named columns + shared `Timestamp` index |

## Build, test, bench

The repository configures with CMake + Ninja + clang-cl (preset `ninja`). On
Windows, run from a Visual Studio Developer shell (or import the dev environment
into the current shell) so `clang-cl` and the MSVC toolchain are on PATH.

```bash
# configure (Debug, clang-cl, emits compile_commands.json)
cmake --preset ninja

# build the library + all tests
cmake --build --preset ninja

# run the full test suite
ctest --preset ninja --output-on-failure

# build + run the benchmark suite (opt-in)
cmake -DATX_BUILD_BENCH=ON --preset ninja
cmake --build --preset ninja --target atx-core-bench
./build/bin/atx-core-bench.exe
```

### Performance note

The `ninja` preset is a **Debug** build (`-O0`). Debug numbers are useful only
for relative sanity checks and regressions — they are **not** representative of
production latency. In particular the SIMD reductions show *no* speedup over
scalar at `-O0` (the xsimd abstraction is not inlined and scalar loops are not
auto-vectorized); a `-O2`/`-O3` Release build is required to measure the SIMD
and lock-free hot paths. Capture real baselines from an optimized build before
drawing conclusions.

Representative Debug-build throughput (single run, illustrative only):

| Benchmark | Debug throughput |
|-----------|------------------|
| RingBuffer push/pop | ~73 M items/s |
| SPSC queue round-trip | ~16 M ops/s |
| MPMC queue round-trip | ~8 M ops/s |
| Disruptor SPSC (2^20 msgs) | ~8.6 M items/s |
| Mutex-queue SPSC (baseline) | ~2.3 M items/s |
| RollingMean update | ~16 M updates/s |
