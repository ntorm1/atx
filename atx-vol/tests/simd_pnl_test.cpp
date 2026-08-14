// Parity gate for the vectorized (AVX2) Taylor P&L-explain batch kernel.
//
// pnl_taylor_explain_batch dispatches to the 4-lane AVX2 path when the host
// supports it (this CI/dev box does). These tests assert that the vectorized
// result reproduces an independent scalar reference of the exact decomposition
// PortfolioPricer::pnl_explain uses — across a broad grid of Greek/shock rows
// (positive/negative/zero, with and without a position weight), every scalar-tail
// residue (n % 4), and the "components sum to total" invariant. The math is pure
// per-position arithmetic with no reduction across positions, so the vector kernel
// owes the scalar loop BIT-identity, not a tolerance: a position's P&L must not
// depend on the dispatched route or on its index within the batch (item 2.6).

#include "simd/pnl_batch.hpp"

#include "atx/vol/api/simd/cpu.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol::simd {
namespace {

// A book of per-position Greeks + state moves, SoA. Values span sign and
// magnitude (long/short qty, up/down moves, zero shocks) to exercise every term.
struct Book {
  std::vector<double> delta, gamma, vega, volga, vanna, theta, rho, charm;
  std::vector<double> qty, dS, dSigma, dt, dr;
  [[nodiscard]] std::size_t size() const { return delta.size(); }
};

Book make_book() {
  Book b;
  // Deterministic, varied rows: a handful of ladders combined so no two rows
  // share all columns, including exact-zero shocks and short (negative) qty.
  const double deltas[] = {-0.95, -0.4, 0.0, 0.35, 0.8};
  const double gammas[] = {0.0, 0.02, 0.15, 0.5};
  const double vegas[] = {0.0, 3.5, 18.0, 42.0};
  const double dss[] = {-18.0, -2.5, 0.0, 4.0, 21.0};
  const double dsigs[] = {-0.25, 0.0, 0.08, 0.3};
  const double qtys[] = {-750.0, -1.0, 1.0, 120.0};
  std::size_t k = 0;
  for (double dl : deltas)
    for (double gm : gammas)
      for (double vg : vegas)
        for (double ds : dss)
          for (double dv : dsigs)
            for (double q : qtys) {
              // Derive the remaining Greeks/moves deterministically from k so the
              // cross terms (vanna·dS·dSigma, charm·dS·dt) get real coverage.
              const double phase = static_cast<double>(k);
              b.delta.push_back(dl);
              b.gamma.push_back(gm);
              b.vega.push_back(vg);
              b.volga.push_back(-90.0 + std::fmod(phase * 7.0, 180.0));
              b.vanna.push_back(-40.0 + std::fmod(phase * 3.0, 80.0));
              b.theta.push_back(-45.0 + std::fmod(phase * 5.0, 45.0));
              b.rho.push_back(-80.0 + std::fmod(phase * 11.0, 160.0));
              b.charm.push_back(-8.0 + std::fmod(phase * 2.0, 16.0));
              b.qty.push_back(q);
              b.dS.push_back(ds);
              b.dSigma.push_back(dv);
              b.dt.push_back(-0.04 + std::fmod(phase * 0.003, 0.05));
              b.dr.push_back(-0.015 + std::fmod(phase * 0.001, 0.02));
              ++k;
            }
  return b;
}

// Independent scalar reference: the exact term-for-term decomposition, weighted.
struct Row {
  double delta_pnl, gamma_pnl, vega_pnl, volga_pnl, vanna_pnl;
  double theta_pnl, rho_pnl, charm_pnl, total;
};

Row reference_row(const Book& b, std::size_t i, bool weighted) {
  const double w = weighted ? b.qty[i] : 1.0;
  const double dS = b.dS[i];
  const double dv = b.dSigma[i];
  const double dt = b.dt[i];
  const double dr = b.dr[i];
  const double pd = b.delta[i] * dS;
  const double pg = 0.5 * b.gamma[i] * dS * dS;
  const double pv = b.vega[i] * dv;
  const double pvol = 0.5 * b.volga[i] * dv * dv;
  const double pvanna = b.vanna[i] * dS * dv;
  const double pth = b.theta[i] * dt;
  const double prho = b.rho[i] * dr;
  const double pcharm = b.charm[i] * dS * dt;
  const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;
  return {w * pd,   w * pg,     w * pv,   w * pvol, w * pvanna,
          w * pth,  w * prho,   w * pcharm, w * explained};
}

PnlExplainInputs make_inputs(const Book& b, bool weighted) {
  PnlExplainInputs in{};
  in.delta = b.delta.data();
  in.gamma = b.gamma.data();
  in.vega = b.vega.data();
  in.volga = b.volga.data();
  in.vanna = b.vanna.data();
  in.theta = b.theta.data();
  in.rho = b.rho.data();
  in.charm = b.charm.data();
  in.qty = weighted ? b.qty.data() : nullptr;
  in.dS = b.dS.data();
  in.dSigma = b.dSigma.data();
  in.dt = b.dt.data();
  in.dr = b.dr.data();
  return in;
}

struct Outs {
  std::vector<double> delta_pnl, gamma_pnl, vega_pnl, volga_pnl, vanna_pnl;
  std::vector<double> theta_pnl, rho_pnl, charm_pnl, total;
  explicit Outs(std::size_t n)
      : delta_pnl(n), gamma_pnl(n), vega_pnl(n), volga_pnl(n), vanna_pnl(n),
        theta_pnl(n), rho_pnl(n), charm_pnl(n), total(n) {}
  PnlExplainOutputs view() {
    return PnlExplainOutputs{delta_pnl.data(), gamma_pnl.data(), vega_pnl.data(),
                             volga_pnl.data(), vanna_pnl.data(), theta_pnl.data(),
                             rho_pnl.data(),   charm_pnl.data(), total.data()};
  }
};

// Combined absolute+relative closeness: the ORIGINAL, deliberately route-agnostic
// gate — a tight relative bound with an absolute floor for the cancellation lanes.
// Kept as-is (rather than tightened to ==) so these grids still pass on a future
// kernel that legitimately re-associates; the bit-identity contract is asserted
// separately by the three 2.6 tests below.
void expect_close(double got, double want, const char* col, std::size_t i) {
  constexpr double kAbs = 1e-7;
  constexpr double kRel = 1e-12;
  EXPECT_LE(std::abs(got - want), kAbs + kRel * std::abs(want))
      << "col=" << col << " i=" << i << " got=" << got << " want=" << want;
}

void check_against_reference(const Book& b, bool weighted) {
  const std::size_t n = b.size();
  const PnlExplainInputs in = make_inputs(b, weighted);
  Outs o(n);
  PnlExplainOutputs out = o.view();
  pnl_taylor_explain_batch(in, out, n);

  double max_sum_err = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Row r = reference_row(b, i, weighted);
    expect_close(o.delta_pnl[i], r.delta_pnl, "delta", i);
    expect_close(o.gamma_pnl[i], r.gamma_pnl, "gamma", i);
    expect_close(o.vega_pnl[i], r.vega_pnl, "vega", i);
    expect_close(o.volga_pnl[i], r.volga_pnl, "volga", i);
    expect_close(o.vanna_pnl[i], r.vanna_pnl, "vanna", i);
    expect_close(o.theta_pnl[i], r.theta_pnl, "theta", i);
    expect_close(o.rho_pnl[i], r.rho_pnl, "rho", i);
    expect_close(o.charm_pnl[i], r.charm_pnl, "charm", i);
    expect_close(o.total[i], r.total, "total", i);

    // The eight components sum to the total (Taylor-explained) P&L.
    const double sum = o.delta_pnl[i] + o.gamma_pnl[i] + o.vega_pnl[i] +
                       o.volga_pnl[i] + o.vanna_pnl[i] + o.theta_pnl[i] +
                       o.rho_pnl[i] + o.charm_pnl[i];
    const double sum_err = std::abs(sum - o.total[i]);
    max_sum_err = std::max(max_sum_err, sum_err);
    EXPECT_LE(sum_err, 1e-7 + 1e-10 * std::abs(o.total[i]))
        << "sum!=total i=" << i << " sum=" << sum << " total=" << o.total[i];
  }
  EXPECT_LT(max_sum_err, 1e-6);
}

// RAII: restore Auto dispatch after a test pins the ISA (mirrors simd_american_test).
struct IsaGuard {
  ~IsaGuard() { set_simd_isa_override(SimdIsa::Auto); }
};

// Bitwise compare every output column of two runs over the same rows.
void expect_bit_identical(const Outs& got, const Outs& want, std::size_t n,
                          const char* what) {
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_EQ(got.delta_pnl[i], want.delta_pnl[i]) << what << " col=delta i=" << i;
    ASSERT_EQ(got.gamma_pnl[i], want.gamma_pnl[i]) << what << " col=gamma i=" << i;
    ASSERT_EQ(got.vega_pnl[i], want.vega_pnl[i]) << what << " col=vega i=" << i;
    ASSERT_EQ(got.volga_pnl[i], want.volga_pnl[i]) << what << " col=volga i=" << i;
    ASSERT_EQ(got.vanna_pnl[i], want.vanna_pnl[i]) << what << " col=vanna i=" << i;
    ASSERT_EQ(got.theta_pnl[i], want.theta_pnl[i]) << what << " col=theta i=" << i;
    ASSERT_EQ(got.rho_pnl[i], want.rho_pnl[i]) << what << " col=rho i=" << i;
    ASSERT_EQ(got.charm_pnl[i], want.charm_pnl[i]) << what << " col=charm i=" << i;
    ASSERT_EQ(got.total[i], want.total[i]) << what << " col=total i=" << i;
  }
}

Outs run_at(const Book& b, bool weighted, std::size_t n) {
  Outs o(n);
  PnlExplainOutputs out = o.view();
  pnl_taylor_explain_batch(make_inputs(b, weighted), out, n);
  return o;
}

TEST(SimdPnlBatch, MatchesScalarWeighted) {
  check_against_reference(make_book(), /*weighted=*/true);
}

TEST(SimdPnlBatch, MatchesScalarUnweightedNullQty) {
  check_against_reference(make_book(), /*weighted=*/false);
}

// The scalar tail (n % 4 != 0) must be handled for every residue class.
TEST(SimdPnlBatch, HandlesEveryTailResidue) {
  const Book b = make_book();
  for (std::size_t n = 1; n <= 11; ++n) {
    const PnlExplainInputs in = make_inputs(b, /*weighted=*/true);
    Outs o(n);
    PnlExplainOutputs out = o.view();
    pnl_taylor_explain_batch(in, out, n);
    for (std::size_t i = 0; i < n; ++i) {
      const Row r = reference_row(b, i, /*weighted=*/true);
      expect_close(o.total[i], r.total, "total", i);
      expect_close(o.gamma_pnl[i], r.gamma_pnl, "gamma", i);
      expect_close(o.charm_pnl[i], r.charm_pnl, "charm", i);
    }
  }
}

TEST(SimdPnlBatch, ZeroLengthIsNoOp) {
  double sentinel = 42.0;
  PnlExplainInputs in{};
  PnlExplainOutputs out{};
  out.total = &sentinel;
  pnl_taylor_explain_batch(in, out, 0);
  // Null input columns are never dereferenced when n == 0.
  EXPECT_EQ(sentinel, 42.0);
}

TEST(SimdPnlBatch, Avx2AvailabilityReported) {
  // Documents which path the parity ran on (informational; scalar is also valid).
  SUCCEED() << "have_avx2=" << (have_avx2() ? 1 : 0);
}

// ── 2.6: one association tree, whatever the route or the batch index ──────
//
// pnl_batch.hpp:60-62 documents the eight component columns as summing, per
// position, to `total`. With qty == nullptr the position weight is exactly 1.0, so
// the scaling multiply is exact and that contract sharpens into a BIT-EXACT
// identity: `total` must BE the left-to-right eight-term sum of the very numbers
// stored in the component columns. RED before the fix on the AVX2 route, which
// accumulated `total` as an FMA chain over the (coefficient, move) products — every
// term entering the sum unrounded, so `total` was not the sum of the rounded
// components it stored beside it.
TEST(SimdPnlBatch, UnweightedRoute_TotalIsExactlyTheComponentSum) {
  const Book b = make_book();
  const std::size_t n = b.size();
  for (const SimdIsa mode : {SimdIsa::ForceScalar, SimdIsa::ForceAvx2}) {
    if (mode == SimdIsa::ForceAvx2 && !have_avx2()) {
      continue;
    }
    IsaGuard g;
    set_simd_isa_override(mode);
    const Outs o = run_at(b, /*weighted=*/false, n);
    for (std::size_t i = 0; i < n; ++i) {
      const double sum = o.delta_pnl[i] + o.gamma_pnl[i] + o.vega_pnl[i] +
                         o.volga_pnl[i] + o.vanna_pnl[i] + o.theta_pnl[i] +
                         o.rho_pnl[i] + o.charm_pnl[i];
      ASSERT_EQ(sum, o.total[i])
          << "isa=" << static_cast<int>(mode) << " i=" << i << " sum=" << sum
          << " total=" << o.total[i];
    }
  }
}

// A position's P&L must not depend on WHERE it sits in the batch. The AVX2 route
// splits a batch into 4-lane vector bodies plus an n % 4 scalar tail, so the same
// row has to produce the same bits either way. RED before the fix: the vector body
// grouped the second-order products differently from the scalar tail (½γ·(dS·dS)
// vs ((½γ)·dS)·dS, and the FMA-chain total), so a row's P&L moved when the batch
// length changed under it.
TEST(SimdPnlBatch, SameRow_VectorBodyAndScalarTail_BitIdentical) {
  if (!have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  IsaGuard g;
  set_simd_isa_override(SimdIsa::ForceAvx2);

  const Book b = make_book();
  const std::size_t n_all = b.size();
  ASSERT_EQ(n_all % 4, 0u) << "the book must be a whole number of vector packs";
  const Outs body = run_at(b, /*weighted=*/true, n_all);

  // n = 4k + j leaves the LAST j rows on the scalar tail; sweeping n over two full
  // packs covers every residue class for every row position within a pack.
  for (std::size_t n = 1; n <= 11; ++n) {
    const Outs o = run_at(b, /*weighted=*/true, n);
    ASSERT_NO_FATAL_FAILURE(expect_bit_identical(o, body, n, "body-vs-tail"));
  }
}

// The plan's exit criterion: a scalar-vs-AVX2 P&L identity. The scalar loop is the
// documented numerical source of truth (pnl_batch.cpp), so the vector kernel owes
// it bit-identity, not just a tolerance — the decomposition is pure multiplies and
// adds with no reduction across positions, so there is nothing to trade away.
TEST(SimdPnlBatch, Avx2Route_IsBitIdenticalToScalarRoute) {
  if (!have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  const Book b = make_book();
  const std::size_t n = b.size();
  for (const bool weighted : {false, true}) {
    Outs scalar_run(n);
    Outs avx2_run(n);
    {
      IsaGuard g;
      set_simd_isa_override(SimdIsa::ForceScalar);
      scalar_run = run_at(b, weighted, n);
    }
    {
      IsaGuard g;
      set_simd_isa_override(SimdIsa::ForceAvx2);
      avx2_run = run_at(b, weighted, n);
    }
    ASSERT_NO_FATAL_FAILURE(expect_bit_identical(
        avx2_run, scalar_run, n, weighted ? "weighted" : "unweighted"));
  }
}

} // namespace
} // namespace atx::vol::simd
