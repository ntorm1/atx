// pnl_attribution — additive P&L attribution on the canonical PricedSurface stack.
//
// The heart of this suite is the PURE-AXIS acceptance (req 4): base/shifted surface
// pairs whose IV MOVE is exactly (a) pure level dsigma(k)=c, (b) pure skew
// dsigma(k)=s*k, (c) pure curvature dsigma(k)=q*k^2, built HONESTLY in IV space from
// explicit (k, w) LinearVariance nodes so the pivot dsigma values are exactly
// symmetric/equal and the non-target quadratic coefficients (a0/a1/a2) are BITWISE
// zero. To make the surface reproduce the intended sigma bit-for-bit despite the
// w = sigma^2*T storage + sqrt(w/T) read-back:
//   * T = 1 and F = 1 (so w/T and K/F are exact),
//   * every sigma is a small dyadic (0.25, 0.3125, ...) whose square is an exact
//     dyadic and whose sqrt round-trips exactly, and
//   * the shifted surface is a set of FLAT PLATEAUS (two equal-w nodes bracketing
//     each pivot), so LinearVarianceCurve's interpolation returns the plateau's w
//     EXACTLY at the pivot (w_lo + a*(w_hi-w_lo) with w_hi==w_lo).
// The base surface is globally flat (all-equal w), so it returns sigma0 exactly at
// every k and every subtraction sigma_shift - sigma_base is exact.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/api/analytics/pnl_attribution.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/surface_parity.hpp"   // SliceContext
#include "atx/vol/api/fitting/vol_curve.hpp"        // CurveSurface, LinearVarianceCurve

using namespace atx::vol;

namespace {

constexpr double kS = 1.0;
constexpr double kR = 0.03;
constexpr double kQ = 0.03; // q_eff == r => pricing forward == spot == F
constexpr double kT = 1.0;
constexpr double kF = 1.0;
constexpr double kSigma0 = 0.25; // dyadic: sqrt(0.0625) == 0.25 exactly
constexpr std::int64_t kNow = 1700000000000000000LL;

// Pivot plateaus at |k| ~ 0.1 (the default k_ref); each pivot query lands strictly
// inside a two-node flat plateau so the surface returns the plateau w exactly.
const std::vector<double> kNodes = {-0.12, -0.08, -0.02, 0.02, 0.08, 0.12};

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] PricingContext make_pc(std::uint32_t uid, double S, double r, std::int64_t now) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = now;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

// A single-slice LinearVariance PricedSurface from explicit (k, w) nodes.
[[nodiscard]] PricedSurface make_lv(std::uint32_t uid, std::vector<double> wnodes, double S = kS,
                                    double r = kR, std::int64_t now = kNow) {
  CurveSurface cs;
  const double df = std::exp(-r * kT);
  cs.push(std::make_unique<LinearVarianceCurve>(kT, kF, df, kNodes, std::move(wnodes)));
  std::vector<SliceContext> ctx;
  ctx.push_back(SliceContext{kT, kF, 0.0, kQ, kNodes.size(), 0});
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pc(uid, S, r, now));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] double w_of(double sigma) noexcept { return sigma * sigma * kT; }

// Flat base surface: sigma0 at every k.
[[nodiscard]] PricedSurface base_surface(std::uint32_t uid, double S = kS, double r = kR,
                                         std::int64_t now = kNow) {
  return make_lv(uid, std::vector<double>(kNodes.size(), w_of(kSigma0)), S, r, now);
}

// Shifted surface with per-plateau sigma (minus plateau, zero plateau, plus plateau).
[[nodiscard]] PricedSurface shifted_surface(std::uint32_t uid, double sig_minus, double sig_zero,
                                            double sig_plus, double S = kS, double r = kR,
                                            std::int64_t now = kNow) {
  const double wm = w_of(sig_minus);
  const double wz = w_of(sig_zero);
  const double wp = w_of(sig_plus);
  return make_lv(uid, std::vector<double>{wm, wm, wz, wz, wp, wp}, S, r, now);
}

[[nodiscard]] SurfaceSet set_of(const PricedSurface &s) {
  const PricedSurface *p = &s;
  auto ss = SurfaceSet::create(std::span<const PricedSurface *const>(&p, 1));
  EXPECT_TRUE(ss.has_value());
  return std::move(*ss);
}

// Three contracts at the pivot strikes K = exp(-k_ref), 1, exp(+k_ref) (F = 1), so
// each contract's dvol is exactly the plateau move at its own pivot. qty=mult=1 so
// the position weight w == 1 (keeps any FP dust at its per-share magnitude).
[[nodiscard]] std::vector<Position> pivot_book() {
  constexpr double kref = 0.10;
  std::vector<Position> b;
  std::uint64_t id = 0;
  b.push_back({id++, {1, std::exp(-kref), kT, Side::Put}, 1.0, 1.0});
  b.push_back({id++, {1, 1.0, kT, Side::Call}, 1.0, 1.0});
  b.push_back({id++, {1, std::exp(kref), kT, Side::Call}, 1.0, 1.0});
  return b;
}

// The frame's per-position vega P&L, for cross-checking the split against pnl_explain.
[[nodiscard]] std::vector<double> frame_pnl_vega(const std::vector<Position> &book,
                                                 const SurfaceSet &base, const SurfaceSet &shifted) {
  auto pf = Portfolio::create(book);
  EXPECT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto fr = pricer.pnl_explain(base, shifted);
  EXPECT_TRUE(fr.has_value());
  return fr->pnl_vega;
}

} // namespace

// ── req 4a: pure LEVEL move -> only vol_atf lights up (non-targets EXACT zero). ──
TEST(PnlAttribution, PureLevelMoveHitsOnlyAtf) {
  const double c = 0.03125; // dyadic level shift: sigma0 + c = 0.28125
  const PricedSurface base = base_surface(1);
  const PricedSurface shifted = shifted_surface(1, kSigma0 + c, kSigma0 + c, kSigma0 + c);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);
  const std::vector<Position> book = pivot_book();
  const std::vector<double> pnl_vega = frame_pnl_vega(book, bset, sset);

  auto ar = pnl_attribution(book, bset, sset);
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  const AttributionFrame &f = *ar;
  ASSERT_EQ(f.size(), book.size());

  int active = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    const AttributionRow &r = f.rows[i];
    ASSERT_EQ(r.status, PriceStatus::Ok) << i;
    EXPECT_EQ(r.vol_skew, 0.0) << i << " vol_skew=" << r.vol_skew; // -0.0 == 0.0 (exact zero)
    EXPECT_EQ(r.vol_curv, 0.0) << i << " vol_curv=" << r.vol_curv; // -0.0 == 0.0 (exact zero)
    EXPECT_EQ(r.vol_resid, 0.0) << i << " vol_resid=" << r.vol_resid; // -0.0 == 0.0 (exact zero)
    // A pure level move puts the WHOLE vega P&L into vol_atf, bit-for-bit.
    EXPECT_TRUE(bits_equal(r.vol_atf, pnl_vega[i]))
        << i << " vol_atf=" << r.vol_atf << " pnl_vega=" << pnl_vega[i];
    if (pnl_vega[i] != 0.0) {
      ++active;
    }
  }
  EXPECT_GT(active, 0) << "pure-level test is vacuous (no vega P&L)";
}

// ── req 4b: pure SKEW move -> only vol_skew (vol_atf/vol_curv EXACT zero). ───────
TEST(PnlAttribution, PureSkewMoveHitsOnlySkew) {
  const double delta = 0.0625; // wings: sigma0 +/- delta = 0.3125 / 0.1875 (dyadic)
  const PricedSurface base = base_surface(1);
  const PricedSurface shifted = shifted_surface(1, kSigma0 - delta, kSigma0, kSigma0 + delta);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);
  const std::vector<Position> book = pivot_book();
  const std::vector<double> pnl_vega = frame_pnl_vega(book, bset, sset);

  auto ar = pnl_attribution(book, bset, sset);
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  const AttributionFrame &f = *ar;

  int active_skew = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    const AttributionRow &r = f.rows[i];
    ASSERT_EQ(r.status, PriceStatus::Ok) << i;
    // a0 == 0 (dsigma(0)==0) and a2 == 0 (dsigma(+)+dsigma(-)==0) are BITWISE zero by
    // the dyadic antisymmetric construction, so vol_atf and vol_curv are exact zero.
    EXPECT_EQ(r.vol_atf, 0.0) << i << " vol_atf=" << r.vol_atf; // -0.0 == 0.0 (exact zero)
    EXPECT_EQ(r.vol_curv, 0.0) << i << " vol_curv=" << r.vol_curv; // -0.0 == 0.0 (exact zero)
    // vol_resid is the higher-order remainder; a pure (pivot-quadratic) move leaves it
    // at FP dust (the log(exp(k)) pivot round-trip), pinned <= 1e-15.
    EXPECT_LE(std::fabs(r.vol_resid), 1e-15) << i << " vol_resid=" << r.vol_resid;
    // Partition identity: the four pieces reconstruct pnl_vega bit-for-bit.
    EXPECT_TRUE(bits_equal(r.vol_resid, pnl_vega[i] - r.vol_atf - r.vol_skew - r.vol_curv)) << i;
    if (r.vol_skew != 0.0) {
      ++active_skew;
    }
  }
  EXPECT_GT(active_skew, 0) << "pure-skew test is vacuous (no skew P&L)";
}

// ── req 4c: pure CURVATURE move -> only vol_curv (vol_atf/vol_skew EXACT zero). ──
TEST(PnlAttribution, PureCurvatureMoveHitsOnlyCurvature) {
  const double gamma = 0.0625; // both wings: sigma0 + gamma = 0.3125 (symmetric)
  const PricedSurface base = base_surface(1);
  const PricedSurface shifted = shifted_surface(1, kSigma0 + gamma, kSigma0, kSigma0 + gamma);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);
  const std::vector<Position> book = pivot_book();
  const std::vector<double> pnl_vega = frame_pnl_vega(book, bset, sset);

  auto ar = pnl_attribution(book, bset, sset);
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  const AttributionFrame &f = *ar;

  int active_curv = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    const AttributionRow &r = f.rows[i];
    ASSERT_EQ(r.status, PriceStatus::Ok) << i;
    // a0 == 0 (dsigma(0)==0) and a1 == 0 (dsigma(+)==dsigma(-), equal plateau w) are
    // BITWISE zero, so vol_atf and vol_skew are exact zero.
    EXPECT_EQ(r.vol_atf, 0.0) << i << " vol_atf=" << r.vol_atf; // -0.0 == 0.0 (exact zero)
    EXPECT_EQ(r.vol_skew, 0.0) << i << " vol_skew=" << r.vol_skew; // -0.0 == 0.0 (exact zero)
    EXPECT_LE(std::fabs(r.vol_resid), 1e-15) << i << " vol_resid=" << r.vol_resid;
    EXPECT_TRUE(bits_equal(r.vol_resid, pnl_vega[i] - r.vol_atf - r.vol_skew - r.vol_curv)) << i;
    if (r.vol_curv != 0.0) {
      ++active_curv;
    }
  }
  EXPECT_GT(active_curv, 0) << "pure-curvature test is vacuous (no curvature P&L)";
}

// ── A mixed move (spot + skew-vol + rate + time all shocked at once). ────────────
namespace {
struct MixedFixture {
  PricedSurface base;
  PricedSurface shifted;
};
[[nodiscard]] MixedFixture make_mixed() {
  const double delta = 0.0625;
  const double dS = 0.01;
  const double dr = 1.0e-4;
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  return MixedFixture{base_surface(1),
                      shifted_surface(1, kSigma0 - delta, kSigma0, kSigma0 + delta, kS + dS,
                                      kR + dr, kNow + one_hour)};
}
} // namespace

// ── req 5: axis sum == pnl_total (mixed move), per row and in totals. ────────────
TEST(PnlAttribution, AxisSumEqualsPnlTotal) {
  const MixedFixture mf = make_mixed();
  const SurfaceSet bset = set_of(mf.base);
  const SurfaceSet sset = set_of(mf.shifted);
  const std::vector<Position> book = pivot_book();

  auto ar = pnl_attribution(book, bset, sset);
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  const AttributionFrame &f = *ar;

  for (std::size_t i = 0; i < f.size(); ++i) {
    const AttributionRow &r = f.rows[i];
    ASSERT_EQ(r.status, PriceStatus::Ok) << i;
    const double sum = r.spot + r.vol_atf + r.vol_skew + r.vol_curv + r.vol_resid + r.vol_second +
                       r.rates + r.time + r.unexplained;
    // Differs from pnl_total only by IEEE reassociation between the regrouped axes and
    // PnlFrame's own `explained` accumulation order (same class as the pnl_explain
    // column tests, which pin 1e-6). Measured here at the ~1e-16 level.
    // Measured dust: bit-exact for most rows, <= 1.24e-16 relative for the worst
    // (IEEE reassociation between the regrouped axes and PnlFrame's `explained`).
    EXPECT_NEAR(sum, r.pnl_total, 1e-12 * (std::fabs(r.pnl_total) + 1.0)) << i;
  }

  const AttributionTotals &t = f.total;
  const double tsum = t.spot + t.vol_atf + t.vol_skew + t.vol_curv + t.vol_resid + t.vol_second +
                      t.rates + t.time + t.unexplained;
  EXPECT_NEAR(tsum, t.pnl_total, 1e-12 * (std::fabs(t.pnl_total) + 1.0));
  EXPECT_EQ(t.n_ok, book.size());
}

// ── The vega partition reconstructs pnl_vega bit-for-bit (mixed move). ───────────
TEST(PnlAttribution, VegaPartitionExact) {
  const MixedFixture mf = make_mixed();
  const SurfaceSet bset = set_of(mf.base);
  const SurfaceSet sset = set_of(mf.shifted);
  const std::vector<Position> book = pivot_book();
  const std::vector<double> pnl_vega = frame_pnl_vega(book, bset, sset);

  auto ar = pnl_attribution(book, bset, sset);
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  const AttributionFrame &f = *ar;

  for (std::size_t i = 0; i < f.size(); ++i) {
    const AttributionRow &r = f.rows[i];
    ASSERT_EQ(r.status, PriceStatus::Ok) << i;
    // vol_resid is DEFINED as pnl_vega - atf - skew - curv, so the four are an exact
    // partition of the frame's pnl_vega column, bit-for-bit.
    EXPECT_TRUE(bits_equal(r.vol_resid, pnl_vega[i] - r.vol_atf - r.vol_skew - r.vol_curv))
        << i << " resid=" << r.vol_resid;
  }
}

// ── spot / vol_second / rates / time / unexplained == PnlFrame columns exactly. ──
TEST(PnlAttribution, MatchesPnlExplainColumns) {
  const MixedFixture mf = make_mixed();
  const SurfaceSet bset = set_of(mf.base);
  const SurfaceSet sset = set_of(mf.shifted);
  const std::vector<Position> book = pivot_book();

  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto fr = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(fr.has_value());
  const PnlFrame &frame = *fr;

  auto ar = pnl_attribution(book, bset, sset);
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  const AttributionFrame &f = *ar;

  for (std::size_t i = 0; i < f.size(); ++i) {
    const AttributionRow &r = f.rows[i];
    ASSERT_EQ(r.status, PriceStatus::Ok) << i;
    EXPECT_TRUE(bits_equal(r.pnl_total, frame.pnl_total[i])) << i;
    EXPECT_TRUE(bits_equal(r.spot, frame.pnl_delta[i] + frame.pnl_gamma[i])) << i;
    EXPECT_TRUE(bits_equal(r.vol_second, frame.pnl_volga[i] + frame.pnl_vanna[i])) << i;
    EXPECT_TRUE(bits_equal(r.rates, frame.pnl_rho[i])) << i;
    EXPECT_TRUE(bits_equal(r.time, frame.pnl_theta[i] + frame.pnl_charm[i])) << i;
    EXPECT_TRUE(bits_equal(r.unexplained, frame.pnl_unexplained[i])) << i;
  }
}

// ── req 2: NaN pivot (domain edge) -> whole vega P&L to vol_resid + counted flag. ─
TEST(PnlAttribution, PivotDomainEdgeFallsBackToResid) {
  // k_ref huge => the pivot strikes F*exp(+/-k_ref) overflow/underflow to inf/0, so
  // every pivot IV is NaN: the group is edge, a0=a1=a2=0, and each contract's vega
  // P&L falls entirely to vol_resid. The contracts themselves still price normally
  // (k_ref only drives MY pivot sampling, never pnl_explain).
  const double delta = 0.0625;
  const PricedSurface base = base_surface(1);
  const PricedSurface shifted = shifted_surface(1, kSigma0 - delta, kSigma0, kSigma0 + delta);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);
  const std::vector<Position> book = pivot_book();
  const std::vector<double> pnl_vega = frame_pnl_vega(book, bset, sset);

  AttributionOptions opts;
  opts.k_ref = 1000.0;
  auto ar = pnl_attribution(book, bset, sset, opts);
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  const AttributionFrame &f = *ar;

  EXPECT_EQ(f.n_pivot_edge_fallback, 1u); // one (uid, T) group, edge
  for (std::size_t i = 0; i < f.size(); ++i) {
    const AttributionRow &r = f.rows[i];
    ASSERT_EQ(r.status, PriceStatus::Ok) << i;
    EXPECT_EQ(r.vol_atf, 0.0) << i;
    EXPECT_EQ(r.vol_skew, 0.0) << i;
    EXPECT_EQ(r.vol_curv, 0.0) << i;
    EXPECT_TRUE(bits_equal(r.vol_resid, pnl_vega[i])) << i; // everything to resid
  }
}

// ── req 6: bit-identical across worker counts. ───────────────────────────────────
TEST(PnlAttribution, BitIdenticalAcrossThreads) {
  const MixedFixture mf = make_mixed();
  const SurfaceSet bset = set_of(mf.base);
  const SurfaceSet sset = set_of(mf.shifted);
  const std::vector<Position> book = pivot_book();

  AttributionOptions o1;
  o1.n_threads = 1;
  AttributionOptions o8;
  o8.n_threads = 8;
  auto a1 = pnl_attribution(book, bset, sset, o1);
  auto a8 = pnl_attribution(book, bset, sset, o8);
  ASSERT_TRUE(a1.has_value());
  ASSERT_TRUE(a8.has_value());
  ASSERT_EQ(a1->size(), a8->size());

  for (std::size_t i = 0; i < a1->size(); ++i) {
    const AttributionRow &x = a1->rows[i];
    const AttributionRow &y = a8->rows[i];
    EXPECT_TRUE(bits_equal(x.pnl_total, y.pnl_total)) << i;
    EXPECT_TRUE(bits_equal(x.spot, y.spot)) << i;
    EXPECT_TRUE(bits_equal(x.vol_atf, y.vol_atf)) << i;
    EXPECT_TRUE(bits_equal(x.vol_skew, y.vol_skew)) << i;
    EXPECT_TRUE(bits_equal(x.vol_curv, y.vol_curv)) << i;
    EXPECT_TRUE(bits_equal(x.vol_resid, y.vol_resid)) << i;
    EXPECT_TRUE(bits_equal(x.vol_second, y.vol_second)) << i;
    EXPECT_TRUE(bits_equal(x.rates, y.rates)) << i;
    EXPECT_TRUE(bits_equal(x.time, y.time)) << i;
    EXPECT_TRUE(bits_equal(x.unexplained, y.unexplained)) << i;
  }
  EXPECT_TRUE(bits_equal(a1->total.pnl_total, a8->total.pnl_total));
  EXPECT_TRUE(bits_equal(a1->total.vol_atf, a8->total.vol_atf));
  EXPECT_TRUE(bits_equal(a1->total.vol_skew, a8->total.vol_skew));
  EXPECT_TRUE(bits_equal(a1->total.vol_curv, a8->total.vol_curv));
  EXPECT_EQ(a1->total.n_ok, a8->total.n_ok);
}

// ── Empty book -> empty frame, zero totals, Ok. ──────────────────────────────────
TEST(PnlAttribution, EmptyBookIsEmptyFrame) {
  const PricedSurface base = base_surface(1);
  const PricedSurface shifted = shifted_surface(1, kSigma0, kSigma0, kSigma0);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);
  auto ar = pnl_attribution({}, bset, sset);
  ASSERT_TRUE(ar.has_value());
  EXPECT_EQ(ar->size(), 0u);
  EXPECT_EQ(ar->total.n_ok, 0u);
  EXPECT_EQ(ar->n_pivot_edge_fallback, 0u);
}

// ── Invalid k_ref -> InvalidArgument. ────────────────────────────────────────────
TEST(PnlAttribution, InvalidKrefRejected) {
  const PricedSurface base = base_surface(1);
  const PricedSurface shifted = shifted_surface(1, kSigma0, kSigma0, kSigma0);
  const SurfaceSet bset = set_of(base);
  const SurfaceSet sset = set_of(shifted);
  AttributionOptions opts;
  opts.k_ref = 0.0;
  auto ar = pnl_attribution(pivot_book(), bset, sset, opts);
  EXPECT_FALSE(ar.has_value());
}
