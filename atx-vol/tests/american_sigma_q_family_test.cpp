#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"

// L2h-b sigma-q family (2026-10-21). AlSigmaQFamily lays Chebyshev-Lobatto nodes over
// (σ, q) so ONE family per (expiry, side) prices every strike at its OWN effective
// yield. Its contract is a MEASURED distance to the accurate cold Reference at the
// strike's own carry (PM ruling (a)), pinned below with 2-figure round-ups of the
// worst case measured per n_q; plus two exact properties: a collapsed q box IS
// AlSigmaFamily at that q, bit for bit, and N threads on one const family reproduce
// the serial loop bit for bit. Numbers: sprints/2026-10-21-L2hb-sigma-q-family-sprint.md.
// The price path's zero-allocation property is counted in its own binary
// (american_sigma_q_family_alloc_test.cpp: it replaces global operator new).

namespace {

using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;
using atx::vol::AlExpiryGeometry;
using atx::vol::AlSigmaFamily;
using atx::vol::AlSigmaFamilyAxis;
using atx::vol::AlSigmaFamilyChord;
using atx::vol::AlSigmaFamilyPrice;
using atx::vol::AlSigmaFamilyStatus;
using atx::vol::AlSigmaQFamily;
using atx::vol::AlSigmaQFamilyChord;
using atx::vol::AlSigmaQFamilyPrice;
using atx::vol::AlSigmaQFamilyStatus;
using atx::vol::andersen_lake;
using atx::vol::kAlPackSigmaBump;
using atx::vol::kAlSigmaFamilyMinSigma;
using atx::vol::kAlSigmaFamilyMinTau;
using atx::vol::kAlSigmaQFamilyDegenerateWidth;
using atx::vol::kAlSigmaQFamilyMaxQNodes;
using atx::vol::kAlSigmaQFamilyMinQNodes;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kSpot = 100.0;
constexpr std::uint16_t kSigmaNodes = 12; // the σ-family's default count (L2h note §8)

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] std::string describe(double S, double K, double T, double sigma, double r,
                                   double q, Side side) {
  std::ostringstream os;
  os << std::setprecision(17) << (side == Side::Put ? "put" : "call") << " S=" << S
     << " K=" << K << " T=" << T << " sigma=" << sigma << " r=" << r << " q=" << q;
  return os.str();
}

// n points log-uniform strictly inside [lo, hi] (cell midpoints): off the σ nodes.
[[nodiscard]] std::vector<double> log_midpoints(double lo, double hi, unsigned n) {
  std::vector<double> v;
  const double a = std::log(lo);
  const double b = std::log(hi);
  for (unsigned i = 0; i < n; ++i) {
    v.push_back(std::exp(a + (b - a) * (static_cast<double>(i) + 0.5) / static_cast<double>(n)));
  }
  return v;
}

// The family's own Chebyshev-Lobatto points of [lo, hi] (linear map, the TU's form).
[[nodiscard]] std::vector<double> lobatto(double lo, double hi, unsigned n) {
  std::vector<double> v;
  for (unsigned s = 0; s < n; ++s) {
    double z = -std::cos(3.14159265358979323846 * static_cast<double>(s) /
                         static_cast<double>(n - 1));
    if (s == 0) {
      z = -1.0;
    }
    if (s == n - 1) {
      z = 1.0;
    }
    v.push_back(lo + 0.5 * (hi - lo) * (z + 1.0));
  }
  return v;
}

// One (expiry, side) family: the strikes carry their own q inside [q_lo, q_hi] (a
// golden-ratio spread that includes both box ends), as a funding-pv chain does.
struct QCase {
  double S;
  double T;
  double r;
  double q_lo;
  double q_hi;
  Side side;
  double sigma_lo;
  double sigma_hi;
  std::vector<double> strikes;
  std::vector<double> sigmas;
};

[[nodiscard]] double strike_q(const QCase &c, std::size_t i) noexcept {
  if (i + 1 == c.strikes.size()) {
    return c.q_hi;
  }
  const double f = std::fmod(static_cast<double>(i) * 0.6180339887498949, 1.0);
  return c.q_lo + (c.q_hi - c.q_lo) * f;
}

struct QWorst {
  double rel = -1.0;    // max |dP| / P over P >= 0.01 (the brief's max rel)
  double scaled = -1.0; // max |dP| / max(P, 1) (the σ-family tests' measure)
  std::size_t priced = 0;
  std::size_t refused = 0;
  std::string at;
};

[[nodiscard]] QWorst qcase_worst(const QCase &c, std::uint16_t n_q) {
  QWorst w;
  const double q_mid = 0.5 * (c.q_lo + c.q_hi);
  const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, q_mid);
  const double anchor = c.S; // put families anchor at the spot too (any K is a rescale)
  AlSigmaQFamily fam(g, c.side, anchor);
  const Status st = fam.prepare(c.sigma_lo, c.sigma_hi, kSigmaNodes, c.q_lo, c.q_hi, n_q);
  if (!st) {
    w.at = "prepare refused: " + st.error().to_string();
    return w;
  }
  for (std::size_t i = 0; i < c.strikes.size(); ++i) {
    const double K = c.strikes[i];
    const double q = strike_q(c, i);
    for (const double sigma : c.sigmas) {
      const Result<double> cold = andersen_lake(c.S, K, c.T, sigma, c.r, q, c.side);
      const AlSigmaQFamilyPrice p = fam.price(c.S, K, sigma, q);
      if (!cold || p.status != AlSigmaQFamilyStatus::Ok) {
        ++w.refused;
        continue;
      }
      ++w.priced;
      const double d = std::abs(p.price - *cold);
      w.scaled = std::max(w.scaled, d / std::max(*cold, 1.0));
      if (*cold >= 0.01 && d / *cold > w.rel) {
        w.rel = d / *cold;
        std::ostringstream os;
        os << std::setprecision(17) << describe(c.S, K, c.T, sigma, c.r, q, c.side)
           << " cold=" << *cold << " family=" << p.price;
        w.at = os.str();
      }
    }
  }
  return w;
}

[[nodiscard]] QWorst qcases_worst(const std::vector<QCase> &cases, std::uint16_t n_q) {
  QWorst w;
  for (const QCase &c : cases) {
    const QWorst cw = qcase_worst(c, n_q);
    w.priced += cw.priced;
    w.refused += cw.refused;
    w.scaled = std::max(w.scaled, cw.scaled);
    if (cw.rel > w.rel) {
      w.rel = cw.rel;
      w.at = cw.at;
    }
  }
  return w;
}

[[nodiscard]] std::vector<double> strikes_around(double S, double lo, double hi, unsigned n) {
  std::vector<double> v;
  for (unsigned i = 0; i < n; ++i) {
    v.push_back(S * (lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(n - 1)));
  }
  return v;
}

// SPY 2026-11-06 (23 d) under funding-pv: S 772.99, r 4.496 %, the chain's measured q
// range [0.001589, 0.001678] (62 distinct q over 322 two-sided rows), both sides.
[[nodiscard]] std::vector<QCase> spy23_cases() {
  std::vector<QCase> v;
  for (const Side side : {Side::Put, Side::Call}) {
    v.push_back(QCase{772.99, 0.0682, 0.04496, 0.001589, 0.001678, side, 0.10, 0.50,
                      strikes_around(772.99, 0.70, 1.30, 25), log_midpoints(0.10, 0.50, 12)});
  }
  return v;
}

// SPY's longest wide-q expiry, 2028-12-15 (2.17 y): r 5.153 %, q in [-0.003298,
// -0.002272] (74 distinct q). Puts only: every call there has q < 0 and never
// exercises early (prepare() refuses it, tested below).
[[nodiscard]] std::vector<QCase> spy_long_cases() {
  return {QCase{772.98, 2.1673, 0.05153, -0.003298, -0.002272, Side::Put, 0.10, 0.45,
                strikes_around(772.98, 0.50, 1.50, 25), log_midpoints(0.10, 0.45, 12)}};
}

// A deliberately wide carry box (4 % of yield inside one family; no straddle of r).
[[nodiscard]] std::vector<QCase> wide_carry_cases() {
  return {QCase{kSpot, 1.0, 0.05, 0.0, 0.04, Side::Put, 0.10, 0.60,
                strikes_around(kSpot, 0.60, 1.40, 17), log_midpoints(0.10, 0.60, 10)},
          QCase{kSpot, 1.0, 0.05, 0.005, 0.045, Side::Call, 0.10, 0.60,
                strikes_around(kSpot, 0.60, 1.40, 17), log_midpoints(0.10, 0.60, 10)}};
}

TEST(AlSigmaQFamily, CollapsedQBoxIsBitIdenticalToTheSigmaFamily) {
  // q_hi - q_lo <= kAlSigmaQFamilyDegenerateWidth: one q node, and every price / chord
  // equals AlSigmaFamily's at that q to the last bit (both axes, both sides, at the
  // σ nodes, between them and at the box edges; strikes off the anchor).
  std::size_t checked = 0;
  std::size_t mismatched = 0;
  std::string first;
  for (const auto &[T, r, q] : {std::tuple{0.0682, 0.04496, 0.0016}, std::tuple{0.5, 0.05, 0.02},
                                std::tuple{2.0, 0.05, -0.003}}) {
    const AlExpiryGeometry g = prepare_al_geometry(T, r, q);
    for (const Side side : {Side::Put, Side::Call}) {
      if (side == Side::Call && q <= 0.0) {
        continue; // a q <= 0 call has no boundary (no σ-family either)
      }
      for (const AlSigmaFamilyAxis axis : {AlSigmaFamilyAxis::LogSigma, AlSigmaFamilyAxis::Sigma}) {
        AlSigmaFamily sf(g, side, kSpot);
        AlSigmaQFamily qf(g, side, kSpot);
        ASSERT_TRUE(sf.prepare(0.1, 0.6, kSigmaNodes, axis));
        ASSERT_TRUE(qf.prepare(0.1, 0.6, kSigmaNodes, q, q, /*n_q=*/4, axis));
        EXPECT_EQ(qf.n_q(), 1);
        std::vector<double> sigmas = lobatto(0.1, 0.6, kSigmaNodes);
        for (const double s : log_midpoints(0.1, 0.6, 9)) {
          sigmas.push_back(s);
        }
        for (const double sigma : sigmas) {
          for (const double other : {60.0, 85.0, 100.0, 112.5, 150.0}) {
            const double S = (side == Side::Put) ? kSpot : other;
            const double K = (side == Side::Put) ? other : kSpot;
            const AlSigmaFamilyPrice a = sf.price(S, K, sigma);
            const AlSigmaQFamilyPrice b = qf.price(S, K, sigma, q);
            const double hs = std::min(sigma, 0.6 - 2.0 * kAlPackSigmaBump);
            const AlSigmaFamilyChord ca = sf.price_and_chord(S, K, hs);
            const AlSigmaQFamilyChord cb = qf.price_and_chord(S, K, hs, q);
            ++checked;
            const bool ok = a.status == AlSigmaFamilyStatus::Ok &&
                            b.status == AlSigmaQFamilyStatus::Ok && same_bits(a.price, b.price) &&
                            ca.status == AlSigmaFamilyStatus::Ok &&
                            cb.status == AlSigmaQFamilyStatus::Ok &&
                            same_bits(ca.price, cb.price) &&
                            same_bits(ca.price_sigma_bumped, cb.price_sigma_bumped);
            if (!ok) {
              if (mismatched < 5u) {
                std::ostringstream os;
                os << std::setprecision(17) << describe(S, K, T, sigma, r, q, side)
                   << " sigma_family=" << a.price << " sigma_q_family=" << b.price << "\n";
                first += os.str();
              }
              ++mismatched;
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(mismatched, 0u) << "of " << checked << "\n" << first;
  EXPECT_EQ(checked, 5u * 2u * 21u * 5u); // 5 (geometry, side) pairs x 2 axes x 21 sigma x 5
  // A box narrower than the threshold collapses too; one wider than it does not.
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaQFamily narrow(g, Side::Put, kSpot);
  ASSERT_TRUE(narrow.prepare(0.1, 0.6, kSigmaNodes, 0.02, 0.02 + 0.5 * kAlSigmaQFamilyDegenerateWidth,
                             4));
  EXPECT_EQ(narrow.n_q(), 1);
  AlSigmaQFamily wide(g, Side::Put, kSpot);
  ASSERT_TRUE(wide.prepare(0.1, 0.6, kSigmaNodes, 0.02, 0.02 + 1.0e-6, 4));
  EXPECT_EQ(wide.n_q(), 4);
}

TEST(AlSigmaQFamily, PrepareRefusesBadInputsAndLeavesTheFamilyUnprepared) {
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  const auto code = [](const Status &st) -> std::optional<ErrorCode> {
    if (st) {
      return std::nullopt; // prepared
    }
    return st.error().code();
  };
  {
    const AlExpiryGeometry unprepared;
    AlSigmaQFamily f(unprepared, Side::Put, kSpot);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4)), ErrorCode::InvalidArgument);
    EXPECT_FALSE(f.prepared());
    EXPECT_EQ(f.price(kSpot, kSpot, 0.3, 0.02).status, AlSigmaQFamilyStatus::NotPrepared);
  }
  {
    AlSigmaQFamily f(g, Side::Put, kSpot);
    // σ box and n_sigma: AlSigmaFamily's rules.
    EXPECT_EQ(code(f.prepare(0.5, 0.1, kSigmaNodes, 0.01, 0.03, 4)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code(f.prepare(0.5 * kAlSigmaFamilyMinSigma, 0.5, kSigmaNodes, 0.01, 0.03, 4)),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, 3, 0.01, 0.03, 4)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, 17, 0.01, 0.03, 4)), ErrorCode::InvalidArgument);
    // q box: non-finite, inverted; n_q outside [2, 8] on a real box.
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, kNaN, 0.03, 4)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.03, 0.01, 4)), ErrorCode::InvalidArgument);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03,
                             static_cast<std::uint16_t>(kAlSigmaQFamilyMinQNodes - 1))),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03,
                             static_cast<std::uint16_t>(kAlSigmaQFamilyMaxQNodes + 1))),
              ErrorCode::InvalidArgument);
    // The box straddles q = r: the scale K min(1, r/q) has a kink inside it.
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.04, 0.06, 4)), ErrorCode::NotImplemented);
    EXPECT_FALSE(f.prepared());
    // Touching r from one side is a smooth box.
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.03, 0.05, 4)), std::nullopt);
  }
  {
    AlSigmaQFamily f(g, Side::Call, 0.0); // anchor must be > 0
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4)), ErrorCode::InvalidArgument);
  }
  {
    const AlExpiryGeometry near = prepare_al_geometry(0.5 * kAlSigmaFamilyMinTau, 0.05, 0.02);
    AlSigmaQFamily f(near, Side::Put, kSpot);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4)), ErrorCode::InvalidArgument);
  }
  {
    // Calls need q > 0 over the whole box: a box reaching q <= 0 is refused, and so is
    // the SPY 2.17 y expiry's (all-negative q) call side.
    AlSigmaQFamily f(g, Side::Call, kSpot);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, 0.0, 0.03, 4)), ErrorCode::NotImplemented);
    EXPECT_EQ(code(f.prepare(0.1, 0.5, kSigmaNodes, -0.01, 0.03, 4)), ErrorCode::NotImplemented);
    const AlExpiryGeometry long_spy = prepare_al_geometry(2.1673, 0.05153, 0.001);
    AlSigmaQFamily lc(long_spy, Side::Call, 772.98);
    EXPECT_EQ(code(lc.prepare(0.1, 0.45, kSigmaNodes, -0.003298, -0.002272, 4)),
              ErrorCode::NotImplemented);
    // A geometry whose own q leaves the call side European has no orientation to serve.
    const AlExpiryGeometry euro_call = prepare_al_geometry(0.5, 0.05, -0.01);
    AlSigmaQFamily ec(euro_call, Side::Call, kSpot);
    EXPECT_EQ(code(ec.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4)), ErrorCode::NotImplemented);
    // The European put (r = 0, q > 0) has no boundary.
    const AlExpiryGeometry euro_put = prepare_al_geometry(0.5, 0.0, 0.03);
    AlSigmaQFamily ep(euro_put, Side::Put, kSpot);
    EXPECT_EQ(code(ep.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4)), ErrorCode::NotImplemented);
  }
  {
    // A refusal after a success discards the earlier preparation (never half-built).
    AlSigmaQFamily f(g, Side::Put, kSpot);
    ASSERT_TRUE(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4));
    EXPECT_EQ(f.price(kSpot, kSpot, 0.3, 0.02).status, AlSigmaQFamilyStatus::Ok);
    EXPECT_FALSE(f.prepare(0.1, 0.5, kSigmaNodes, 0.03, 0.01, 4));
    EXPECT_FALSE(f.prepared());
    EXPECT_EQ(f.price(kSpot, kSpot, 0.3, 0.02).status, AlSigmaQFamilyStatus::NotPrepared);
  }
  {
    AlSigmaQFamily f(g, Side::Call, kSpot);
    ASSERT_TRUE(f.prepare(0.1, 0.5, 8, 0.01, 0.03, 3));
    EXPECT_EQ(f.n_sigma(), 8);
    EXPECT_EQ(f.n_q(), 3);
    EXPECT_EQ(f.sigma_lo(), 0.1);
    EXPECT_EQ(f.sigma_hi(), 0.5);
    EXPECT_EQ(f.q_lo(), 0.01);
    EXPECT_EQ(f.q_hi(), 0.03);
    EXPECT_EQ(f.anchor(), kSpot);
    EXPECT_EQ(f.side(), Side::Call);
  }
}

TEST(AlSigmaQFamily, RefusalsAreTypedAndCarryNaN) {
  AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  AlSigmaQFamily f(g, Side::Put, kSpot);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4));
  const auto refused = [&](double S, double K, double sigma, double q, AlSigmaQFamilyStatus want) {
    const AlSigmaQFamilyPrice p = f.price(S, K, sigma, q);
    EXPECT_EQ(p.status, want) << "S=" << S << " K=" << K << " sigma=" << sigma << " q=" << q;
    EXPECT_TRUE(std::isnan(p.price));
    const AlSigmaQFamilyChord c = f.price_and_chord(S, K, sigma, q);
    EXPECT_EQ(c.status, want);
    EXPECT_TRUE(std::isnan(c.price) && std::isnan(c.price_sigma_bumped));
  };
  refused(kSpot, kSpot, 0.5 + 1.0e-9, 0.02, AlSigmaQFamilyStatus::SigmaOutsideBox);
  refused(kSpot, kSpot, 0.1 - 1.0e-9, 0.02, AlSigmaQFamilyStatus::SigmaOutsideBox);
  refused(kSpot, kSpot, 0.5 * kAlSigmaFamilyMinSigma, 0.02, AlSigmaQFamilyStatus::SigmaBelowGuard);
  refused(kSpot, kSpot, 0.3, 0.03 + 1.0e-12, AlSigmaQFamilyStatus::YieldOutsideBox);
  refused(kSpot, kSpot, 0.3, 0.01 - 1.0e-12, AlSigmaQFamilyStatus::YieldOutsideBox);
  refused(0.0, kSpot, 0.3, 0.02, AlSigmaQFamilyStatus::InvalidArgument);
  refused(kSpot, -1.0, 0.3, 0.02, AlSigmaQFamilyStatus::InvalidArgument);
  refused(kSpot, kSpot, kNaN, 0.02, AlSigmaQFamilyStatus::InvalidArgument);
  refused(kSpot, kSpot, 0.3, kNaN, AlSigmaQFamilyStatus::InvalidArgument);
  // The box corners price; the chord needs kAlPackSigmaBump of σ headroom.
  for (const double s : {0.1, 0.5}) {
    for (const double q : {0.01, 0.03}) {
      EXPECT_EQ(f.price(kSpot, kSpot, s, q).status, AlSigmaQFamilyStatus::Ok);
    }
  }
  EXPECT_EQ(f.price_and_chord(kSpot, kSpot, 0.5, 0.02).status,
            AlSigmaQFamilyStatus::SigmaOutsideBox);
  EXPECT_EQ(f.price_and_chord(kSpot, kSpot, 0.5 - 2.0 * kAlPackSigmaBump, 0.02).status,
            AlSigmaQFamilyStatus::Ok);
  // The geometry re-prepared under the family: refuse until prepared again.
  g.prepare(0.75, 0.05, 0.02);
  refused(kSpot, kSpot, 0.3, 0.02, AlSigmaQFamilyStatus::GeometryRePrepared);
  ASSERT_TRUE(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4));
  // At the q-box end (a q node) only the σ interpolation remains: the new T is served.
  const AlSigmaQFamilyPrice again = f.price(kSpot, kSpot, 0.3, 0.03);
  ASSERT_EQ(again.status, AlSigmaQFamilyStatus::Ok);
  const Result<double> cold = andersen_lake(kSpot, kSpot, 0.75, 0.3, 0.05, 0.03, Side::Put);
  ASSERT_TRUE(cold);
  EXPECT_NEAR(again.price, *cold, 1.0e-7 * std::max(*cold, 1.0));
  // Moved-from family.
  AlSigmaQFamily moved(std::move(f));
  EXPECT_EQ(moved.price(kSpot, kSpot, 0.3, 0.02).status, AlSigmaQFamilyStatus::Ok);
  EXPECT_EQ(f.price(kSpot, kSpot, 0.3, 0.02).status, AlSigmaQFamilyStatus::NotPrepared); // NOLINT
  EXPECT_FALSE(f.prepare(0.1, 0.5, kSigmaNodes, 0.01, 0.03, 4));                         // NOLINT
}

TEST(AlSigmaQFamily, NodePricesAtTheAnchorMatchTheColdSolve) {
  // At a (σ, q) node of the Sigma axis and the anchor strike neither axis interpolates
  // beyond the node -> z -> node coordinate round trip and no rescale happens: the
  // price is the cold solve's to rounding. Bound: 2-figure round-up of the measured.
  constexpr double kBound = 1.6e-16; // measured 1.50707e-16
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  std::size_t checked = 0;
  double worst = 0.0;
  for (const Side side : {Side::Put, Side::Call}) {
    AlSigmaQFamily f(g, side, kSpot);
    ASSERT_TRUE(f.prepare(0.1, 0.6, kSigmaNodes, 0.01, 0.03, 4, AlSigmaFamilyAxis::Sigma));
    for (const double sigma : lobatto(0.1, 0.6, kSigmaNodes)) {
      std::vector<double> qs = lobatto(0.01, 0.03, 4);
      qs.back() = 0.03; // the TU lays the box ends down exactly
      for (const double q : qs) {
        const Result<double> cold = andersen_lake(kSpot, kSpot, 0.5, sigma, 0.05, q, side);
        const AlSigmaQFamilyPrice p = f.price(kSpot, kSpot, sigma, q);
        ASSERT_TRUE(cold);
        ASSERT_EQ(p.status, AlSigmaQFamilyStatus::Ok);
        ++checked;
        worst = std::max(worst, std::abs(p.price - *cold) / std::max(*cold, 1.0));
      }
    }
  }
  EXPECT_EQ(checked, 2u * kSigmaNodes * 4u);
  EXPECT_LE(worst, kBound);
  std::cout << "[AlSigmaQFamily] node/anchor worst |dP|/max(P,1)=" << worst << "\n";
}

TEST(AlSigmaQFamily, Spy23ChainAtItsOwnYieldsWithinMeasuredBound) {
  // SPY 23 d, both sides, 25 strikes x 12 off-node σ at each strike's own q (the
  // chain's measured q range). Bounds: 2-figure round-ups of the measured worst.
  constexpr double kBound3 = 2.8e-11; // n_q = 3: measured 2.76293e-11
  constexpr double kBound4 = 2.8e-11; // n_q = 4: measured 2.70866e-11 (the σ floor)
  const QWorst w3 = qcases_worst(spy23_cases(), 3);
  const QWorst w4 = qcases_worst(spy23_cases(), 4);
  const QWorst w6 = qcases_worst(spy23_cases(), 6);
  EXPECT_EQ(w4.priced, 2u * 25u * 12u);
  EXPECT_EQ(w4.refused, 0u);
  EXPECT_LE(w3.rel, kBound3) << w3.at;
  EXPECT_LE(w4.rel, kBound4) << w4.at;
  std::cout << "[AlSigmaQFamily] spy23 max rel (P>=0.01): n_q=3 " << w3.rel << " | n_q=4 "
            << w4.rel << " | n_q=6 " << w6.rel << "\n[AlSigmaQFamily] spy23 max |dP|/max(P,1): "
            << w3.scaled << " | " << w4.scaled << " | " << w6.scaled
            << "\n[AlSigmaQFamily] spy23 n_q=4 worst at " << w4.at << "\n";
}

TEST(AlSigmaQFamily, SpyLongDatedPutsAtTheirOwnYieldsWithinMeasuredBound) {
  // SPY 2.17 y (the widest q spread of the board's long expiries), puts.
  constexpr double kBound3 = 4.9e-8; // n_q = 3: measured 4.88476e-8 (the q axis)
  constexpr double kBound4 = 2.3e-9; // n_q = 4: measured 2.25697e-9 (≈ the 12-node σ floor)
  const QWorst w3 = qcases_worst(spy_long_cases(), 3);
  const QWorst w4 = qcases_worst(spy_long_cases(), 4);
  const QWorst w6 = qcases_worst(spy_long_cases(), 6);
  EXPECT_EQ(w4.priced, 25u * 12u);
  EXPECT_LE(w3.rel, kBound3) << w3.at;
  EXPECT_LE(w4.rel, kBound4) << w4.at;
  std::cout << "[AlSigmaQFamily] spy-long max rel (P>=0.01): n_q=3 " << w3.rel << " | n_q=4 "
            << w4.rel << " | n_q=6 " << w6.rel << "\n[AlSigmaQFamily] spy-long max |dP|/max(P,1): "
            << w3.scaled << " | " << w4.scaled << " | " << w6.scaled
            << "\n[AlSigmaQFamily] spy-long n_q=4 worst at " << w4.at << "\n";
}

TEST(AlSigmaQFamily, WideCarryBoxErrorFallsWithTheQNodeCount) {
  // A 4 %-wide q box (put [0, 4 %], call [0.5 %, 4.5 %]; r = 5 %, T = 1): the q axis
  // converges — each step in n_q tightens the worst error — and the 6-node worst is
  // pinned.
  // Measured 1.25512e-4 (call K 75, q 3.9 %): a 4 %-wide box is ~20x SPY's widest — the
  // q axis converges slowly near q = r (puts) and q -> 0 (calls); split such boxes.
  constexpr double kBound6 = 1.3e-4;
  const QWorst w3 = qcases_worst(wide_carry_cases(), 3);
  const QWorst w4 = qcases_worst(wide_carry_cases(), 4);
  const QWorst w6 = qcases_worst(wide_carry_cases(), 6);
  EXPECT_EQ(w6.refused, 0u);
  EXPECT_LT(w4.scaled, w3.scaled);
  EXPECT_LT(w6.scaled, w4.scaled);
  EXPECT_LE(w6.rel, kBound6) << w6.at;
  std::cout << "[AlSigmaQFamily] wide-carry max rel (P>=0.01): n_q=3 " << w3.rel << " | n_q=4 "
            << w4.rel << " | n_q=6 " << w6.rel
            << "\n[AlSigmaQFamily] wide-carry max |dP|/max(P,1): " << w3.scaled << " | "
            << w4.scaled << " | " << w6.scaled << "\n[AlSigmaQFamily] wide-carry n_q=6 worst at "
            << w6.at << "\n";
}

TEST(AlSigmaQFamily, ConcurrentPricesAreBitIdenticalToTheSerialLoop) {
  // N threads pricing through ONE const family (no per-thread scratch, no locks)
  // reproduce the single-thread loop bit for bit.
  constexpr unsigned kThreads = 8;
  const AlExpiryGeometry g = prepare_al_geometry(0.25, 0.045, 0.01);
  AlSigmaQFamily fam(g, Side::Put, kSpot);
  ASSERT_TRUE(fam.prepare(0.1, 0.5, kSigmaNodes, 0.005, 0.015, 4));
  const AlSigmaQFamily &shared = fam;
  std::vector<double> strikes;
  std::vector<double> sigmas;
  std::vector<double> yields;
  for (const double K : {70.0, 85.0, 95.0, 100.0, 105.0, 115.0, 130.0}) {
    for (const double s : log_midpoints(0.1, 0.5, 11)) {
      strikes.push_back(K);
      sigmas.push_back(s);
      yields.push_back(0.005 + 0.01 * std::fmod(static_cast<double>(yields.size()) * 0.618, 1.0));
    }
  }
  const std::size_t n = strikes.size();
  std::vector<double> serial(n);
  std::vector<double> serial_bumped(n);
  for (std::size_t i = 0; i < n; ++i) {
    const AlSigmaQFamilyChord c = shared.price_and_chord(kSpot, strikes[i], sigmas[i], yields[i]);
    ASSERT_EQ(c.status, AlSigmaQFamilyStatus::Ok);
    serial[i] = c.price;
    serial_bumped[i] = c.price_sigma_bumped;
  }
  std::vector<std::vector<double>> got(kThreads, std::vector<double>(n, kNaN));
  std::vector<std::vector<double>> got_bumped(kThreads, std::vector<double>(n, kNaN));
  {
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t]() {
        for (unsigned rep = 0; rep < 4; ++rep) {
          for (std::size_t j = 0; j < n; ++j) {
            const std::size_t i = (j + t * 7u) % n;
            const AlSigmaQFamilyChord c =
                shared.price_and_chord(kSpot, strikes[i], sigmas[i], yields[i]);
            got[t][i] = c.status == AlSigmaQFamilyStatus::Ok ? c.price : kNaN;
            got_bumped[t][i] = c.status == AlSigmaQFamilyStatus::Ok ? c.price_sigma_bumped : kNaN;
          }
        }
      });
    }
  }
  std::size_t mismatched = 0;
  for (unsigned t = 0; t < kThreads; ++t) {
    for (std::size_t i = 0; i < n; ++i) {
      if (!same_bits(got[t][i], serial[i]) || !same_bits(got_bumped[t][i], serial_bumped[i])) {
        ++mismatched;
      }
    }
  }
  EXPECT_EQ(mismatched, 0u) << "of " << kThreads * n;
  EXPECT_EQ(n, 77u);
}

TEST(AlSigmaQFamily, StateBytesIsPinned) {
  // One heap block per family: the fixed-capacity (kAlMaxNodes x 8 q x 16 σ) tensor plus
  // the node grid. The memory budget a caller provisions per (expiry, side).
  std::cout << "[AlSigmaQFamily] state_bytes=" << AlSigmaQFamily::state_bytes() << "\n";
  EXPECT_EQ(AlSigmaQFamily::state_bytes(), 34592u);
}

} // namespace
