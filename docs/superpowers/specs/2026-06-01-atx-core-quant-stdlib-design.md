# atx-core — Quant Backtesting Standard Library — Design

**Date:** 2026-06-01
**Status:** Approved (design); pending spec review
**Author:** atx + Claude
**Standard:** C++20 · CMake · GoogleTest · Google Benchmark
**Authority:** `.agents/cpp/agent.md` (safety-critical-grade; JPL/MISRA/CERT discipline) governs all code.

---

## 1. Goal

Build the foundational, STL-grade C++ library (`atx-core`) that a production quant
backtesting engine (`atx-engine`) is built on. Deliver, in one pass, all the core
building blocks: vocabulary/domain types, numeric layer, data structures,
algorithms, statistics, linear algebra, concurrency primitives, and a columnar
time-series store. Quality bar: no UB, illegal states unrepresentable, tested
first, benchmark-gated on hot paths.

### Decisions (locked)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Delivery | Full build, one pass (all modules) | User directive. |
| Price/Qty/Notional representation | Fixed-point `Decimal` (exact) + strong types | No float drift on money; param-swap-proof. |
| Dependency policy | Vet-in proven libs where they fit; build the rest | Don't reinvent hash maps / SIMD / linalg. |
| Performance | Low-latency from day one (lock-free, SIMD, cache-aligned, bench-gated) | Matches engine target (Disruptor pattern, weak-signal aggregation at scale). |
| Execution | Subagent-driven development, one module per agent | Tight context windows; isolated, reviewable units. |

---

## 2. Approaches considered

- **A (chosen):** Layered header-first core, thin wrappers over vetted libraries,
  benchmark-gated, one module = header + test (+ bench). Subagent-friendly.
- **B (rejected):** Everything from scratch, zero new deps. Conflicts with the
  vet-in policy; reimplementing a flat hash map / SIMD abstraction / linalg would
  be lower quality than battle-tested libraries.
- **C (rejected):** Mega-framework substrate (Apache Arrow + Abseil). Arrow is
  overkill for a *core* library, heavy build cost, less hot-path control.

---

## 3. Third-party dependencies (vet-in)

All via CMake `FetchContent`, pinned tags, mostly header-only.

| Need | Library | Tag policy | Notes |
|------|---------|-----------|-------|
| flat hash map/set | `ankerl::unordered_dense` | pinned | header-only, faster than Abseil, MIT, bundles wyhash |
| linear algebra | `Eigen` | pinned | header-only, SIMD-vectorized |
| SIMD abstraction | `xsimd` | pinned | header-only, portable MSVC+Clang |
| fast hashing | wyhash | via unordered_dense | reuse bundled wyhash; no extra dep |
| perf gates | Google Benchmark | pinned | **test/bench-only**, not a public link dep |
| existing | spdlog, tl::expected, GoogleTest | already pinned | — |

Columnar time-series store is built **in-house** (lightweight, aligned, SIMD-ready),
NOT Arrow.

---

## 4. Architecture — dependency layers

Built bottom-up. A module may depend only on lower (or same-layer, acyclic) modules.
Namespaces: foundation in `atx::core`; submodules in `atx::core::{container,
concurrent, stats, linalg, time, domain, series}`.

### L0 — Foundation (extend existing)
- `types`, `error`, `macro`, `log` — **exist**, keep.
- `bit` — popcount, count{l,t}z, `next_pow2`, byteswap, `bit_width` (thin over `<bit>` + fallbacks).
- `util` — `ScopeGuard`/`ATX_DEFER`, `NonNull<T*>` (non-null observed ptr), `enum_flags<E>` (type-safe bit flags), `to_underlying`.
- `platform` — `kCacheLineSize` (64), `ATX_ALIGN`, `ATX_CACHE_ALIGNED`, prefetch intrinsics, `ATX_RESTRICT`.

### L1 — Numeric
- `safe_math` — checked + saturating integer arithmetic (`checked_add/sub/mul` → `Result`; `sat_add` etc.). No signed-overflow UB.
- `decimal` — **fixed-point `Decimal`**: `i64` mantissa, fixed scale `10^-9` (nano). Exact add/sub; mul/div via a **portable 128-bit intermediate** then rescale (truncation — see §6.2). `constexpr` throughout, `[[nodiscard]]`. Range ±9.22e9 whole units; out-of-range construction/ops return `Result` or saturate (documented per op). `from_string`/`to_string`, comparison, `<=>`.
  - **Portability:** `__int128` is unavailable on MSVC. Use an internal `mul128`/`div128` helper: `__int128`/`unsigned __int128` on Clang/GCC; `_mul128`/`_umul128` + `_div128` intrinsics (or a 64×64→128 fallback) on MSVC. Single seam, `// SAFETY:`-documented, unit-tested against both paths.
- `math` — f64 helpers: `clamp`, `lerp`, `isclose` (ULP + abs/rel tol), `sign`, `deg/rad` (only if needed). Keep minimal.
- `random` — deterministic PRNG (`xoshiro256pp`), seedable, `jump()`-splittable streams for reproducible parallel backtests; `uniform`, `normal` (ziggurat), `bernoulli`. No global state.
- `hash` — `hash_combine`, `wyhash` bytes helper, `Hashable` concept; integrates with unordered_dense.

### L2 — Memory (no hot-path allocation)
- `arena` — monotonic bump allocator over a caller-owned buffer; O(1) alloc, bulk reset; alignment-correct; `std::pmr::memory_resource` adapter.
- `object_pool` — fixed-capacity free-list pool, O(1) acquire/release, typed handles; optional growth disabled on hot path.
- `aligned` — cache-line aligned allocation helpers, `aligned_buffer<T,N>`, pmr hooks.

### L3 — Containers
- `ring_buffer` — fixed-capacity circular buffer (power-of-two cap, mask indexing); push/pop/overwrite modes; iterator; the substrate for rolling windows + queues.
- `fixed_vector` — static-capacity vector, zero heap, contiguous, full vector API subset.
- `small_vector` — inline storage with heap spill (`small_vector<T,N>`).
- `hash_map` / `hash_set` — thin alias + ergonomic wrapper over `ankerl::unordered_dense::map/set` with `atx` hash/`Result`-returning `at`.
- `intrusive_list` — non-owning doubly-linked list via member hooks (zero-alloc queues).

### L4 — Concurrency (low-latency)
- `spinlock` — TTAS spinlock, `lock_guard`-compatible; cache-line padded.
- `seqlock` — lock-free reader / single-writer for small POD snapshots.
- `spsc_queue` — lock-free single-producer/single-consumer bounded ring (acquire/release).
- `mpmc_queue` — bounded lock-free MPMC (Vyukov ticket ring).
- `disruptor` — sequenced ring buffer with producer/consumer sequence barriers (LMAX pattern); the engine event-bus core. Single + multi producer.

### L5 — SIMD
- `simd` — wrap `xsimd`: width-agnostic vectorized reductions and elementwise ops over `std::span<const f64>` / `<const f32>`: `sum`, `dot`, `axpy`, `mean`, `min`, `max`, `scale`, `add`. Scalar fallback; tail handling; alignment-aware. Used by L6/L9.

### L6 — Statistics & algorithms
- `online_stats` — `RunningMean`, `RunningVariance` (Welford), `EWMA`, `RunningMinMax` (monotonic deque over ring_buffer). O(1) update.
- `rolling` — fixed-window rolling `mean/var/std/zscore`, `RollingCovariance`, `RollingCorrelation` over `ring_buffer`. O(1) amortized update.
- `quantile` — P² streaming quantile/percentile estimator (constant memory).
- `cross_section` — cross-sectional ops over a `span` snapshot (one timestamp, many instruments): `rank`, `zscore`, `winsorize`, `demean`, `scale_to_unit` — WorldQuant-alpha primitives.
- `algo` — generic extras beyond `<algorithm>`: `argsort`, `partial_rank`, `topk`, `nth_element`-by-key.

### L7 — Linear algebra
- `linalg` — Eigen aliases: `Vec`, `Mat`, `VecX`, `MatX`, dynamic + fixed; convenience over `std::span` ↔ Eigen map.
- `regression` — `ols`, `ridge`, `wls` (weighted least squares) returning coefficients + diagnostics (R², residuals); signal-combination math (Grinold-Kahn weak-signal aggregation). `Result`-returning on rank-deficiency.

### L8 — Time & domain
- `time` — `Timestamp` (i64 nanoseconds since epoch, strong type, `<=>`), `Duration` (i64 ns), arithmetic, conversions (s/ms/us/ns), formatting; `Calendar`/`Session` (trading-day + session membership, next/prev session boundary). Calendar rule data in a `.cpp`.
- `domain` — strong types over `Decimal`: `Price`, `Quantity`, `Notional` (with `Price*Quantity → Notional` typed arithmetic); `Side` (`Buy`/`Sell`, `enum class`); `Symbol` (interned id → `SymbolTable` in a `.cpp`, O(1) id↔string); records `Bar`/`OHLCV` and `Tick` (POD, packed, with `Timestamp`).

### L9 — Series (columnar time-series store)
- `column` — typed, cache-line-aligned, column-major contiguous buffer (`Column<T>`); SIMD-ready; bulk append; `span` views; optional validity bitmap.
- `frame` — set of named columns sharing a `Timestamp` index; row count invariant; column lookup; zero-copy column `span` for SIMD/stats; basic select/slice.

---

## 5. Cross-cutting requirements (from agent.md)

### Testing (§7) — TDD, non-negotiable
- Every module: failing GoogleTest **first**, then implement. One behavior per `TEST`.
- Cover happy path, **boundaries** (0/1/max/empty/overflow edge), error paths, invariant violations (`EXPECT_DEATH` for documented preconditions).
- Deterministic, isolated, fast — no real clock/network; inject. `random` seeded.

### Benchmarks — perf gate
- L4 (concurrency), L5 (simd), L6 (stats), L9 (series): ship a Google Benchmark.
- Record ns/op baseline in the module; CI flags regressions. Document allocation budget (target: zero alloc in steady-state hot paths).

### Build / tooling (§8)
- Warnings = errors: MSVC `/W4 /permissive- /WX`; Clang `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.
- clang-tidy: `cppcoreguidelines-*,bugprone-*,cert-*,misc-*,performance-*`. clang-format enforced.
- Sanitizers in test runs: ASan + UBSan everywhere; **TSan for L4**.
- Header-first; `.cpp` only where required (`Symbol` table, `Calendar` data, PRNG/regression non-template helpers). Reproducible builds; pinned dep tags.

### API discipline (§2, §4, §6)
- `const`/`constexpr`/`noexcept`/`[[nodiscard]]` applied wherever they hold.
- Expected failures → `Result<T>` / `Status`; exceptions only for truly exceptional API-boundary errors. No throw for control flow.
- Weakest sufficient type at interfaces (`span`/`string_view`/concepts). No owning raw pointers. Rule of Zero (Rule of Five only when unavoidable, all-or-`=delete`).
- Every `switch` on `enum class` exhaustive; loops bounded; functions ≤ ~60 lines.
- `// SAFETY:` rationale on every deviation (`reinterpret_cast`, relaxed atomic, disabled lint).

---

## 6. Key interface contracts (subagent inputs)

These are the binding contracts each module's subagent implements. Signatures are
indicative; the implementing agent finalizes details under agent.md.

### 6.1 `safe_math`
```cpp
template <std::integral T> [[nodiscard]] constexpr Result<T> checked_add(T a, T b) noexcept;
template <std::integral T> [[nodiscard]] constexpr Result<T> checked_sub(T, T) noexcept;
template <std::integral T> [[nodiscard]] constexpr Result<T> checked_mul(T, T) noexcept;
template <std::integral T> [[nodiscard]] constexpr T sat_add(T, T) noexcept;  // saturating
```

### 6.2 `decimal`
```cpp
class Decimal {                       // i64 mantissa, scale = 1e-9 (nano), exact
 public:
  static constexpr i64 kScale = 1'000'000'000;
  constexpr Decimal() noexcept = default;
  static constexpr Decimal from_raw(i64 mantissa) noexcept;
  static constexpr Decimal from_int(i64 whole) noexcept;          // checked range
  static Result<Decimal> from_double(f64) noexcept;               // documented rounding
  static Result<Decimal> from_string(std::string_view);
  [[nodiscard]] constexpr i64 raw() const noexcept;
  [[nodiscard]] f64 to_double() const noexcept;
  [[nodiscard]] std::string to_string() const;
  // +,-: exact, checked. *,/: portable 128-bit intermediate (mul128/div128), truncate-toward-zero (documented).
  friend constexpr Decimal operator+(Decimal, Decimal);          // ATX_ASSERT no overflow
  friend constexpr auto operator<=>(Decimal, Decimal) = default;
};
```
Truncation mode (truncate-toward-zero) is the documented default; a `round()` helper is provided for explicit rounding.

### 6.3 `ring_buffer`
```cpp
template <class T, usize Capacity>   // Capacity rounded up to power of two
class RingBuffer {
  bool push(const T&);               // false if full (no-overwrite mode)
  void push_overwrite(const T&);     // drops oldest
  std::optional<T> pop();
  [[nodiscard]] usize size() const noexcept; bool empty() const noexcept;
  // contiguous-ish iteration for windowed stats
};
```

### 6.4 `disruptor`
```cpp
template <class Event, usize Capacity>  // power-of-two
class Disruptor {
  // single & multi producer; consumers track sequence barriers; no alloc after init
  i64 claim();                          // producer: reserve a slot sequence
  Event& at(i64 seq) noexcept;
  void publish(i64 seq) noexcept;
  // consumer: wait_for(seq) -> highest available; batch consume
};
```

### 6.5 `online_stats` / `rolling`
```cpp
class RunningVariance {                 // Welford, O(1)
  void update(f64 x) noexcept; void remove(f64 x) noexcept;  // remove for rolling
  [[nodiscard]] f64 mean() const noexcept; f64 variance() const noexcept; usize count() const noexcept;
};
template <usize Window> class RollingCorrelation { f64 update(f64 x, f64 y) noexcept; };
```

### 6.6 `cross_section`
```cpp
void rank(std::span<const f64> in, std::span<f64> out) noexcept;        // [0,1] or ordinal
void zscore(std::span<const f64> in, std::span<f64> out) noexcept;
void winsorize(std::span<f64> v, f64 lo_q, f64 hi_q) noexcept;
void demean(std::span<f64> v) noexcept;
```

### 6.7 `regression`
```cpp
struct OlsResult { VecX beta; f64 r2; VecX residuals; };
[[nodiscard]] Result<OlsResult> ols(const MatX& X, const VecX& y);
[[nodiscard]] Result<OlsResult> ridge(const MatX& X, const VecX& y, f64 lambda);
[[nodiscard]] Result<OlsResult> wls(const MatX& X, const VecX& y, const VecX& w);
```

### 6.8 `domain`
```cpp
class Price    { Decimal v; /* strong type, arithmetic with Quantity → Notional */ };
class Quantity { Decimal v; };
class Notional { Decimal v; };
[[nodiscard]] constexpr Notional operator*(Price, Quantity) noexcept;
enum class Side : u8 { Buy, Sell };
class Symbol { u32 id; };  // interned; SymbolTable resolves id <-> string_view
struct Bar { Timestamp ts; Price open, high, low, close; Quantity volume; };
struct Tick { Timestamp ts; Price price; Quantity size; Side aggressor; };
```

### 6.9 `column` / `frame`
```cpp
template <class T> class Column {        // cache-aligned, column-major
  void append(const T&); std::span<const T> view() const noexcept;
  usize size() const noexcept; T& operator[](usize); // bounds-checked at() -> Result
};
class Frame {                            // named columns, shared Timestamp index
  template <class T> Result<Column<T>&> column(std::string_view name);
  usize rows() const noexcept;
};
```

---

## 7. Execution model — subagent-driven development

- Each module is one isolated work unit: **header + test (+ bench)**.
- Subagents run TDD: write failing tests → implement → green → refactor, under agent.md.
- Each subagent receives **minimal context**: its module contract (§6) + the headers of the lower-layer modules it depends on (and their tests as usage examples) — nothing else.
- Orchestrator (main thread) drives dependency order, integrates each module into CMake (`atx-core/CMakeLists.txt` + `tests/CMakeLists.txt`), and runs the full build/test/sanitizer gate after each layer before proceeding.
- Independent modules within a layer dispatched in parallel; layers are barriers (an upper layer waits on the lower it depends on).

### Build order (waves)
1. **Wave L0:** `bit`, `util`, `platform` (parallel).
2. **Wave L1:** `safe_math`, `math`, `hash` (parallel) → then `decimal`, `random`.
3. **Wave L2:** `arena`, `object_pool`, `aligned` (parallel).
4. **Wave L3:** `ring_buffer`, `fixed_vector`, `small_vector`, `hash_map`, `intrusive_list` (parallel).
5. **Wave L4:** `spinlock`, `seqlock` → `spsc_queue`, `mpmc_queue` → `disruptor`.
6. **Wave L5:** `simd`.
7. **Wave L6:** `online_stats`, `quantile`, `algo` → `rolling`, `cross_section`.
8. **Wave L7:** `linalg` → `regression`.
9. **Wave L8:** `time` → `domain`.
10. **Wave L9:** `column` → `frame`.

Each wave: integrate, build `/W4 /WX`, run tests under ASan/UBSan (TSan for L4), run benches, then advance.

---

## 8. Acceptance criteria

- All modules implemented with tests passing under ASan + UBSan (TSan for L4).
- `/W4 /permissive- /WX` clean; clang-tidy clean.
- Benchmarks present and passing baseline for L4/L5/L6/L9.
- `atx-core` builds as a static lib; public headers under `include/atx/core/...`.
- A short `README`/module index documenting each module's purpose and contract.
- No UB, no owning raw pointers, no hot-path allocation in steady state.

## 9. Out of scope (engine layer, later)

Strategy/Portfolio/ExecutionHandler/DataHandler classes, order-matching engine,
broker simulation, P&L/risk attribution, alpha library, persistence/serialization
formats, network I/O. These live in `atx-engine` and consume `atx-core`.

## 10. Open defaults (resolved)

- Decimal scale fixed at 9 dp (nano), single scale for Price/Quantity/Notional;
  multiplication uses a portable 128-bit intermediate (`__int128` on Clang/GCC,
  `_umul128`/`_div128` on MSVC). Range ±9.22e9 whole units documented.
- Full module scope retained (no deferrals) per "Good" approval.
