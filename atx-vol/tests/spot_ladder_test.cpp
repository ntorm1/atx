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
[[nodiscard]] SmallChain make_chain(double s0 = kS0) {
  SmallChain c;
  for (const double T : {1.0 / 365.0, 0.25, 1.5}) {
    const double half = 3.0 * 0.25 * std::sqrt(T);
    for (const Side sd : {Side::Call, Side::Put}) {
      for (int j = 0; j <= 6; ++j) {
        const double k = -half + 2.0 * half * j / 6.0;
        c.strike.push_back(s0 * std::exp((kR - kQ) * T + k));
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

// Contract i's (k, sigma) at spot S lies inside its record's certified box.
[[nodiscard]] bool in_record_box(std::size_t i, double S) {
  const ExpiryRecord &rec = record_of(i);
  const double k = std::log(chain().strike[i] / (S * rec.forward_factor()));
  return rec.contains(k, chain().sigma[i]);
}

[[nodiscard]] double intrinsic(std::size_t i, double S) {
  const double v = chain().side[i] == Side::Put ? chain().strike[i] - S : S - chain().strike[i];
  return v > 0.0 ? v : 0.0;
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
  double worst_slack = -1.0;
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
      // The certificate itself, no allowance (review dcb6e3f1 nit: the old
      // +1e-6 $ slack was larger than most certificates it was checking).
      const double err = std::abs(out.price[i] - ref->price);
      worst_slack = std::max(worst_slack, err - lad.certified_error_usd()[i]);
      EXPECT_LE(err, lad.certified_error_usd()[i]) << i;
    }
  }
  std::printf("[ladder] vs cold: price %.3e delta %.3e gamma %.3e vega %.3e theta %.3e "
              "(worst err - cert %.3e)\n",
              wp, wd, wg, wv, wt, worst_slack);
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
  // +4.5%: past window_max (4%) for every contract, yet inside every record box
  // (ChainRecordPolicy::spot_move = 5% beyond the outermost strikes).
  const double S = kS0 * std::exp(0.045);
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(in_record_box(i, S)) << i;
  }
  ASSERT_TRUE(lad.refresh(S, out.view()).has_value());
  EXPECT_EQ(lad.rebuilds(), 32u);
  EXPECT_EQ(lad.fallbacks(), n - 32u);
  // Every contract is in its box: all of them are budget pressure, none a refit.
  EXPECT_EQ(lad.counters().budget_fallbacks, n - 32u);
  EXPECT_EQ(lad.counters().out_of_box_fallbacks, 0u);
  EXPECT_EQ(lad.counters().refused_fallbacks, 0u);
  for (std::size_t i = 0; i < n; ++i) {
    const RecordGreeks g = record_of(i).greeks(S, chain().strike[i], chain().sigma[i]);
    if (out.status[i] == TickStatus::Fallback) {
      EXPECT_TRUE(bits_equal(out.price[i], g.price)) << i;
    } else {
      EXPECT_LE(std::abs(out.price[i] - g.price), lad.certified_error_usd()[i] + 1.0e-12) << i;
    }
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

// Review finding (dcb6e3f1): a rebuilt window used to extend past the record's
// (k, sigma) box, serving box-clamped record values as Ok with a certificate
// that only covered the box — $6.0e-3 off the cold solver at +6% and $1.9e-2 at
// +8% on this chain. Walk the spot well past ChainRecordPolicy::spot_move both
// ways (and back) with an unlimited rebuild budget: every Ok / Exercised lane
// must lie inside its record box and within its certificate of the cold solver;
// every lane outside its box must be Fallback.
TEST(ChainSpotLadder, WalkPastTheRecordBoxNeverServesAnUncertifiedLane) {
  SpotLadderPolicy pol;
  pol.max_rebuilds_per_tick = 1000;
  ChainSpotLadder lad = build_ladder(pol);
  const std::size_t n = chain().strike.size();
  Outputs out(n);
  std::vector<double> path;
  for (int t = 1; t <= 40; ++t) {
    path.push_back(0.0025 * t); // up to +10%
  }
  for (int t = 39; t >= -40; --t) {
    path.push_back(0.0025 * t); // back down through 0 to -10%
  }
  double worst_served = 0.0, worst_slack = -1.0, worst_fallback = 0.0;
  std::size_t served = 0, outside = 0, checks = 0;
  for (std::size_t p = 0; p < path.size(); ++p) {
    const double x = path[p];
    const double S = kS0 * std::exp(x);
    ASSERT_TRUE(lad.refresh(S, out.view()).has_value());
    const bool cold_check = (std::lround(x / 0.0025) % 4) == 0; // every 1% of log-spot
    for (std::size_t i = 0; i < n; ++i) {
      const bool box = in_record_box(i, S);
      const bool is_served =
          out.status[i] == TickStatus::Ok || out.status[i] == TickStatus::Exercised;
      if (!box) {
        ++outside;
        EXPECT_EQ(out.status[i], TickStatus::Fallback) << "x=" << x << " i=" << i;
      } else if (in_record_box(i, S * (1.0 + 1.0e-9)) && in_record_box(i, S * (1.0 - 1.0e-9))) {
        // Strictly inside (the walk lands exactly on some box edges, where the
        // ladder's box is pulled in by 1e-12 and serves the record instead).
        EXPECT_TRUE(is_served) << "x=" << x << " i=" << i << " (budget is unlimited)";
      }
      if (!cold_check) {
        continue;
      }
      const Result<double> cold =
          andersen_lake(S, chain().strike[i], chain().expiry[i], chain().sigma[i], kR, kQ,
                        chain().side[i], std::nullopt);
      ASSERT_TRUE(cold.has_value());
      const double err = std::abs(out.price[i] - *cold);
      if (is_served) {
        ++served;
        const double cert = lad.certified_error_usd()[i];
        worst_served = std::max(worst_served, err);
        worst_slack = std::max(worst_slack, err - cert);
        EXPECT_LE(err, cert) << "x=" << x << " i=" << i; // no allowance
      } else if (!box) {
        worst_fallback = std::max(worst_fallback, err);
      }
      ++checks;
    }
  }
  std::printf("[ladder] box walk: %zu cold checks; served Ok/Exercised %zu, worst vs cold %.3e $ "
              "(worst err - cert %.3e); out-of-box lane-ticks %zu (Fallback), worst clamped "
              "record vs cold %.3e $\n",
              checks, served, worst_served, worst_slack, outside, worst_fallback);
  EXPECT_GT(outside, 0u) << "the walk must leave some record boxes";
  EXPECT_GT(served, 0u);
  // Back at the build spot every contract is inside its box again and rebuilt.
  ASSERT_TRUE(lad.refresh(kS0, out.view()).has_value());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_NE(out.status[i], TickStatus::Fallback) << i;
  }
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
  // -6% is past spot_move for the outer strikes: those are out of their box (a
  // refit), the rest are budget pressure. Each lane lands in exactly one bucket.
  std::size_t out_of_box = 0;
  for (std::size_t i = 0; i < out.price.size(); ++i) {
    out_of_box += in_record_box(i, S) ? 0u : 1u;
  }
  EXPECT_EQ(lad.counters().out_of_box_fallbacks, out_of_box);
  EXPECT_EQ(lad.counters().budget_fallbacks, out.price.size() - out_of_box);
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
  pol.max_cert_rel = 0.0;
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

// Review dcb6e3f1 regression: a record built with certify = false has no
// certificate to stand on, so every contract on it is REFUSED — served from the
// record, flagged Fallback, never rebuilt and never reported Ok — and the
// fallbacks land in the refused bucket (a refit, not budget pressure).
TEST(ChainSpotLadder, UncertifiedRecordsAreRefusedAndNeverServedOk) {
  ChainRecordPolicy rp;
  rp.certify = false;
  const Result<ChainRecords> recs = ChainRecords::build(kS0, chain().view(), rp);
  ASSERT_TRUE(recs.has_value());
  for (const std::uint32_t r : recs->record_of_contract()) {
    ASSERT_TRUE(std::isnan(recs->bank().at(r)->certified_max_abs_error_fwd()));
  }
  SpotLadderPolicy pol;
  pol.max_rebuilds_per_tick = 1000;
  Result<ChainSpotLadder> lad = ChainSpotLadder::build(kS0, chain().view(), *recs, pol);
  ASSERT_TRUE(lad.has_value());
  const std::size_t n = chain().strike.size();
  EXPECT_EQ(lad->refused_contracts(), n);
  EXPECT_EQ(lad->sigma_out_of_box_contracts(), 0u);
  for (const double cert : lad->certified_error_usd()) {
    EXPECT_TRUE(std::isnan(cert));
  }
  Outputs out(n);
  for (const double S : {kS0, kS0 * std::exp(0.03)}) {
    ASSERT_TRUE(lad->refresh(S, out.view()).has_value());
    for (std::size_t i = 0; i < n; ++i) {
      const ExpiryRecord &rec = *recs->bank().at(recs->record_of_contract()[i]);
      const RecordGreeks g = rec.greeks(S, chain().strike[i], chain().sigma[i]);
      EXPECT_EQ(out.status[i], TickStatus::Fallback) << i;
      EXPECT_TRUE(bits_equal(out.price[i], g.price)) << i;
      EXPECT_TRUE(bits_equal(out.delta[i], g.delta)) << i;
      EXPECT_TRUE(bits_equal(out.vega[i], g.vega)) << i;
    }
  }
  const SpotLadderCounters k = lad->counters();
  EXPECT_EQ(k.rebuilds, 0u) << "a refused contract is never rebuilt";
  EXPECT_EQ(k.refused_fallbacks, 2u * n);
  EXPECT_EQ(k.budget_fallbacks, 0u);
  EXPECT_EQ(k.out_of_box_fallbacks, 0u);
  EXPECT_EQ(k.sigma_out_of_box_fallbacks, 0u);
  EXPECT_EQ(lad->fallbacks(), 2u * n);
}

// Review dcb6e3f1 regression: a contract whose sigma has moved outside its
// record's sigma box has no certified window at any spot. It is served from the
// record (clamped), flagged Fallback, never rebuilt, has a NaN certificate and
// counts as sigma_out_of_box — never as budget pressure.
TEST(ChainSpotLadder, SigmaOutsideTheRecordBoxIsServedFromTheRecordAndNeverRebuilt) {
  SmallChain moved = chain();
  const std::size_t hi = 3;  // 1-day call, ATM
  const std::size_t lo = 30; // 1.5y call
  moved.sigma[hi] = record_of(hi).sigma_max() * 1.05;
  moved.sigma[lo] = record_of(lo).sigma_min() * 0.95;
  SpotLadderPolicy pol;
  pol.max_rebuilds_per_tick = 1000;
  Result<ChainSpotLadder> lad = ChainSpotLadder::build(kS0, moved.view(), records(), pol);
  ASSERT_TRUE(lad.has_value());
  const std::size_t n = moved.strike.size();
  EXPECT_EQ(lad->sigma_out_of_box_contracts(), 2u);
  EXPECT_EQ(lad->refused_contracts(), 0u);
  EXPECT_TRUE(std::isnan(lad->certified_error_usd()[hi]));
  EXPECT_TRUE(std::isnan(lad->certified_error_usd()[lo]));
  Outputs out(n);
  for (const double S : {kS0, kS0 * std::exp(0.01)}) {
    ASSERT_TRUE(lad->refresh(S, out.view()).has_value());
    for (std::size_t i = 0; i < n; ++i) {
      if (i == hi || i == lo) {
        const RecordGreeks g = record_of(i).greeks(S, moved.strike[i], moved.sigma[i]);
        EXPECT_FALSE(g.in_box) << i;
        EXPECT_EQ(out.status[i], TickStatus::Fallback) << i;
        EXPECT_TRUE(bits_equal(out.price[i], g.price)) << i;
        EXPECT_TRUE(bits_equal(out.delta[i], g.delta)) << i;
      } else {
        EXPECT_NE(out.status[i], TickStatus::Fallback) << i;
      }
    }
  }
  const SpotLadderCounters k = lad->counters();
  EXPECT_EQ(k.rebuilds, 0u);
  EXPECT_EQ(k.sigma_out_of_box_fallbacks, 4u); // 2 contracts x 2 ticks
  EXPECT_EQ(k.budget_fallbacks + k.out_of_box_fallbacks + k.refused_fallbacks, 0u);
  EXPECT_EQ(lad->fallbacks(), 4u);
}

// Review dcb6e3f1: the acceptance threshold scales with the forward. The record
// certificate is a forward-unit bound and the series error grows with the price
// level, so a fixed $5e-4 starves a high-priced underlying; max(max_cert_usd,
// max_cert_rel x F_max) does not. Below F ~ $1,000 the floor binds, so the $100
// chain is certified bit-identically with and without the relative term. These
// fixture records certify to <= 1.3e-8 fwd, so the fixed floor only starts to
// refuse past F ~ $38,000 (a BRK.A-scale price); the 40 bench records (<= 3.3e-7
// fwd, LEDGER) already cross it at F ~ $1,500.
TEST(ChainSpotLadder, CertificateThresholdScalesWithTheForward) {
  SpotLadderPolicy abs_only;
  abs_only.max_cert_rel = 0.0; // the pre-2026-10-16 fixed threshold
  ChainSpotLadder base = build_ladder();
  ChainSpotLadder fixed = build_ladder(abs_only);
  ASSERT_EQ(base.size(), fixed.size());
  for (std::size_t i = 0; i < base.size(); ++i) {
    EXPECT_TRUE(bits_equal(base.certified_error_usd()[i], fixed.certified_error_usd()[i])) << i;
  }

  constexpr double kBig = 50000.0;
  const SmallChain big = make_chain(kBig);
  const Result<ChainRecords> recs = ChainRecords::build(kBig, big.view());
  ASSERT_TRUE(recs.has_value());
  Result<ChainSpotLadder> scaled = ChainSpotLadder::build(kBig, big.view(), *recs);
  const Result<ChainSpotLadder> starved = ChainSpotLadder::build(kBig, big.view(), *recs, abs_only);
  ASSERT_TRUE(scaled.has_value());
  ASSERT_TRUE(starved.has_value());
  double worst = 0.0;
  for (std::size_t i = 0; i < scaled->size(); ++i) {
    const double cert = scaled->certified_error_usd()[i];
    worst = std::max(worst, cert);
    // F_max <= S0 e^{window_max} x the largest forward factor (1.5y) < 1.1 S0.
    EXPECT_LE(cert, SpotLadderPolicy{}.max_cert_rel * 1.1 * kBig) << i;
  }
  std::printf("[ladder] $%.0f chain: refused %zu (relative threshold) vs %zu (fixed $%.1e); "
              "worst certificate %.3e $\n",
              kBig, scaled->refused_contracts(), starved->refused_contracts(),
              abs_only.max_cert_usd, worst);
  EXPECT_EQ(scaled->refused_contracts(), 0u);
  EXPECT_GT(starved->refused_contracts(), 0u);

  // The scaled ladder's ticks stay within their certificates of the record.
  Outputs out(big.strike.size());
  ASSERT_TRUE(scaled->refresh(kBig * 1.001, out.view()).has_value());
  for (std::size_t i = 0; i < out.price.size(); ++i) {
    ASSERT_NE(out.status[i], TickStatus::Fallback) << i;
    const ExpiryRecord &rec = *recs->bank().at(recs->record_of_contract()[i]);
    const double ref = rec.price(kBig * 1.001, big.strike[i], big.sigma[i]);
    EXPECT_LE(std::abs(out.price[i] - ref), scaled->certified_error_usd()[i]) << i;
  }
}

// Review dcb6e3f1 nit 12: the Greeks belong to the price that is served. A lane
// served intrinsic (past the edge, or the series floored there) reports
// Exercised with the exercise Greeks; a lane floored at a worthless zero reports
// zero Greeks; every other served lane is strictly above intrinsic.
TEST(ChainSpotLadder, ServedGreeksBelongToTheServedPrice) {
  ChainSpotLadder lad = build_ladder();
  const std::size_t n = chain().strike.size();
  Outputs out(n);
  std::size_t exercised = 0, above = 0, worthless = 0;
  for (int t = -12; t <= 12; ++t) {
    const double S = kS0 * std::exp(0.0025 * t);
    ASSERT_TRUE(lad.refresh(S, out.view()).has_value());
    for (std::size_t i = 0; i < n; ++i) {
      const double intr = intrinsic(i, S);
      const double unit = chain().side[i] == Side::Put ? -1.0 : 1.0;
      if (out.status[i] == TickStatus::Exercised) {
        ++exercised;
        EXPECT_TRUE(bits_equal(out.price[i], intr)) << "t=" << t << " i=" << i;
        EXPECT_EQ(out.delta[i], unit) << "t=" << t << " i=" << i;
        EXPECT_EQ(out.gamma[i], 0.0);
        EXPECT_EQ(out.vega[i], 0.0);
        EXPECT_EQ(out.theta[i], 0.0);
      } else if (out.status[i] == TickStatus::Ok) {
        if (out.price[i] > intr) {
          ++above;
          continue;
        }
        ++worthless;
        EXPECT_EQ(out.price[i], 0.0) << "t=" << t << " i=" << i;
        EXPECT_EQ(intr, 0.0) << "an Ok lane at a positive intrinsic must be Exercised";
        EXPECT_EQ(out.delta[i], 0.0);
        EXPECT_EQ(out.gamma[i], 0.0);
        EXPECT_EQ(out.vega[i], 0.0);
        EXPECT_EQ(out.theta[i], 0.0);
      }
    }
  }
  std::printf("[ladder] served lanes: %zu above intrinsic, %zu exercised, %zu worthless\n", above,
              exercised, worthless);
  EXPECT_GT(exercised, 0u);
  EXPECT_GT(above, 0u);
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
