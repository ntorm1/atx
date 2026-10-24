#pragma once

// Process-wide allocation counting for atx-vol-chain-batch-tests (batch-pricing lane,
// review fe6a516f fix round 1).
//
// process_alloc_counter.cpp replaces the global scalar/array operator new of the
// binary it is linked into (instead of alloc_counter.cpp, never alongside it). It
// keeps alloc_counter.hpp's thread-local API and adds a process-wide tally: while
// armed, an allocation on ANY thread counts, so PricingExecutor pool workers are
// covered. Arm it only around a call whose other threads are known to be idle.
//
// Usage:
//   const std::uint64_t n = testkit::count_process_allocations([&] { fanned_call(); });
//   EXPECT_EQ(n, 0u);

#include <cstdint>

#include "support/alloc_counter.hpp"

namespace atx::vol::testkit {

void process_alloc_counter_arm() noexcept;
[[nodiscard]] std::uint64_t process_alloc_counter_disarm() noexcept;

template <class Fn> [[nodiscard]] std::uint64_t count_process_allocations(Fn &&fn) {
  process_alloc_counter_arm();
  fn();
  return process_alloc_counter_disarm();
}

} // namespace atx::vol::testkit
