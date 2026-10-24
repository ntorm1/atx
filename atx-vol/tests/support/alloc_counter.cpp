// Global operator new replacement for the tick-path test binary. See
// alloc_counter.hpp. Linked ONLY into atx-vol-tick-path-tests.

#include "support/alloc_counter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {
thread_local bool t_armed = false;
thread_local std::uint64_t t_count = 0;
thread_local std::uint64_t t_fail_at = 0;
} // namespace

namespace atx::vol::testkit {

void alloc_counter_arm() noexcept {
  t_count = 0;
  t_fail_at = 0;
  t_armed = true;
}

std::uint64_t alloc_counter_disarm() noexcept {
  t_armed = false;
  t_fail_at = 0;
  return t_count;
}

void alloc_counter_fail_once_at(std::uint64_t ordinal) noexcept {
  t_fail_at = ordinal;
}

} // namespace atx::vol::testkit

void *operator new(std::size_t sz) {
  if (t_armed) {
    ++t_count;
    if (t_fail_at != 0 && t_count == t_fail_at) {
      t_fail_at = 0;
      throw std::bad_alloc();
    }
  }
  void *p = std::malloc(sz != 0 ? sz : 1);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t sz) { return ::operator new(sz); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
