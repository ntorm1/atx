#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_iv_batch.hpp"
#include "pricing/american_sigma_q_family.hpp"

// K2 (r5, 2026-10-22): the varying-parameter batch inversion (src/pricing/american_iv_batch.hpp).
// Every root here is a root of the FAMILY map (K1's fused price + vega): the targets are
// family prices at known σ, so "the true root" is that σ and the pins are in absolute vol
// against the batch's own tolerance. Accuracy against the accurate American map is the
// fit-level gate (the served-set identity runs), not this suite's.

namespace {

using atx::vol::AlExpiryGeometry;
using atx::vol::AlSigmaQFamily;
using atx::vol::prepare_al_geometry;
using atx::vol::Side;
namespace sq = atx::vol::sigma_q_family_detail;
namespace ib = atx::vol::iv_batch;
using ib::InverseBatchError;
using ib::InverseIsa;
using ib::InverseStatus;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTol = 1.0e-7;
constexpr double kS = 100.0;
constexpr double kT = 0.5;
constexpr double kR = 0.05;
constexpr double kQlo = 0.02;
constexpr double kQhi = 0.021;
constexpr double kSigLo = 0.08;
constexpr double kSigHi = 0.9;

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// A family, its geometry and its precontracted table (built once per suite).
struct Fam {
  Fam(Side s) : g(prepare_al_geometry(kT, kR, 0.5 * (kQlo + kQhi))), f(g, s, kS), side(s) {}
  AlExpiryGeometry g;
  AlSigmaQFamily f;
  sq::PremiumTable table;
  Side side;
};

[[nodiscard]] std::unique_ptr<Fam> make_family(Side side) {
  auto p = std::make_unique<Fam>(side);
  if (!p->f.prepare(kSigLo, kSigHi, 16, kQlo, kQhi, 4)) {
    ADD_FAILURE() << "family prepare refused";
    return nullptr;
  }
  if (sq::build_premium_table(p->f, p->table) != sq::FusedStatus::Ok) {
    ADD_FAILURE() << "table build refused";
    return nullptr;
  }
  return p;
}

// One batch's arrays (owned) and the spans the API takes.
struct Batch {
  explicit Batch(std::size_t n)
      : K(n, kNaN), target(n, kNaN), q(n, kNaN), is_call(n, 0), active(n, 1), seed(n, kNaN),
        blo(n, kNaN), bhi(n, kNaN), plo(n, kNaN), phi(n, kNaN), anchor(n, -1), sigma(n, kNaN),
        olo(n, kNaN), ohi(n, kNaN), oplo(n, kNaN), ophi(n, kNaN), res(n, kNaN), vega(n, kNaN),
        last(n, kNaN), evals(n, 0), status(n, InverseStatus::Inactive), flags(n, 0), acc(n) {}
  std::vector<double> K, target, q;
  std::vector<std::uint8_t> is_call, active;
  std::vector<double> seed, blo, bhi, plo, phi;
  std::vector<std::int32_t> anchor;
  std::vector<double> sigma, olo, ohi, oplo, ophi, res, vega, last;
  std::vector<std::uint16_t> evals;
  std::vector<InverseStatus> status;
  std::vector<std::uint8_t> flags;
  std::vector<ib::LaneAcceptance> acc;
  std::vector<double> truth; // the σ each target was priced at (NaN: none)

  [[nodiscard]] ib::InverseBatchIn in(const Fam &fam, bool table, InverseIsa isa) const {
    ib::InverseBatchIn r;
    if (table) {
      r.table = &fam.table;
    } else {
      r.family = &fam.f;
    }
    r.S = kS;
    r.tol = kTol;
    r.isa = isa;
    r.K = K;
    r.target = target;
    r.q = q;
    r.is_call = is_call;
    r.active = active;
    r.seed_sigma = seed;
    r.bracket_lo = blo;
    r.bracket_hi = bhi;
    r.price_lo = plo;
    r.price_hi = phi;
    r.anchor = anchor;
    return r;
  }
  [[nodiscard]] ib::InverseBatchOut out() {
    return ib::InverseBatchOut{sigma, olo, ohi, oplo, ophi, res, vega, last, evals, status, flags,
                               acc};
  }
};

// The family price at (K, sigma, q) (the map the batch inverts).
[[nodiscard]] double family_price(const Fam &fam, double K, double sigma, double q) {
  const sq::FusedPriceVega p = sq::price_and_vega(fam.f, kS, K, sigma, q);
  EXPECT_TRUE(p.status == sq::FusedStatus::Ok || p.status == sq::FusedStatus::NearNodeDegenerate);
  return p.price;
}

// Lane i: strike K, its root sigma, q; target = the family price there; a Black-76-like
// seed perturbation (seed = sigma * (1 + bump)).
void set_lane(Batch &b, const Fam &fam, std::size_t i, double K, double sigma, double q,
              double bump) {
  b.K[i] = K;
  b.q[i] = q;
  b.is_call[i] = fam.side == Side::Call ? 1u : 0u;
  b.target[i] = family_price(fam, K, sigma, q);
  b.seed[i] = sigma * (1.0 + bump);
  if (b.truth.size() < b.K.size()) b.truth.resize(b.K.size(), kNaN);
  b.truth[i] = sigma;
}

// A deterministic spread of lanes over strike, σ and q whose roots are identifiable: a
// draw on the exercise plateau (price at intrinsic, an ExerciseFlat outcome by design) or
// with a vega under twice the floor is redrawn.
void fill_spread(Batch &b, const Fam &fam, std::uint32_t seed_value) {
  std::mt19937 rng(seed_value);
  std::uniform_real_distribution<double> uk(80.0, 125.0);
  std::uniform_real_distribution<double> us(0.12, 0.7);
  std::uniform_real_distribution<double> uq(kQlo, kQhi);
  std::uniform_real_distribution<double> ub(-0.3, 0.3);
  for (std::size_t i = 0; i < b.K.size(); ++i) {
    for (int draw = 0; draw < 64; ++draw) {
      const double K = uk(rng);
      const double sigma = us(rng);
      const double q = uq(rng);
      const double bump = ub(rng);
      const sq::FusedPriceVega pv = sq::price_and_vega(fam.f, kS, K, sigma, q);
      if (pv.status != sq::FusedStatus::Ok || pv.exercised ||
          !(pv.vega > 2.0 * ib::kInverseVegaFloorRel * K)) {
        continue;
      }
      set_lane(b, fam, i, K, sigma, q, bump);
      break;
    }
    ASSERT_TRUE(std::isfinite(b.target[i])) << "no identifiable draw for lane " << i;
  }
}

class IvBatch : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    put_ = make_family(Side::Put).release();
    call_ = make_family(Side::Call).release();
  }
  static void TearDownTestSuite() {
    delete put_;
    delete call_;
    put_ = nullptr;
    call_ = nullptr;
  }
  static const Fam &put() { return *put_; }
  static const Fam &call() { return *call_; }
  static bool avx2() { return atx::vol::simd::have_avx2(); }

private:
  static Fam *put_;
  static Fam *call_;
};
Fam *IvBatch::put_ = nullptr;
Fam *IvBatch::call_ = nullptr;

// Lane counts 0..5 and vector tails (6, 7, 9) on both ISAs and both sources: every lane
// converges to its family root within the tolerance; n == 0 writes nothing.
TEST_F(IvBatch, LaneCountsAndTailsConvergeOnEveryPath) {
  ASSERT_NE(&put(), nullptr);
  for (const std::size_t n : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u}) {
    for (const int path : {0, 1, 2}) { // 0 table scalar, 1 table AVX2, 2 family scalar
      if (path == 1 && !avx2()) continue;
      for (const Fam *fam : {&put(), &call()}) {
        Batch b(n);
        fill_spread(b, *fam, static_cast<std::uint32_t>(17u + n));
        ib::InverseScratch scratch;
        ib::InverseBatchWork work;
        const InverseIsa isa = path == 1 ? InverseIsa::Avx2 : InverseIsa::Scalar;
        const InverseBatchError e =
            ib::invert_many(b.in(*fam, path != 2, isa), b.out(), scratch, &work);
        ASSERT_EQ(e, InverseBatchError::None) << "n=" << n << " path=" << path;
        EXPECT_EQ(work.lanes, n);
        EXPECT_EQ(work.avx2, path == 1);
        for (std::size_t i = 0; i < n; ++i) {
          ASSERT_EQ(b.status[i], InverseStatus::Converged)
              << "n=" << n << " path=" << path << " lane " << i << " K=" << b.K[i]
              << " truth=" << b.truth[i] << " " << ib::inverse_status_name(b.status[i]);
          EXPECT_LE(std::fabs(b.sigma[i] - b.truth[i]), 2.0 * kTol) << "lane " << i;
          EXPECT_GE(b.evals[i], 1u);
          EXPECT_LE(b.evals[i], ib::kInverseMaxEvaluations);
          EXPECT_NE(b.acc[i].kind, ib::AcceptanceKind::None);
        }
        if (path == 1 && n > 0) {
          EXPECT_GT(work.vector_steps, 0u);
          EXPECT_LE(work.vector_lane_slots, work.vector_steps * ib::kInverseLaneWidth);
          EXPECT_EQ(work.vector_lane_slots, work.evaluations);
        }
      }
    }
  }
}

// The scalar table path IS K1's map: the last evaluated point of every lane reproduces
// price_vega_on (table) / price_and_vega (family) bit for bit.
TEST_F(IvBatch, ScalarPathIsTheFusedFamilyMapBitForBit) {
  for (const bool table : {true, false}) {
    Batch b(7);
    fill_spread(b, put(), 5u);
    ib::InverseScratch scratch;
    ASSERT_EQ(ib::invert_many(b.in(put(), table, InverseIsa::Scalar), b.out(), scratch),
              InverseBatchError::None);
    for (std::size_t i = 0; i < 7; ++i) {
      ASSERT_EQ(b.status[i], InverseStatus::Converged);
      sq::FusedPriceVega ref;
      if (table) {
        sq::QTile tile;
        ASSERT_EQ(sq::contract_q(put().table, b.q[i], tile), sq::FusedStatus::Ok);
        ref = sq::price_vega_on(put().table, tile, kS, b.K[i], b.last[i]);
      } else {
        ref = sq::price_and_vega(put().f, kS, b.K[i], b.last[i], b.q[i]);
      }
      EXPECT_TRUE(same_bits(ref.price - b.target[i], b.res[i])) << "lane " << i;
      EXPECT_TRUE(same_bits(ref.vega, b.vega[i])) << "lane " << i;
    }
  }
}

// AVX2 vs scalar parity (rounding class): equal statuses, roots within the tolerance class,
// residuals within a few ulp of the price scale.
TEST_F(IvBatch, Avx2MatchesScalarToTheRoundingClass) {
  if (!avx2()) GTEST_SKIP() << "host has no AVX2";
  double worst_sigma = 0.0;
  double worst_price = 0.0;
  for (const Fam *fam : {&put(), &call()}) {
    Batch a(64);
    fill_spread(a, *fam, 99u);
    Batch s = a;
    ib::InverseScratch scratch;
    ASSERT_EQ(ib::invert_many(a.in(*fam, true, InverseIsa::Avx2), a.out(), scratch),
              InverseBatchError::None);
    ASSERT_EQ(ib::invert_many(s.in(*fam, true, InverseIsa::Scalar), s.out(), scratch),
              InverseBatchError::None);
    for (std::size_t i = 0; i < 64; ++i) {
      ASSERT_EQ(a.status[i], s.status[i]) << "lane " << i;
      worst_sigma = std::max(worst_sigma, std::fabs(a.sigma[i] - s.sigma[i]));
      // The same point priced on both paths: the AVX2 kernel's price class.
      if (same_bits(a.last[i], s.last[i])) {
        worst_price = std::max(worst_price, std::fabs(a.res[i] - s.res[i]) /
                                                std::max(1.0, a.target[i]));
      }
    }
  }
  std::cout << "[IvBatch] AVX2 vs scalar: max |dsigma| " << worst_sigma
            << ", max scaled |dP| at a shared point " << worst_price << "\n";
  EXPECT_LE(worst_sigma, 2.0 * kTol);
  EXPECT_LE(worst_price, 1.0e-13);
}

// The AVX2 kernel's price + vega at a fixed σ against K1's scalar map (one evaluation each,
// max_evaluations = 1 so the lane is priced exactly at its seed).
TEST_F(IvBatch, Avx2KernelPriceAndVegaMatchTheScalarMap) {
  if (!avx2()) GTEST_SKIP() << "host has no AVX2";
  double worst_p = 0.0;
  double worst_v = 0.0;
  for (const Fam *fam : {&put(), &call()}) {
    Batch a(40);
    fill_spread(a, *fam, 7u);
    for (std::size_t i = 0; i < 40; ++i) {
      a.target[i] *= 1.37; // far from the seed: one evaluation, no convergence
    }
    Batch s = a;
    ib::InverseScratch scratch;
    ib::InverseBatchIn ia = a.in(*fam, true, InverseIsa::Avx2);
    ia.max_evaluations = 1;
    ib::InverseBatchIn is = s.in(*fam, true, InverseIsa::Scalar);
    is.max_evaluations = 1;
    ASSERT_EQ(ib::invert_many(ia, a.out(), scratch), InverseBatchError::None);
    ASSERT_EQ(ib::invert_many(is, s.out(), scratch), InverseBatchError::None);
    for (std::size_t i = 0; i < 40; ++i) {
      ASSERT_TRUE(same_bits(a.last[i], s.last[i])) << "lane " << i;
      const double scale = std::max(1.0, a.target[i]);
      worst_p = std::max(worst_p, std::fabs(a.res[i] - s.res[i]) / scale);
      worst_v = std::max(worst_v, std::fabs(a.vega[i] - s.vega[i]) / std::max(1.0, s.vega[i]));
    }
  }
  std::cout << "[IvBatch] AVX2 kernel vs scalar map: max scaled |dP| " << worst_p
            << ", max scaled |dvega| " << worst_v << "\n";
  EXPECT_LE(worst_p, 1.0e-13);
  EXPECT_LE(worst_v, 1.0e-11);
}

// Mixed refusals in ONE batch, on both ISAs: each lane gets its own typed status and the
// valid lanes are unaffected by their neighbours (equal to the same lanes inverted alone).
TEST_F(IvBatch, MixedRefusalsAreTypedPerLaneAndDoNotPoisonTheBatch) {
  for (const InverseIsa isa : {InverseIsa::Scalar, InverseIsa::Avx2}) {
    if (isa == InverseIsa::Avx2 && !avx2()) continue;
    Batch b(10);
    set_lane(b, put(), 0, 100.0, 0.3, 0.0205, 0.1); // valid
    set_lane(b, put(), 1, 95.0, 0.25, 0.0205, 0.0);
    b.target[1] = 0.5 * family_price(put(), 95.0, kSigLo, 0.0205); // below P(σ_lo)
    set_lane(b, put(), 2, 105.0, 0.25, 0.0205, 0.0);
    b.target[2] = 1.2 * family_price(put(), 105.0, kSigHi, 0.0205); // above P(σ_hi)
    set_lane(b, put(), 3, 100.0, 0.3, 0.0205, 0.0);
    b.q[3] = 0.05; // q outside the box
    set_lane(b, put(), 4, 100.0, 0.3, 0.0205, 0.0);
    b.K[4] = kNaN;
    set_lane(b, put(), 5, 100.0, 0.3, 0.0205, 0.0);
    b.is_call[5] = 1; // the wrong side for a put family
    set_lane(b, put(), 6, 200.0, 0.2, 0.0205, 0.0);
    b.target[6] = 100.0; // a deep in-the-money put at intrinsic: the exercise plateau
    b.seed[6] = 0.15;
    set_lane(b, put(), 7, 100.0, 0.3, 0.0205, 0.0);
    b.active[7] = 0; // masked
    set_lane(b, put(), 8, 110.0, 0.45, 0.0202, -0.2); // valid
    set_lane(b, put(), 9, 100.0, 0.3, 0.0205, 0.0);
    b.target[9] = -1.0; // invalid target
    ib::InverseScratch scratch;
    ib::InverseBatchWork work;
    ASSERT_EQ(ib::invert_many(b.in(put(), true, isa), b.out(), scratch, &work),
              InverseBatchError::None);
    EXPECT_EQ(b.status[0], InverseStatus::Converged);
    EXPECT_EQ(b.status[1], InverseStatus::SigmaBelowBox);
    EXPECT_EQ(b.status[2], InverseStatus::SigmaAboveBox);
    EXPECT_EQ(b.status[3], InverseStatus::QOutsideBox);
    EXPECT_EQ(b.status[4], InverseStatus::InvalidInput);
    EXPECT_EQ(b.status[5], InverseStatus::InvalidInput);
    EXPECT_EQ(b.status[6], InverseStatus::ExerciseFlat);
    EXPECT_EQ(b.status[7], InverseStatus::Inactive);
    EXPECT_EQ(b.status[8], InverseStatus::Converged);
    EXPECT_EQ(b.status[9], InverseStatus::InvalidInput);
    EXPECT_EQ(b.evals[3], 0u);
    EXPECT_EQ(b.evals[4], 0u);
    EXPECT_TRUE(std::isnan(b.sigma[7])); // untouched
    // Box misses carry the evaluated box edge as their bracket end.
    EXPECT_EQ(b.ohi[1], kSigLo);
    EXPECT_TRUE(std::isfinite(b.ophi[1]));
    EXPECT_EQ(b.olo[2], kSigHi);
    EXPECT_TRUE(std::isfinite(b.oplo[2]));
    EXPECT_EQ(work.by_status[static_cast<std::size_t>(InverseStatus::InvalidInput)], 3u);
    EXPECT_EQ(work.active_lanes, 9u);
    // The valid lanes alone: the same bits.
    for (const std::size_t lane : {0u, 8u}) {
      Batch one(1);
      set_lane(one, put(), 0, b.K[lane], b.truth[lane], b.q[lane], 0.0);
      one.seed[0] = b.seed[lane];
      ASSERT_EQ(ib::invert_many(one.in(put(), true, isa), one.out(), scratch),
                InverseBatchError::None);
      EXPECT_TRUE(same_bits(one.sigma[0], b.sigma[lane])) << "lane " << lane;
      EXPECT_EQ(one.evals[0], b.evals[lane]);
    }
  }
}

// NaN / invalid arguments: batch-level (spans, source, S) and lane-level refusals.
TEST_F(IvBatch, InvalidArgumentsAreRefusedTyped) {
  ib::InverseScratch scratch;
  Batch b(3);
  fill_spread(b, put(), 3u);
  {
    ib::InverseBatchIn in = b.in(put(), true, InverseIsa::Scalar);
    in.family = &put().f; // both sources
    EXPECT_EQ(ib::invert_many(in, b.out(), scratch), InverseBatchError::NoFamily);
    in.family = nullptr;
    in.table = nullptr; // none
    EXPECT_EQ(ib::invert_many(in, b.out(), scratch), InverseBatchError::NoFamily);
  }
  {
    ib::InverseBatchIn in = b.in(put(), true, InverseIsa::Scalar);
    in.q = std::span<const double>(b.q).first(2); // short required span
    EXPECT_EQ(ib::invert_many(in, b.out(), scratch), InverseBatchError::SpanMismatch);
    ib::InverseBatchOut out = b.out();
    out.vega = std::span<double>(b.vega).first(1);
    EXPECT_EQ(ib::invert_many(b.in(put(), true, InverseIsa::Scalar), out, scratch),
              InverseBatchError::SpanMismatch);
  }
  {
    ib::InverseBatchIn in = b.in(put(), true, InverseIsa::Scalar);
    in.S = kNaN; // every lane invalid
    ASSERT_EQ(ib::invert_many(in, b.out(), scratch), InverseBatchError::None);
    for (std::size_t i = 0; i < 3; ++i) EXPECT_EQ(b.status[i], InverseStatus::InvalidInput);
  }
  for (const int kind : {0, 1, 2, 3, 4, 5, 6}) {
    Batch c(3);
    fill_spread(c, put(), 4u);
    switch (kind) {
    case 0: c.target[1] = kNaN; break;
    case 1: c.K[1] = std::numeric_limits<double>::infinity(); break;
    case 2: c.K[1] = -5.0; break;
    case 3: c.q[1] = kNaN; break;
    case 4: c.blo[1] = 0.5; c.bhi[1] = 0.2; break; // inverted retained bracket
    case 5: c.anchor[1] = 7; break;                // out of range
    case 6: c.anchor[1] = 1; break;                // self
    default: break;
    }
    ASSERT_EQ(ib::invert_many(c.in(put(), true, InverseIsa::Scalar), c.out(), scratch),
              InverseBatchError::None);
    EXPECT_EQ(c.status[1], InverseStatus::InvalidInput) << "kind " << kind;
    EXPECT_EQ(c.status[0], InverseStatus::Converged) << "kind " << kind;
    EXPECT_EQ(c.status[2], InverseStatus::Converged) << "kind " << kind;
  }
}

// Exact family nodes: a root ON a σ node (the Chebyshev-Lobatto nodes of the box, including
// both box edges) and a seed exactly on it.
TEST_F(IvBatch, RootsAndSeedsOnExactFamilyNodesConverge) {
  const unsigned n = 16;
  const double u_lo = std::log(kSigLo);
  const double u_hi = std::log(kSigHi);
  for (const InverseIsa isa : {InverseIsa::Scalar, InverseIsa::Avx2}) {
    if (isa == InverseIsa::Avx2 && !avx2()) continue;
    Batch b(n);
    for (unsigned j = 0; j < n; ++j) {
      const double frac = 0.5 * (1.0 - std::cos(3.14159265358979323846 * j / (n - 1)));
      double sigma = std::exp(u_lo + frac * (u_hi - u_lo));
      if (j == 0) sigma = kSigLo;
      if (j == n - 1) sigma = kSigHi;
      set_lane(b, put(), j, 90.0 + j, sigma, j % 2 == 0 ? kQlo : kQhi, 0.0);
    }
    ib::InverseScratch scratch;
    ASSERT_EQ(ib::invert_many(b.in(put(), true, isa), b.out(), scratch), InverseBatchError::None);
    for (unsigned j = 0; j < n; ++j) {
      EXPECT_EQ(b.status[j], InverseStatus::Converged) << "node " << j;
      EXPECT_LE(std::fabs(b.sigma[j] - b.truth[j]), 2.0 * kTol) << "node " << j;
      EXPECT_EQ(b.evals[j], 1u) << "a seed on the root converges in one evaluation";
    }
  }
}

// Nonconvergence forced by the evaluation cap: NonConverged with the retained bracket
// enclosing the root and the best iterate in it.
TEST_F(IvBatch, TheEvaluationCapReturnsNonConvergedWithTheRetainedBracket) {
  for (const InverseIsa isa : {InverseIsa::Scalar, InverseIsa::Avx2}) {
    if (isa == InverseIsa::Avx2 && !avx2()) continue;
    Batch b(5);
    fill_spread(b, put(), 11u);
    for (std::size_t i = 0; i < 5; ++i) b.seed[i] = (i % 2 == 0) ? 0.85 : 0.09; // far seeds
    ib::InverseBatchIn in = b.in(put(), true, isa);
    in.max_evaluations = 2;
    ib::InverseScratch scratch;
    ib::InverseBatchWork work;
    ASSERT_EQ(ib::invert_many(in, b.out(), scratch, &work), InverseBatchError::None);
    for (std::size_t i = 0; i < 5; ++i) {
      ASSERT_EQ(b.status[i], InverseStatus::NonConverged) << "lane " << i;
      EXPECT_EQ(b.evals[i], 2u);
      EXPECT_LE(b.olo[i], b.truth[i]);
      EXPECT_GE(b.ohi[i], b.truth[i]);
      EXPECT_GE(b.sigma[i], b.olo[i]);
      EXPECT_LE(b.sigma[i], b.ohi[i]);
      EXPECT_TRUE(std::isfinite(b.res[i]));
      EXPECT_EQ(b.acc[i].kind, ib::AcceptanceKind::None);
    }
    EXPECT_EQ(work.by_status[static_cast<std::size_t>(InverseStatus::NonConverged)], 5u);
  }
}

// ISA dispatch: Avx2 required on a family source (no AVX2 kernel) is UnsupportedIsa on
// every active lane; the pure decision covers a host without AVX2.
TEST_F(IvBatch, UnsupportedIsaDispatchIsTyped) {
  Batch b(3);
  fill_spread(b, put(), 2u);
  b.active[2] = 0;
  ib::InverseScratch scratch;
  ASSERT_EQ(ib::invert_many(b.in(put(), false, InverseIsa::Avx2), b.out(), scratch),
            InverseBatchError::None);
  EXPECT_EQ(b.status[0], InverseStatus::UnsupportedIsa);
  EXPECT_EQ(b.status[1], InverseStatus::UnsupportedIsa);
  EXPECT_EQ(b.status[2], InverseStatus::Inactive);
  EXPECT_EQ(b.evals[0], 0u);
  // Table source, host without AVX2 (the pure decision): Avx2 -> false (UnsupportedIsa),
  // Auto -> scalar; a ForceScalar process override (auto_allows false) -> scalar.
  EXPECT_FALSE(ib::resolve_avx2(InverseIsa::Avx2, true, false, true));
  EXPECT_FALSE(ib::resolve_avx2(InverseIsa::Auto, true, false, true));
  EXPECT_TRUE(ib::resolve_avx2(InverseIsa::Auto, true, true, true));
  EXPECT_FALSE(ib::resolve_avx2(InverseIsa::Auto, true, true, false));
  EXPECT_FALSE(ib::resolve_avx2(InverseIsa::Auto, false, true, true));
  EXPECT_FALSE(ib::resolve_avx2(InverseIsa::Scalar, true, true, true));
  EXPECT_TRUE(ib::resolve_avx2(InverseIsa::Avx2, true, true, false));
}

// Determinism across batch order: every lane's outputs are a pure function of its own
// inputs (reversed and shuffled batches give each lane the same bits), on both ISAs.
TEST_F(IvBatch, ResultsAreIndependentOfBatchOrder) {
  for (const InverseIsa isa : {InverseIsa::Scalar, InverseIsa::Avx2}) {
    if (isa == InverseIsa::Avx2 && !avx2()) continue;
    constexpr std::size_t n = 23;
    Batch base(n);
    fill_spread(base, call(), 42u);
    ib::InverseScratch scratch;
    ASSERT_EQ(ib::invert_many(base.in(call(), true, isa), base.out(), scratch),
              InverseBatchError::None);
    std::vector<std::size_t> perm(n);
    std::iota(perm.begin(), perm.end(), std::size_t{0});
    for (const int round : {0, 1}) {
      if (round == 0) {
        std::reverse(perm.begin(), perm.end());
      } else {
        std::mt19937 rng(5u);
        std::shuffle(perm.begin(), perm.end(), rng);
      }
      Batch p(n);
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = perm[i];
        p.K[i] = base.K[j];
        p.target[i] = base.target[j];
        p.q[i] = base.q[j];
        p.is_call[i] = base.is_call[j];
        p.seed[i] = base.seed[j];
      }
      ASSERT_EQ(ib::invert_many(p.in(call(), true, isa), p.out(), scratch),
                InverseBatchError::None);
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = perm[i];
        EXPECT_TRUE(same_bits(p.sigma[i], base.sigma[j])) << "round " << round << " lane " << j;
        EXPECT_TRUE(same_bits(p.res[i], base.res[j]));
        EXPECT_EQ(p.evals[i], base.evals[j]);
        EXPECT_EQ(p.status[i], base.status[j]);
      }
    }
  }
}

// BID -> MID -> ASK: bid and ask lanes anchored to their mid start from its evaluated
// points (a bracket side and a Newton seed) and need fewer evaluations than from scratch;
// an ask above the box behind a mid above the box is settled with no evaluation.
TEST_F(IvBatch, AnchoredBidAndAskStartFromTheMidAndNeverRestart) {
  for (const InverseIsa isa : {InverseIsa::Scalar, InverseIsa::Avx2}) {
    if (isa == InverseIsa::Avx2 && !avx2()) continue;
    constexpr std::size_t quotes = 6;
    Batch b(3 * quotes + 2);
    Batch cold(3 * quotes);
    for (std::size_t k = 0; k < quotes; ++k) {
      const double K = 85.0 + 6.0 * static_cast<double>(k);
      const double s_mid = 0.2 + 0.05 * static_cast<double>(k);
      const double q = kQlo + 0.0001 * static_cast<double>(k);
      // mid (seeded far), bid and ask (no seed: the anchor provides it)
      set_lane(b, put(), 3 * k, K, s_mid, q, 0.25);
      set_lane(b, put(), 3 * k + 1, K, s_mid - 0.01, q, 0.0);
      set_lane(b, put(), 3 * k + 2, K, s_mid + 0.01, q, 0.0);
      b.seed[3 * k + 1] = kNaN;
      b.seed[3 * k + 2] = kNaN;
      b.anchor[3 * k + 1] = static_cast<std::int32_t>(3 * k);
      b.anchor[3 * k + 2] = static_cast<std::int32_t>(3 * k);
      for (std::size_t j = 0; j < 3; ++j) {
        set_lane(cold, put(), 3 * k + j, K, b.truth[3 * k + j], q, 0.25);
        cold.seed[3 * k + j] = kNaN;
      }
    }
    // A mid above the box and its ask (anchored): the ask is settled by monotonicity.
    const std::size_t m = 3 * quotes;
    set_lane(b, put(), m, 100.0, 0.5, 0.0205, 0.0);
    b.target[m] = 1.3 * family_price(put(), 100.0, kSigHi, 0.0205);
    set_lane(b, put(), m + 1, 100.0, 0.5, 0.0205, 0.0);
    b.target[m + 1] = 1.4 * family_price(put(), 100.0, kSigHi, 0.0205);
    b.anchor[m + 1] = static_cast<std::int32_t>(m);
    ib::InverseScratch scratch;
    ib::InverseBatchWork work;
    ASSERT_EQ(ib::invert_many(b.in(put(), true, isa), b.out(), scratch, &work),
              InverseBatchError::None);
    ib::InverseBatchWork cold_work;
    ASSERT_EQ(ib::invert_many(cold.in(put(), true, isa), cold.out(), scratch, &cold_work),
              InverseBatchError::None);
    unsigned anchored_evals = 0;
    unsigned cold_evals = 0;
    for (std::size_t k = 0; k < quotes; ++k) {
      for (std::size_t j = 0; j < 3; ++j) {
        const std::size_t i = 3 * k + j;
        ASSERT_EQ(b.status[i], InverseStatus::Converged) << "lane " << i;
        EXPECT_LE(std::fabs(b.sigma[i] - b.truth[i]), 2.0 * kTol);
        if (j > 0) {
          EXPECT_NE(b.flags[i] & ib::lane_flag::kAnchored, 0u);
          anchored_evals += b.evals[i];
          cold_evals += cold.evals[i];
        }
      }
    }
    EXPECT_LT(anchored_evals, cold_evals);
    EXPECT_EQ(b.status[m], InverseStatus::SigmaAboveBox);
    EXPECT_EQ(b.status[m + 1], InverseStatus::SigmaAboveBox);
    EXPECT_EQ(b.evals[m + 1], 0u);
    EXPECT_EQ(work.anchored, 2u * quotes + 1u);
    if (isa == InverseIsa::Avx2) {
      EXPECT_GE(work.tile_reuses, 2u * quotes); // a chain shares its mid's tile
    }
    // An anchor that is itself anchored, or names another contract, is refused.
    Batch bad(3);
    fill_spread(bad, put(), 8u);
    set_lane(bad, put(), 1, bad.K[0], 0.33, bad.q[0], 0.0);
    bad.anchor[1] = 0;
    bad.anchor[2] = 1; // anchored to an anchored lane
    ASSERT_EQ(ib::invert_many(bad.in(put(), true, isa), bad.out(), scratch),
              InverseBatchError::None);
    EXPECT_EQ(bad.status[1], InverseStatus::Converged);
    EXPECT_EQ(bad.status[2], InverseStatus::InvalidInput);
    Batch other(2);
    fill_spread(other, put(), 9u);
    other.anchor[1] = 0; // a different K
    ASSERT_EQ(ib::invert_many(other.in(put(), true, isa), other.out(), scratch),
              InverseBatchError::None);
    EXPECT_EQ(other.status[1], InverseStatus::InvalidInput);
  }
}

// A failed seed narrows, never discards: no seed -> the bracket midpoint (kSeedFailed); a seed
// above the box (the Black-76 IV of a deep in-the-money put) -> the box top is evaluated and
// Newton runs from there with the fused vega (kSeedClamped); a retained bracket is used.
TEST_F(IvBatch, FailedAndOutOfBoxSeedsConvergeFromTheBracket) {
  for (const InverseIsa isa : {InverseIsa::Scalar, InverseIsa::Avx2}) {
    if (isa == InverseIsa::Avx2 && !avx2()) continue;
    Batch b(4);
    set_lane(b, put(), 0, 100.0, 0.31, 0.0205, 0.0);
    b.seed[0] = kNaN;
    set_lane(b, put(), 1, 125.0, 0.6, 0.0205, 0.0);
    b.seed[1] = 2.5; // above the box top
    set_lane(b, put(), 2, 90.0, 0.2, 0.0205, 0.0);
    b.seed[2] = 0.01; // below the box bottom
    set_lane(b, put(), 3, 100.0, 0.4, 0.0205, 0.0);
    b.seed[3] = kNaN;
    b.blo[3] = 0.35;
    b.bhi[3] = 0.45; // a retained bracket (unevaluated ends)
    ib::InverseScratch scratch;
    ib::InverseBatchWork work;
    ASSERT_EQ(ib::invert_many(b.in(put(), true, isa), b.out(), scratch, &work),
              InverseBatchError::None);
    for (std::size_t i = 0; i < 4; ++i) {
      ASSERT_EQ(b.status[i], InverseStatus::Converged) << "lane " << i;
      EXPECT_LE(std::fabs(b.sigma[i] - b.truth[i]), 2.0 * kTol) << "lane " << i;
    }
    EXPECT_NE(b.flags[0] & ib::lane_flag::kSeedFailed, 0u);
    EXPECT_NE(b.flags[1] & ib::lane_flag::kSeedClamped, 0u);
    EXPECT_NE(b.flags[1] & ib::lane_flag::kBoxEdgeEvaluated, 0u);
    EXPECT_NE(b.flags[2] & ib::lane_flag::kSeedClamped, 0u);
    EXPECT_NE(b.flags[3] & ib::lane_flag::kRetainedBracket, 0u);
    EXPECT_GE(b.olo[3], 0.35);
    EXPECT_LE(b.ohi[3], 0.45);
    EXPECT_EQ(work.seed_failed, 2u);
    EXPECT_EQ(work.seed_clamped, 2u);
    // A WRONG retained bracket (root outside it, ends unevaluated) is widened back to the
    // box when the iterates probe it, and the lane still converges.
    Batch w(1);
    set_lane(w, put(), 0, 100.0, 0.6, 0.0205, 0.0);
    w.seed[0] = kNaN;
    w.blo[0] = 0.2;
    w.bhi[0] = 0.3;
    ASSERT_EQ(ib::invert_many(w.in(put(), true, isa), w.out(), scratch), InverseBatchError::None);
    EXPECT_EQ(w.status[0], InverseStatus::Converged);
    EXPECT_LE(std::fabs(w.sigma[0] - 0.6), 2.0 * kTol);
  }
}

// Low vega off the plateau: a far out-of-the-money call priced where its family vega is
// below the floor — the root is not identified by Newton; the lane bisects and ends
// LowVega (or converges on the bracket width) with the bracket retained, never a wrong root.
TEST_F(IvBatch, LowVegaRootsAreTypedAndKeepTheirBracket) {
  for (const InverseIsa isa : {InverseIsa::Scalar, InverseIsa::Avx2}) {
    if (isa == InverseIsa::Avx2 && !avx2()) continue;
    Batch b(1);
    set_lane(b, call(), 0, 190.0, 0.15, 0.0205, 0.0);
    const sq::FusedPriceVega pv = sq::price_and_vega(call().f, kS, 190.0, 0.15, 0.0205);
    ASSERT_EQ(pv.status, sq::FusedStatus::Ok);
    ASSERT_LT(pv.vega, ib::kInverseVegaFloorRel * 190.0) << "fixture must sit below the floor";
    ASSERT_GT(pv.price, 0.0);
    b.seed[0] = 0.5;
    ib::InverseScratch scratch;
    ASSERT_EQ(ib::invert_many(b.in(call(), true, isa), b.out(), scratch), InverseBatchError::None);
    const InverseStatus s = b.status[0];
    EXPECT_TRUE(s == InverseStatus::LowVega ||
                (s == InverseStatus::Converged &&
                 (b.flags[0] & ib::lane_flag::kBracketConverged) != 0u))
        << ib::inverse_status_name(s);
    EXPECT_LE(b.olo[0], 0.15 + 2.0 * kTol);
    EXPECT_GE(b.ohi[0], 0.15 - 2.0 * kTol);
  }
}

// The scratch: no growth on a batch it has seen; work counters add.
TEST_F(IvBatch, ScratchCapacityAndWorkCountersAccumulate) {
  ib::InverseScratch scratch;
  EXPECT_EQ(scratch.capacity(), 0u);
  scratch.reserve(12);
  EXPECT_GE(scratch.capacity(), 12u);
  Batch b(12);
  fill_spread(b, put(), 21u);
  ib::InverseBatchWork work;
  ASSERT_EQ(ib::invert_many(b.in(put(), true, InverseIsa::Auto), b.out(), scratch, &work),
            InverseBatchError::None);
  ASSERT_EQ(ib::invert_many(b.in(put(), true, InverseIsa::Auto), b.out(), scratch, &work),
            InverseBatchError::None);
  EXPECT_EQ(work.lanes, 24u);
  EXPECT_EQ(work.by_status[static_cast<std::size_t>(InverseStatus::Converged)], 24u);
  EXPECT_GE(work.evaluations, 24u);
  ib::InverseBatchWork sum;
  sum.add(work);
  sum.add(work);
  EXPECT_EQ(sum.evaluations, 2u * work.evaluations);
  EXPECT_EQ(sum.by_status[1], 48u);
}

} // namespace
