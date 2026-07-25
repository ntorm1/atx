// FIX-3/F3-A: the Greek Ok-stamp must mean the SAME thing on the laned route and on
// the scalar route.
//
// THE DEFECT. FIX-2/F2-B (c601504) guarded `detail::laned_greek_run`'s scatter, so a
// lane carrying a non-finite REQUESTED greek is demoted by status there. But
// `evaluate_resolved` -- the scalar routing BOTH `PricedSurface` and `PricedSurfaceView`
// take on `ForceScalar`, on a non-AVX2 host, on the FD route, and as the laned driver's
// own fallback -- was left unchanged: it returns a default-constructed (Ok) status
// whenever `greeks_resolved` yields a value, non-finite columns included
// (priced_surface.cpp:865-875, priced_surface_view.cpp:707-717). So the SAME lane with
// the SAME inputs is certified Ok on one ISA and demoted on another.
//
// That is the shape of the M1-1 defect this sprint already found and fixed once
// (unrequested-Greek zeroing present on the put batch and missing on the call batch,
// producing an ISA- and lane-dependent value in a live P&L column). This file pins the
// AGREEMENT property so a second instance cannot be reintroduced on either side.
//
// THE SEMANTICS (matched to c601504 / 740b040 / 9c3e1d0, not re-derived): guard the
// REQUESTED greek set only -- price/delta/gamma/theta always, vega/volga/vanna, rho and
// charm only when asked for -- and NORMALIZE an unrequested non-finite slot to its
// canonical unmaterialized 0.0 rather than letting it poison a downstream product
// (`g.rho * dr` is NaN even when dr is 0.0). Over-guarding vetoes good lanes; FIX-1/F3
// proved that half is a defect in its own right.
//
// WHY THE OVER-GUARD HALF MATTERS MORE HERE THAN IN THE LANED DRIVER. F2-B recorded
// honestly that the laned kernel honors its "unrequested greeks are left 0" contract, so
// a naive full-bundle guard was not reachable through it. That is NOT true of
// `evaluate_resolved`: its FD route (`american_greeks_fd`) and its cached-correction
// route both IGNORE `GreekNeeds` and return the full oracle bundle, so an UNREQUESTED
// column really can come back non-finite on this path. The over-guard case below is
// therefore a live input, not a synthetic one.
//
// THE TRIGGER. The same deep-ITM long-dated put FIX-1/F3 and FIX-2/F2-B used:
// S = 1e-8, K = 100, T = 1e7, sigma = 0.2, r = q = 0. The r+/r- differenced rho is
// non-finite while price/delta/gamma/theta/vega/volga/vanna/charm are all finite.

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/priced_surface_view.hpp"
#include "atx/vol/simd/cpu.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol {
namespace {

using EF = PricedSurface::EvalField;

constexpr double kTrigS = 1.0e-8; // deep ITM vs the K = 100 contract
constexpr double kTrigK = 100.0;
constexpr double kTrigT = 1.0e7; // long enough that the rho stencil goes non-finite
constexpr double kTrigSigma = 0.2;
constexpr std::int64_t kNow = 1700000000000000000LL;

// The full bundle request: exactly what a priced book asks for.
constexpr EF kGreekFields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid, double S, double r) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = kNow;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

// The FIX-1/F3 long-dated flat-smile surface (r == q_eff == 0 keeps F == S and the
// discount at 1.0, so sigma == sqrt(theta / T) exactly).
[[nodiscard]] PricedSurface make_long_dated(std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  EssviParams e{};
  e.theta = kTrigSigma * kTrigSigma * kTrigT;
  e.phi = 0.0;
  e.rho = 0.0;
  e.psi = 0.5;
  e.p = 0.5;
  e.lambda = 0.5;
  e.T = kTrigT;
  e.F = kTrigS;
  e.expiry_id = 0;
  cs.push(std::make_unique<EssviCurve>(e, 1.0));
  ctx.push_back(SliceContext{kTrigT, kTrigS, 0.0, 0.0, 250, 7});
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid, kTrigS, 0.0));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// An ordinary, entirely well-behaved surface: the regression pin.
[[nodiscard]] PricedSurface make_ordinary(std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < 3; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = 100.0 * std::exp((0.043 - 0.02) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-0.043 * T)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid, 100.0, 0.043));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

struct LaneOut {
  double iv{};
  double price{};
  AmericanGreeks greeks{};
  bool ok{false};
};

// One lane through `evaluate_batch` under an explicit ISA. `Auto` selects the laned
// analytic-Greek driver on an AVX2 host; `ForceScalar` takes the per-entry
// `evaluate_resolved` loop. Same surface, same inputs, same `needs` -- only the route
// differs, which is exactly the property under test.
template <class SurfaceT>
[[nodiscard]] LaneOut batch_lane(const SurfaceT &s, simd::SimdIsa isa, bool analytic,
                                 GreekNeeds needs) {
  const std::array<double, 1> K{kTrigK};
  const std::array<double, 1> T{kTrigT};
  const std::array<Side, 1> side{Side::Put};
  std::array<double, 1> iv{};
  std::array<double, 1> price{};
  std::array<AmericanGreeks, 1> greeks{};
  std::array<Status, 1> status{};
  const Status st = s.evaluate_batch(K, T, side, kGreekFields, analytic,
                                     typename SurfaceT::EvaluationSoA{iv, price, greeks, status,
                                                                      {},
                                                                      {}},
                                     isa, QueryExecution::ColdReference, needs);
  EXPECT_TRUE(st.has_value());
  LaneOut o;
  o.iv = iv[0];
  o.price = price[0];
  o.greeks = greeks[0];
  o.ok = status[0].has_value();
  return o;
}

// The archive round-trip is entirely in memory (no file IO, no temp dir).
[[nodiscard]] std::vector<std::byte> build_v2(const PricedSurface &ps, std::string_view symbol) {
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{symbol, &ps, std::nullopt}};
  auto built = write_surface_archive_v2(items);
  EXPECT_TRUE(built.has_value());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}

// ---------------------------------------------------------------------------------
// Precondition: the trigger really is finite-everything-but-rho on BOTH routes.
// ---------------------------------------------------------------------------------

TEST(EvaluateResolvedOkStamp, F3A_TriggerIsFiniteEverywhereExceptRho) {
  const PricedSurface ps = make_long_dated(1);

  const auto expect_shape = [](const AmericanGreeks &g, const char *route) {
    EXPECT_TRUE(std::isfinite(g.price)) << route;
    EXPECT_TRUE(std::isfinite(g.delta)) << route;
    EXPECT_TRUE(std::isfinite(g.gamma)) << route;
    EXPECT_TRUE(std::isfinite(g.theta)) << route;
    EXPECT_TRUE(std::isfinite(g.vega)) << route;
    EXPECT_TRUE(std::isfinite(g.volga)) << route;
    EXPECT_TRUE(std::isfinite(g.vanna)) << route;
    EXPECT_TRUE(std::isfinite(g.charm)) << route;
    EXPECT_FALSE(std::isfinite(g.rho)) << route << ": trigger no longer produces a non-finite rho";
  };

  const auto al = ps.greeks_analytic(kTrigK, kTrigT, Side::Put, QueryExecution::ColdReference,
                                     GreekNeeds{});
  ASSERT_TRUE(al.has_value()) << al.error().to_string();
  expect_shape(*al, "analytic AL");

  const auto fd = ps.greeks(kTrigK, kTrigT, Side::Put, QueryExecution::ColdReference);
  ASSERT_TRUE(fd.has_value()) << fd.error().to_string();
  expect_shape(*fd, "FD oracle");

  // The FD bundle above IS what `evaluate_resolved` sees on the FD route regardless of
  // `GreekNeeds` (american_greeks_fd takes no mask), so a non-finite rho reaches the
  // stamp even for a caller that never requested it. That is why the over-guard half is
  // reachable here and was not reachable through the laned kernel (F2-B recorded that).
  GreekNeeds no_rho{};
  no_rho.rho = false;
  const auto al_masked =
      ps.greeks_analytic(kTrigK, kTrigT, Side::Put, QueryExecution::ColdReference, no_rho);
  ASSERT_TRUE(al_masked.has_value());
  std::printf("[precondition] AL needs.rho=false -> rho=%g (finite=%d); FD ignores the mask -> "
              "rho=%g (finite=%d)\n",
              al_masked->rho, static_cast<int>(std::isfinite(al_masked->rho)), fd->rho,
              static_cast<int>(std::isfinite(fd->rho)));
}

// ---------------------------------------------------------------------------------
// F3-A, half 1: the scalar route must not stamp Ok on a non-finite REQUESTED greek.
// ---------------------------------------------------------------------------------

TEST(EvaluateResolvedOkStamp, F3A_ScalarEvaluateDoesNotStampOkOnANonFiniteRequestedGreek) {
  const PricedSurface ps = make_long_dated(1);
  const PricedSurface::FusedResult fr =
      ps.evaluate(kTrigK, kTrigT, Side::Put, kGreekFields, /*analytic=*/true,
                  QueryExecution::ColdReference, GreekNeeds{});

  ASSERT_TRUE(std::isfinite(fr.price)) << "precondition: the mark itself is finite";
  ASSERT_FALSE(std::isfinite(fr.greeks.rho))
      << "precondition: this lane must carry a non-finite REQUESTED rho";
  EXPECT_FALSE(fr.status.has_value())
      << "evaluate_resolved stamped Ok on a lane carrying a non-finite REQUESTED greek, so every "
         "ForceScalar / non-AVX2 / FD consumer sees a NaN greek on an Ok lane";
}

TEST(EvaluateResolvedOkStamp, F3A_ViewScalarEvaluateDoesNotStampOkOnANonFiniteRequestedGreek) {
  const PricedSurface ps = make_long_dated(1);
  auto arch = SurfaceArchiveV2::open(build_v2(ps, "trg"));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto v = arch->map_symbol("trg");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  const PricedSurfaceView::FusedResult fr =
      v->evaluate(kTrigK, kTrigT, Side::Put, kGreekFields, /*analytic=*/true,
                  QueryExecution::ColdReference, GreekNeeds{});

  ASSERT_TRUE(std::isfinite(fr.price));
  ASSERT_FALSE(std::isfinite(fr.greeks.rho));
  EXPECT_FALSE(fr.status.has_value())
      << "the mapped-archive replay path stamps Ok on a lane carrying a non-finite REQUESTED greek";
}

// ---------------------------------------------------------------------------------
// F3-A, the property that actually matters: the two routes must AGREE.
// ---------------------------------------------------------------------------------

TEST(EvaluateResolvedOkStamp, F3A_LanedAndScalarRoutesAgreeOnTheOkStamp) {
  const PricedSurface ps = make_long_dated(1);
  const LaneOut laned = batch_lane(ps, simd::SimdIsa::Auto, /*analytic=*/true, GreekNeeds{});
  const LaneOut scalar =
      batch_lane(ps, simd::SimdIsa::ForceScalar, /*analytic=*/true, GreekNeeds{});

  ASSERT_FALSE(std::isfinite(scalar.greeks.rho)) << "precondition: the scalar lane carries NaN rho";
  EXPECT_EQ(laned.ok, scalar.ok)
      << "the same lane with the same inputs is certified Ok on one ISA and demoted on the other "
         "(laned ok=" << laned.ok << ", scalar ok=" << scalar.ok << ")";
  EXPECT_FALSE(scalar.ok) << "and the agreed-on answer must be the demotion, not the Ok";
  EXPECT_FALSE(laned.ok);
}

TEST(EvaluateResolvedOkStamp, F3A_ViewLanedAndScalarRoutesAgreeOnTheOkStamp) {
  const PricedSurface ps = make_long_dated(1);
  auto arch = SurfaceArchiveV2::open(build_v2(ps, "trg"));
  ASSERT_TRUE(arch.has_value());
  auto v = arch->map_symbol("trg");
  ASSERT_TRUE(v.has_value());

  const LaneOut laned = batch_lane(*v, simd::SimdIsa::Auto, /*analytic=*/true, GreekNeeds{});
  const LaneOut scalar =
      batch_lane(*v, simd::SimdIsa::ForceScalar, /*analytic=*/true, GreekNeeds{});

  EXPECT_EQ(laned.ok, scalar.ok) << "view: Ok-stamp is ISA-dependent";
  EXPECT_FALSE(scalar.ok);
  EXPECT_FALSE(laned.ok);
}

// ---------------------------------------------------------------------------------
// F3-A, half 2 (the FIX-1/F3 mirror image): a column the caller never requested must
// NOT veto the lane, and must not be handed to the consumer as a NaN either.
//
// The FD route is the reachable case: it ignores GreekNeeds and materializes rho, so a
// full-bundle guard here would demote a lane on a column that cannot reach any output.
// ---------------------------------------------------------------------------------

TEST(EvaluateResolvedOkStamp, F3A_UnrequestedNonFiniteGreekDoesNotVetoAndIsNormalized) {
  const PricedSurface ps = make_long_dated(1);
  GreekNeeds needs{};
  needs.rho = false; // the K4 first-order tier / an ordinary dr == 0.0 P&L step

  const PricedSurface::FusedResult fr =
      ps.evaluate(kTrigK, kTrigT, Side::Put, kGreekFields, /*analytic=*/false,
                  QueryExecution::ColdReference, needs);

  EXPECT_TRUE(fr.status.has_value())
      << "the lane was vetoed on rho -- a column the caller never requested (FIX-1/F3: "
         "over-guarding is its own defect)";
  EXPECT_TRUE(std::isfinite(fr.greeks.rho))
      << "an UNREQUESTED non-finite rho was passed through to the consumer; NaN * 0.0 is NaN, so "
         "it poisons the downstream product rather than vanishing";

  // Every column the caller DID ask for is bit-identical to the unmasked oracle.
  const auto ref = ps.greeks(kTrigK, kTrigT, Side::Put, QueryExecution::ColdReference);
  ASSERT_TRUE(ref.has_value());
  EXPECT_TRUE(bits_equal(fr.price, ref->price));
  EXPECT_TRUE(bits_equal(fr.greeks.price, ref->price));
  EXPECT_TRUE(bits_equal(fr.greeks.delta, ref->delta));
  EXPECT_TRUE(bits_equal(fr.greeks.gamma, ref->gamma));
  EXPECT_TRUE(bits_equal(fr.greeks.theta, ref->theta));
  EXPECT_TRUE(bits_equal(fr.greeks.vega, ref->vega));
  EXPECT_TRUE(bits_equal(fr.greeks.volga, ref->volga));
  EXPECT_TRUE(bits_equal(fr.greeks.vanna, ref->vanna));
  EXPECT_TRUE(bits_equal(fr.greeks.charm, ref->charm));
}

// ---------------------------------------------------------------------------------
// Regression pin: an ordinary lane is untouched by the guard, on both routes.
// ---------------------------------------------------------------------------------

TEST(EvaluateResolvedOkStamp, F3A_OrdinaryLaneStillStampsOkBitIdentically) {
  const PricedSurface ps = make_ordinary(9);
  const double K = 100.0;
  const double T = 0.15;

  const PricedSurface::FusedResult fr = ps.evaluate(
      K, T, Side::Put, kGreekFields, /*analytic=*/true, QueryExecution::ColdReference, GreekNeeds{});
  const auto ref =
      ps.greeks_analytic(K, T, Side::Put, QueryExecution::ColdReference, GreekNeeds{});
  ASSERT_TRUE(ref.has_value());

  EXPECT_TRUE(fr.status.has_value()) << "an ordinary lane must still be stamped Ok";
  EXPECT_TRUE(bits_equal(fr.price, ref->price));
  EXPECT_TRUE(bits_equal(fr.greeks.delta, ref->delta));
  EXPECT_TRUE(bits_equal(fr.greeks.gamma, ref->gamma));
  EXPECT_TRUE(bits_equal(fr.greeks.theta, ref->theta));
  EXPECT_TRUE(bits_equal(fr.greeks.vega, ref->vega));
  EXPECT_TRUE(bits_equal(fr.greeks.rho, ref->rho));
  EXPECT_TRUE(bits_equal(fr.greeks.volga, ref->volga));
  EXPECT_TRUE(bits_equal(fr.greeks.vanna, ref->vanna));
  EXPECT_TRUE(bits_equal(fr.greeks.charm, ref->charm));

  // And the FD route on the same lane, likewise unchanged.
  const PricedSurface::FusedResult fd = ps.evaluate(
      K, T, Side::Put, kGreekFields, /*analytic=*/false, QueryExecution::ColdReference,
      GreekNeeds{});
  const auto fd_ref = ps.greeks(K, T, Side::Put, QueryExecution::ColdReference);
  ASSERT_TRUE(fd_ref.has_value());
  EXPECT_TRUE(fd.status.has_value());
  EXPECT_TRUE(bits_equal(fd.price, fd_ref->price));
  EXPECT_TRUE(bits_equal(fd.greeks.rho, fd_ref->rho));
}

} // namespace
} // namespace atx::vol
