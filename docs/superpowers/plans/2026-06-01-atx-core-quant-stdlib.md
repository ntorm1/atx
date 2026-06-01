# atx-core Quant Standard Library — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the full `atx-core` C++20 standard library (numeric, containers, concurrency, SIMD, statistics, linear algebra, time/domain, columnar series) that a quant backtesting engine is built on.

**Architecture:** Layered, header-first modules (L0→L9) built bottom-up. Thin wrappers over vetted libs (unordered_dense, Eigen, xsimd, Google Benchmark) where they fit; everything else built in-house. Fixed-point `Decimal` for money. Low-latency: lock-free, cache-aligned, SIMD, benchmark-gated. Each module = header + test (+ bench), one subagent per module.

**Tech Stack:** C++20, CMake (Ninja + clang-cl / MSBuild), GoogleTest, Google Benchmark, spdlog, tl::expected, ankerl::unordered_dense, Eigen, xsimd. Governed by `.agents/cpp/agent.md` (safety-critical-grade).

**Spec:** `docs/superpowers/specs/2026-06-01-atx-core-quant-stdlib-design.md`

---

## Conventions (read once, apply to every task)

### agent.md is law
Every module obeys `.agents/cpp/agent.md`: no UB, `const`/`constexpr`/`noexcept`/`[[nodiscard]]` where they hold, Rule of Zero, weakest-sufficient-type interfaces, `Result<T>`/`Status` for expected failures (no throw for control flow), exhaustive `switch`, bounded loops, functions ≤~60 lines, `// SAFETY:` on every deviation.

### TDD loop (every module follows this micro-sequence)
1. Write the failing test file (seed cases shown in each task).
2. Run it; confirm it fails to **compile/link** (symbol not defined) — the right kind of red.
3. Implement the header (minimal to satisfy contract).
4. Run; confirm green.
5. (perf modules) add the benchmark.
6. Integrate into CMake; full build at `/W4 /WX`; run full test suite.
7. Commit.

**The seed tests in each task are the minimum.** The implementing agent MUST expand to full boundary + error-path + invariant coverage per agent.md §7 (0/1/max/empty/overflow edges, `Result` error branches, `EXPECT_DEATH` for documented preconditions) before marking the task done.

### File layout per module
- Foundation/numeric (L0–L1): header `atx-core/include/atx/core/<name>.hpp`, namespace `atx::core`.
- Submodule layers: header `atx-core/include/atx/core/<sub>/<name>.hpp`, namespace `atx::core::<sub>` where `<sub>` ∈ {container, concurrent, stats, linalg, time, domain, series}.
- Test: `atx-core/tests/<name>_test.cpp`.
- Bench: `atx-core/bench/<name>_bench.cpp`.

### Build / test / bench commands (exact)
```powershell
cmake --preset ninja                          # configure (run once / after CMake edits)
cmake --build --preset ninja                  # build all (errors-as-errors gate)
ctest --preset ninja                          # run all tests
ctest --preset ninja -R <name>_test           # run one module's tests
cmake --build --preset ninja --target atx-core-bench   # build benchmarks
./build/bin/atx-core-bench --benchmark_filter=<name>   # run a bench
```
Run from a VS Developer shell (MSVC env present) or via VSCode CMake Tools.

### CMake integration (every module touches these)
- Add the test source to `atx-core/tests/CMakeLists.txt` (the `add_executable(atx-core-tests ...)` list).
- Add the bench source (perf modules) to `atx-core/bench/CMakeLists.txt`.
- Pure-header modules need no `atx-core` source change; `.cpp`-bearing modules (`decimal` opt, `random`, `symbol`, `calendar`, `regression`) add their `.cpp` to `atx-core/CMakeLists.txt` `add_library` list.

### Commit format
```bash
git add <files>
git commit -m "feat(core): <module> — <one-line summary>"
```

### Warnings gate
Build must be clean under MSVC `/W4 /permissive- /WX` and clang `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. Task 1 wires these flags; every later task must keep them clean.

---

## Task 0: Dependencies + bench/sanitizer scaffolding

**Files:**
- Modify: `CMakeLists.txt` (add FetchContent for new deps, options)
- Modify: `atx-core/CMakeLists.txt` (link new deps, add bench subdir)
- Create: `atx-core/bench/CMakeLists.txt`
- Create: `atx-core/bench/bench_main.cpp`

- [ ] **Step 1: Add dependencies to top-level `CMakeLists.txt`** (after the `tl-expected` block, before the `ATX_BUILD_TESTS` option):

```cmake
# ankerl::unordered_dense: fast flat hash map/set (header-only).
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG        v4.4.0
)
FetchContent_MakeAvailable(unordered_dense)

# Eigen: header-only linear algebra.
set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    Eigen3
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
)
FetchContent_MakeAvailable(Eigen3)

# xsimd: header-only SIMD abstraction.
FetchContent_Declare(
    xsimd
    GIT_REPOSITORY https://github.com/xtensor-stack/xsimd.git
    GIT_TAG        13.0.0
)
FetchContent_MakeAvailable(xsimd)

option(ATX_BUILD_BENCH "Build ATX benchmarks" OFF)
if(ATX_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.0
    )
    FetchContent_MakeAvailable(benchmark)
endif()
```

- [ ] **Step 2: Link deps + bench in `atx-core/CMakeLists.txt`.** Extend `target_link_libraries(atx-core PUBLIC ...)` with `unordered_dense::unordered_dense Eigen3::Eigen xsimd`. After the tests block add:

```cmake
if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()
```

Also add the warnings gate (once):
```cmake
if(MSVC)
    target_compile_options(atx-core INTERFACE /W4 /permissive-)
else()
    target_compile_options(atx-core INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
```
(Apply `/WX` / `-Werror` to the targets via a cache option `ATX_WERROR` default ON; keep it toggleable so a vetted-lib header warning can't block unrelated work — but default ON.)

- [ ] **Step 3: Create `atx-core/bench/bench_main.cpp`:**
```cpp
#include <benchmark/benchmark.h>
BENCHMARK_MAIN();
```

- [ ] **Step 4: Create `atx-core/bench/CMakeLists.txt`:**
```cmake
add_executable(atx-core-bench
    bench_main.cpp
)
target_link_libraries(atx-core-bench PRIVATE atx::core benchmark::benchmark)
```

- [ ] **Step 5: Configure + build to verify deps resolve.**
Run: `cmake --preset ninja` then `cmake --build --preset ninja`.
Expected: configure downloads unordered_dense/Eigen/xsimd; build succeeds (existing tests still pass).

- [ ] **Step 6: (optional) init git for frequent commits.** `git init` if not already a repo; add a `.gitignore` line for `build/` `build-vs/` if missing.

- [ ] **Step 7: Commit.** `git commit -m "build(core): vet-in unordered_dense/Eigen/xsimd + bench harness"`

---

# WAVE L0 — Foundation

## Task 1: `platform`

**Files:** Create `atx-core/include/atx/core/platform.hpp`, `atx-core/tests/platform_test.cpp`. Modify `atx-core/tests/CMakeLists.txt`.

- [ ] **Step 1: Failing test** (`platform_test.cpp`):
```cpp
#include <atx/core/platform.hpp>
#include <cstdint>
#include <gtest/gtest.h>

TEST(Platform, CacheLineSizeIs64) {
  static_assert(atx::core::kCacheLineSize == 64);
  EXPECT_EQ(atx::core::kCacheLineSize, 64u);
}

TEST(Platform, CacheAlignedTypeIsAligned) {
  struct alignas(atx::core::kCacheLineSize) Padded { int x; };
  EXPECT_EQ(alignof(Padded), atx::core::kCacheLineSize);
}
```

- [ ] **Step 2: Run, expect compile-fail** (`kCacheLineSize` undefined). `ctest --preset ninja -R platform_test`.

- [ ] **Step 3: Implement** `platform.hpp`: define `inline constexpr atx::core::usize kCacheLineSize = 64;`; macros `ATX_CACHE_ALIGNED` (`alignas(kCacheLineSize)`), `ATX_RESTRICT` (`__restrict`), `atx_prefetch(ptr)` (`__builtin_prefetch` / `_mm_prefetch` per compiler, `(void)ptr` fallback). Include `types.hpp`. `// SAFETY:` on each intrinsic seam.

- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Integrate CMake + full build `/WX`.** Add `platform_test.cpp` to tests list; `cmake --build --preset ninja`.
- [ ] **Step 6: Commit.** `feat(core): platform — cache line, alignment, prefetch`

## Task 2: `bit`

**Files:** Create `include/atx/core/bit.hpp`, `tests/bit_test.cpp`. Modify tests CMake.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/bit.hpp>
#include <gtest/gtest.h>
using namespace atx::core;
TEST(Bit, NextPow2) {
  EXPECT_EQ(next_pow2(1u), 1u);
  EXPECT_EQ(next_pow2(5u), 8u);
  EXPECT_EQ(next_pow2(8u), 8u);
  EXPECT_EQ(next_pow2(1023u), 1024u);
}
TEST(Bit, IsPow2) {
  EXPECT_TRUE(is_pow2(1u)); EXPECT_TRUE(is_pow2(64u));
  EXPECT_FALSE(is_pow2(0u)); EXPECT_FALSE(is_pow2(3u));
}
TEST(Bit, Popcount) { EXPECT_EQ(popcount(0b1011u), 3u); }
```

- [ ] **Step 2: Run, expect fail.**
- [ ] **Step 3: Implement** thin wrappers over `<bit>` (`std::bit_ceil`, `std::has_single_bit`, `std::popcount`, `std::countl_zero`, `std::countr_zero`, `std::byteswap`) re-exported as `next_pow2`, `is_pow2`, `popcount`, `clz`, `ctz`, `byteswap`, all `constexpr noexcept`, `[[nodiscard]]`, constrained `std::unsigned_integral`. `next_pow2(0) == 1` (document).
- [ ] **Step 4–6:** PASS → CMake/build → commit `feat(core): bit — pow2/popcount/clz helpers`.

## Task 3: `util`

**Files:** Create `include/atx/core/util.hpp`, `tests/util_test.cpp`. Modify tests CMake.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/util.hpp>
#include <gtest/gtest.h>
using namespace atx::core;
TEST(ScopeGuard, RunsOnScopeExit) {
  int n = 0;
  { auto g = make_scope_guard([&] { ++n; }); EXPECT_EQ(n, 0); }
  EXPECT_EQ(n, 1);
}
TEST(ScopeGuard, DismissPrevents) {
  int n = 0;
  { auto g = make_scope_guard([&] { ++n; }); g.dismiss(); }
  EXPECT_EQ(n, 0);
}
enum class Perm : unsigned { Read = 1, Write = 2, Exec = 4 };
TEST(EnumFlags, OrAndTest) {
  auto f = EnumFlags<Perm>{Perm::Read} | Perm::Write;
  EXPECT_TRUE(f.test(Perm::Read)); EXPECT_FALSE(f.test(Perm::Exec));
}
```

- [ ] **Step 2: Run, expect fail.**
- [ ] **Step 3: Implement:** `ScopeGuard<F>` (move-only, RAII, `dismiss()`, runs `F` in dtor unless dismissed, `noexcept` dtor) + `make_scope_guard` + `ATX_DEFER` macro (uses `ATX_UNIQUE_NAME`). `NonNull<T*>` (constructed from non-null, `ATX_ASSERT` on null, implicit deref, no null state). `EnumFlags<E>` (type-safe bitset over `enum class`, `|`/`&`/`test`/`set`/`clear`, `to_underlying`). All Rule-of-Zero / move-only as appropriate.
- [ ] **Step 4–6:** PASS → CMake/build → commit `feat(core): util — ScopeGuard, NonNull, EnumFlags`.

---

# WAVE L1 — Numeric

## Task 4: `safe_math`

**Files:** Create `include/atx/core/safe_math.hpp`, `tests/safe_math_test.cpp`. Modify tests CMake. Depends: `error`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/safe_math.hpp>
#include <gtest/gtest.h>
#include <cstdint>
using namespace atx::core;
TEST(SafeMath, CheckedAddOk) {
  auto r = checked_add<std::int32_t>(2, 3);
  ASSERT_TRUE(r.has_value()); EXPECT_EQ(*r, 5);
}
TEST(SafeMath, CheckedAddOverflowErrs) {
  auto r = checked_add<std::int32_t>(INT32_MAX, 1);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}
TEST(SafeMath, SaturatingAdd) {
  EXPECT_EQ(sat_add<std::int32_t>(INT32_MAX, 10), INT32_MAX);
  EXPECT_EQ(sat_add<std::int32_t>(INT32_MIN, -10), INT32_MIN);
}
TEST(SafeMath, CheckedMulOverflow) {
  EXPECT_FALSE(checked_mul<std::int64_t>(INT64_MAX, 2).has_value());
}
```

- [ ] **Step 2: Run, expect fail.**
- [ ] **Step 3: Implement:** `checked_add/sub/mul<std::integral T>` returning `Result<T>` using compiler builtins where available (`__builtin_add_overflow` on clang/gcc; MSVC: manual pre-check via limits) — single `// SAFETY:` seam. `sat_add/sub/mul` clamping to `std::numeric_limits<T>`. All `constexpr noexcept [[nodiscard]]`. No signed-overflow UB anywhere.
- [ ] **Step 4–6:** PASS → CMake/build → commit `feat(core): safe_math — checked & saturating integer ops`.

## Task 5: `math`

**Files:** Create `include/atx/core/math.hpp`, `tests/math_test.cpp`. Modify tests CMake.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/math.hpp>
#include <gtest/gtest.h>
using namespace atx::core;
TEST(Math, IsClose) {
  EXPECT_TRUE(isclose(0.1 + 0.2, 0.3));
  EXPECT_FALSE(isclose(1.0, 1.1));
}
TEST(Math, Clamp) { EXPECT_EQ(clamp(5.0, 0.0, 3.0), 3.0); }
TEST(Math, Lerp) { EXPECT_DOUBLE_EQ(lerp(0.0, 10.0, 0.5), 5.0); }
TEST(Math, Sign) { EXPECT_EQ(sign(-2.0), -1); EXPECT_EQ(sign(0.0), 0); }
```

- [ ] **Step 2–4:** Implement `isclose(a,b,rel=1e-9,abs=1e-12)` (combined abs+rel test), `clamp`, `lerp`, `sign`, all `constexpr noexcept` over `f64`/`f32`. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): math — isclose/clamp/lerp/sign`.

## Task 6: `hash`

**Files:** Create `include/atx/core/hash.hpp`, `tests/hash_test.cpp`. Modify tests CMake. Depends: unordered_dense (bundled wyhash), `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/hash.hpp>
#include <gtest/gtest.h>
#include <string_view>
using namespace atx::core;
TEST(Hash, CombineDiffers) {
  std::size_t a = hash_combine(0, 1, 2, 3);
  std::size_t b = hash_combine(0, 3, 2, 1);
  EXPECT_NE(a, b);
}
TEST(Hash, BytesDeterministic) {
  std::string_view s = "AAPL";
  EXPECT_EQ(hash_bytes(s.data(), s.size()), hash_bytes(s.data(), s.size()));
}
```

- [ ] **Step 2–4:** Implement variadic `hash_combine(seed, xs...)` (boost-style mix), `hash_bytes(ptr,len)` delegating to `ankerl::unordered_dense::detail::wyhash::hash`. `// SAFETY:` on the wyhash internal-namespace use. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): hash — combine + wyhash bytes`.

## Task 7: `decimal`

**Files:** Create `include/atx/core/decimal.hpp`, `src/decimal.cpp` (for `from_string`/`to_string` non-template bodies), `tests/decimal_test.cpp`. Modify `atx-core/CMakeLists.txt` + tests CMake. Depends: `safe_math`, `types`, `error`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/decimal.hpp>
#include <gtest/gtest.h>
using namespace atx::core;
TEST(Decimal, FromIntAndBack) {
  auto d = Decimal::from_int(5);
  EXPECT_EQ(d.raw(), 5 * Decimal::kScale);
  EXPECT_DOUBLE_EQ(d.to_double(), 5.0);
}
TEST(Decimal, AddExact) {
  EXPECT_EQ(Decimal::from_int(2) + Decimal::from_int(3), Decimal::from_int(5));
}
TEST(Decimal, MulRescales) {
  auto a = *Decimal::from_string("1.5");
  auto b = *Decimal::from_string("2.0");
  EXPECT_EQ((a * b), *Decimal::from_string("3.0"));
}
TEST(Decimal, MulTruncatesTowardZero) {
  auto a = Decimal::from_raw(3);            // 3e-9
  auto b = Decimal::from_raw(2);            // 2e-9 -> product 6e-18 -> 0 at scale 1e-9
  EXPECT_EQ((a * b).raw(), 0);
}
TEST(Decimal, FromStringParsesAndRoundTrips) {
  auto d = Decimal::from_string("-123.456789");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->to_string(), "-123.456789");
}
TEST(Decimal, FromStringRejectsGarbage) {
  EXPECT_FALSE(Decimal::from_string("1.2.3").has_value());
}
TEST(Decimal, FromIntRangeChecked) {
  EXPECT_DEATH(Decimal::from_int(10'000'000'000), "");   // > ±9.22e9 range
}
```

- [ ] **Step 2: Run, expect fail.**
- [ ] **Step 3: Implement** `Decimal` per spec §6.2: `i64 mantissa_`, `kScale = 1'000'000'000`. `from_raw`/`from_int` (range-checked via `ATX_ASSERT`), `from_double`/`from_string` → `Result`, `raw()`, `to_double()`, `to_string()`. `operator+ - ` exact with `ATX_ASSERT` no-overflow (debug) / saturate or checked (document). `operator* /` via **portable `mul128`/`div128`** helper:
  - clang/gcc: `__int128`.
  - MSVC: `_mul128`/`_umul128` + `_div128` intrinsics (`<intrin.h>`), or a 64×64→128 fallback. Single `// SAFETY:`-documented seam, tested on both code paths.
  - Truncate toward zero (documented); provide `round()` for explicit rounding.
  `constexpr` for arithmetic that doesn't need 128-intrinsics at constant-eval (provide constexpr path via `__int128` when available, runtime path otherwise). `operator<=>` defaulted. `from_string`/`to_string` bodies in `decimal.cpp`.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Integrate CMake** (add `src/decimal.cpp` to library, test to tests). Full build `/WX` on **both** clang-cl and (if available) MSVC to exercise both 128 paths.
- [ ] **Step 6: Commit.** `feat(core): decimal — exact fixed-point money type`

## Task 8: `random`

**Files:** Create `include/atx/core/random.hpp`, `src/random.cpp` (only if non-inline helpers needed; prefer header-only), `tests/random_test.cpp`. Modify tests (+ lib if .cpp) CMake. Depends: `types`, `bit`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/random.hpp>
#include <gtest/gtest.h>
using namespace atx::core;
TEST(Random, Deterministic) {
  Xoshiro256pp a{42}, b{42};
  for (int i = 0; i < 100; ++i) EXPECT_EQ(a.next_u64(), b.next_u64());
}
TEST(Random, DifferentSeedsDiffer) {
  Xoshiro256pp a{1}, b{2};
  EXPECT_NE(a.next_u64(), b.next_u64());
}
TEST(Random, UniformInRange) {
  Xoshiro256pp r{7};
  for (int i = 0; i < 1000; ++i) { double u = r.uniform01(); EXPECT_GE(u, 0.0); EXPECT_LT(u, 1.0); }
}
TEST(Random, JumpProducesIndependentStream) {
  Xoshiro256pp a{42}; auto b = a; b.jump();
  EXPECT_NE(a.next_u64(), b.next_u64());
}
```

- [ ] **Step 2–4:** Implement `Xoshiro256pp` (seed via SplitMix64 expansion, `next_u64`, `uniform01` → [0,1), `uniform(lo,hi)`, `normal()` via ziggurat or Box-Muller, `bernoulli(p)`, `jump()` for stream splitting). No global state; all explicit-state. Deterministic. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): random — deterministic xoshiro256++ PRNG`.

---

# WAVE L2 — Memory

## Task 9: `arena`

**Files:** Create `include/atx/core/arena.hpp`, `tests/arena_test.cpp`. Modify tests CMake. Depends: `platform`, `bit`, `types`, `macro`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/arena.hpp>
#include <gtest/gtest.h>
#include <cstddef>
using namespace atx::core;
TEST(Arena, AllocatesAligned) {
  alignas(64) std::byte buf[1024];
  Arena a{buf, sizeof(buf)};
  void* p = a.allocate(16, 16);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16, 0u);
}
TEST(Arena, ReturnsNullWhenExhausted) {
  alignas(64) std::byte buf[32];
  Arena a{buf, sizeof(buf)};
  EXPECT_NE(a.allocate(32, 1), nullptr);
  EXPECT_EQ(a.allocate(1, 1), nullptr);
}
TEST(Arena, ResetReclaims) {
  alignas(64) std::byte buf[64];
  Arena a{buf, sizeof(buf)};
  (void)a.allocate(64, 1); a.reset();
  EXPECT_NE(a.allocate(64, 1), nullptr);
}
```

- [ ] **Step 2–4:** Implement `Arena` over caller-owned `std::span<std::byte>`: bump pointer, alignment-correct `allocate(size, align)` returning `void*`/`nullptr`, `reset()`, `used()`/`capacity()`. `create<T>(args...)` placement-new helper returning `T*`. No ownership of buffer (non-owning). `// SAFETY:` on the pointer arithmetic + placement new. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): arena — monotonic bump allocator`.

## Task 10: `object_pool`

**Files:** Create `include/atx/core/object_pool.hpp`, `tests/object_pool_test.cpp`. Modify tests CMake. Depends: `arena`/`aligned`, `types`, `macro`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/object_pool.hpp>
#include <gtest/gtest.h>
using namespace atx::core;
TEST(ObjectPool, AcquireRelease) {
  ObjectPool<int, 4> p;
  int* a = p.acquire(); int* b = p.acquire();
  ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr); EXPECT_NE(a, b);
  p.release(a);
  EXPECT_EQ(p.acquire(), a);   // freed slot reused
}
TEST(ObjectPool, ExhaustionReturnsNull) {
  ObjectPool<int, 2> p;
  (void)p.acquire(); (void)p.acquire();
  EXPECT_EQ(p.acquire(), nullptr);
}
```

- [ ] **Step 2–4:** Implement fixed-capacity `ObjectPool<T, N>`: inline storage (`std::array` of aligned raw storage) + intrusive free-list (union the free slot with a `next` index). O(1) `acquire()` (placement-new), `release(T*)` (dtor + push free). No heap. `// SAFETY:` on raw-storage placement-new/launder. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): object_pool — fixed-capacity O(1) pool`.

## Task 11: `aligned`

**Files:** Create `include/atx/core/aligned.hpp`, `tests/aligned_test.cpp`. Modify tests CMake. Depends: `platform`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/aligned.hpp>
#include <gtest/gtest.h>
using namespace atx::core;
TEST(Aligned, BufferIsCacheAligned) {
  AlignedBuffer<double, 16> b;
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b.data()) % kCacheLineSize, 0u);
  EXPECT_EQ(b.size(), 16u);
}
TEST(Aligned, AllocFreeRoundTrip) {
  void* p = aligned_alloc_bytes(256, 64);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64, 0u);
  aligned_free(p);
}
```

- [ ] **Step 2–4:** Implement `aligned_alloc_bytes(size, align)` / `aligned_free` (portable: `_aligned_malloc`/`_aligned_free` on MSVC, `std::aligned_alloc`/`free` elsewhere — `// SAFETY:` seam), `AlignedBuffer<T, N>` (cache-line-aligned `std::array`-like, `data()`/`size()`/`span()`). PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): aligned — cache-aligned alloc + buffer`.

---

# WAVE L3 — Containers

## Task 12: `container/ring_buffer`

**Files:** Create `include/atx/core/container/ring_buffer.hpp`, `tests/ring_buffer_test.cpp`, `bench/ring_buffer_bench.cpp`. Modify tests + bench CMake. Depends: `bit`, `types`, `macro`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/container/ring_buffer.hpp>
#include <gtest/gtest.h>
using namespace atx::core::container;
TEST(RingBuffer, PushPopFifo) {
  RingBuffer<int, 4> r;
  EXPECT_TRUE(r.push(1)); EXPECT_TRUE(r.push(2));
  EXPECT_EQ(r.pop().value(), 1); EXPECT_EQ(r.pop().value(), 2);
  EXPECT_FALSE(r.pop().has_value());
}
TEST(RingBuffer, PushFailsWhenFull) {
  RingBuffer<int, 2> r;   // cap rounds to 2
  EXPECT_TRUE(r.push(1)); EXPECT_TRUE(r.push(2));
  EXPECT_FALSE(r.push(3));
  EXPECT_EQ(r.size(), 2u);
}
TEST(RingBuffer, OverwriteDropsOldest) {
  RingBuffer<int, 2> r;
  r.push_overwrite(1); r.push_overwrite(2); r.push_overwrite(3);
  EXPECT_EQ(r.pop().value(), 2); EXPECT_EQ(r.pop().value(), 3);
}
TEST(RingBuffer, IndexedWindowAccess) {
  RingBuffer<int, 4> r; r.push(10); r.push(20); r.push(30);
  EXPECT_EQ(r[0], 10); EXPECT_EQ(r[2], 30);   // oldest..newest
}
```

- [ ] **Step 2–4:** Implement `RingBuffer<T, Capacity>`: capacity rounded up to power-of-two (`next_pow2`), mask indexing (`head & mask`), inline `std::array` storage, `push`/`push_overwrite`/`pop`(→`std::optional<T>`)/`size`/`empty`/`full`/`operator[]` (oldest-relative, `ATX_ASSERT` bounds) / `clear`. Trivial-type fast path; non-trivial: placement-new/dtor. PASS.
- [ ] **Step 5: Bench** push/pop throughput in `ring_buffer_bench.cpp`.
- [ ] **Step 6–7:** CMake/build → commit `feat(core): container/ring_buffer — fixed-cap circular buffer`.

## Task 13: `container/fixed_vector`

**Files:** Create `include/atx/core/container/fixed_vector.hpp`, `tests/fixed_vector_test.cpp`. Modify tests CMake. Depends: `types`, `macro`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/container/fixed_vector.hpp>
#include <gtest/gtest.h>
using namespace atx::core::container;
TEST(FixedVector, PushBackAndIndex) {
  FixedVector<int, 4> v; v.push_back(1); v.push_back(2);
  EXPECT_EQ(v.size(), 2u); EXPECT_EQ(v[0], 1); EXPECT_EQ(v[1], 2);
}
TEST(FixedVector, FullReturnsFalseOnTryPush) {
  FixedVector<int, 2> v; v.push_back(1); v.push_back(2);
  EXPECT_FALSE(v.try_push_back(3));
}
TEST(FixedVector, PopBackAndClear) {
  FixedVector<int, 4> v; v.push_back(1); v.pop_back();
  EXPECT_TRUE(v.empty());
}
```

- [ ] **Step 2–4:** Implement `FixedVector<T, N>`: inline aligned storage, `size_`, `push_back` (`ATX_ASSERT` capacity), `try_push_back`→bool, `pop_back`, `operator[]`/`at`(→`Result`), `begin/end`, `clear`, `data`, dtor destroys live elements. Contiguous, no heap. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): container/fixed_vector — static-capacity vector`.

## Task 14: `container/small_vector`

**Files:** Create `include/atx/core/container/small_vector.hpp`, `tests/small_vector_test.cpp`. Modify tests CMake. Depends: `types`, `aligned`, `macro`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/container/small_vector.hpp>
#include <gtest/gtest.h>
using namespace atx::core::container;
TEST(SmallVector, StaysInlineUnderN) {
  SmallVector<int, 4> v; for (int i=0;i<4;++i) v.push_back(i);
  EXPECT_FALSE(v.spilled()); EXPECT_EQ(v.size(), 4u);
}
TEST(SmallVector, SpillsToHeapOverN) {
  SmallVector<int, 2> v; for (int i=0;i<10;++i) v.push_back(i);
  EXPECT_TRUE(v.spilled()); EXPECT_EQ(v.size(), 10u); EXPECT_EQ(v[9], 9);
}
TEST(SmallVector, MovePreservesElements) {
  SmallVector<int, 2> a; for (int i=0;i<5;++i) a.push_back(i);
  SmallVector<int, 2> b = std::move(a);
  EXPECT_EQ(b.size(), 5u); EXPECT_EQ(b[4], 4);
}
```

- [ ] **Step 2–4:** Implement `SmallVector<T, N>`: inline buffer of N, spill to heap beyond; `push_back`/`pop_back`/`operator[]`/`size`/`capacity`/`reserve`/`spilled()`/iterators; correct move/copy (Rule of Five — owns heap when spilled), grow by 2×. `// SAFETY:` on raw storage + relocation. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): container/small_vector — inline+spill vector`.

## Task 15: `container/hash_map` + `hash_set`

**Files:** Create `include/atx/core/container/hash_map.hpp`, `tests/hash_map_test.cpp`. Modify tests CMake. Depends: unordered_dense, `hash`, `error`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/container/hash_map.hpp>
#include <gtest/gtest.h>
#include <string>
using namespace atx::core::container;
TEST(HashMap, InsertAndFind) {
  HashMap<std::string, int> m; m.insert_or_assign("AAPL", 42);
  ASSERT_TRUE(m.contains("AAPL")); EXPECT_EQ(m.at("AAPL").value(), 42);
}
TEST(HashMap, AtMissingReturnsError) {
  HashMap<std::string, int> m;
  EXPECT_FALSE(m.at("MSFT").has_value());
}
TEST(HashSet, InsertContains) {
  HashSet<int> s; s.insert(7);
  EXPECT_TRUE(s.contains(7)); EXPECT_FALSE(s.contains(8));
}
```

- [ ] **Step 2–4:** Implement `HashMap<K,V>` / `HashSet<K>` as thin wrappers exposing `ankerl::unordered_dense::map/set` API plus a `Result`-returning `at(key)` (no-throw lookup) and default `atx` hashing. Re-export iterators/`insert_or_assign`/`erase`/`contains`/`size`/`reserve`. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): container/hash_map — unordered_dense wrapper`.

## Task 16: `container/intrusive_list`

**Files:** Create `include/atx/core/container/intrusive_list.hpp`, `tests/intrusive_list_test.cpp`. Modify tests CMake. Depends: `macro`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/container/intrusive_list.hpp>
#include <gtest/gtest.h>
using namespace atx::core::container;
struct Node { int v; ListHook hook; };
TEST(IntrusiveList, PushPopOrder) {
  IntrusiveList<Node, &Node::hook> l;
  Node a{1,{}}, b{2,{}};
  l.push_back(a); l.push_back(b);
  EXPECT_EQ(&l.front(), &a); EXPECT_EQ(&l.back(), &b);
  l.pop_front(); EXPECT_EQ(&l.front(), &b);
}
TEST(IntrusiveList, UnlinkNode) {
  IntrusiveList<Node, &Node::hook> l;
  Node a{1,{}}, b{2,{}}, c{3,{}};
  l.push_back(a); l.push_back(b); l.push_back(c);
  l.unlink(b); EXPECT_EQ(l.size(), 2u);
}
```

- [ ] **Step 2–4:** Implement `ListHook` (prev/next ptrs) + `IntrusiveList<T, HookPtr>` (member-pointer to hook): `push_back`/`push_front`/`pop_front`/`pop_back`/`unlink(node)`/`front`/`back`/`size`/`empty`/iterators. Non-owning (caller owns nodes). O(1) unlink. `// SAFETY:` on pointer wiring. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): container/intrusive_list — zero-alloc linked list`.

---

# WAVE L4 — Concurrency

## Task 17: `concurrent/spinlock`

**Files:** Create `include/atx/core/concurrent/spinlock.hpp`, `tests/spinlock_test.cpp`. Modify tests CMake. Depends: `platform`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/concurrent/spinlock.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
using namespace atx::core::concurrent;
TEST(SpinLock, MutualExclusionCounter) {
  SpinLock lk; long counter = 0;
  std::vector<std::thread> ts;
  for (int t=0;t<4;++t) ts.emplace_back([&]{
    for (int i=0;i<10000;++i){ std::lock_guard g{lk}; ++counter; }});
  for (auto& t: ts) t.join();
  EXPECT_EQ(counter, 40000);
}
```

- [ ] **Step 2–4:** Implement `SpinLock`: cache-line-padded `std::atomic_flag`/`atomic<bool>`, TTAS (`test` then `test_and_set` acquire / `clear` release), `try_lock`, `lock`/`unlock` (Lockable for `std::lock_guard`), CPU pause hint in spin. `// SAFETY:` on memory ordering. PASS under TSan.
- [ ] **Step 5–6:** CMake/build (TSan run) → commit `feat(core): concurrent/spinlock — TTAS spinlock`.

## Task 18: `concurrent/seqlock`

**Files:** Create `include/atx/core/concurrent/seqlock.hpp`, `tests/seqlock_test.cpp`. Modify tests CMake. Depends: `platform`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/concurrent/seqlock.hpp>
#include <gtest/gtest.h>
#include <thread>
using namespace atx::core::concurrent;
struct Quote { double bid, ask; };
TEST(SeqLock, ReadSeesConsistentSnapshot) {
  SeqLock<Quote> q; q.store({1.0, 2.0});
  std::atomic<bool> stop{false};
  std::thread w([&]{ double x=0; while(!stop) { q.store({x, x+1.0}); x+=1.0; } });
  for (int i=0;i<100000;++i){ Quote r = q.load(); EXPECT_DOUBLE_EQ(r.ask - r.bid, 1.0); }
  stop = true; w.join();
}
```

- [ ] **Step 2–4:** Implement `SeqLock<T>` (T trivially-copyable, `static_assert`): even/odd sequence counter; writer increments-to-odd, stores, increments-to-even (release); reader loops reading seq (acquire), copies, re-reads seq, retries on change/odd. `// SAFETY:` ordering rationale. PASS under TSan.
- [ ] **Step 5–6:** CMake/build (TSan) → commit `feat(core): concurrent/seqlock — lock-free snapshot`.

## Task 19: `concurrent/spsc_queue`

**Files:** Create `include/atx/core/concurrent/spsc_queue.hpp`, `tests/spsc_queue_test.cpp`, `bench/spsc_queue_bench.cpp`. Modify tests+bench CMake. Depends: `bit`, `platform`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/concurrent/spsc_queue.hpp>
#include <gtest/gtest.h>
#include <thread>
using namespace atx::core::concurrent;
TEST(SpscQueue, SingleThreadFifo) {
  SpscQueue<int, 8> q;
  EXPECT_TRUE(q.try_push(1)); EXPECT_TRUE(q.try_push(2));
  int v=0; EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v,1);
}
TEST(SpscQueue, ProducerConsumerAllItems) {
  SpscQueue<int, 1024> q; constexpr int N = 1'000'000;
  std::thread prod([&]{ for(int i=0;i<N;){ if(q.try_push(i)) ++i; } });
  long sum=0; for(int i=0;i<N;){ int v; if(q.try_pop(v)){ sum+=v; ++i; } }
  prod.join();
  EXPECT_EQ(sum, (long)(N-1)*N/2);
}
```

- [ ] **Step 2–4:** Implement `SpscQueue<T, Capacity>`: power-of-two ring, **cache-line-padded** head (consumer) / tail (producer) atomics to avoid false sharing, acquire/release ordering, `try_push`/`try_pop` (bool). No alloc after construction. `// SAFETY:` ordering. PASS under TSan.
- [ ] **Step 5: Bench** ping-pong latency / throughput.
- [ ] **Step 6–7:** CMake/build (TSan) → commit `feat(core): concurrent/spsc_queue — lock-free SPSC ring`.

## Task 20: `concurrent/mpmc_queue`

**Files:** Create `include/atx/core/concurrent/mpmc_queue.hpp`, `tests/mpmc_queue_test.cpp`, `bench/mpmc_queue_bench.cpp`. Modify tests+bench CMake. Depends: `bit`, `platform`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/concurrent/mpmc_queue.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
using namespace atx::core::concurrent;
TEST(MpmcQueue, AllItemsTransferredExactlyOnce) {
  MpmcQueue<int, 1024> q; constexpr int P=4, C=4, PER=100000;
  std::atomic<long> sum{0}; std::atomic<int> consumed{0};
  std::vector<std::thread> th;
  for(int p=0;p<P;++p) th.emplace_back([&,p]{ for(int i=0;i<PER;){ if(q.try_push(p*PER+i)) ++i; } });
  for(int c=0;c<C;++c) th.emplace_back([&]{ int v; while(consumed.load() < P*PER){ if(q.try_pop(v)){ sum+=v; consumed.fetch_add(1);} } });
  for(auto& t: th) t.join();
  long expected=0; for(long i=0;i<(long)P*PER;++i) expected+=i;
  EXPECT_EQ(sum.load(), expected);
}
```

- [ ] **Step 2–4:** Implement Vyukov bounded MPMC: array of cells each with a sequence atomic; `try_push`/`try_pop` via CAS on enqueue/dequeue positions; power-of-two cap; padded positions. `// SAFETY:` ordering. PASS under TSan.
- [ ] **Step 5: Bench.** **Step 6–7:** CMake/build (TSan) → commit `feat(core): concurrent/mpmc_queue — Vyukov bounded MPMC`.

## Task 21: `concurrent/disruptor`

**Files:** Create `include/atx/core/concurrent/disruptor.hpp`, `tests/disruptor_test.cpp`, `bench/disruptor_bench.cpp`. Modify tests+bench CMake. Depends: `ring_buffer` concepts, `bit`, `platform`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/concurrent/disruptor.hpp>
#include <gtest/gtest.h>
#include <thread>
using namespace atx::core::concurrent;
TEST(Disruptor, SingleProducerSingleConsumerOrdered) {
  Disruptor<long, 1024> d; constexpr long N = 500000;
  std::thread prod([&]{ for(long i=0;i<N;++i){ long s=d.claim(); d.at(s)=i; d.publish(s);} });
  long expected=0, got;
  for(long i=0;i<N;++i){ long s = d.wait_for(i); got = d.at(i); EXPECT_EQ(got, expected); ++expected; d.consumed(i); }
  prod.join();
}
```

- [ ] **Step 2–4:** Implement `Disruptor<Event, Capacity>` (LMAX pattern): power-of-two ring of `Event`; producer `claim()` (reserve next sequence, single & multi-producer variants via CAS on cursor), `at(seq)` mask-indexed slot ref, `publish(seq)` (advance available cursor / availability buffer for MP); consumer `wait_for(seq)` (spin/yield until available cursor ≥ seq, returns highest available for batching), `consumed(seq)` (advance gating sequence so producer can wrap). No alloc after init; padded cursors. `// SAFETY:` ordering + wrap-gating rationale. PASS under TSan.
- [ ] **Step 5: Bench** throughput vs spsc_queue. **Step 6–7:** CMake/build (TSan) → commit `feat(core): concurrent/disruptor — LMAX sequenced ring`.

---

# WAVE L5 — SIMD

## Task 22: `simd`

**Files:** Create `include/atx/core/simd.hpp`, `tests/simd_test.cpp`, `bench/simd_bench.cpp`. Modify tests+bench CMake. Depends: xsimd, `types`, `platform`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/simd.hpp>
#include <gtest/gtest.h>
#include <vector>
using namespace atx::core::simd;
TEST(Simd, SumMatchesScalar) {
  std::vector<double> v(1000); for (size_t i=0;i<v.size();++i) v[i]=double(i);
  EXPECT_DOUBLE_EQ(sum(std::span<const double>(v)), 999.0*1000.0/2.0);
}
TEST(Simd, DotProduct) {
  std::vector<double> a{1,2,3,4}, b{5,6,7,8};
  EXPECT_DOUBLE_EQ(dot(std::span<const double>(a), std::span<const double>(b)), 70.0);
}
TEST(Simd, MeanAndMinMax) {
  std::vector<double> v{3,1,4,1,5,9,2,6};
  EXPECT_DOUBLE_EQ(mean(std::span<const double>(v)), 31.0/8.0);
  EXPECT_DOUBLE_EQ(min(std::span<const double>(v)), 1.0);
  EXPECT_DOUBLE_EQ(max(std::span<const double>(v)), 9.0);
}
TEST(Simd, AxpyTailHandled) {                       // size not multiple of width
  std::vector<double> x{1,2,3}; std::vector<double> y{10,10,10};
  axpy(2.0, std::span<const double>(x), std::span<double>(y));   // y += 2x
  EXPECT_DOUBLE_EQ(y[0],12); EXPECT_DOUBLE_EQ(y[2],16);
}
```

- [ ] **Step 2–4:** Implement over `xsimd::batch`: `sum`, `dot`, `mean`, `min`, `max`, `scale`, `add`, `axpy` for `std::span<const f64>`/`f32`. Loop in batches of `xsimd::batch<T>::size`, scalar tail. Alignment-aware (unaligned load fallback). `static_assert` span element-type constraints. PASS.
- [ ] **Step 5: Bench** vs scalar. **Step 6–7:** CMake/build → commit `feat(core): simd — vectorized span reductions (xsimd)`.

---

# WAVE L6 — Statistics & algorithms

## Task 23: `stats/online_stats`

**Files:** Create `include/atx/core/stats/online_stats.hpp`, `tests/online_stats_test.cpp`. Modify tests CMake. Depends: `math`, `types`, `container/ring_buffer` (for RunningMinMax).

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/stats/online_stats.hpp>
#include <gtest/gtest.h>
using namespace atx::core::stats;
TEST(RunningVariance, MeanAndVariance) {
  RunningVariance s; for (double x : {2.,4.,4.,4.,5.,5.,7.,9.}) s.update(x);
  EXPECT_DOUBLE_EQ(s.mean(), 5.0);
  EXPECT_NEAR(s.variance(), 4.0, 1e-9);     // population variance
}
TEST(RunningVariance, RemoveSupportsRolling) {
  RunningVariance s; s.update(1); s.update(2); s.update(3); s.remove(1);
  EXPECT_DOUBLE_EQ(s.mean(), 2.5); EXPECT_EQ(s.count(), 2u);
}
TEST(Ewma, ConvergesToConstant) {
  Ewma e{0.1}; for (int i=0;i<1000;++i) e.update(5.0);
  EXPECT_NEAR(e.value(), 5.0, 1e-6);
}
TEST(RunningMinMax, TracksWindowExtremes) {
  RunningMinMax<4> mm; for (double x : {3.,1.,4.,1.,5.}) mm.update(x);
  EXPECT_DOUBLE_EQ(mm.max(), 5.0); EXPECT_DOUBLE_EQ(mm.min(), 1.0);  // last 4: 1,4,1,5
}
```

- [ ] **Step 2–4:** Implement `RunningMean`, `RunningVariance` (Welford `update` + `remove` for rolling, population & sample variance accessors), `Ewma{alpha}` (`update`/`value`), `RunningMinMax<Window>` (monotonic deque over `ring_buffer`, O(1) amortized). All `noexcept`, numerically stable. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): stats/online_stats — Welford/EWMA/min-max`.

## Task 24: `stats/quantile`

**Files:** Create `include/atx/core/stats/quantile.hpp`, `tests/quantile_test.cpp`. Modify tests CMake. Depends: `math`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/stats/quantile.hpp>
#include <gtest/gtest.h>
#include <random>
using namespace atx::core::stats;
TEST(P2Quantile, ApproximatesMedianUniform) {
  P2Quantile q{0.5}; std::mt19937 rng{1}; std::uniform_real_distribution<double> u{0,1};
  for (int i=0;i<100000;++i) q.update(u(rng));
  EXPECT_NEAR(q.value(), 0.5, 0.02);
}
TEST(P2Quantile, P90) {
  P2Quantile q{0.9}; for (int i=1;i<=1000;++i) q.update(double(i));
  EXPECT_NEAR(q.value(), 900.0, 20.0);
}
```

- [ ] **Step 2–4:** Implement P² streaming quantile estimator (5 markers, constant memory, parabolic/linear marker adjustment). `value()`, `update(x)`, `count()`. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): stats/quantile — P² streaming estimator`.

## Task 25: `stats/algo`

**Files:** Create `include/atx/core/stats/algo.hpp`, `tests/algo_test.cpp`. Modify tests CMake. Depends: `types`, `container/fixed_vector` (optional), std `<algorithm>`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/stats/algo.hpp>
#include <gtest/gtest.h>
#include <vector>
using namespace atx::core::stats;
TEST(Algo, ArgsortAscending) {
  std::vector<double> v{3,1,2}; std::vector<std::size_t> idx(3);
  argsort(std::span<const double>(v), std::span<std::size_t>(idx));
  EXPECT_EQ(idx[0],1u); EXPECT_EQ(idx[1],2u); EXPECT_EQ(idx[2],0u);
}
TEST(Algo, TopKIndices) {
  std::vector<double> v{5,1,4,2,3}; std::vector<std::size_t> out(2);
  topk(std::span<const double>(v), std::span<std::size_t>(out));  // largest 2
  EXPECT_EQ(out[0],0u); EXPECT_EQ(out[1],2u);
}
```

- [ ] **Step 2–4:** Implement `argsort(in, out_idx)` (stable index sort), `partial_rank`, `topk(in, out_idx)` (largest-k via `nth_element` + sort). Span-based, no alloc when caller supplies output. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): stats/algo — argsort/topk`.

## Task 26: `stats/rolling`

**Files:** Create `include/atx/core/stats/rolling.hpp`, `tests/rolling_test.cpp`, `bench/rolling_bench.cpp`. Modify tests+bench CMake. Depends: `online_stats`, `container/ring_buffer`, `math`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/stats/rolling.hpp>
#include <gtest/gtest.h>
using namespace atx::core::stats;
TEST(RollingMean, WindowedAverage) {
  RollingMean<3> m;
  EXPECT_DOUBLE_EQ(m.update(1), 1.0);
  EXPECT_DOUBLE_EQ(m.update(2), 1.5);
  EXPECT_DOUBLE_EQ(m.update(3), 2.0);
  EXPECT_DOUBLE_EQ(m.update(4), 3.0);   // window {2,3,4}
}
TEST(RollingCorrelation, PerfectPositive) {
  RollingCorrelation<4> c; double r=0;
  for (int i=1;i<=4;++i) r = c.update(double(i), double(2*i));
  EXPECT_NEAR(r, 1.0, 1e-9);
}
TEST(RollingZScore, CenteredScale) {
  RollingZScore<5> z; double s=0;
  for (double x : {1.,2.,3.,4.,5.}) s = z.update(x);
  EXPECT_NEAR(s, 1.2649110640673518, 1e-9);   // (5-3)/std
}
```

- [ ] **Step 2–4:** Implement `RollingMean<W>`, `RollingStd<W>`, `RollingZScore<W>`, `RollingCovariance<W>`, `RollingCorrelation<W>` over `ring_buffer` + `RunningVariance::remove`. Each `update(x[,y])` returns current stat, O(1). PASS.
- [ ] **Step 5: Bench** update throughput. **Step 6–7:** CMake/build → commit `feat(core): stats/rolling — windowed mean/std/corr`.

## Task 27: `stats/cross_section`

**Files:** Create `include/atx/core/stats/cross_section.hpp`, `tests/cross_section_test.cpp`. Modify tests CMake. Depends: `stats/algo`, `simd`, `math`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/stats/cross_section.hpp>
#include <gtest/gtest.h>
#include <vector>
using namespace atx::core::stats;
TEST(CrossSection, RankNormalized) {
  std::vector<double> in{10,30,20}, out(3);
  rank(std::span<const double>(in), std::span<double>(out));   // [0,1]
  EXPECT_DOUBLE_EQ(out[0],0.0); EXPECT_DOUBLE_EQ(out[1],1.0); EXPECT_DOUBLE_EQ(out[2],0.5);
}
TEST(CrossSection, ZScoreZeroMean) {
  std::vector<double> v{1,2,3,4,5}; std::vector<double> out(5);
  zscore(std::span<const double>(v), std::span<double>(out));
  double s=0; for (double x: out) s+=x; EXPECT_NEAR(s, 0.0, 1e-9);
}
TEST(CrossSection, DemeanSumsZero) {
  std::vector<double> v{1,2,3}; demean(std::span<double>(v));
  EXPECT_NEAR(v[0]+v[1]+v[2], 0.0, 1e-9);
}
TEST(CrossSection, WinsorizeClampsTails) {
  std::vector<double> v{1,2,3,4,100}; winsorize(std::span<double>(v), 0.0, 0.8);
  EXPECT_LE(v[4], 4.0);
}
```

- [ ] **Step 2–4:** Implement `rank(in,out)` ([0,1] normalized ordinal, ties averaged), `zscore(in,out)` (subtract mean / divide std, uses simd mean), `demean(v)`, `winsorize(v, lo_q, hi_q)` (clamp to quantiles), `scale_to_unit(v)` (L1/L2 normalize). WorldQuant-alpha primitives. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): stats/cross_section — rank/zscore/winsorize`.

---

# WAVE L7 — Linear algebra

## Task 28: `linalg/linalg`

**Files:** Create `include/atx/core/linalg/linalg.hpp`, `tests/linalg_test.cpp`. Modify tests CMake. Depends: Eigen, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/linalg/linalg.hpp>
#include <gtest/gtest.h>
using namespace atx::core::linalg;
TEST(Linalg, MapSpanToVector) {
  std::array<double,3> a{1,2,3};
  VecMap v = as_vector(std::span<double>(a));
  EXPECT_DOUBLE_EQ(v.sum(), 6.0);
}
TEST(Linalg, MatVecProduct) {
  Mat2 m; m << 1,2, 3,4; Vec2 x; x << 1,1;
  Vec2 y = m * x; EXPECT_DOUBLE_EQ(y[0],3.0); EXPECT_DOUBLE_EQ(y[1],7.0);
}
```

- [ ] **Step 2–4:** Implement aliases over Eigen: `Vec2/3/4`, `VecX`, `Mat2/3/4`, `MatX` (column-major `double`), `VecMap`/`MatMap` (`Eigen::Map`), `as_vector(span)`/`as_matrix(span,rows,cols)` zero-copy bridges. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): linalg — Eigen aliases + span bridges`.

## Task 29: `linalg/regression`

**Files:** Create `include/atx/core/linalg/regression.hpp`, `src/regression.cpp` (optional, keep header-only if clean), `tests/regression_test.cpp`. Modify tests (+lib) CMake. Depends: `linalg`, `error`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/linalg/regression.hpp>
#include <gtest/gtest.h>
using namespace atx::core::linalg;
TEST(Regression, OlsRecoversKnownCoeffs) {
  MatX X(4,2); X << 1,1, 1,2, 1,3, 1,4;   // intercept + slope
  VecX y(4); y << 3,5,7,9;                 // y = 1 + 2x
  auto r = ols(X, y);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->beta[0], 1.0, 1e-9); EXPECT_NEAR(r->beta[1], 2.0, 1e-9);
  EXPECT_NEAR(r->r2, 1.0, 1e-9);
}
TEST(Regression, RankDeficientErrs) {
  MatX X(2,2); X << 1,1, 1,1;  VecX y(2); y << 1,2;
  EXPECT_FALSE(ols(X, y).has_value());
}
TEST(Regression, RidgeShrinks) {
  MatX X(3,1); X << 1,2,3; VecX y(3); y << 2,4,6;
  auto r = ridge(X, y, 10.0);
  ASSERT_TRUE(r.has_value()); EXPECT_LT(r->beta[0], 2.0);   // shrunk toward 0
}
```

- [ ] **Step 2–4:** Implement `ols` (QR/SVD solve, rank check → `Result`, R²/residuals), `ridge(X,y,lambda)` (penalized normal equations), `wls(X,y,w)` (weighted). `OlsResult{ VecX beta; f64 r2; VecX residuals; }`. Rank-deficiency → `Err(ErrorCode::InvalidArgument)`. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): linalg/regression — OLS/ridge/WLS`.

---

# WAVE L8 — Time & domain

## Task 30: `time/time`

**Files:** Create `include/atx/core/time/time.hpp`, `src/calendar.cpp`, `tests/time_test.cpp`. Modify tests+lib CMake. Depends: `types`, `safe_math`, `error`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/time/time.hpp>
#include <gtest/gtest.h>
using namespace atx::core::time;
TEST(Timestamp, ArithmeticWithDuration) {
  Timestamp t = Timestamp::from_seconds(100);
  Timestamp t2 = t + Duration::seconds(5);
  EXPECT_EQ((t2 - t).to_nanos(), 5'000'000'000LL);
}
TEST(Timestamp, Ordering) {
  EXPECT_LT(Timestamp::from_nanos(1), Timestamp::from_nanos(2));
}
TEST(Duration, UnitConversions) {
  EXPECT_EQ(Duration::millis(1).to_nanos(), 1'000'000LL);
  EXPECT_EQ(Duration::micros(1).to_nanos(), 1'000LL);
}
TEST(Calendar, SessionMembership) {
  Calendar cal = Calendar::us_equities();
  // 2026-06-01 is a Monday (open); a Saturday is closed
  EXPECT_TRUE(cal.is_session_open(Timestamp::from_unix_seconds(1'748'793'600))); // example weekday open
}
```

- [ ] **Step 2–4:** Implement `Timestamp` (i64 ns since unix epoch, strong type, `<=>`, `from_nanos/seconds/unix_seconds`, `+ Duration`, `- → Duration`), `Duration` (i64 ns, `seconds/millis/micros/nanos` factories, `to_nanos`), `Calendar`/`Session` (`us_equities()` factory, `is_session_open(ts)`, `next_session_open`/`prev_session_close`). Calendar weekday/holiday rules in `calendar.cpp`. Overflow-checked arithmetic via `safe_math`. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): time — Timestamp/Duration/Calendar`.

## Task 31: `domain/domain`

**Files:** Create `include/atx/core/domain/domain.hpp`, `include/atx/core/domain/symbol.hpp`, `src/symbol.cpp`, `tests/domain_test.cpp`. Modify tests+lib CMake. Depends: `decimal`, `time`, `container/hash_map`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/domain/domain.hpp>
#include <gtest/gtest.h>
using namespace atx::core::domain;
TEST(Domain, PriceTimesQuantityIsNotional) {
  Price p = Price::from_string("10.50").value();
  Quantity q = Quantity::from_int(3);
  Notional n = p * q;
  EXPECT_EQ(n.to_string(), "31.500000000");   // or trimmed per to_string policy
}
TEST(Domain, SideEnum) {
  EXPECT_NE(Side::Buy, Side::Sell);
}
TEST(Symbol, InternRoundTrip) {
  SymbolTable tbl; Symbol a = tbl.intern("AAPL"); Symbol b = tbl.intern("AAPL");
  EXPECT_EQ(a, b); EXPECT_EQ(tbl.name(a), "AAPL");
  EXPECT_NE(a, tbl.intern("MSFT"));
}
TEST(Domain, BarConstruction) {
  Bar bar{ Timestamp::from_seconds(1), Price::from_int(10), Price::from_int(12),
           Price::from_int(9), Price::from_int(11), Quantity::from_int(100) };
  EXPECT_EQ(bar.close, Price::from_int(11));
}
```

- [ ] **Step 2–4:** Implement strong types `Price`/`Quantity`/`Notional` wrapping `Decimal` (constructors, `from_int`/`from_string`/`to_string`, comparisons; typed `operator*(Price,Quantity)→Notional`, `Notional +/-`). `enum class Side : u8 { Buy, Sell }`. `Symbol{ u32 id }` + `SymbolTable` (`intern(string_view)→Symbol`, `name(Symbol)→string_view`, O(1) via `hash_map` + vector, body in `symbol.cpp`). POD records `Bar`/`OHLCV`, `Tick`. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): domain — Price/Quantity/Notional/Symbol/Bar/Tick`.

---

# WAVE L9 — Series (columnar)

## Task 32: `series/column`

**Files:** Create `include/atx/core/series/column.hpp`, `tests/column_test.cpp`, `bench/column_bench.cpp`. Modify tests+bench CMake. Depends: `aligned`, `simd`, `error`, `types`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/series/column.hpp>
#include <gtest/gtest.h>
using namespace atx::core::series;
TEST(Column, AppendAndView) {
  Column<double> c; for (int i=0;i<5;++i) c.append(double(i));
  EXPECT_EQ(c.size(), 5u);
  auto v = c.view(); EXPECT_DOUBLE_EQ(v[4], 4.0);
}
TEST(Column, CacheAlignedData) {
  Column<double> c; c.append(1.0);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(c.data()) % atx::core::kCacheLineSize, 0u);
}
TEST(Column, AtBoundsChecked) {
  Column<double> c; c.append(1.0);
  EXPECT_FALSE(c.at(5).has_value());
}
```

- [ ] **Step 2–4:** Implement `Column<T>`: cache-aligned growing buffer (via `aligned`), `append`/`append_bulk`/`reserve`/`size`/`data`/`view()`→`std::span<const T>`/`operator[]`/`at`→`Result`/`clear`. SIMD-ready contiguous layout. Optional validity bitmap (`is_valid(i)`, `append_null`). PASS.
- [ ] **Step 5: Bench** append + bulk sum (via simd). **Step 6–7:** CMake/build → commit `feat(core): series/column — aligned columnar buffer`.

## Task 33: `series/frame`

**Files:** Create `include/atx/core/series/frame.hpp`, `tests/frame_test.cpp`. Modify tests CMake. Depends: `series/column`, `container/hash_map`, `time`, `domain`, `error`.

- [ ] **Step 1: Failing test:**
```cpp
#include <atx/core/series/frame.hpp>
#include <gtest/gtest.h>
using namespace atx::core::series;
TEST(Frame, AddColumnsAndRows) {
  Frame f;
  auto& px = f.add_column<double>("price").value();
  auto& vol = f.add_column<double>("volume").value();
  px.append(10.0); vol.append(100.0);
  EXPECT_EQ(f.rows(), 1u);
  EXPECT_DOUBLE_EQ(f.column<double>("price").value().get().view()[0], 10.0);
}
TEST(Frame, MissingColumnErrs) {
  Frame f; EXPECT_FALSE(f.column<double>("nope").has_value());
}
TEST(Frame, RowCountInvariantAcrossColumns) {
  Frame f; auto& a = f.add_column<double>("a").value();
  a.append(1.0); a.append(2.0);
  EXPECT_EQ(f.rows(), 2u);
}
```

- [ ] **Step 2–4:** Implement `Frame`: named columns (heterogeneous via type-erased column holder keyed by name in `hash_map`), shared `Timestamp` index column, `add_column<T>(name)→Result<Column<T>&>`, `column<T>(name)→Result<ref>`, `rows()` (max column length / index length), `slice(begin,end)` view. Type mismatch / missing → `Result` error. PASS.
- [ ] **Step 5–6:** CMake/build → commit `feat(core): series/frame — multi-column time-series store`.

---

## Task 34: Module index + final integration

**Files:** Create `atx-core/README.md`. Modify `atx-core/include/atx/core/core.hpp` (umbrella include), remove placeholder `add()` if unused.

- [ ] **Step 1:** Write `atx-core/README.md`: one-line purpose + contract per module, grouped by layer; build/test/bench commands.
- [ ] **Step 2:** Make `core.hpp` an umbrella header including the public module headers (grouped, commented). Keep or remove the placeholder `add()` (and its test) — if removing, delete from `core.cpp`/`core_test.cpp`/CMake.
- [ ] **Step 3:** Full clean build `/W4 /WX` + complete `ctest --preset ninja` (all green) + `cmake --build --preset ninja --target atx-core-bench` builds.
- [ ] **Step 4:** Run benches once, record baselines in README.
- [ ] **Step 5: Commit.** `docs(core): module index + umbrella header + bench baselines`

---

## Self-review notes (author)

- **Spec coverage:** every spec §4 module (L0–L9) maps to a task (Tasks 1–33); deps/bench/gates in Task 0 + conventions; acceptance (§8) in Task 34. ✔
- **Decimal `__int128`/MSVC:** handled in Task 7 Step 3 (portable `mul128`/`div128` seam). ✔
- **Type consistency:** `Result<T>`/`Status`, `Ok/Err`, `ATX_ASSERT`, `kCacheLineSize`, `Decimal::kScale`, `OlsResult.beta/r2/residuals`, `Column::view()/at()` used consistently across tasks. ✔
- **Naming:** namespaces `atx::core::{container,concurrent,stats,linalg,time,domain,series}` consistent; foundation/numeric in `atx::core`. ✔
- **Parallelism:** intra-wave modules are independent (e.g. L0 bit/util/platform; L3 ring/fixed/small/hash/intrusive) → dispatch in parallel; inter-wave is a barrier on the lower layer.
