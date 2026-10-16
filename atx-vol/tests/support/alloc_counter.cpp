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
} // namespace

namespace atx::vol::testkit {

void alloc_counter_arm() noexcept {
  t_count = 0;
  t_armed = true;
}

std::uint64_t alloc_counter_disarm() noexcept {
  t_armed = false;
  return t_count;
}

} // namespace atx::vol::testkit

void *operator new(std::size_t sz) {
  if (t_armed) {
    ++t_count;
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
