// Per-contract Chebyshev spot ladders (ChainSpotLadder, api/pricing/chain_tick.hpp).
// Pricing hot-path audit 2026-10-16, items 1 and 4: the tick path. Accuracy is
// checked against the record the ladder is built from (the certificate's own
// reference) and against the cold accurate Andersen-Lake solver.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/api/fitting/expiry_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "support/alloc_counter.hpp"

namespace {

using namespace atx::vol;

constexpr double kS0 = 100.0;
constexpr double kR = 0.043;
constexpr double kQ = 0.015;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] double smile(double k) noexcept { return 0.25 + 0.10 * std::abs(k); }

struct SmallChain {
  std::vector<double> strike, expiry, sigma, rate, carry;
  std::vector<Side> side;
  [[nodiscard]] ChainContractsView view() const {
    return ChainContractsView{strike, expiry, sigma, rate, carry, side};
  }
};

// Three expiries (1 day .. 1.5 years) x 7 strikes over +/-3 sigma sqrt(T) x both
// sides = 42 contracts: not a multiple of the 4-lane block, on purpose.
[[nodiscard]] SmallChain make_chain() {
  SmallChain c;
  for (const double T : {1.0 / 365.0, 0.25, 1.5}) {
    const double half = 3.0 * 0.25 * std::sqrt(T);
    for (const Side sd : {Side::Call, Side::Put}) {
      for (int j = 0; j <= 6; ++j) {
        const double k = -half + 2.0 * half * j / 6.0;
        c.strike.push_back(kS0 * std::exp((kR - kQ) * T + k));
        c.expiry.push_back(T);
        c.sigma.push_back(smile(k));
        c.rate.push_back(kR);
        c.carry.push_back(kQ);
        c.side.push_back(sd);
      }
    }
  }
  return c;
}

[[nodiscard]] const SmallChain &chain() {
  static const SmallChain c = make_chain();
  return c;
}

[[nodiscard]] const ChainRecords &records() {
  static const ChainRecords r = [] {
    Result<ChainRecords> b = ChainRecords::build(kS0, chain().view());
    EXPECT_TRUE(b.has_value());
    return b ? std::move(*b) : ChainRecords{};
  }();
  return r;
}

struct Outputs {
  std::vector<double> price, delta, gamma, vega, theta;
  std::vector<TickStatus> status;
  explicit Outputs(std::size_t n)
      : price(n, -7.0), delta(n), gamma(n), vega(n), theta(n), status(n, TickStatus::Unpriceable) {}
  [[nodiscard]] ChainTickOutputs view() {
    return ChainTickOutputs{price, delta, gamma, vega, theta, status};
  }
};

[[nodiscard]] ChainSpotLadder build_ladder(const SpotLadderPolicy &pol = {}) {
  Result<ChainSpotLadder> l = ChainSpotLadder::build(kS0, chain().view(), records(), pol);
  EXPECT_TRUE(l.has_value());
  return l ? std::move(*l) : ChainSpotLadder{};
}

[[nodiscard]] const ExpiryRecord &record_of(std::size_t i) {
  return *records().bank().at(records().record_of_contract()[i]);
}

TEST(ChainSpotLadder, EveryContractIsCertifiedWithinPolicy) {
  ChainSpotLadder lad = build_ladder();
  ASSERT_EQ(lad.size(), chain().strike.size());
  EXPECT_EQ(lad.refused_contracts(), 0u);
  double worst = 0.0;
  for (const double e : lad.certified_error_usd()) {
    EXPECT_LE(e, SpotLadderPolicy{}.max_cert_usd);
    worst = std::max(worst, e);
  }
  std::printf("[ladder] worst certified error %.3e $ (policy %.1e); tick bytes %zu for %zu\n",
              worst, SpotLadderPolicy{}.max_cert_usd, lad.tick_bytes(), lad.size());
  EXPECT_EQ(lad.rebuilds(), 0u);
  EXPECT_EQ(lad.fallbacks(), 0u);
}

TEST(ChainSpotLadder, TickMatchesTheRecordInsideTheWindow) {
  ChainSpotLadder lad = build_ladder();
  Outputs out(chain().strike.size());
  double wp = 0.0, wd = 0.0, wv = 0.0;
  for (int t = -10; t <= 10; ++t) {
    const double S = kS0 * std::exp(0.0002 * t); // +/-20 bp: inside every window
    ASSERT_TRUE(lad.refresh(S, out.view()).has_value());
    for (std::size_t i = 0; i < out.price.size(); ++i) {
      const RecordGreeks g = record_of(i).greeks(S, chain().strike[i], chain().sigma[i]);
      ASSERT_NE(out.status[i], TickStatus::Fallback) << i;
      EXPECT_EQ(out.status[i] == TickStatus::Exercised, g.exercised) << i;
      wp = std::max(wp, std::abs(out.price[i] - g.price));
      wd = std::max(wd, std::abs(out.delta[i] - g.delta));
      wv = std::max(wv, std::abs(out.vega[i] - g.vega));
      EXPECT_LE(std::abs(out.price[i] - g.price), lad.certified_error_usd()[i] + 1.0e-12) << i;
    }
  }
  EXPECT_EQ(lad.rebuilds(), 0u);
  std::printf("[ladder] vs record: price %.3e delta %.3e vega %.3e\n", wp, wd, wv);
  EXPECT_LT(wd, 1.0e-4);
  EXPECT_LT(wv, 1.0e-2);
}

TEST(ChainSpotLadder, TickTracksTheColdSolverIncludingGreeks) {
  ChainSpotLadder lad = build_ladder();
  Outputs out(chain().strike.size());
  double wp = 0.0, wd = 0.0, wg = 0.0, wv = 0.0, wt = 0.0;
  for (const double S : {99.6, 100.0, 100.3}) {
    ASSERT_TRUE(lad.refresh(S, out.view()).has_value());
    for (std::size_t i = 0; i < out.price.size(); ++i) {
      const Result<AmericanGreeks> ref =
          american_greeks_al(S, chain().strike[i], chain().expiry[i], chain().sigma[i], kR, kQ,
                             chain().side[i], std::nullopt, true, false, false);
      ASSERT_TRUE(ref.has_value());
      wp = std::max(wp, std::abs(out.price[i] - ref->price));
      wd = std::max(wd, std::abs(out.delta[i] - ref->delta));
      if (std::abs(out.gamma[i] - ref->gamma) > wg) {
        std::printf("[ladder] new worst gamma: S=%.2f i=%zu T=%.4f %s K=%.3f status=%d "
                    "ladder=%.6e cold=%.6e price=%.6f\n",
                    S, i, chain().expiry[i], chain().side[i] == Side::Put ? "put" : "call",
                    chain().strike[i], static_cast<int>(out.status[i]), out.gamma[i],
                    ref->gamma, out.price[i]);
      }
      wg = std::max(wg, std::abs(out.gamma[i] - ref->gamma));
      wv = std::max(wv, std::abs(out.vega[i] - ref->vega));
      wt = std::max(wt, std::abs(out.theta[i] - ref->theta));
      EXPECT_LE(std::abs(out.price[i] - ref->price), lad.certified_error_usd()[i] + 1.0e-6) << i;
    }
  }
  std::printf("[ladder] vs cold: price %.3e delta %.3e gamma %.3e vega %.3e theta %.3e\n", wp, wd,
              wg, wv, wt);
  EXPECT_LT(wd, 2.0e-3);
  EXPECT_LT(wg, 2.0e-3);
  EXPECT_LT(wv, 5.0e-2);
  EXPECT_LT(wt, 0.5);
}

TEST(ChainSpotLadder, WindowExitRebuildsWithinBudgetThenCatchesUpNextTick) {
  SpotLadderPolicy pol;
  pol.max_rebuilds_per_tick = 32; // fewer than the 42 contracts that all exit at once
  ChainSpotLadder lad = build_ladder(pol);
  const std::size_t n = chain().strike.size();
  Outputs out(n);
  const double S = kS0 * std::exp(0.06); // past window_max (4%) for every contract
  ASSERT_TRUE(lad.refresh(S, out.view()).has_value());
  EXPECT_EQ(lad.rebuilds(), 32u);
  EXPECT_EQ(lad.fallbacks(), n - 32u);
  for (std::size_t i = 0; i < n; ++i) {
    const RecordGreeks g = record_of(i).greeks(S, chain().strike[i], chain().sigma[i]);
    EXPECT_LE(std::abs(out.price[i] - g.price), lad.certified_error_usd()[i] + 1.0e-12) << i;
  }
  // The next tick rebuilds the rest; the one after rebuilds nothing.
  ASSERT_TRUE(lad.refresh(S * 1.0005, out.view()).has_value());
  EXPECT_EQ(lad.rebuilds(), n);
  EXPECT_EQ(lad.fallbacks(), n - 32u);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_NE(out.status[i], TickStatus::Fallback) << i;
  }
  ASSERT_TRUE(lad.refresh(S * 1.001, out.view()).has_value());
  EXPECT_EQ(lad.rebuilds(), n) << "a rebuilt window must contain the next small move";
}

TEST(ChainSpotLadder, RebuildBudgetExhaustedServesTheRecordFlaggedFallback) {
  SpotLadderPolicy pol;
  pol.max_rebuilds_per_tick = 0;
  ChainSpotLadder lad = build_ladder(pol);
  Outputs out(chain().strike.size());
  const double S = kS0 * std::exp(-0.06);
  ASSERT_TRUE(lad.refresh(S, out.view()).has_value());
  EXPECT_EQ(lad.rebuilds(), 0u);
  EXPECT_EQ(lad.fallbacks(), out.price.size());
  for (std::size_t i = 0; i < out.price.size(); ++i) {
    EXPECT_EQ(out.status[i], TickStatus::Fallback) << i;
    const RecordGreeks g = record_of(i).greeks(S, chain().strike[i], chain().sigma[i]);
    EXPECT_TRUE(bits_equal(out.price[i], g.price)) << i;
    EXPECT_TRUE(bits_equal(out.theta[i], g.theta)) << i;
  }
}

TEST(ChainSpotLadder, UncertifiableContractsAreRefusedAndServedFromTheRecord) {
  SpotLadderPolicy pol;
  pol.max_cert_usd = 1.0e-300; // nothing can meet it
  ChainSpotLadder lad = build_ladder(pol);
  EXPECT_GT(lad.refused_contracts(), 0u);
  Outputs out(chain().strike.size());
  ASSERT_TRUE(lad.refresh(kS0, out.view()).has_value());
  EXPECT_EQ(lad.rebuilds(), 0u) << "a refused contract is never rebuilt";
  for (std::size_t i = 0; i < out.price.size(); ++i) {
    if (out.status[i] == TickStatus::Fallback) {
      const RecordGreeks g = record_of(i).greeks(kS0, chain().strike[i], chain().sigma[i]);
      EXPECT_TRUE(bits_equal(out.price[i], g.price)) << i;
    }
  }
}

TEST(ChainSpotLadder, DeepInTheMoneyPutsPastTheSplitAreIntrinsic) {
  ChainSpotLadder lad = build_ladder();
  Outputs out(chain().strike.size());
  ASSERT_TRUE(lad.refresh(kS0, out.view()).has_value());
  std::size_t exercised = 0;
  for (std::size_t i = 0; i < out.price.size(); ++i) {
    if (out.status[i] != TickStatus::Exercised) {
      continue;
    }
    ++exercised;
    EXPECT_EQ(chain().side[i], Side::Put);
    EXPECT_DOUBLE_EQ(out.price[i], chain().strike[i] - kS0);
    EXPECT_EQ(out.delta[i], -1.0);
    EXPECT_EQ(out.gamma[i], 0.0);
    EXPECT_EQ(out.theta[i], 0.0);
  }
  EXPECT_GT(exercised, 0u) << "the 1.5y deep-ITM put row should sit past the boundary";
}

TEST(ChainSpotLadder, TicksAllocateNothingEvenWhenTheyRebuild) {
  ChainSpotLadder lad = build_ladder();
  Outputs out(chain().strike.size());
  const ChainTickOutputs ov = out.view();
  const std::uint64_t n = atx::vol::testkit::count_allocations([&] {
    (void)lad.refresh(100.2, ov);
    (void)lad.refresh(kS0 * std::exp(0.07), ov); // forces rebuilds
  });
  EXPECT_GT(lad.rebuilds(), 0u);
  EXPECT_EQ(n, 0u);
}

TEST(ChainSpotLadder, ShapeErrorsAreRefusedWithoutWriting) {
  ChainSpotLadder lad = build_ladder();
  Outputs short_out(chain().strike.size() - 1u);
  EXPECT_FALSE(lad.refresh(kS0, short_out.view()).has_value());
  EXPECT_EQ(short_out.price.front(), -7.0);
  Outputs out(chain().strike.size());
  EXPECT_FALSE(lad.refresh(std::numeric_limits<double>::quiet_NaN(), out.view()).has_value());
  EXPECT_FALSE(lad.refresh(-1.0, out.view()).has_value());
  EXPECT_EQ(out.price.front(), -7.0);
  ChainSpotLadder empty;
  EXPECT_FALSE(empty.refresh(kS0, out.view()).has_value());
  SpotLadderPolicy bad;
  bad.window_min = 0.0;
  EXPECT_FALSE(ChainSpotLadder::build(kS0, chain().view(), records(), bad).has_value());
}

TEST(ChainSpotLadder, TwoThousandContractsFitInL2) {
  // 2,000 contracts = 500 blocks; the audit's L2 budget is 1.25 MB per P-core.
  ChainSpotLadder lad = build_ladder();
  const std::size_t per_block = lad.tick_bytes() / ((lad.size() + 3u) / 4u);
  EXPECT_LT(500u * per_block, 1250u * 1024u) << per_block << " bytes per 4-contract block";
}

} // namespace
