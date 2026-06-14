#pragma once

// atx::engine::parallel — the built-in `Test` shard kernel (S7-3).
//
// A pure, deterministic, RNG-state-free, wall-clock-free arithmetic kernel that
// is SHARED by two TUs that must agree to the bit:
//
//   * the worker executable (`shm_worker_main.cpp`) REGISTERS `test_shard` as the
//     body for `WorkloadId::Test`, runs it per shard into a shared-memory slot;
//   * the test (`parallel_process_executor_test.cpp`) computes the REFERENCE
//     table by calling `test_value(i, seed)` in a sequential loop.
//
// Because the kernel is a pure function of `(id, seed)` only (no captures, no
// shared mutable state, no clock, no RNG state — R3), the gathered process-pool
// output is byte-identical to the thread-pool output and to the sequential
// reference (R1/R7/§0.5) BY CONSTRUCTION: only the address space the slots live
// in differs across the three paths, never a result bit.
//
// Header-only `inline` so both the worker exe and the test link the SAME bytes.

#include <cstddef>
#include <cstring> // std::memcpy
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp"

#include "atx/engine/parallel/executor.hpp" // ShardId, InputView

namespace atx::engine::parallel {

// Sentinel meaning "no shard is forced to fail" — the happy path. Encoded as the
// bytes[8..16] field of the `Test` input so the kernel stays pure (the failure id
// travels in the read-only input, not in any out-of-band state). SIZE_MAX can
// never equal a real dense ShardId in [0, n), so the happy path is unperturbed.
inline constexpr atx::u64 kTestNoFail = static_cast<atx::u64>(-1);

// Pure deterministic scalar for shard `id` under `seed`: a SplitMix64 finalizer
// over (id ^ seed). The result is a normalized f64 in [0, 1) derived from the top
// 53 bits of the mixed u64 — a stable arithmetic mapping (no bit-cast of an
// arbitrary u64 to f64, which could yield a NaN whose bit pattern is unstable
// under the strict-FP build's signalling-NaN handling). No RNG STATE, no clock.
[[nodiscard]] inline atx::f64 test_value(ShardId id, atx::u64 seed) noexcept {
  // SplitMix64 finalizer (Steele/Vigna) — a pure avalanche mix, no state object.
  atx::u64 z = static_cast<atx::u64>(id) ^ seed;
  z += 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  z = z ^ (z >> 31U);
  // Top 53 bits -> a dyadic rational in [0, 1): exact, order-free, and identical
  // in every process/thread that runs the same `z` (no IEEE-754 reassociation).
  const atx::u64 mantissa = z >> 11U; // 53 significant bits
  return static_cast<atx::f64>(mantissa) * (1.0 / 9007199254740992.0); // / 2^53
}

// Read a little-endian u64 from `bytes` at `off`, or `fallback` if the field is
// not fully present. Pure; no UB (bounds-checked memcpy from the span).
[[nodiscard]] inline atx::u64 read_u64_le(std::span<const std::byte> bytes, atx::usize off,
                                          atx::u64 fallback) noexcept {
  if (bytes.size() < off + sizeof(atx::u64)) {
    return fallback;
  }
  atx::u64 v = 0;
  std::memcpy(&v, bytes.data() + off, sizeof(v)); // same-binary same-endian: stable
  return v;
}

// The `ShardFn`-compatible (plain free function, no captures) body for the `Test`
// workload. Input layout (all little-endian, optional fields default safely):
//   bytes[0 .. 8)   : u64 seed         (0 if the input is empty / too short)
//   bytes[8 .. 16)  : u64 fail_at      (kTestNoFail if absent => never fails)
// Writes `test_value(id, seed)` as raw f64 bytes into `out_slot`. Returns
// Err(OutOfRange) iff `id == fail_at` — the deterministic forced-failure used to
// exercise the cross-process lowest-id error reduction. The happy path is
// untouched (fail_at defaults to kTestNoFail, which no dense id can equal).
[[nodiscard]] inline atx::core::Status test_shard(InputView inputs, ShardId id,
                                                  std::span<std::byte> out_slot) noexcept {
  ATX_ASSERT(out_slot.size() >= sizeof(atx::f64)); // the parent sizes slots >= sizeof(f64)

  const atx::u64 seed = read_u64_le(inputs.bytes, 0, 0);
  const atx::u64 fail_at = read_u64_le(inputs.bytes, sizeof(atx::u64), kTestNoFail);

  if (static_cast<atx::u64>(id) == fail_at) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "test_shard: forced failure at id");
  }

  const atx::f64 value = test_value(id, seed);
  std::memcpy(out_slot.data(), &value, sizeof(value)); // raw f64 bytes — NO hashing here (§0.3)
  return atx::core::Ok();
}

} // namespace atx::engine::parallel
