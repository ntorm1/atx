#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_boundary.hpp"
#include "pricing/american_family_error.hpp"
#include "pricing/american_sigma_family.hpp"
#include "pricing/american_sigma_q_family.hpp"
#include "pricing/boundary_interp.hpp"

// Lane K3 (r5, 2026-10-22): the (sigma, q) family's error envelope. The envelope is BUILT
// from the family's own nodes (american_family_error.cpp); everything below that measures
// the family against the cold accurate kernel is an independent audit of it, at points
// that are no construction node (random, cell midpoints of both axes, near exercise).

namespace {

using atx::vol::AlExpiryGeometry;
using atx::vol::AlSigmaQFamily;
using atx::vol::AlSigmaQFamilyStatus;
using atx::vol::andersen_lake;
using atx::vol::FamilyBoundedPrice;
using atx::vol::FamilyErrorClass;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;
using atx::vol::ValidatedErrorEnvelope;

struct FamilyCase {
  double S;
  double T;
  double r;
  double q_lo;
  double q_hi;
  Side side;
  double sigma_lo;
  double sigma_hi;
  std::uint16_t n_q;
};

// SPY-shaped expiries (funding-pv yields ~1.2-1.4 %, rate 4.5 %), a long-dated wide box,
// a short-dated narrow one, and both sides.
[[nodiscard]] std::vector<FamilyCase> family_cases() {
  std::vector<FamilyCase> v;
  for (const Side side : {Side::Put, Side::Call}) {
    v.push_back({600.0, 0.05, 0.045, 0.0120, 0.0135, side, 0.05, 1.2, 4});
    v.push_back({600.0, 0.25, 0.045, 0.0120, 0.0138, side, 0.05, 1.2, 4});
    v.push_back({600.0, 1.0, 0.045, 0.0115, 0.0140, side, 0.06, 0.9, 4});
    v.push_back({100.0, 2.0, 0.04, 0.010, 0.012, side, 0.05, 1.2, 4});
    v.push_back({100.0, 0.02, 0.04, 0.0100, 0.0105, side, 0.08, 2.0, 3});
  }
  return v;
}

struct Built {
  AlExpiryGeometry geometry;
  AlSigmaQFamily family;
  explicit Built(const FamilyCase &c)
      : geometry(prepare_al_geometry(c.T, c.r, 0.5 * (c.q_lo + c.q_hi))),
        family(geometry, c.side, c.S) {}
};

struct Audit {
  std::size_t points = 0;
  std::size_t violations = 0;
  double max_ratio = 0.0;    // |P_family - A| / bound
  double max_err = 0.0;      // |P_family - A| (USD)
  double max_bound = 0.0;    // bound (USD)
  std::vector<double> ratios;
};

void audit_point(const FamilyCase &c, const AlSigmaQFamily &f, const ValidatedErrorEnvelope &env,
                 double K, double sigma, double q, Audit &a) {
  const FamilyBoundedPrice fb = atx::vol::family_price_bounded(f, env, c.S, K, sigma, q);
  ASSERT_EQ(fb.status, AlSigmaQFamilyStatus::Ok);
  const auto cold = andersen_lake(c.S, K, c.T, sigma, c.r, q, c.side);
  ASSERT_TRUE(cold);
  EXPECT_EQ(fb.price, f.price(c.S, K, sigma, q).price); // the same price, bit for bit
  const double err = std::abs(fb.price - *cold);
  ++a.points;
  a.max_err = std::max(a.max_err, err);
  a.max_bound = std::max(a.max_bound, fb.bound_usd);
  const double ratio = err / fb.bound_usd;
  a.ratios.push_back(ratio);
  a.max_ratio = std::max(a.max_ratio, ratio);
  if (!(err <= fb.bound_usd)) {
    ++a.violations;
    ADD_FAILURE() << "envelope violated: side " << static_cast<int>(c.side) << " T " << c.T
                  << " K " << K << " sigma " << sigma << " q " << q << " err " << err
                  << " bound " << fb.bound_usd;
  }
}

} // namespace

// The construction covers every case above, and at 400 random off-node points per family
// (strike log-uniform over [0.5 S, 1.5 S], log sigma and q uniform over the box) the
// measured |P_family - A| never exceeds the per-point bound.
TEST(AmericanFamilyError, EnvelopeBoundsTheMeasuredErrorAtRandomOffNodePoints) {
  std::mt19937_64 rng(0x4b33c0de2026ull);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  for (const FamilyCase &c : family_cases()) {
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(b.family);
    std::printf("[envelope] side %d T %.3f: class %d status %d interp %.3g solve %.3g ref %.3g "
                "round %.3g sigma ratio %.3g q ratio %.3g\n",
                static_cast<int>(c.side), c.T, static_cast<int>(env.error_class),
                static_cast<int>(env.status), env.contributions.interpolation_y,
                env.contributions.boundary_solve_y, env.contributions.reference_y,
                env.contributions.rounding_y, env.contributions.sigma_ratio,
                env.contributions.q_ratio);
    ASSERT_EQ(env.error_class, FamilyErrorClass::RefinementValidated);
    Audit a;
    for (int i = 0; i < 400; ++i) {
      const double K = c.S * std::exp(std::log(0.5) + std::log(3.0) * u01(rng));
      const double sigma =
          c.sigma_lo * std::exp(std::log(c.sigma_hi / c.sigma_lo) * u01(rng));
      const double q = c.q_lo + (c.q_hi - c.q_lo) * u01(rng);
      audit_point(c, b.family, env, K, sigma, q, a);
    }
    std::sort(a.ratios.begin(), a.ratios.end());
    std::printf("[envelope audit] side %d T %.3f: %zu points, %zu violations, max err %.3g USD, "
                "max bound %.3g USD, err/bound median %.3g max %.3g\n",
                static_cast<int>(c.side), c.T, a.points, a.violations, a.max_err, a.max_bound,
                a.ratios[a.ratios.size() / 2], a.max_ratio);
    EXPECT_EQ(a.violations, 0u);
  }
}

TEST(AmericanFamilyError, BatchPreservesPriceBitsAndNeverTightensScalarBounds) {
  for (const auto &c : family_cases()) {
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    const auto env = atx::vol::build_family_error_envelope(b.family);
    std::vector<atx::vol::FamilyBoundedRequest> requests;
    for (const double k : {0.25, 0.7, 1.0, 1.3, 3.0}) {
      for (const double sigma : {c.sigma_lo, std::sqrt(c.sigma_lo * c.sigma_hi), c.sigma_hi}) {
        for (const double q : {c.q_lo, 0.5 * (c.q_lo + c.q_hi), c.q_hi}) {
          requests.push_back({c.S, c.S * k, sigma, q});
        }
      }
    }
    std::vector<FamilyBoundedPrice> batch(requests.size());
    atx::vol::FamilyBoundedBatchWork work;
    ASSERT_TRUE(atx::vol::family_prices_bounded(b.family, env, requests, batch, &work));
    EXPECT_EQ(work.fused_prices, requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const auto &r = requests[i];
      const auto scalar = atx::vol::family_price_bounded(b.family, env, r.S, r.K, r.sigma, r.q);
      EXPECT_EQ(batch[i].status, scalar.status) << i;
      EXPECT_EQ(std::bit_cast<std::uint64_t>(batch[i].price),
                std::bit_cast<std::uint64_t>(scalar.price)) << i;
      EXPECT_GE(batch[i].bound_usd, scalar.bound_usd) << i;
      const auto cold = andersen_lake(r.S, r.K, c.T, r.sigma, c.r, r.q, c.side);
      ASSERT_TRUE(cold);
      EXPECT_LE(std::abs(batch[i].price - *cold), batch[i].bound_usd) << i;
    }
  }
}

TEST(AmericanFamilyError, BatchKeepsRefusalsAndStaleEnvelopeIdentity) {
  const auto c = family_cases()[0];
  Built b(c);
  ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  const auto env = atx::vol::build_family_error_envelope(b.family);
  const std::array<atx::vol::FamilyBoundedRequest, 5> requests{{
      {c.S, c.S, .3, c.q_lo}, {c.S, -1, .3, c.q_lo}, {c.S, c.S, 5, c.q_lo},
      {c.S, c.S, .3, c.q_hi + .01}, {c.S, c.S, .3, std::numeric_limits<double>::quiet_NaN()}}};
  std::array<FamilyBoundedPrice, 5> batch;
  for (int pass = 0; pass < 2; ++pass) {
    ASSERT_TRUE(atx::vol::family_prices_bounded(b.family, env, requests, batch));
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const auto &r = requests[i];
      const auto scalar = atx::vol::family_price_bounded(b.family, env, r.S, r.K, r.sigma, r.q);
      EXPECT_EQ(batch[i].status, scalar.status);
      if (std::isfinite(scalar.price)) EXPECT_EQ(batch[i].price, scalar.price);
      else EXPECT_TRUE(std::isnan(batch[i].price));
      if (pass == 1 || i > 0) EXPECT_TRUE(std::isinf(batch[i].bound_usd));
    }
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  }
  batch[0].price = 123;
  EXPECT_FALSE(atx::vol::family_prices_bounded(b.family, env, requests, std::span(batch).first(4)));
  EXPECT_EQ(batch[0].price, 123);
}

TEST(AmericanFamilyError, BatchPreservesUnverifiedFastAndInvalidatedGeometryPaths) {
  const auto c = family_cases()[0];
  for (const auto profile : {atx::vol::AlProfile::Reference, atx::vol::AlProfile::Fast}) {
    atx::vol::AlOpts opts;
    opts.profile = profile;
    auto geometry = prepare_al_geometry(c.T, c.r, c.q_lo, opts);
    AlSigmaQFamily family(geometry, c.side, c.S);
    ASSERT_TRUE(family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    auto env = atx::vol::build_family_error_envelope(family);
    env.bound_y.fill(std::numeric_limits<double>::infinity());
    std::array<atx::vol::FamilyBoundedRequest, 3> requests{{
        {c.S, .3 * c.S, .1, c.q_lo}, {c.S, c.S, .3, c.q_hi},
        {c.S, 2.0 * c.S, 1.1, c.q_lo}}};
    std::array<FamilyBoundedPrice, 3> out;
    for (int pass = 0; pass < 2; ++pass) {
      atx::vol::FamilyBoundedBatchWork work;
      ASSERT_TRUE(atx::vol::family_prices_bounded(family, env, requests, out, &work));
      for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto &rq = requests[i];
        const auto scalar = atx::vol::family_price_bounded(
            family, env, rq.S, rq.K, rq.sigma, rq.q);
        EXPECT_EQ(out[i].status, scalar.status);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(out[i].price),
                  std::bit_cast<std::uint64_t>(scalar.price));
        EXPECT_EQ(out[i].bound_usd, scalar.bound_usd);
      }
      if (profile == atx::vol::AlProfile::Fast || pass != 0) {
        EXPECT_EQ(work.scalar_prices, requests.size());
      }
      if (pass == 0) geometry.prepare(c.T * 2, c.r, c.q_lo, opts);
    }
  }
}

// Isolated repeated endpoint workload. Opt-in and citable only from vol-rel-avx2
// on a quiet pinned core; family construction is outside the measured region.
TEST(AmericanFamilyError, BatchIsolatedCostProbe) {
  bool enabled = false;
#ifdef _MSC_VER
  std::size_t length = 0;
  static_cast<void>(getenv_s(&length, nullptr, 0, "ATX_K6_COST"));
  enabled = length != 0;
#else
  enabled = std::getenv("ATX_K6_COST") != nullptr;
#endif
  if (!enabled) GTEST_SKIP() << "set ATX_K6_COST";
  constexpr std::size_t count = 8192;
  for (const auto side : {Side::Put, Side::Call}) {
    const FamilyCase c{773.0, .5, .04, .011, .014, side, .05, 1.2, 4};
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    const auto env = atx::vol::build_family_error_envelope(b.family);
    std::vector<atx::vol::FamilyBoundedRequest> requests(count);
    std::vector<FamilyBoundedPrice> scalar(count), batch(count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto j = i / 2u;
      requests[i] = {c.S, c.S * (.7 + .6 * static_cast<double>(j % 251u) / 250.0),
                     .15 + .4 * static_cast<double>(j % 239u) / 238.0 +
                         ((i & 1u) ? 5.0e-8 : -5.0e-8),
                     .012 + .001 * static_cast<double>(j % 211u) / 210.0};
    }
    for (int round = 0; round < 4; ++round) {
      const auto run = [&](bool fused) {
        const auto t0 = std::chrono::steady_clock::now();
        if (fused) {
          static_cast<void>(atx::vol::family_prices_bounded(b.family, env, requests, batch));
        } else {
          for (std::size_t i = 0; i < count; ++i) {
            const auto &rq = requests[i];
            scalar[i] = atx::vol::family_price_bounded(
                b.family, env, rq.S, rq.K, rq.sigma, rq.q);
          }
        }
        return std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - t0).count() / static_cast<double>(count);
      };
      double scalar_ns = 0.0, batch_ns = 0.0;
      if ((round & 1) != 0) { batch_ns = run(true); scalar_ns = run(false); }
      else { scalar_ns = run(false); batch_ns = run(true); }
      for (std::size_t i = 0; i < count; ++i) {
        ASSERT_EQ(std::bit_cast<std::uint64_t>(batch[i].price),
                  std::bit_cast<std::uint64_t>(scalar[i].price));
        ASSERT_GE(batch[i].bound_usd, scalar[i].bound_usd);
      }
      std::printf("[K6 bounded cost] side %d round %d points %zu scalar_ns %.3f "
                  "batch_ns %.3f speedup %.3f\n", static_cast<int>(side), round,
                  count, scalar_ns, batch_ns, scalar_ns / batch_ns);
    }
  }
}

namespace {

using atx::vol::kFamilyBuildParityY;
using atx::vol::kFamilyReferenceNoiseY;
namespace amer = atx::vol::amer;
namespace detail = atx::vol::detail;
using atx::vol::sigma_family_detail::bary_eval;
using atx::vol::sigma_q_family_detail::QFamilyAccess;

// The family's own node values (prepare()'s layout: the sigma axis coordinate, q linear
// with the box ends exact).
[[nodiscard]] double node_sigma(const AlSigmaQFamily &f, unsigned s) {
  const auto *st = QFamilyAccess::state(f);
  const double u = st->u_lo + 0.5 * (st->u_hi - st->u_lo) * (st->sz[s] + 1.0);
  return st->axis == atx::vol::AlSigmaFamilyAxis::LogSigma ? std::exp(u) : u;
}

[[nodiscard]] double node_q(const AlSigmaQFamily &f, unsigned j) {
  const auto *st = QFamilyAccess::state(f);
  if (j == 0u) return st->q_lo;
  if (j + 1u == st->n_q) return st->q_hi;
  return st->q_lo + 0.5 * (st->q_hi - st->q_lo) * (st->qz[j] + 1.0);
}

// The target map's boundary at (sigma, q): the cold scalar solve on the family's scheme
// (strike-free y; the family's anchor as the internal strike).
[[nodiscard]] amer::AlBoundary target_boundary(const FamilyCase &c, const AlSigmaQFamily &f,
                                               double sigma, double q,
                                               amer::AlSolveResid *res = nullptr) {
  const auto *st = QFamilyAccess::state(f);
  const detail::InternalPutRates rq = detail::internal_put_rates(c.side, c.r, q);
  amer::AlBoundary bnd{};
  amer::AlWorkspace ws{};
  EXPECT_EQ(amer::al_solve_put_boundary(c.S, c.T, sigma, rq.rp, rq.qp, st->g->sch, bnd, ws,
                                        true, res),
            amer::AlSolveStatus::Ok);
  return bnd;
}

// The process-wide SimdIsa override for one scope.
class IsaScope {
public:
  explicit IsaScope(atx::vol::simd::SimdIsa isa) : prev_(atx::vol::simd::simd_isa_override()) {
    atx::vol::simd::set_simd_isa_override(isa);
  }
  ~IsaScope() { atx::vol::simd::set_simd_isa_override(prev_); }
  IsaScope(const IsaScope &) = delete;
  IsaScope &operator=(const IsaScope &) = delete;

private:
  atx::vol::simd::SimdIsa prev_;
};

} // namespace

// The envelope's own statement, audited in y: at 200 random (sigma, q) per family and at
// every premium quadrature node whose order verified, the target map's boundary (a cold
// scalar solve) is within bound_y of the family's interpolated one. (A node whose order
// does not verify carries no y bound; the price bound uses its trivial box bound there.)
TEST(AmericanFamilyError, EnvelopeBoundsTheBoundaryErrorAtEveryQuadratureNode) {
  std::mt19937_64 rng(0x4b33a11du);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  std::size_t total = 0;
  for (const FamilyCase &c : family_cases()) {
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(b.family);
    ASSERT_TRUE(env.covered());
    const auto *st = QFamilyAccess::state(b.family);
    double worst_ratio = 0.0;
    std::size_t checked = 0;
    for (int it = 0; it < 200; ++it) {
      const double sigma = c.sigma_lo * std::exp(std::log(c.sigma_hi / c.sigma_lo) * u01(rng));
      const double q = c.q_lo + (c.q_hi - c.q_lo) * u01(rng);
      const amer::AlBoundary bnd = target_boundary(c, b.family, sigma, q);
      amer::AlBoundary fam = st->grid;
      st->interpolate(sigma, q, fam);
      for (unsigned i = 0; i < env.n_quad; ++i) {
        if (std::isnan(env.zc[i]) || !(env.bound_y[i] < 1.0e300)) continue;
        const double zc = env.zc[i];
        const double ya = bary_eval(bnd.z.data(), bnd.wbary.data(), bnd.y.data(), bnd.n, zc);
        const double yf = bary_eval(fam.z.data(), fam.wbary.data(), fam.y.data(), fam.n, zc);
        ++checked;
        const double ratio = std::abs(ya - yf) / env.bound_y[i];
        worst_ratio = std::max(worst_ratio, ratio);
        EXPECT_LE(std::abs(ya - yf), env.bound_y[i])
            << "side " << static_cast<int>(c.side) << " T " << c.T << " node " << i;
      }
    }
    std::printf("[envelope y] side %d T %.3f: %zu node checks, %u unverified nodes, worst "
                "|y_A - y_f| / bound_y %.3g\n",
                static_cast<int>(c.side), c.T, checked,
                static_cast<unsigned>(env.contributions.unverified_nodes), worst_ratio);
    total += checked;
  }
  EXPECT_GT(total, 50000u);
}

// kFamilyReferenceNoiseY's pin: the target map's boundary is a FIXED sweep budget with an
// early exit, smooth in (sigma, q) while the sweep count does not switch. On a dense
// log-sigma line at both q ends and the q midpoint of every family, every solve runs the
// same number of sweeps (the accurate scheme exhausts its budget without meeting its tol),
// so the target has no early-exit jump between the family's nodes.
TEST(AmericanFamilyError, TargetBoundaryIsSmoothBetweenNodes) {
  for (const FamilyCase &c : family_cases()) {
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    unsigned min_sweeps = 1000u;
    unsigned max_sweeps = 0u;
    double min_resid = 1.0e300;
    for (const double q : {c.q_lo, 0.5 * (c.q_lo + c.q_hi), c.q_hi}) {
      for (int i = 0; i <= 240; ++i) {
        const double sigma = c.sigma_lo * std::exp(std::log(c.sigma_hi / c.sigma_lo) * i / 240.0);
        amer::AlSolveResid res{};
        static_cast<void>(target_boundary(c, b.family, sigma, q, &res));
        min_sweeps = std::min(min_sweeps, static_cast<unsigned>(res.sweeps));
        max_sweeps = std::max(max_sweeps, static_cast<unsigned>(res.sweeps));
        min_resid = std::min(min_resid, res.resid);
      }
    }
    std::printf("[target smoothness] side %d T %.3f: sweeps %u..%u, min residual %.3g\n",
                static_cast<int>(c.side), c.T, min_sweeps, max_sweeps, min_resid);
    EXPECT_EQ(min_sweeps, max_sweeps) << "the target's sweep count switches inside the box";
  }
}

// kFamilyBuildParityY's pin: the AVX2-built node tensor against the scalar build (whose
// nodes are the target map's own scalar solves) differs by at most the allowance.
TEST(AmericanFamilyError, NodeTensorMatchesTheScalarBuild) {
  if (!atx::vol::simd::have_avx2()) {
    GTEST_SKIP() << "host has no AVX2";
  }
  double worst = 0.0;
  for (const FamilyCase &c : family_cases()) {
    Built a(c);
    Built s(c);
    {
      const IsaScope avx2(atx::vol::simd::SimdIsa::ForceAvx2);
      ASSERT_TRUE(a.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    }
    {
      const IsaScope scalar(atx::vol::simd::SimdIsa::ForceScalar);
      ASSERT_TRUE(s.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    }
    const auto *sa = QFamilyAccess::state(a.family);
    const auto *ss = QFamilyAccess::state(s.family);
    ASSERT_EQ(sa->series_size, ss->series_size);
    for (std::size_t k = 0; k < ss->series_size; ++k) {
      worst = std::max(worst, std::abs(sa->series[k] - ss->series[k]));
    }
  }
  std::printf("[build parity] max |y_avx2 - y_scalar| over the node tensors %.3g (allowance "
              "%.3g)\n",
              worst, kFamilyBuildParityY);
  EXPECT_LE(worst, kFamilyBuildParityY);
}

// The quadrature contribution is zero by construction: the family and the target share the
// European leg, the time interpolation and the premium quadrature, so at a node (sigma_s,
// q_j) of a scalar-built family (node boundary == the target's solve) the two prices agree
// to rounding at every strike - any quadrature difference would show here at its own size.
TEST(AmericanFamilyError, OnNodeFamilyPriceIsTheTargetPriceToRounding) {
  const IsaScope scalar(atx::vol::simd::SimdIsa::ForceScalar);
  double worst = 0.0;
  for (const FamilyCase &c : family_cases()) {
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    for (unsigned s = 0; s < 16u; s += 3u) {
      for (unsigned j = 0; j < c.n_q; ++j) {
        const double sigma = node_sigma(b.family, s);
        const double q = node_q(b.family, j);
        for (const double m : {0.7, 0.9, 1.0, 1.1, 1.3}) {
          const double K = m * c.S;
          const auto fp = b.family.price(c.S, K, sigma, q);
          if (fp.status != AlSigmaQFamilyStatus::Ok) continue; // a node at a box guard
          const auto cold = andersen_lake(c.S, K, c.T, sigma, c.r, q, c.side);
          ASSERT_TRUE(cold);
          const double rel = std::abs(fp.price - *cold) / (c.S + K);
          worst = std::max(worst, rel);
          EXPECT_LE(rel, 256.0 * std::numeric_limits<double>::epsilon())
              << "side " << static_cast<int>(c.side) << " T " << c.T << " s " << s << " j " << j
              << " K " << K;
        }
      }
    }
  }
  std::printf("[on-node] max |P_family - A| / (S + K) at the nodes %.3g\n", worst);
}

// Every Uncovered class carries its reason and an infinite bound; an envelope prices
// nothing it was not built for; a probe violation retracts a covered envelope.
TEST(AmericanFamilyError, UncoveredFamiliesCarryTheirReasonAndAnInfiniteBound) {
  using atx::vol::FamilyEnvelopeStatus;
  const FamilyCase c{100.0, 0.5, 0.04, 0.010, 0.012, Side::Put, 0.05, 1.2, 4};
  {
    Built b(c); // never prepared
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(b.family);
    EXPECT_EQ(env.status, FamilyEnvelopeStatus::NotPrepared);
    EXPECT_FALSE(env.covered());
    EXPECT_FALSE(std::isfinite(atx::vol::family_domain_bound_usd(env, c.S, c.S, c.r)));
  }
  {
    Built b(c); // five sigma nodes: below the construction's three tail pairs
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 5, c.q_lo, c.q_hi, c.n_q));
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(b.family);
    EXPECT_EQ(env.status, FamilyEnvelopeStatus::UnsupportedShape);
    const FamilyBoundedPrice fb =
        atx::vol::family_price_bounded(b.family, env, c.S, c.S, 0.3, 0.011);
    EXPECT_EQ(fb.status, AlSigmaQFamilyStatus::Ok);
    EXPECT_EQ(fb.price, b.family.price(c.S, c.S, 0.3, 0.011).price);
    EXPECT_FALSE(std::isfinite(fb.bound_usd));
  }
  {
    Built b(c); // a collapsed q box of non-zero width
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, 0.011, 0.011 + 5.0e-13, c.n_q));
    ASSERT_EQ(b.family.n_q(), 1u);
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(b.family);
    EXPECT_EQ(env.status, FamilyEnvelopeStatus::DegenerateQBox);
  }
  {
    Built b(c); // a zero-width box is the sigma family at that q: covered
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, 0.011, 0.011, c.n_q));
    EXPECT_TRUE(atx::vol::build_family_error_envelope(b.family).covered());
  }
  Built b(c);
  Built other(c);
  ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  ASSERT_TRUE(other.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(b.family);
  ASSERT_TRUE(env.covered());
  EXPECT_TRUE(std::isfinite(atx::vol::family_price_bounded(b.family, env, c.S, c.S, 0.3, 0.011)
                                .bound_usd));
  EXPECT_FALSE(std::isfinite(
      atx::vol::family_price_bounded(other.family, env, c.S, c.S, 0.3, 0.011).bound_usd));
  atx::vol::SampledErrorEstimate pass{};
  pass.probes = 9;
  atx::vol::record_family_probe_audit(env, pass);
  EXPECT_TRUE(env.covered());
  EXPECT_EQ(env.audit.probes, 9u);
  atx::vol::SampledErrorEstimate fail = pass;
  fail.violations = 1;
  atx::vol::record_family_probe_audit(env, fail);
  EXPECT_EQ(env.status, FamilyEnvelopeStatus::ProbeViolation);
  EXPECT_FALSE(env.covered());
  EXPECT_EQ(env.audit.violations, 1u);
  EXPECT_FALSE(std::isfinite(
      atx::vol::family_price_bounded(b.family, env, c.S, c.S, 0.3, 0.011).bound_usd));
}

// The scoped context: an envelope is read only for the family it was built for, scopes
// nest and restore, and the context is the calling thread's alone.
TEST(AmericanFamilyError, EnvelopeScopeNestsChecksIdentityAndIsThreadLocal) {
  using atx::vol::current_family_envelope;
  using atx::vol::FamilyEnvelopeScope;
  const FamilyCase c{100.0, 0.5, 0.04, 0.010, 0.012, Side::Put, 0.05, 1.2, 4};
  Built a(c);
  Built b(c);
  ASSERT_TRUE(a.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  const ValidatedErrorEnvelope ea = atx::vol::build_family_error_envelope(a.family);
  const ValidatedErrorEnvelope eb = atx::vol::build_family_error_envelope(b.family);
  EXPECT_EQ(current_family_envelope(&a.family), nullptr);
  {
    const FamilyEnvelopeScope outer(&ea);
    EXPECT_EQ(current_family_envelope(&a.family), &ea);
    EXPECT_EQ(current_family_envelope(&b.family), nullptr);
    EXPECT_EQ(current_family_envelope(nullptr), nullptr);
    {
      const FamilyEnvelopeScope inner(&eb);
      EXPECT_EQ(current_family_envelope(&b.family), &eb);
      EXPECT_EQ(current_family_envelope(&a.family), nullptr);
    }
    EXPECT_EQ(current_family_envelope(&a.family), &ea);
    const atx::vol::ValidatedErrorEnvelope *seen = &ea;
    std::thread t([&] { seen = current_family_envelope(&a.family); });
    t.join();
    EXPECT_EQ(seen, nullptr);
    // A re-prepare onto another box invalidates the envelope.
    ASSERT_TRUE(a.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, 0.013, c.n_q));
    EXPECT_EQ(current_family_envelope(&a.family), nullptr);
  }
  EXPECT_EQ(current_family_envelope(&b.family), nullptr);
}

TEST(AmericanFamilyError, EnvelopeRejectsChangedPreparationAtTheSameFamilyAddress) {
  const FamilyCase c{100.0, 0.5, 0.04, 0.010, 0.012, Side::Put, 0.05, 1.2, 4};
  for (const unsigned change : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(change);
    Built b(c);
    Built replacement(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    const auto old = atx::vol::build_family_error_envelope(b.family);
    ASSERT_TRUE(old.covered());
    const atx::vol::FamilyEnvelopeScope scope(&old);
    ASSERT_EQ(atx::vol::current_family_envelope(&b.family), &old);
    if (change == 0u) {
      // Same geometry, domain and counts; only the interpolation axis changed.
      ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q,
                                   atx::vol::AlSigmaFamilyAxis::Sigma));
    } else if (change == 1u) {
      b.geometry.prepare(c.T, 0.08, 0.011);
      ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    } else if (change == 2u) {
      atx::vol::AlOpts options;
      options.n_collocation = 8;
      options.n_quad_price = 48; // same premium order as the original nullopt scheme
      b.geometry.prepare(c.T, c.r, 0.011, options);
      ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    } else {
      // A local revision of one per family would collide after move assignment.
      replacement.geometry.prepare(c.T, 0.08, 0.011);
      ASSERT_TRUE(replacement.family.prepare(c.sigma_lo, c.sigma_hi, 16,
                                             c.q_lo, c.q_hi, c.n_q));
      b.family = std::move(replacement.family);
    }
    const auto stale = atx::vol::family_price_bounded(b.family, old, c.S, c.S, 0.3, 0.011);
    ASSERT_EQ(stale.status, AlSigmaQFamilyStatus::Ok);
    EXPECT_FALSE(std::isfinite(stale.bound_usd));
    EXPECT_EQ(atx::vol::current_family_envelope(&b.family), nullptr);
    const auto fresh = atx::vol::build_family_error_envelope(b.family);
    ASSERT_TRUE(fresh.covered());
    const auto valid = atx::vol::family_price_bounded(b.family, fresh, c.S, c.S, 0.3, 0.011);
    EXPECT_TRUE(std::isfinite(valid.bound_usd));
    EXPECT_EQ(stale.price, valid.price); // invalidating a certificate never changes pricing
    std::printf("[envelope preparation] change %u stale %.17g fresh %.17g\n",
                change, stale.bound_usd, valid.bound_usd);
  }
}

TEST(AmericanFamilyError, EnvelopeRejectsGeometryInvalidationAndFailedReprepare) {
  const FamilyCase c{100.0, 0.5, 0.04, 0.010, 0.012, Side::Put, 0.05, 1.2, 4};
  Built b(c);
  ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  const auto old = atx::vol::build_family_error_envelope(b.family);
  ASSERT_TRUE(old.covered());
  const atx::vol::FamilyEnvelopeScope scope(&old);
  b.geometry.prepare(c.T, 0.08, 0.011);
  EXPECT_EQ(atx::vol::current_family_envelope(&b.family), nullptr);
  EXPECT_FALSE(atx::vol::build_family_error_envelope(b.family).covered());
  EXPECT_EQ(atx::vol::family_price_bounded(b.family, old, c.S, c.S, 0.3, 0.011).status,
            AlSigmaQFamilyStatus::GeometryRePrepared);
  ASSERT_FALSE(b.family.prepare(0.3, 0.1, 16, c.q_lo, c.q_hi, c.n_q));
  EXPECT_EQ(atx::vol::current_family_envelope(&b.family), nullptr);
  EXPECT_FALSE(atx::vol::build_family_error_envelope(b.family).covered());
  EXPECT_EQ(atx::vol::family_price_bounded(b.family, old, c.S, c.S, 0.3, 0.011).status,
            AlSigmaQFamilyStatus::NotPrepared);
  ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
  EXPECT_FALSE(std::isfinite(
      atx::vol::family_price_bounded(b.family, old, c.S, c.S, 0.3, 0.011).bound_usd));
  EXPECT_EQ(atx::vol::current_family_envelope(&b.family), nullptr);
}

// The near-exercise probe point: spot sits on the family's boundary now at the returned
// strike - deeper strikes price at intrinsic, shallower ones above it.
TEST(AmericanFamilyError, ExerciseStrikeSitsOnTheFamilysBoundary) {
  for (const Side side : {Side::Put, Side::Call}) {
    const FamilyCase c{100.0, 0.5, 0.05, 0.020, 0.022, side, 0.05, 1.2, 4};
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    for (const double sigma : {0.15, 0.3}) {
      const double q = 0.021;
      const double ke = atx::vol::family_exercise_strike(b.family, c.S, sigma, q);
      ASSERT_TRUE(std::isfinite(ke) && ke > 0.0);
      const double deep = side == Side::Put ? ke * 1.01 : ke / 1.01;
      const double shallow = side == Side::Put ? ke / 1.01 : ke * 1.01;
      const auto intrinsic = [&](double K) {
        return side == Side::Put ? std::max(K - c.S, 0.0) : std::max(c.S - K, 0.0);
      };
      const auto pd = b.family.price(c.S, deep, sigma, q);
      const auto ps = b.family.price(c.S, shallow, sigma, q);
      ASSERT_EQ(pd.status, AlSigmaQFamilyStatus::Ok);
      ASSERT_EQ(ps.status, AlSigmaQFamilyStatus::Ok);
      std::printf("[exercise strike] side %d sigma %.2f: K* %.4f, deep time value %.3g, "
                  "shallow %.3g\n",
                  static_cast<int>(side), sigma, ke, pd.price - intrinsic(deep),
                  ps.price - intrinsic(shallow));
      EXPECT_LE(pd.price - intrinsic(deep), 1.0e-9);
      EXPECT_GT(ps.price - intrinsic(shallow), 1.0e-6);
    }
  }
}

// review-K3 mn-7 (fix round 1): family_domain_bound_usd is ONE bound for every (sigma, q) of
// the domain at a strike, so it dominates the per-point bound family_price_bounded states
// anywhere in the box - also on a negative-carry put box, where a quadrature node's box bound
// g(xmax) carries |q| S e^{-q t} with e^{-q t} > 1 (hard-to-borrow underlyings). Checked on the
// envelope as built and on the same envelope with every node unverified (bound_y +inf, a state
// a covered envelope reaches node by node: each node then contributes its box bound alone),
// deep in the money included, at a short and a long expiry.
TEST(AmericanFamilyError, DomainBoundDominatesThePointBoundOnANegativeCarryBox) {
  double worst_built = 0.0;
  double worst_unverified = 0.0;
  for (const double T : {0.5, 3.0}) {
    const FamilyCase c{100.0, T, 0.04, -0.095, -0.085, Side::Put, 0.1, 0.8, 4};
    Built b(c);
    ASSERT_TRUE(b.family.prepare(c.sigma_lo, c.sigma_hi, 16, c.q_lo, c.q_hi, c.n_q));
    const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(b.family);
    ASSERT_TRUE(env.covered()) << static_cast<int>(env.status);
    ValidatedErrorEnvelope unverified = env;
    unverified.bound_y.fill(std::numeric_limits<double>::infinity());
    const std::array<const ValidatedErrorEnvelope *, 2> envelopes{&env, &unverified};
    for (const ValidatedErrorEnvelope *e : envelopes) {
      double &worst = e == &env ? worst_built : worst_unverified;
      for (const double K : {60.0, 80.0, 100.0, 125.0, 160.0, 200.0}) {
        const double domain = atx::vol::family_domain_bound_usd(*e, c.S, K, c.r);
        ASSERT_TRUE(std::isfinite(domain));
        for (const double sigma : {0.11, 0.2, 0.4, 0.79}) {
          for (const double q : {c.q_lo, -0.09, c.q_hi}) {
            const FamilyBoundedPrice fb =
                atx::vol::family_price_bounded(b.family, *e, c.S, K, sigma, q);
            ASSERT_EQ(fb.status, AlSigmaQFamilyStatus::Ok);
            worst = std::max(worst, fb.bound_usd / domain);
            EXPECT_LE(fb.bound_usd, domain) << "T " << T << " K " << K << " sigma " << sigma
                                            << " q " << q << " unverified " << (e != &env);
          }
        }
      }
    }
  }
  std::printf("[domain bound, q < 0] worst point bound / domain bound: built %.3g, every node "
              "unverified %.3g\n",
              worst_built, worst_unverified);
}
