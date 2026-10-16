#pragma once

// Allocation counting for the tick-path tests (atx-vol-tick-path-tests only).
//
// alloc_counter.cpp replaces the global scalar/array operator new of the binary it
// is linked into with a malloc passthrough that, while ARMED on the calling
// thread, bumps a thread-local tally. Arming is thread-local so gtest's own
// bookkeeping on other threads never counts. Over-aligned operator new is not
// replaced (no tick-path container uses over-aligned allocation).
//
// Usage:
//   const std::uint64_t n = testkit::count_allocations([&] { refresh(...); });
//   EXPECT_EQ(n, 0u);

#include <cstdint>

namespace atx::vol::testkit {

void alloc_counter_arm() noexcept;
[[nodiscard]] std::uint64_t alloc_counter_disarm() noexcept;

template <class Fn> [[nodiscard]] std::uint64_t count_allocations(Fn &&fn) {
  alloc_counter_arm();
  fn();
  return alloc_counter_disarm();
}

} // namespace atx::vol::testkit
