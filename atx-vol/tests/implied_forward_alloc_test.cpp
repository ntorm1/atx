// Allocation budget of estimate_implied_forward. This TU replaces the global
// operator new/delete, so it is linked ONLY into atx-vol-implied-forward-tests,
// never into the monolithic atx-vol-tests (which has its own replacement in
// backtest_exec_test.cpp). Counting is armed only around the measured call.
#include "atx/vol/api/marketdata/implied_forward.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"

namespace implied_forward_alloc {
std::atomic<bool> g_armed{false};
std::atomic<std::size_t> g_count{0};
} // namespace implied_forward_alloc

void *operator new(std::size_t size) {
  if (implied_forward_alloc::g_armed.load(std::memory_order_relaxed)) {
    implied_forward_alloc::g_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (void *p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

namespace atx::vol {
namespace {

struct Counted {
  Counted() {
    implied_forward_alloc::g_count.store(0);
    implied_forward_alloc::g_armed.store(true);
  }
  ~Counted() { implied_forward_alloc::g_armed.store(false); }
  std::size_t count() const { return implied_forward_alloc::g_count.load(); }
};

std::vector<ImpliedForwardPair> chain(bool american) {
  const double spot = 100.0;
  const double years = 1.0;
  const double rate = 0.05;
  const double forward = spot * std::exp(rate * years);
  const double df = std::exp(-rate * years);
  std::vector<ImpliedForwardPair> pairs;
  for (double k = 70.0; k <= 140.0; k += 5.0) {
    double call = 0.0;
    double put = 0.0;
    if (american) {
      call = *andersen_lake(spot, k, years, 0.3, rate, 0.0, Side::Call);
      put = *andersen_lake(spot, k, years, 0.3, rate, 0.0, Side::Put);
    } else {
      call = black76_price(forward, k, years, 0.3, df, Side::Call);
      put = black76_price(forward, k, years, 0.3, df, Side::Put);
    }
    pairs.push_back({k, call - 0.01, call + 0.01, put - 0.01, put + 0.01});
  }
  return pairs;
}

TEST(ImpliedForwardAllocation, ParityStageIsAllocationFree) {
  const auto pairs = chain(false);
  ImpliedForwardConfig cfg;
  cfg.exercise = ImpliedForwardExercise::European;
  cfg.method = ImpliedForwardMethod::Parity;
  const ImpliedForwardMarket market{100.0, 1.0, 0.05, {}};
  std::size_t allocations = 0;
  ImpliedForwardStatus status = ImpliedForwardStatus::Invalid;
  {
    const Counted counted;
    status = estimate_implied_forward(market, pairs, cfg).status;
    allocations = counted.count();
  }
  EXPECT_EQ(status, ImpliedForwardStatus::Ok);
  EXPECT_EQ(allocations, 0u);
}

TEST(ImpliedForwardAllocation, AmericanStageReusesPricerScratchOnSuccessPaths) {
  const auto pairs = chain(true);
  const ImpliedForwardMarket market{100.0, 1.0, 0.05, {}};
  // Warm the pricer's thread-local scratch, then measure a steady-state call.
  ASSERT_EQ(estimate_implied_forward(market, pairs).status, ImpliedForwardStatus::Ok);
  std::size_t allocations = 0;
  ImpliedForwardStatus status = ImpliedForwardStatus::Invalid;
  {
    const Counted counted;
    status = estimate_implied_forward(market, pairs).status;
    allocations = counted.count();
  }
  EXPECT_EQ(status, ImpliedForwardStatus::Ok);
  EXPECT_EQ(allocations, 0u);
}

} // namespace
} // namespace atx::vol
