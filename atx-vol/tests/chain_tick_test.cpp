// Tick-path chain refresh: the cached baseline route (chain_tick.hpp) and the
// P-core pinning seam the benchmark main thread uses (cpu_topology.hpp).
// Pricing hot-path audit 2026-10-16, items 1 / 5a.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

#include "atx/vol/api/fitting/correction.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "pricing/cpu_topology.hpp"
#include "support/alloc_counter.hpp"

namespace {

using namespace atx::vol;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

struct SmallChain {
  std::vector<double> strike, expiry, sigma, rate, carry;
  std::vector<Side> side;
  [[nodiscard]] ChainContractsView view() const {
    return ChainContractsView{strike, expiry, sigma, rate, carry, side};
  }
};

// Two expiries x 5 strikes x both sides around S = 100.
[[nodiscard]] SmallChain make_chain() {
  SmallChain c;
  for (const double T : {0.08, 0.5}) {
    for (const Side sd : {Side::Call, Side::Put}) {
      for (const double K : {85.0, 95.0, 100.0, 105.0, 120.0}) {
        c.strike.push_back(K);
        c.expiry.push_back(T);
        c.sigma.push_back(0.22 + 0.10 * std::abs(std::log(K / 100.0)));
        c.rate.push_back(0.043);
        c.carry.push_back(0.015);
        c.side.push_back(sd);
      }
    }
  }
  return c;
}

struct Caches {
  std::optional<CorrectionCache> call, put;
};

[[nodiscard]] const Caches &caches() {
  static const Caches c = [] {
    Caches out;
    for (const Side sd : {Side::Call, Side::Put}) {
      Result<CorrectionCache> cc = CorrectionCache::build(
          16, 8, 12, 0.043, 0.015, -0.35, 0.35, 0.05, 0.75, 0.05, 1.5, sd, al_fast_opts());
      if (cc) {
        (sd == Side::Call ? out.call : out.put) = std::move(*cc);
      }
    }
    return out;
  }();
  return c;
}

struct Outputs {
  std::vector<double> price, delta, gamma, vega, theta;
  std::vector<TickStatus> status;
  explicit Outputs(std::size_t n, double fill = -7.0)
      : price(n, fill), delta(n, fill), gamma(n, fill), vega(n, fill), theta(n, fill),
        status(n, TickStatus::Unpriceable) {}
  [[nodiscard]] ChainTickOutputs view() {
    return ChainTickOutputs{price, delta, gamma, vega, theta, status};
  }
};

TEST(ChainTickCached, MatchesPerContractCachedGreeksBitForBit) {
  const SmallChain c = make_chain();
  ASSERT_TRUE(caches().call.has_value());
  ASSERT_TRUE(caches().put.has_value());
  const AmericanCorrectionCaches cc{&*caches().call, &*caches().put};
  Outputs out(c.strike.size());
  for (const double spot : {97.0, 100.0, 103.5}) {
    ASSERT_TRUE(refresh_chain_cached(spot, c.view(), cc, out.view()).has_value());
    for (std::size_t i = 0; i < c.strike.size(); ++i) {
      const Result<AmericanGreeks> g =
          american_greeks(spot, c.strike[i], c.expiry[i], c.sigma[i], c.rate[i], c.carry[i],
                          c.side[i], cc.for_side(c.side[i]));
      ASSERT_TRUE(g.has_value());
      EXPECT_TRUE(bits_equal(out.price[i], g->price)) << i;
      EXPECT_TRUE(bits_equal(out.delta[i], g->delta)) << i;
      EXPECT_TRUE(bits_equal(out.gamma[i], g->gamma)) << i;
      EXPECT_TRUE(bits_equal(out.vega[i], g->vega)) << i;
      EXPECT_TRUE(bits_equal(out.theta[i], g->theta)) << i;
      EXPECT_EQ(out.status[i], TickStatus::Ok) << i;
    }
  }
}

TEST(ChainTickCached, MissingSideCacheServesBlack76LegWithFallbackStatus) {
  const SmallChain c = make_chain();
  const AmericanCorrectionCaches only_put{nullptr, &*caches().put};
  Outputs out(c.strike.size());
  ASSERT_TRUE(refresh_chain_cached(100.0, c.view(), only_put, out.view()).has_value());
  for (std::size_t i = 0; i < c.strike.size(); ++i) {
    EXPECT_EQ(out.status[i], c.side[i] == Side::Call ? TickStatus::Fallback : TickStatus::Ok);
  }
}

TEST(ChainTickCached, RejectsMismatchedColumnsAndBadSpotWithoutWriting) {
  const SmallChain c = make_chain();
  const AmericanCorrectionCaches cc{&*caches().call, &*caches().put};
  Outputs short_out(c.strike.size() - 1u);
  EXPECT_FALSE(refresh_chain_cached(100.0, c.view(), cc, short_out.view()).has_value());
  EXPECT_EQ(short_out.price.front(), -7.0);

  Outputs out(c.strike.size());
  for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
    EXPECT_FALSE(refresh_chain_cached(bad, c.view(), cc, out.view()).has_value());
  }
  EXPECT_EQ(out.price.front(), -7.0);
}

TEST(ChainTickCached, InvalidContractIsUnpriceableNaNNotACallFailure) {
  SmallChain c = make_chain();
  c.sigma[3] = -0.2; // american_greeks refuses non-positive sigma
  const AmericanCorrectionCaches cc{&*caches().call, &*caches().put};
  Outputs out(c.strike.size());
  ASSERT_TRUE(refresh_chain_cached(100.0, c.view(), cc, out.view()).has_value());
  EXPECT_EQ(out.status[3], TickStatus::Unpriceable);
  EXPECT_TRUE(std::isnan(out.price[3]));
  EXPECT_EQ(out.status[4], TickStatus::Ok);
}

TEST(ChainTickCached, HappyPathTickAllocatesNothing) {
  const SmallChain c = make_chain();
  const AmericanCorrectionCaches cc{&*caches().call, &*caches().put};
  Outputs out(c.strike.size());
  const ChainContractsView v = c.view();
  const ChainTickOutputs ov = out.view();
  const std::uint64_t n = atx::vol::testkit::count_allocations(
      [&] { (void)refresh_chain_cached(101.0, v, cc, ov); });
  EXPECT_EQ(n, 0u);
}

// ── cpu_topology.hpp (audit item 5a) ─────────────────────────────────────────

TEST(CpuTopology, PinsACallingThreadOntoADiscoveredPerformanceCore) {
  const std::vector<unsigned> pcores = atx::vol::detail::performance_core_cpus();
#if defined(_MSC_VER)
  ASSERT_FALSE(pcores.empty()) << "P-core discovery returned nothing on Windows";
  // Pin a scratch thread, never the gtest main thread, so later tests are unaffected.
  atx::vol::detail::ThreadPinReport rep{};
  std::thread t([&] { rep = atx::vol::detail::pin_current_thread_for_latency(pcores.back()); });
  t.join();
  EXPECT_TRUE(rep.affinity_set);
  EXPECT_EQ(rep.observed_cpu, static_cast<int>(pcores.back()));
  EXPECT_TRUE(rep.pinned());
#else
  EXPECT_TRUE(pcores.empty());
#endif
}

TEST(CpuTopology, ZeroMaskAndOutOfRangeCpuAreRefusedNotApplied) {
  EXPECT_FALSE(atx::vol::detail::pin_current_thread_affinity(0));
  atx::vol::detail::ThreadPinReport rep{};
  std::thread t([&] { rep = atx::vol::detail::pin_current_thread_for_latency(64u); });
  t.join();
  EXPECT_FALSE(rep.affinity_set);
  EXPECT_FALSE(rep.pinned());
}

} // namespace
