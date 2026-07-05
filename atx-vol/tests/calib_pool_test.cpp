#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/vol/black76.hpp"      // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"        // CalibOpts, calib_default_opts
#include "atx/vol/calib_pool.hpp"   // the unit under test
#include "atx/vol/curve.hpp"        // CurveSet, ForwardPoint
#include "atx/vol/profile.hpp"      // ProfileKind, profile_tier_priority
#include "atx/vol/types.hpp"        // Side, ErrorCode
#include "atx/vol/universe.hpp"     // Universe, Underlying, Chain, chain_index, Uid
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_backbone_w

// Calibration pool + refit-cadence coverage, ported from the C ats-vol tests.
//
//  - The CadenceQueue block mirrors test_universe_cadence.c: a uid pops at its
//    cadence; an ULTRA_LIQUID name does not starve under an ILLIQUID backlog;
//    pop_due is capacity-bounded; and profile_tier_priority orders ULTRA before
//    MEGA before ORDINARY before ILLIQUID (VOL_PRODUCT == ULTRA).
//  - The CalibratePool block builds a small multi-underlier Universe whose
//    chains are Black-76 prices at KNOWN eSSVI IVs, runs calibrate_pool, and
//    asserts each underlier's fitted surface recovers its generating IVs and
//    that the result is deterministic across runs / thread counts.

namespace {

using atx::vol::black76_price;
using atx::vol::black76_value_and_vega;
using atx::vol::CadenceQueue;
using atx::vol::calibrate_pool;
using atx::vol::calib_default_opts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveProvider;
using atx::vol::CurveSet;
using atx::vol::ErrorCode;
using atx::vol::essvi_backbone_w;
using atx::vol::EssviParams;
using atx::vol::ExpiryId;
using atx::vol::FitStatus;
using atx::vol::ForwardPoint;
using atx::vol::PoolResult;
using atx::vol::ProfileKind;
using atx::vol::profile_tier_priority;
using atx::vol::Side;
using atx::vol::Uid;
using atx::vol::Underlying;
using atx::vol::Universe;

// ─────────────────────────────────────────────────────────────────────────
//  CadenceQueue — mirrors test_universe_cadence.c
// ─────────────────────────────────────────────────────────────────────────

TEST(CadenceQueue, SingleUid_PopsOnceThenAgainAfterCadence) {
  CadenceQueue q(16u);
  const std::int64_t now0 = 1'000'000'000LL;
  q.push(1u, 0u, now0);

  std::array<std::int64_t, 16> cadence{};
  cadence[1] = 250LL * 1'000'000LL;  // 250 ms
  std::array<Uid, 8> out{};

  // Pop now: fires once.
  std::size_t n = q.pop_due(now0, out, cadence);
  ASSERT_EQ(n, 1u);
  EXPECT_EQ(out[0], 1u);

  // Same instant: empty (next due in 250 ms).
  n = q.pop_due(now0, out, cadence);
  EXPECT_EQ(n, 0u);

  // Advance by 250 ms: fires again.
  n = q.pop_due(now0 + cadence[1], out, cadence);
  EXPECT_EQ(n, 1u);
}

TEST(CadenceQueue, UltraLiquid_DoesNotStarveUnderIlliquidBacklog) {
  constexpr int kNIll = 1000;
  constexpr std::size_t kCap = static_cast<std::size_t>(kNIll) + 8u;

  CadenceQueue q(kCap);
  const std::int64_t one_ms = 1'000'000LL;
  const std::int64_t now0 = 1000LL * one_ms;
  const std::int64_t spy_cadence = 250LL * one_ms;
  const std::int64_t ill_cadence = 5000LL * one_ms;

  std::vector<std::int64_t> cadence(kCap, 0LL);
  cadence[1] = spy_cadence;  // uid 1 = SPY-like
  for (int i = 0; i < kNIll; ++i) {
    cadence[static_cast<std::size_t>(2 + i)] = ill_cadence;
  }

  // Register all due immediately: 1 ULTRA (tier 0) + 1000 ILLIQUID (tier 3).
  q.push(1u, 0u, now0);
  for (int i = 0; i < kNIll; ++i) {
    q.push(static_cast<Uid>(2 + i), 3u, now0);
  }

  // Simulate 1 s, polling every 5 ms.
  int spy_pops = 0;
  int ill_pops = 0;
  std::vector<Uid> out(kCap);
  for (int t = 0; t <= 1000; t += 5) {
    const std::int64_t now = now0 + static_cast<std::int64_t>(t) * one_ms;
    const std::size_t n = q.pop_due(now, out, cadence);
    for (std::size_t i = 0; i < n; ++i) {
      if (out[i] == 1u) {
        ++spy_pops;
      } else {
        ++ill_pops;
      }
    }
  }

  // SPY fires at t = 0, 250, 500, 750 (and 1000 on the boundary) => >= 4.
  // Each ILLIQUID name fires exactly once (its 5 s cadence never recurs in 1 s).
  EXPECT_GE(spy_pops, 4);
  EXPECT_EQ(ill_pops, kNIll);
}

TEST(CadenceQueue, PopDue_CapsPerCall_RemainderStaysQueued) {
  CadenceQueue q(100u);
  const std::int64_t now = 1'000'000'000LL;
  std::vector<std::int64_t> cadence(100u, 1'000'000'000LL);  // 1 s each

  for (int i = 0; i < 50; ++i) {
    q.push(static_cast<Uid>(i + 1), 0u, now);
  }

  std::array<Uid, 16> out{};
  std::size_t total = 0u;
  // 16 + 16 + 16 + 2 = 50 across four capacity-bounded calls.
  for (int call = 0; call < 4; ++call) {
    total += q.pop_due(now, out, cadence);
  }
  EXPECT_EQ(total, 50u);
  EXPECT_TRUE(q.empty() || q.peek().next_refit_ts_ns > now);
}

TEST(CadenceQueue, Peek_OrdersByDueThenTierThenUid) {
  CadenceQueue q;
  q.push(10u, 3u, 200);  // latest due
  q.push(11u, 3u, 100);  // earliest due, tier 3
  q.push(12u, 0u, 100);  // same due as 11 but lower tier => ahead of 11

  ASSERT_FALSE(q.empty());
  EXPECT_EQ(q.peek().uid, 12u);          // due 100, tier 0
  EXPECT_EQ(q.pop().uid, 12u);
  EXPECT_EQ(q.pop().uid, 11u);           // due 100, tier 3
  EXPECT_EQ(q.pop().uid, 10u);           // due 200
  EXPECT_TRUE(q.empty());
}

// ─────────────────────────────────────────────────────────────────────────
//  profile_tier_priority ordering (the shard key the pool driver uses)
// ─────────────────────────────────────────────────────────────────────────

TEST(ProfileTierPriority, OrdersUltraBeforeMegaBeforeOrdinaryBeforeIlliquid) {
  EXPECT_LT(profile_tier_priority(ProfileKind::IndexEtfUltraLiquid),
            profile_tier_priority(ProfileKind::MegaCapEvent));
  EXPECT_LT(profile_tier_priority(ProfileKind::MegaCapEvent),
            profile_tier_priority(ProfileKind::OrdinarySingleName));
  EXPECT_LT(profile_tier_priority(ProfileKind::OrdinarySingleName),
            profile_tier_priority(ProfileKind::IlliquidSmallCap));
  // VOL_PRODUCT maps to the ULTRA tier.
  EXPECT_EQ(profile_tier_priority(ProfileKind::VolProduct),
            profile_tier_priority(ProfileKind::IndexEtfUltraLiquid));
}

// ─────────────────────────────────────────────────────────────────────────
//  calibrate_pool — multi-underlier fan-out over synthetic eSSVI chains
// ─────────────────────────────────────────────────────────────────────────

constexpr double kF = 100.0;
constexpr double kPhi = 1.0;
constexpr double kRho = -0.25;

const std::vector<double>& shared_ts() {
  static const std::vector<double> ts{0.25, 0.50, 1.00};
  return ts;
}

EssviParams backbone(double theta, double phi, double rho, double T) {
  EssviParams s{};
  s.theta = theta;
  s.phi = phi;
  s.rho = rho;
  s.T = T;
  return s;
}

double slice_iv(double theta, double phi, double rho, double T, double k) {
  return std::sqrt(essvi_backbone_w(backbone(theta, phi, rho, T), k) / T);
}

// One underlier's generating truth (its per-expiry theta term structure).
struct Truth {
  Uid uid{atx::vol::kInvalidUid};
  std::vector<double> thetas;
};

// Build one underlier into `u` (chains = Black-76 prices at known eSSVI IVs),
// register its flat-forward CurveSet in `curves_map`, and return its truth.
Truth build_underlier(Universe& u, std::unordered_map<Uid, CurveSet>& curves_map,
                      std::string_view ticker, const std::vector<double>& thetas) {
  const Uid uid = u.intern_ticker(ticker).value();
  u.get_underlying(uid).value()->spot = kF;

  std::vector<double> strikes;
  for (double K = kF - 16.0; K <= kF + 16.0 + 1e-9; K += 2.0) {
    strikes.push_back(K);
  }

  const std::vector<double>& ts = shared_ts();
  for (std::size_t si = 0; si < ts.size(); ++si) {
    const auto expiry_ns = static_cast<std::int64_t>(ts[si] * 3.15e16);
    const ExpiryId eid = u.add_expiry(uid, expiry_ns).value();
    for (double K : strikes) {
      const auto sr = u.add_strike(uid, eid, K);
      (void)sr;  // fixtures never overflow the strike cap
    }
  }

  Underlying* under = u.get_underlying(uid).value();
  for (std::size_t si = 0; si < ts.size(); ++si) {
    const double T = ts[si];
    Chain& c = under->chains[si];
    c.T = T;
    const EssviParams tr = backbone(thetas[si], kPhi, kRho, T);
    for (std::size_t s = 0; s < strikes.size(); ++s) {
      const double K = strikes[s];
      const double k = std::log(K / kF);
      const double sig = std::sqrt(essvi_backbone_w(tr, k) / T);
      for (int side_i = 0; side_i < 2; ++side_i) {
        const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
        const std::size_t idx = chain_index(static_cast<std::uint16_t>(s), side);
        const double mid = black76_price(kF, K, T, sig, 1.0, side);
        const double vega = black76_value_and_vega(kF, K, T, sig, 1.0, side).vega;
        const double half = std::min(0.005 * vega, 0.25 * mid);
        c.mids[idx] = mid;
        c.bids[idx] = mid - half;
        c.asks[idx] = mid + half;
        c.bid_sizes[idx] = 1;
        c.ask_sizes[idx] = 1;
      }
    }
  }

  CurveSet cs;
  cs.spot = kF;
  std::vector<ForwardPoint> fps;
  for (const Chain& c : under->chains) {
    ForwardPoint fp{};
    fp.expiry_ns = c.expiry_ns;
    fp.T = c.T;
    fp.F = kF;
    fps.push_back(fp);
  }
  cs.forward.set(fps);
  curves_map.emplace(uid, std::move(cs));

  return Truth{uid, thetas};
}

CurveProvider make_provider(const std::unordered_map<Uid, CurveSet>& curves_map) {
  return [&curves_map](Uid uid) -> const CurveSet* {
    const auto it = curves_map.find(uid);
    return it == curves_map.end() ? nullptr : &it->second;
  };
}

// Deterministic signature: uid, status, and an iv sample grid per Ok entry.
std::vector<double> signature(const PoolResult& pr) {
  std::vector<double> sig;
  for (const auto& e : pr.entries) {
    sig.push_back(static_cast<double>(e.uid));
    sig.push_back(static_cast<double>(static_cast<std::uint8_t>(e.status)));
    if (e.status == FitStatus::Ok && e.surface.has_value()) {
      const auto& s = *e.surface;
      for (std::size_t si = 0; si < s.n_slices(); ++si) {
        for (int j = -5; j <= 5; ++j) {
          const double k = 0.02 * static_cast<double>(j);
          sig.push_back(s.iv_on_slice(static_cast<std::uint16_t>(si), k));
        }
      }
    }
  }
  return sig;
}

TEST(CalibratePool, RecoversEachUnderlierSurface_WithinTolerance) {
  Universe u;
  std::unordered_map<Uid, CurveSet> curves_map;
  std::vector<Truth> truths;
  truths.push_back(build_underlier(u, curves_map, "AAA", {0.020, 0.045, 0.100}));
  truths.push_back(build_underlier(u, curves_map, "BBB", {0.030, 0.060, 0.120}));
  truths.push_back(build_underlier(u, curves_map, "CCC", {0.015, 0.035, 0.080}));

  const auto res = calibrate_pool(u, make_provider(curves_map),
                                  calib_default_opts(), /*n_threads*/ 2u);
  ASSERT_TRUE(res.has_value());
  const PoolResult& pr = *res;

  EXPECT_EQ(pr.n_attempted, 3u);
  EXPECT_EQ(pr.n_fit_ok, 3u);
  EXPECT_EQ(pr.n_fit_failed, 0u);
  EXPECT_EQ(pr.n_skipped, 0u);
  ASSERT_EQ(pr.entries.size(), 3u);

  // Entries are sorted ascending by uid.
  for (std::size_t i = 1; i < pr.entries.size(); ++i) {
    EXPECT_LT(pr.entries[i - 1].uid, pr.entries[i].uid);
  }

  const std::vector<double>& ts = shared_ts();
  for (const Truth& tr : truths) {
    // Locate the entry for this uid.
    const atx::vol::PoolEntry* entry = nullptr;
    for (const auto& e : pr.entries) {
      if (e.uid == tr.uid) {
        entry = &e;
        break;
      }
    }
    ASSERT_NE(entry, nullptr);
    ASSERT_EQ(entry->status, FitStatus::Ok);
    ASSERT_TRUE(entry->surface.has_value());
    const auto& surface = *entry->surface;
    ASSERT_EQ(surface.n_slices(), ts.size());

    double max_dv = 0.0;
    for (std::size_t si = 0; si < surface.n_slices(); ++si) {
      for (int j = -5; j <= 5; ++j) {
        const double k = 0.02 * static_cast<double>(j);
        const double iv_true = slice_iv(tr.thetas[si], kPhi, kRho, ts[si], k);
        const double iv_fit = surface.iv_on_slice(static_cast<std::uint16_t>(si), k);
        ASSERT_TRUE(std::isfinite(iv_fit));
        max_dv = std::max(max_dv, std::fabs(iv_fit - iv_true));
      }
    }
    EXPECT_LT(max_dv, 5.0e-3) << "uid " << tr.uid;
  }
}

TEST(CalibratePool, ResultIsDeterministicAcrossThreadCounts) {
  Universe u;
  std::unordered_map<Uid, CurveSet> curves_map;
  build_underlier(u, curves_map, "AAA", {0.020, 0.045, 0.100});
  build_underlier(u, curves_map, "BBB", {0.030, 0.060, 0.120});
  build_underlier(u, curves_map, "CCC", {0.015, 0.035, 0.080});
  const CurveProvider provider = make_provider(curves_map);
  const auto opts = calib_default_opts();

  const auto r1 = calibrate_pool(u, provider, opts, /*n_threads*/ 1u);
  const auto r2 = calibrate_pool(u, provider, opts, /*n_threads*/ 4u);
  const auto r0 = calibrate_pool(u, provider, opts, /*n_threads*/ 0u);  // auto
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r0.has_value());

  const std::vector<double> s1 = signature(*r1);
  const std::vector<double> s2 = signature(*r2);
  const std::vector<double> s0 = signature(*r0);

  ASSERT_EQ(s1.size(), s2.size());
  ASSERT_EQ(s1.size(), s0.size());
  for (std::size_t i = 0; i < s1.size(); ++i) {
    EXPECT_EQ(s1[i], s2[i]) << "thread-count 1 vs 4 diverged at " << i;
    EXPECT_EQ(s1[i], s0[i]) << "thread-count 1 vs auto diverged at " << i;
  }
  EXPECT_EQ(r1->n_fit_ok, r2->n_fit_ok);
  EXPECT_EQ(r1->n_fit_ok, r0->n_fit_ok);
}

TEST(CalibratePool, SharedCurvesOverload_FitsEveryUnderlier) {
  Universe u;
  std::unordered_map<Uid, CurveSet> unused;
  build_underlier(u, unused, "AAA", {0.020, 0.045, 0.100});
  build_underlier(u, unused, "BBB", {0.030, 0.060, 0.120});

  // One flat shared curve set (F == spot at every expiry, df == 1).
  CurveSet shared;
  shared.spot = kF;
  std::vector<ForwardPoint> fps;
  for (double T : shared_ts()) {
    ForwardPoint fp{};
    fp.expiry_ns = static_cast<std::int64_t>(T * 3.15e16);
    fp.T = T;
    fp.F = kF;
    fps.push_back(fp);
  }
  shared.forward.set(fps);

  const auto res = calibrate_pool(u, shared, calib_default_opts(), /*n_threads*/ 3u);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->n_fit_ok, 2u);
  EXPECT_EQ(res->n_skipped, 0u);
}

TEST(CalibratePool, UnderlierWithNoCurves_IsSkipped) {
  Universe u;
  std::unordered_map<Uid, CurveSet> curves_map;
  const Truth fit_me = build_underlier(u, curves_map, "AAA", {0.020, 0.045, 0.100});

  // A second underlier with chains but NO curves entry => Skipped.
  const Truth no_curve = build_underlier(u, curves_map, "BBB", {0.030, 0.060, 0.120});
  curves_map.erase(no_curve.uid);

  const auto res = calibrate_pool(u, make_provider(curves_map),
                                  calib_default_opts(), /*n_threads*/ 2u);
  ASSERT_TRUE(res.has_value());
  const PoolResult& pr = *res;
  EXPECT_EQ(pr.n_attempted, 2u);
  EXPECT_EQ(pr.n_fit_ok, 1u);
  EXPECT_EQ(pr.n_skipped, 1u);

  for (const auto& e : pr.entries) {
    if (e.uid == fit_me.uid) {
      EXPECT_EQ(e.status, FitStatus::Ok);
    } else if (e.uid == no_curve.uid) {
      EXPECT_EQ(e.status, FitStatus::Skipped);
    }
  }
}

TEST(CalibratePool, NullProvider_ReturnsInvalidArgument) {
  Universe u;
  std::unordered_map<Uid, CurveSet> curves_map;
  build_underlier(u, curves_map, "AAA", {0.020, 0.045, 0.100});

  const CurveProvider empty_provider;  // default-constructed => empty
  const auto res = calibrate_pool(u, empty_provider, calib_default_opts(), 1u);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(CalibratePool, EmptyUniverse_ReturnsZeroedResult) {
  Universe u;
  std::unordered_map<Uid, CurveSet> curves_map;
  const auto res = calibrate_pool(u, make_provider(curves_map), calib_default_opts(), 4u);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->n_attempted, 0u);
  EXPECT_EQ(res->n_fit_ok, 0u);
  EXPECT_TRUE(res->entries.empty());
}

}  // namespace
