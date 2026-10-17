// Global operator new replacement with a thread-local AND a process-wide tally. See
// process_alloc_counter.hpp. Linked ONLY into atx-vol-chain-batch-tests.

#include "support/process_alloc_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {
thread_local bool t_armed = false;
thread_local std::uint64_t t_count = 0;
std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_count{0};
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

void process_alloc_counter_arm() noexcept {
  g_count.store(0, std::memory_order_relaxed);
  g_armed.store(true, std::memory_order_seq_cst);
}

std::uint64_t process_alloc_counter_disarm() noexcept {
  g_armed.store(false, std::memory_order_seq_cst);
  return g_count.load(std::memory_order_relaxed);
}

} // namespace atx::vol::testkit

void *operator new(std::size_t sz) {
  if (t_armed) {
    ++t_count;
  }
  if (g_armed.load(std::memory_order_relaxed)) {
    g_count.fetch_add(1, std::memory_order_relaxed);
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
