// TheoEngine unit suite (THEO-7) -- the identity-semantics gate: with zero
// overlays engaged, every TheoValue this engine produces is bit-for-bit
// identical to what the served PricedSurface itself reports.
//
// Fixture: the deterministic synthetic SPY-like index surface (spy_fixture.hpp
// / panel.hpp), fit once per test suite via VolaSession::from_frame + Fast
// preset (mirrors examples/spy_surface_bench.cpp's build), the same fixture
// tests/breakeven_test.cpp's BevPathLoader section is built on. A second,
// much cheaper directly-constructed surface (mirrors
// contract_projection_test.cpp's `make_surface`) is used where a specific
// `QueryPricingTier` needs to be prepared (FastTierRoute tests below) --
// fitting the SPY fixture through a fast tier is unnecessary machinery for
// that.

#include "atx/vol/theo.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/event_vol.hpp" // EventSchedule, censored_total_variance, event_recombined_vol, count_events_at
#include "atx/vol/panel.hpp" // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/query_pricing.hpp"  // QueryPricingTier
#include "atx/vol/realized_vol.hpp"   // RvPanel
#include "atx/vol/session.hpp"        // VolaSession, SessionInputs, FitPreset
#include "atx/vol/spy_fixture.hpp"    // make_spy_synthetic_spec, make_spy_session_inputs
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/types.hpp"          // Side
#include "atx/vol/vol_curve.hpp"      // CurveSurface, EssviParams, EssviCurve
#include "atx/vol/vol_time.hpp"       // ns_from_year_fraction

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

// ── Test-only overlay stubs ──────────────────────────────────────────────

// A fixed vol-space (dvol, band) adjustment for every query.
class ConstantAdjustOverlay final : public ITheoOverlay {
public:
  ConstantAdjustOverlay(double dvol, double band) : dvol_(dvol), band_(band) {}

  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_constant_adjust_overlay";
  }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = OverlayAdjust{.dvol = dvol_, .band = band_};
    }
    return Ok();
  }

private:
  double dvol_;
  double band_;
};

// Always fails: proves a non-OK overlay adjust() fails the whole batch.
class FailingOverlay final : public ITheoOverlay {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "test_failing_overlay"; }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> /*queries*/,
                              std::span<OverlayAdjust> /*out*/) const override {
    return Err(ErrorCode::Internal, "test_failing_overlay: deliberate failure");
  }
};

// Always returns a non-finite dvol: proves the engine fails loud (M3) rather
// than silently clamping NaN/Inf into theo_vol.
class NonFiniteDvolOverlay final : public ITheoOverlay {
public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_non_finite_dvol_overlay";
  }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = OverlayAdjust{.dvol = std::numeric_limits<double>::quiet_NaN(), .band = 0.0};
    }
    return Ok();
  }
};

// Deterministic dvol as a pure function of the query's strike alone (never
// the query's position within a chunk) -- shared between the stub below and
// the test's own independent expectation recomputation, so the two can never
// silently drift apart.
[[nodiscard]] double strike_scaled_dvol(const TheoQuery &q, double scale) noexcept {
  return scale * q.strike;
}

class StrikeScaledDvolOverlay final : public ITheoOverlay {
public:
  explicit StrikeScaledDvolOverlay(double scale) : scale_(scale) {}

  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_strike_scaled_dvol_overlay";
  }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = OverlayAdjust{.dvol = strike_scaled_dvol(queries[i], scale_), .band = 0.0};
    }
    return Ok();
  }

private:
  double scale_;
};

// A large dvol on exactly one targeted strike, a small (unclamped) dvol on
// every other query -- proves OverlayClamped is per-query, not batch-wide.
class StrikeTargetedDvolOverlay final : public ITheoOverlay {
public:
  StrikeTargetedDvolOverlay(double target_strike, double target_dvol, double other_dvol)
      : target_strike_(target_strike), target_dvol_(target_dvol), other_dvol_(other_dvol) {}

  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_strike_targeted_dvol_overlay";
  }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      const double dvol = (queries[i].strike == target_strike_) ? target_dvol_ : other_dvol_;
      out[i] = OverlayAdjust{.dvol = dvol, .band = 0.0};
    }
    return Ok();
  }

private:
  double target_strike_;
  double target_dvol_;
  double other_dvol_;
};

// Sets ModelMissing on one targeted query, zero flags elsewhere -- proves
// OverlayAdjust::flags (Task 8) propagates into TheoValue::flags per-query,
// the same isolation contract OverlayClamped already has (see
// ClampedQueryFlagDoesNotLeakToNeighbors below).
class FlagOnTargetedQueryOverlay final : public ITheoOverlay {
public:
  explicit FlagOnTargetedQueryOverlay(double target_strike) : target_strike_(target_strike) {}

  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_flag_on_targeted_query_overlay";
  }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      const std::uint32_t flags = (queries[i].strike == target_strike_)
                                      ? static_cast<std::uint32_t>(TheoFlagBits::ModelMissing)
                                      : 0u;
      out[i] = OverlayAdjust{.dvol = 0.0, .band = 0.0, .flags = flags};
    }
    return Ok();
  }

private:
  double target_strike_;
};

// ── A cheap, directly-constructed fast-tier-prepared surface ────────────────
//
// Mirrors contract_projection_test.cpp's `make_surface`: a raw synthetic
// eSSVI CurveSurface, no VolaSession fit, no OPRA/panel machinery -- the
// cheapest working recipe to get a `PricedSurface` and then prepare it onto
// `QueryPricingTier::RepresentativeFast` for the FastTierRoute tests (I1).

// Returns a `Result` (not a bare `PricedSurface`) precisely so callers can
// `ASSERT_TRUE` on it -- a free function returning a value type cannot itself
// use gtest's fatal `ASSERT_*` macros (they expand to a bare `return;`, which
// only compiles in a `void`-returning function).
[[nodiscard]] Result<PricedSurface> make_fast_tier_surface() {
  constexpr double kSpot = 100.0;
  constexpr double kRate = 0.043;
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.05, 0.25, 1.00}) {
    EssviParams parameters{};
    parameters.theta = 0.03 + 0.01 * term;
    parameters.phi = 1.3;
    parameters.rho = -0.3;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = kSpot;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, kSpot, 0.0, 0.0, 50, 0});
  }
  PricingContext pricing;
  pricing.S = kSpot;
  pricing.r = kRate;
  pricing.now_ts_ns = 1'700'000'000'000'000'000LL;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = 99;
  auto surface = PricedSurface::create(std::move(curves), std::move(context), pricing);
  if (!surface.has_value()) {
    return Err(surface.error());
  }
  return std::move(*surface).with_query_pricing(QueryPricingTier::RepresentativeFast);
}

// ── A minimal flat-ATM-vol surface (Task 8 RvBlend arithmetic tests) ────────
//
// One eSSVI slice per tenor in `tenors`, each tuned so its ATM (K == F ==
// spot, i.e. log-moneyness k == 0) implied vol is exactly `sigma0`: eSSVI's
// `theta` parameter IS the ATM total variance by construction
// (`essvi_total_w(slice, 0) == theta` whenever `resid_scale <= 0`, the
// default here and in `make_fast_tier_surface` above -- vol_surface.hpp), so
// `theta = sigma0^2 * T` makes `iv(spot, T) == sigma0` regardless of the
// wing shape (phi/rho/psi/p/lambda only matter away from k == 0). Lets the
// RvBlend arithmetic/monotonicity tests control `market_vol` exactly instead
// of reading whatever the SPY fixture's fitted term structure happens to
// produce at a given tenor.
[[nodiscard]] Result<PricedSurface> make_flat_vol_surface(double sigma0,
                                                          std::span<const double> tenors) {
  constexpr double kSpot = 100.0;
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : tenors) {
    EssviParams parameters{};
    parameters.theta = sigma0 * sigma0 * term;
    parameters.phi = 1.3;
    parameters.rho = -0.3;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = kSpot;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, /*df=*/1.0));
    context.push_back(SliceContext{term, kSpot, 0.0, 0.0, 50, 0});
  }
  PricingContext pricing;
  pricing.S = kSpot;
  pricing.r = 0.0;
  pricing.now_ts_ns = 1'700'000'000'000'000'000LL;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = 101;
  return PricedSurface::create(std::move(curves), std::move(context), pricing);
}

// ── SPY fixture: one PricedSurface, built once per suite ────────────────────

class TheoEngineTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    const SynthPanelSpec spec = make_spy_synthetic_spec();
    const auto panel = make_synthetic_american_panel(spec);
    ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
    const SessionInputs in = make_spy_session_inputs(spec, FitPreset::Fast);
    auto sess = VolaSession::from_frame(panel->frame, in);
    ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
    auto ps = sess->to_priced_surface();
    ASSERT_TRUE(ps.has_value()) << ps.error().to_string();
    surface_ = std::move(*ps);
    // M4: every test below indexes context()[1]/[3] unchecked; guard the one
    // shared construction point instead of repeating the check per test.
    ASSERT_GE(surface_->context().size(), std::size_t{4})
        << "fixture must have at least 4 fitted slices for context()[1]/[3] use below";
  }

  static std::optional<PricedSurface> surface_;
};

std::optional<PricedSurface> TheoEngineTest::surface_{};

// ── (a) identity: zero overlays reproduces the surface exactly ─────────────

TEST_F(TheoEngineTest, EngineWithNoOverlaysReproducesSurfaceExactly) {
  auto engine = TheoEngine::create({});
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};

  const std::array<double, 3> strikes{580.0, 600.0, 620.0};
  const std::array<double, 2> tenors{surface_->context()[1].T, surface_->context()[3].T};
  const std::array<Side, 2> sides{Side::Call, Side::Put};

  for (const double K : strikes) {
    for (const double T : tenors) {
      for (const Side side : sides) {
        const TheoQuery q{.strike = K, .tenor_years = T, .side = side};
        const auto v = engine->value(ctx, q);
        ASSERT_TRUE(v.has_value()) << v.error().to_string();

        const double expected_vol = surface_->iv(K, T);
        const auto expected_price = surface_->fair_value(K, T, side);
        ASSERT_TRUE(expected_price.has_value()) << expected_price.error().to_string();

        EXPECT_DOUBLE_EQ(v->market_vol, expected_vol);
        EXPECT_DOUBLE_EQ(v->theo_vol, expected_vol);
        EXPECT_DOUBLE_EQ(v->market_price, *expected_price);
        EXPECT_DOUBLE_EQ(v->theo_price, *expected_price);
        EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
        EXPECT_EQ(v->flags, 0u);
      }
    }
  }
}

// ── (b) null surface is rejected ────────────────────────────────────────────

TEST_F(TheoEngineTest, NullSurfaceIsRejected) {
  auto engine = TheoEngine::create({});
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{}; // surface == nullptr
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::InvalidArgument);
}

// ── (c) short out span is rejected before any mutation ──────────────────────

TEST_F(TheoEngineTest, OutSpanSizeMismatchIsRejectedBeforeMutation) {
  auto engine = TheoEngine::create({});
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const double T = surface_->context()[1].T;
  const std::array<TheoQuery, 2> qs{
      TheoQuery{.strike = 590.0, .tenor_years = T, .side = Side::Call},
      TheoQuery{.strike = 610.0, .tenor_years = T, .side = Side::Call},
  };
  // Deliberately short (1 slot for 2 queries), poisoned with sentinel values
  // so any write -- even a zero-init -- would be detectable.
  std::array<TheoValue, 1> out{};
  out[0].theo_vol = 12345.0;
  out[0].market_price = -9.0;
  out[0].flags = 0xDEADBEEFu;
  const TheoValue sentinel = out[0];

  const Status st = engine->value_into(ctx, qs, out);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(out[0].theo_vol, sentinel.theo_vol);
  EXPECT_EQ(out[0].market_price, sentinel.market_price);
  EXPECT_EQ(out[0].flags, sentinel.flags);
}

// ── (d) a stub overlay shifts theo_vol by exactly its dvol ─────────────────

TEST_F(TheoEngineTest, StubOverlayShiftsTheoVolByExactDvol) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.02, 0.0));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  const double expected_theo_vol = v->market_vol + 0.02;
  EXPECT_DOUBLE_EQ(v->theo_vol, expected_theo_vol);
  EXPECT_DOUBLE_EQ(v->edge_vol, v->market_vol - expected_theo_vol);
  EXPECT_EQ(v->flags & static_cast<std::uint32_t>(TheoFlagBits::OverlayClamped), 0u);
}

// ── (e) an overlay dvol beyond max_abs_dvol clamps and flags ────────────────

TEST_F(TheoEngineTest, OverlayDvolBeyondMaxAbsIsClampedAndFlagged) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(5.0, 0.0)); // far beyond default 0.15
  auto engine = TheoEngine::create(std::move(overlays)); // default cfg: max_abs_dvol = 0.15
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  EXPECT_DOUBLE_EQ(v->theo_vol, v->market_vol + 0.15);
  EXPECT_NE(v->flags & static_cast<std::uint32_t>(TheoFlagBits::OverlayClamped), 0u);
}

// ── create() validation ──────────────────────────────────────────────────

TEST_F(TheoEngineTest, CreateRejectsNullOverlay) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(nullptr);
  const auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_FALSE(engine.has_value());
  EXPECT_EQ(engine.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(TheoEngineTest, CreateRejectsNonPositiveMaxAbsDvol) {
  const auto engine = TheoEngine::create({}, TheoConfig{.max_abs_dvol = 0.0});
  ASSERT_FALSE(engine.has_value());
  EXPECT_EQ(engine.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(TheoEngineTest, CreateRejectsNegativeBandFloorVol) {
  const auto engine = TheoEngine::create({}, TheoConfig{.band_floor_vol = -1e-6});
  ASSERT_FALSE(engine.has_value());
  EXPECT_EQ(engine.error().code(), ErrorCode::InvalidArgument);
}

// ── price_theo=false: vol-space-only screening sheet ────────────────────────

TEST_F(TheoEngineTest, PriceTheoFalseFillsBothPricesOnZeroDvolButNaNsOnNonzeroDvol) {
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  // Zero overlays: even with price_theo=false, both prices are filled (free --
  // theo_vol == market_vol, so no extra American solve is needed).
  auto flat_engine = TheoEngine::create({}, TheoConfig{.price_theo = false});
  ASSERT_TRUE(flat_engine.has_value()) << flat_engine.error().to_string();
  const auto flat_v = flat_engine->value(ctx, q);
  ASSERT_TRUE(flat_v.has_value()) << flat_v.error().to_string();
  EXPECT_DOUBLE_EQ(flat_v->theo_price, flat_v->market_price);
  EXPECT_FALSE(std::isnan(flat_v->theo_price));

  // A nonzero-dvol overlay: price_theo=false skips the reprice -> NaN.
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.02, 0.0));
  auto shifted_engine = TheoEngine::create(std::move(overlays), TheoConfig{.price_theo = false});
  ASSERT_TRUE(shifted_engine.has_value()) << shifted_engine.error().to_string();
  const auto shifted_v = shifted_engine->value(ctx, q);
  ASSERT_TRUE(shifted_v.has_value()) << shifted_v.error().to_string();
  EXPECT_TRUE(std::isnan(shifted_v->theo_price));
  EXPECT_FALSE(std::isnan(shifted_v->market_price)); // market_price is unaffected
}

// ── overlay adjust() failure fails the whole batch ──────────────────────────

TEST_F(TheoEngineTest, FailingOverlayAdjustPropagatesAndFailsTheBatch) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<FailingOverlay>());
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::Internal);
}

// ── M3: a non-finite overlay adjustment fails loud, not silently ───────────

TEST_F(TheoEngineTest, NonFiniteOverlayDvolFailsLoudInsteadOfPoisoningTheBatch) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<NonFiniteDvolOverlay>());
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::Internal);
}

// ── I2: band_vol quadrature ──────────────────────────────────────────────

TEST_F(TheoEngineTest, ZeroOverlaysBandVolEqualsFloor) {
  auto engine = TheoEngine::create({});
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->band_vol, TheoConfig{}.band_floor_vol);
}

TEST_F(TheoEngineTest, TwoOverlayBandsCombineInQuadrature) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.0, 0.03));
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.0, 0.04));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  // 3-4-5: computed the same way the engine does (sum of squares -> sqrt) so
  // the comparison is bit-exact regardless of any rounding in that formula.
  const double expected = std::sqrt(0.03 * 0.03 + 0.04 * 0.04);
  EXPECT_DOUBLE_EQ(v->band_vol, expected);
  EXPECT_NEAR(v->band_vol, 0.05, 1e-12);
  EXPECT_DOUBLE_EQ(v->theo_vol, v->market_vol); // dvol == 0 -> identity still holds
}

TEST_F(TheoEngineTest, BandQuadratureBelowFloorStaysAtFloor) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.0, 0.0001));
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.0, 0.0001));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->band_vol, TheoConfig{}.band_floor_vol);
}

// ── I3: multi-overlay accumulation + per-query clamp isolation ─────────────

TEST_F(TheoEngineTest, MultiOverlayAccumulationHoldsAcrossChunkBoundary) {
  constexpr double kScaleA = 5e-5;
  constexpr double kScaleB = 3e-5;
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<StrikeScaledDvolOverlay>(kScaleA));
  overlays.push_back(std::make_unique<StrikeScaledDvolOverlay>(kScaleB));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};

  const std::size_t n = kTheoMaxBatch + 5;
  const std::array<double, 3> strikes{580.0, 600.0, 620.0};
  const std::array<double, 2> tenors{surface_->context()[1].T, surface_->context()[3].T};

  std::vector<TheoQuery> qs;
  qs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Side side = (i % 2 == 0) ? Side::Call : Side::Put;
    qs.push_back(TheoQuery{.strike = strikes[i % strikes.size()],
                           .tenor_years = tenors[i % tenors.size()],
                           .side = side});
  }
  std::vector<TheoValue> out(n);
  const Status st = engine->value_into(ctx, qs, out);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // Spot-check indices spanning both chunks and the seam itself; per-query
  // correctness here is chunk-position-independent (dvol is a pure function
  // of strike alone), so any chunk-boundary dispatch/reset bug would show up
  // as a mismatch at these indices just as readily as anywhere else.
  const std::array<std::size_t, 5> check_indices{0, kTheoMaxBatch - 1, kTheoMaxBatch,
                                                 kTheoMaxBatch + 1, n - 1};
  for (const std::size_t i : check_indices) {
    const TheoQuery &q = qs[i];
    const double expected_dvol = std::clamp(strike_scaled_dvol(q, kScaleA), -0.15, 0.15) +
                                 std::clamp(strike_scaled_dvol(q, kScaleB), -0.15, 0.15);
    const double market_vol = surface_->iv(q.strike, q.tenor_years);
    EXPECT_DOUBLE_EQ(out[i].theo_vol, market_vol + expected_dvol) << i;
    EXPECT_EQ(out[i].flags & static_cast<std::uint32_t>(TheoFlagBits::OverlayClamped), 0u) << i;
  }
}

TEST_F(TheoEngineTest, ClampedQueryFlagDoesNotLeakToNeighbors) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<StrikeTargetedDvolOverlay>(600.0, 5.0, 0.01));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const double T = surface_->context()[1].T;

  const std::array<TheoQuery, 3> qs{
      TheoQuery{.strike = 590.0, .tenor_years = T, .side = Side::Call},
      TheoQuery{.strike = 600.0, .tenor_years = T, .side = Side::Call},
      TheoQuery{.strike = 610.0, .tenor_years = T, .side = Side::Call},
  };
  std::array<TheoValue, 3> out{};
  const Status st = engine->value_into(ctx, qs, out);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  constexpr auto kClamped = static_cast<std::uint32_t>(TheoFlagBits::OverlayClamped);
  EXPECT_EQ(out[0].flags & kClamped, 0u);
  EXPECT_NE(out[1].flags & kClamped, 0u); // exactly the targeted (600-strike) query
  EXPECT_EQ(out[2].flags & kClamped, 0u);
  EXPECT_DOUBLE_EQ(out[0].theo_vol, out[0].market_vol + 0.01);
  EXPECT_DOUBLE_EQ(out[1].theo_vol, out[1].market_vol + 0.15); // clamped
  EXPECT_DOUBLE_EQ(out[2].theo_vol, out[2].market_vol + 0.01);
}

// ── chunk-boundary regression: a batch above kTheoMaxBatch keeps per-query
//    identity at (and around) the chunk seam ─────────────────────────────────

TEST_F(TheoEngineTest, BatchAboveChunkCapKeepsPerQueryIdentityAtTheSeam) {
  auto engine = TheoEngine::create({});
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};

  const std::size_t n = kTheoMaxBatch + 5;
  const std::array<double, 3> strikes{580.0, 600.0, 620.0};
  const std::array<double, 2> tenors{surface_->context()[1].T, surface_->context()[3].T};

  std::vector<TheoQuery> qs;
  qs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Side side = (i % 2 == 0) ? Side::Call : Side::Put;
    qs.push_back(TheoQuery{.strike = strikes[i % strikes.size()],
                           .tenor_years = tenors[i % tenors.size()],
                           .side = side});
  }
  std::vector<TheoValue> out(n);

  const Status st = engine->value_into(ctx, qs, out);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // Spot-check bit-for-bit identity right at the chunk seam (last index of
  // chunk 0, first index of chunk 1) and at the very end of the batch.
  const std::array<std::size_t, 4> boundary_indices{kTheoMaxBatch - 1, kTheoMaxBatch,
                                                    kTheoMaxBatch + 1, n - 1};
  for (const std::size_t i : boundary_indices) {
    const TheoQuery &q = qs[i];
    const double expected_vol = surface_->iv(q.strike, q.tenor_years);
    const auto expected_price = surface_->fair_value(q.strike, q.tenor_years, q.side);
    ASSERT_TRUE(expected_price.has_value()) << i << ": " << expected_price.error().to_string();
    EXPECT_DOUBLE_EQ(out[i].theo_vol, expected_vol) << i;
    EXPECT_DOUBLE_EQ(out[i].theo_price, *expected_price) << i;
    EXPECT_DOUBLE_EQ(out[i].edge_vol, 0.0) << i;
  }
}

// ── I1: FastTierRoute ────────────────────────────────────────────────────

TEST_F(TheoEngineTest, FastTierRouteFlagNotSetOnColdSurfaceEvenWithNonzeroDvol) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.02, 0.0));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  // surface_ (the SPY fixture) is never prepared onto a fast tier, so it
  // serves LegacyCompatible/ColdReference -- the cold route already.
  ASSERT_EQ(surface_->query_pricing_tier(), QueryPricingTier::LegacyCompatible);
  const TheoContext ctx{.surface = &*surface_};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->flags & static_cast<std::uint32_t>(TheoFlagBits::FastTierRoute), 0u);
}

TEST_F(TheoEngineTest, FastTierRouteFlagSetOnFastTierSurfaceWithNonzeroDvol) {
  auto fast_surface_result = make_fast_tier_surface();
  ASSERT_TRUE(fast_surface_result.has_value()) << fast_surface_result.error().to_string();
  const PricedSurface &fast_surface = *fast_surface_result;
  ASSERT_EQ(fast_surface.query_pricing_tier(), QueryPricingTier::RepresentativeFast);
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantAdjustOverlay>(0.02, 0.0));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &fast_surface};
  const TheoQuery q{.strike = 100.0, .tenor_years = 0.25, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_NE(v->flags & static_cast<std::uint32_t>(TheoFlagBits::FastTierRoute), 0u);
}

TEST_F(TheoEngineTest, FastTierRouteFlagNotSetOnFastTierSurfaceWithZeroDvol) {
  auto fast_surface_result = make_fast_tier_surface();
  ASSERT_TRUE(fast_surface_result.has_value()) << fast_surface_result.error().to_string();
  const PricedSurface &fast_surface = *fast_surface_result;
  auto engine = TheoEngine::create({}); // zero overlays -> dvol == 0, identity path
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &fast_surface};
  const TheoQuery q{.strike = 100.0, .tenor_years = 0.25, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->flags & static_cast<std::uint32_t>(TheoFlagBits::FastTierRoute), 0u);
}

// ── Task 8: OverlayAdjust::flags propagates into TheoValue::flags ──────────

TEST_F(TheoEngineTest, OverlayAdjustFlagsPropagateIntoTheoValueFlagsPerQuery) {
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<FlagOnTargetedQueryOverlay>(600.0));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_};
  const double T = surface_->context()[1].T;

  const std::array<TheoQuery, 3> qs{
      TheoQuery{.strike = 590.0, .tenor_years = T, .side = Side::Call},
      TheoQuery{.strike = 600.0, .tenor_years = T, .side = Side::Call},
      TheoQuery{.strike = 610.0, .tenor_years = T, .side = Side::Call},
  };
  std::array<TheoValue, 3> out{};
  const Status st = engine->value_into(ctx, qs, out);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  constexpr auto kModelMissing = static_cast<std::uint32_t>(TheoFlagBits::ModelMissing);
  EXPECT_EQ(out[0].flags & kModelMissing, 0u);
  EXPECT_NE(out[1].flags & kModelMissing, 0u); // exactly the targeted (600-strike) query
  EXPECT_EQ(out[2].flags & kModelMissing, 0u);
  // The overlay never touches dvol/band -- the identity path still holds.
  EXPECT_DOUBLE_EQ(out[1].theo_vol, out[1].market_vol);
}

// ── Task 8a: RV-blend fair vol ──────────────────────────────────────────────

// (a) weight == 0 is an identity transform, regardless of how far rv_anchor
// sits from market_vol, and does NOT itself signal ModelMissing (ctx.rv is
// present -- the zero dvol comes from the weight, not a degraded model).
TEST_F(TheoEngineTest, RvBlendWeightZeroIsIdentity) {
  const std::array<double, 1> tenors{0.10};
  auto flat = make_flat_vol_surface(0.25, tenors);
  ASSERT_TRUE(flat.has_value()) << flat.error().to_string();
  RvPanel rv{};
  rv.vol = {0.60, 0.60, 0.60, 0.60}; // deliberately far from market_vol
  auto overlay = make_rv_blend_overlay(RvBlendConfig{.weight = 0.0});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*flat, .rv = &rv};
  const TheoQuery q{.strike = 100.0, .tenor_years = 0.10, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->theo_vol, v->market_vol);
  EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
  EXPECT_EQ(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

// (b) known arithmetic: market 0.30, rv anchor 0.20, weight 0.5, T -> 0
// (negligible tenor damping) => dvol == -0.05. "T -> 0" is realized via a
// `tenor_damp_years` so large that `T / tenor_damp_years` is below double
// epsilon and `exp(...)` rounds to exactly 1.0 -- NOT via T == 0 itself,
// which `PricedSurface::iv` rejects (non-positive T is outside its domain).
TEST_F(TheoEngineTest, RvBlendKnownArithmeticAtNegligibleTenorDamping) {
  const std::array<double, 1> tenors{0.05};
  auto flat = make_flat_vol_surface(0.30, tenors);
  ASSERT_TRUE(flat.has_value()) << flat.error().to_string();
  RvPanel rv{};
  rv.vol = {0.20, 0.20, 0.20, 0.20};
  auto overlay = make_rv_blend_overlay(
      RvBlendConfig{.weight = 0.5, .tenor_damp_years = 1e18, .rv_window_idx = 0});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*flat, .rv = &rv};
  const TheoQuery q{.strike = 100.0, .tenor_years = 0.05, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_NEAR(v->market_vol, 0.30, 1e-12);
  EXPECT_NEAR(v->theo_vol - v->market_vol, -0.05, 1e-12);
}

// (c) tenor damping monotone: |dvol| strictly decreasing in T, holding
// market_vol/rv_anchor/weight fixed via the flat-vol surface (isolates the
// exp(-T/tenor_damp_years) factor from any real term-structure drift in
// market_vol itself).
TEST_F(TheoEngineTest, RvBlendTenorDampingIsMonotoneDecreasingInAbsDvol) {
  const std::array<double, 3> tenors{0.05, 0.25, 1.00};
  auto flat = make_flat_vol_surface(0.25, tenors);
  ASSERT_TRUE(flat.has_value()) << flat.error().to_string();
  RvPanel rv{};
  rv.vol = {0.15, 0.15, 0.15, 0.15}; // != market_vol -> nonzero diff at every T
  auto overlay = make_rv_blend_overlay(
      RvBlendConfig{.weight = 0.5, .tenor_damp_years = 1.0, .rv_window_idx = 0});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*flat, .rv = &rv};

  double prev_abs_dvol = std::numeric_limits<double>::infinity();
  for (const double T : tenors) {
    const TheoQuery q{.strike = 100.0, .tenor_years = T, .side = Side::Call};
    const auto v = engine->value(ctx, q);
    ASSERT_TRUE(v.has_value()) << v.error().to_string();
    const double abs_dvol = std::abs(v->theo_vol - v->market_vol);
    EXPECT_LT(abs_dvol, prev_abs_dvol) << "T=" << T;
    prev_abs_dvol = abs_dvol;
  }
}

// (d) missing ctx.rv sets ModelMissing, edge 0.
TEST_F(TheoEngineTest, RvBlendMissingRvContextSetsModelMissingAndZeroEdge) {
  auto overlay = make_rv_blend_overlay(RvBlendConfig{});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_}; // rv == nullptr
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
  EXPECT_NE(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

// Bonus (not in the brief's Step 1 list, but explicitly called out in the
// task's interface notes): an out-of-range rv_window_idx degrades the same
// way as a missing panel rather than reading out of bounds.
TEST_F(TheoEngineTest, RvBlendOutOfRangeWindowIdxSetsModelMissing) {
  RvPanel rv{};
  rv.vol = {0.10, 0.20, 0.30, 0.40};
  auto overlay = make_rv_blend_overlay(RvBlendConfig{.rv_window_idx = 200});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_, .rv = &rv};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
  EXPECT_NE(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

TEST_F(TheoEngineTest, MakeRvBlendOverlayRejectsNonFiniteWeight) {
  const auto overlay =
      make_rv_blend_overlay(RvBlendConfig{.weight = std::numeric_limits<double>::quiet_NaN()});
  ASSERT_FALSE(overlay.has_value());
  EXPECT_EQ(overlay.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(TheoEngineTest, MakeRvBlendOverlayRejectsNonPositiveTenorDampYears) {
  const auto overlay = make_rv_blend_overlay(RvBlendConfig{.tenor_damp_years = 0.0});
  ASSERT_FALSE(overlay.has_value());
  EXPECT_EQ(overlay.error().code(), ErrorCode::InvalidArgument);
}

// ── Task 8b: event variance swap ────────────────────────────────────────────

// (e) emove_forecast == emove_market is identity (within 1e-12): the strip
// and the re-inject exactly cancel whenever the censoring floor isn't hit.
TEST_F(TheoEngineTest, EventVarForecastEqualsMarketIsIdentity) {
  const std::int64_t now_ns = surface_->pricing().now_ts_ns;
  const double T_short = surface_->context()[1].T;
  const double T_long = surface_->context()[3].T;
  const std::int64_t event_ts = ns_from_year_fraction(now_ns, 0.5 * (T_short + T_long));
  EventSchedule events(std::vector<std::int64_t>{event_ts});
  ASSERT_GT(count_events_at(events, now_ns, T_long), std::size_t{0});

  constexpr double kEmove = 0.03;
  auto overlay =
      make_event_var_overlay(EventVarConfig{.emove_forecast = kEmove, .emove_market = kEmove});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_, .events = &events};
  const TheoQuery q{.strike = 600.0, .tenor_years = T_long, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_NEAR(v->theo_vol, v->market_vol, 1e-12);
}

// (f) forecast < market lowers theo vol only for expiries containing the
// event (count_between > 0); an expiry with no events before it is an exact
// no-op regardless of emove_forecast/emove_market.
TEST_F(TheoEngineTest, EventVarLowerForecastOnlyMovesEventBearingExpiries) {
  const std::int64_t now_ns = surface_->pricing().now_ts_ns;
  const double T_short = surface_->context()[1].T; // no event before this expiry
  const double T_long = surface_->context()[3].T;  // one event before this expiry
  const double T_mid = 0.5 * (T_short + T_long);
  const std::int64_t event_ts = ns_from_year_fraction(now_ns, T_mid);
  EventSchedule events(std::vector<std::int64_t>{event_ts});

  ASSERT_EQ(count_events_at(events, now_ns, T_short), std::size_t{0});
  ASSERT_EQ(count_events_at(events, now_ns, T_long), std::size_t{1});

  auto overlay =
      make_event_var_overlay(EventVarConfig{.emove_forecast = 0.01, .emove_market = 0.05});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_, .events = &events};

  const TheoQuery q_short{.strike = 600.0, .tenor_years = T_short, .side = Side::Call};
  const auto v_short = engine->value(ctx, q_short);
  ASSERT_TRUE(v_short.has_value()) << v_short.error().to_string();
  EXPECT_NEAR(v_short->theo_vol, v_short->market_vol, 1e-12); // n == 0 -> no-op

  const TheoQuery q_long{.strike = 600.0, .tenor_years = T_long, .side = Side::Call};
  const auto v_long = engine->value(ctx, q_long);
  ASSERT_TRUE(v_long.has_value()) << v_long.error().to_string();
  EXPECT_LT(v_long->theo_vol, v_long->market_vol); // forecast < market -> theo vol lowered
}

TEST_F(TheoEngineTest, EventVarMissingEventsContextSetsModelMissingAndZeroEdge) {
  auto overlay =
      make_event_var_overlay(EventVarConfig{.emove_forecast = 0.02, .emove_market = 0.03});
  ASSERT_TRUE(overlay.has_value()) << overlay.error().to_string();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  ASSERT_TRUE(engine.has_value()) << engine.error().to_string();
  const TheoContext ctx{.surface = &*surface_}; // events == nullptr
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = engine->value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
  EXPECT_NE(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

TEST_F(TheoEngineTest, MakeEventVarOverlayRejectsNegativeEmoveForecast) {
  const auto overlay =
      make_event_var_overlay(EventVarConfig{.emove_forecast = -0.01, .emove_market = 0.0});
  ASSERT_FALSE(overlay.has_value());
  EXPECT_EQ(overlay.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(TheoEngineTest, MakeEventVarOverlayRejectsNegativeEmoveMarket) {
  const auto overlay =
      make_event_var_overlay(EventVarConfig{.emove_forecast = 0.0, .emove_market = -0.01});
  ASSERT_FALSE(overlay.has_value());
  EXPECT_EQ(overlay.error().code(), ErrorCode::InvalidArgument);
}

// ── Task 9: IFairVolModel seam -- linear v1 loader + model overlay ─────────

namespace {

// Writes `content` to a fresh temp file; removed when the guard goes out of
// scope. Mirrors var_test.cpp's `ScopedTempDirectory` for a single-file
// fixture instead of a directory tree (Task 5/6 loader tests write their
// input fixtures the same way -- see bev_label_factory.cpp's dividends
// loader test fixtures / earnings_forecast_loader_test.cpp's convention).
// Deliberately immovable AND non-copyable (Rule of Five, explicit): every
// use below is a single local variable whose file is consumed in place, so
// there is no call site that needs to relocate ownership of the temp path.
class ScopedTempFile {
public:
  ScopedTempFile(std::string_view label, std::string_view content) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("atx_theo_" + std::string(label) + "_" + std::to_string(tick) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tsv");
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  ~ScopedTempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  ScopedTempFile(const ScopedTempFile &) = delete;
  ScopedTempFile &operator=(const ScopedTempFile &) = delete;
  ScopedTempFile(ScopedTempFile &&) = delete;
  ScopedTempFile &operator=(ScopedTempFile &&) = delete;

  [[nodiscard]] std::string path_string() const { return path_.string(); }

private:
  std::filesystem::path path_{};
};

// "%.17g" -- max_digits10, the minimum that round-trips a double bit-exactly
// back through strtod/from_chars (mirrors bev_label_factory.cpp's
// append_double) -- so the intercept-only arithmetic test below can assert
// to 1e-10 without losing precision to std::to_string's fixed 6-digit default.
[[nodiscard]] std::string fmt_double(double v) {
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// Formats a linear-model coef TSV: a "# schema=<schema>" header line, then
// the intercept and every coefficient whitespace-separated on one line.
[[nodiscard]] std::string make_coef_tsv(std::uint32_t schema, double intercept,
                                        std::span<const double> coefs) {
  std::string out = "# schema=" + std::to_string(schema) + "\n";
  out += fmt_double(intercept);
  for (const double c : coefs) {
    out += ' ';
    out += fmt_double(c);
  }
  out += '\n';
  return out;
}

// Builds a ready-to-use model-overlay engine from `intercept`/`coefs`, or
// returns nullopt (callers ASSERT on the optional -- a free function can't
// itself ASSERT and return a non-void type). The coef TSV's content is fully
// consumed by `load_linear_fair_vol_model` before this returns, so the
// `ScopedTempFile` (deliberately non-movable -- see its own declaration) only
// needs to outlive that one call, not the returned fixture.
struct FairVolModelFixture {
  std::shared_ptr<const IFairVolModel> model;
  TheoEngine engine;
};

[[nodiscard]] std::optional<FairVolModelFixture>
make_fair_vol_model_fixture(double intercept, std::span<const double> coefs) {
  const ScopedTempFile coef_file("fixture",
                                 make_coef_tsv(kFairVolFeatureSchemaV1, intercept, coefs));
  auto loaded = load_linear_fair_vol_model(coef_file.path_string());
  if (!loaded.has_value()) {
    return std::nullopt;
  }
  std::shared_ptr<const IFairVolModel> model = std::move(*loaded);
  auto overlay = make_fair_vol_model_overlay(model);
  if (!overlay.has_value()) {
    return std::nullopt;
  }
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::move(*overlay));
  auto engine = TheoEngine::create(std::move(overlays));
  if (!engine.has_value()) {
    return std::nullopt;
  }
  return FairVolModelFixture{std::move(model), std::move(*engine)};
}

// A minimal IFairVolModel stub that reports an arbitrary `feature_schema()`
// and, if ever actually called, a trivial y == 0 prediction. Used only to
// prove `make_fair_vol_model_overlay`'s construction-time schema check
// (I1, review fix round 1) without needing a real coefficient file.
class StubSchemaFairVolModel final : public IFairVolModel {
public:
  explicit StubSchemaFairVolModel(std::uint32_t schema) : schema_(schema) {}

  [[nodiscard]] std::uint32_t feature_schema() const noexcept override { return schema_; }

  [[nodiscard]] Status predict(std::span<const double> /*features_row_major*/, std::size_t n_rows,
                               std::span<double> log_ratio_out) const override {
    if (log_ratio_out.size() != n_rows) {
      return Err(ErrorCode::InvalidArgument, "StubSchemaFairVolModel::predict: size mismatch");
    }
    std::fill(log_ratio_out.begin(), log_ratio_out.end(), 0.0);
    return Ok();
  }

private:
  std::uint32_t schema_;
};

} // namespace

// (a) zero coefficients -> y == 0 for every row -> engine identity, and
// ModelMissing is NOT set (ctx.rv/ctx.events are both present -- the zero
// dvol comes from the model, not a degraded context).
TEST_F(TheoEngineTest, FairVolModelZeroCoefficientsIsIdentity) {
  const std::array<double, kFairVolFeatureCount> coefs{};
  auto fixture = make_fair_vol_model_fixture(0.0, coefs);
  ASSERT_TRUE(fixture.has_value());

  RvPanel rv{};
  rv.vol = {0.20, 0.22, 0.24, 0.26};
  EventSchedule events(std::vector<std::int64_t>{});
  const TheoContext ctx{.surface = &*surface_, .events = &events, .rv = &rv};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = fixture->engine.value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->theo_vol, v->market_vol);
  EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
  EXPECT_EQ(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

// (b) intercept-only model, b0 = ln(0.9): every row's y == b0 regardless of
// features, so dvol = market_vol * (0.9 - 1) => theo_vol == 0.9 * market_vol.
TEST_F(TheoEngineTest, FairVolModelInterceptOnlyScalesTheoVolByExpB0) {
  const std::array<double, kFairVolFeatureCount> coefs{};
  const double b0 = std::log(0.9);
  auto fixture = make_fair_vol_model_fixture(b0, coefs);
  ASSERT_TRUE(fixture.has_value());

  RvPanel rv{};
  rv.vol = {0.20, 0.22, 0.24, 0.26};
  EventSchedule events(std::vector<std::int64_t>{});
  const TheoContext ctx{.surface = &*surface_, .events = &events, .rv = &rv};
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = fixture->engine.value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_NEAR(v->theo_vol, 0.9 * v->market_vol, 1e-10);
  EXPECT_EQ(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

// (c) a declared schema that isn't kFairVolFeatureSchemaV1 is refused at
// load, ParseError family (matches this module's own schema-mismatch
// precedent -- SurfaceArchiveV2::open, backtest_db.cpp).
TEST_F(TheoEngineTest, LoadLinearFairVolModelRejectsSchemaMismatch) {
  const std::array<double, kFairVolFeatureCount> coefs{};
  const ScopedTempFile coef_file("schema_mismatch", make_coef_tsv(2, 0.0, coefs));
  const auto model = load_linear_fair_vol_model(coef_file.path_string());
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::ParseError);
}

// (d) wrong value count (intercept + coefficients != kFairVolFeatureCount+1)
// is refused, ParseError.
TEST_F(TheoEngineTest, LoadLinearFairVolModelRejectsWrongCoefficientCount) {
  const ScopedTempFile coef_file("wrong_count", "# schema=1\n0.0 0.0 0.0\n");
  const auto model = load_linear_fair_vol_model(coef_file.path_string());
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::ParseError);
}

// (e) missing ctx.rv sets ModelMissing, edge 0 -- the overlay fails OPEN
// exactly like RvBlendOverlay/EventVarOverlay (Task 8).
TEST_F(TheoEngineTest, FairVolModelMissingRvContextSetsModelMissingAndZeroEdge) {
  const std::array<double, kFairVolFeatureCount> coefs{};
  auto fixture = make_fair_vol_model_fixture(0.0, coefs);
  ASSERT_TRUE(fixture.has_value());

  EventSchedule events(std::vector<std::int64_t>{});
  const TheoContext ctx{.surface = &*surface_, .events = &events}; // rv == nullptr
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = fixture->engine.value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
  EXPECT_NE(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

// Bonus (not in the brief's Step 1 list): missing ctx.events is the same
// fail-open path as missing ctx.rv above -- both are required to assemble
// the full feature row.
TEST_F(TheoEngineTest, FairVolModelMissingEventsContextSetsModelMissingAndZeroEdge) {
  const std::array<double, kFairVolFeatureCount> coefs{};
  auto fixture = make_fair_vol_model_fixture(0.0, coefs);
  ASSERT_TRUE(fixture.has_value());

  RvPanel rv{};
  rv.vol = {0.20, 0.22, 0.24, 0.26};
  const TheoContext ctx{.surface = &*surface_, .rv = &rv}; // events == nullptr
  const TheoQuery q{.strike = 600.0, .tenor_years = surface_->context()[1].T, .side = Side::Call};

  const auto v = fixture->engine.value(ctx, q);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_DOUBLE_EQ(v->edge_vol, 0.0);
  EXPECT_NE(v->flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u);
}

// Bonus: a file with no "# schema=<n>" line anywhere is refused, ParseError
// (distinct from a present-but-wrong schema, which is (c) above).
TEST_F(TheoEngineTest, LoadLinearFairVolModelRejectsMissingSchemaHeader) {
  const ScopedTempFile coef_file("no_schema", "0 0 0 0 0 0 0 0 0\n");
  const auto model = load_linear_fair_vol_model(coef_file.path_string());
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::ParseError);
}

// Bonus: an unparseable coefficient token is refused, ParseError.
TEST_F(TheoEngineTest, LoadLinearFairVolModelRejectsUnparseableToken) {
  const ScopedTempFile coef_file("bad_token", "# schema=1\n0 0 0 0 0 0 0 0 not_a_number\n");
  const auto model = load_linear_fair_vol_model(coef_file.path_string());
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::ParseError);
}

// Bonus: a nonexistent path is IoError, not ParseError -- mirrors every
// other TSV loader in this repo (load_dividends_tsv, load_earnings_events).
TEST_F(TheoEngineTest, LoadLinearFairVolModelMissingFileIsIoError) {
  const auto model = load_linear_fair_vol_model("this/path/does/not/exist.tsv");
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::IoError);
}

// Bonus: make_fair_vol_model_overlay rejects a null model at construction
// (never deferred to first use).
TEST_F(TheoEngineTest, MakeFairVolModelOverlayRejectsNullModel) {
  const auto overlay = make_fair_vol_model_overlay(nullptr);
  ASSERT_FALSE(overlay.has_value());
  EXPECT_EQ(overlay.error().code(), ErrorCode::InvalidArgument);
}

// I1 (review fix round 1): make_fair_vol_model_overlay checks
// model->feature_schema() against kFairVolFeatureSchemaV1 at construction --
// a model trained against a different schema must be refused rather than
// silently handed a feature block laid out for kFairVolFeatureSchemaV1.
TEST_F(TheoEngineTest, MakeFairVolModelOverlayRejectsSchemaMismatch) {
  const std::shared_ptr<const IFairVolModel> model = std::make_shared<StubSchemaFairVolModel>(2);
  const auto overlay = make_fair_vol_model_overlay(model);
  ASSERT_FALSE(overlay.has_value());
  EXPECT_EQ(overlay.error().code(), ErrorCode::InvalidArgument);
}

// Bonus: IFairVolModel::predict itself validates span sizes (the caller's
// bug, not a graceful-degrade data condition -- see the interface doc).
TEST_F(TheoEngineTest, LinearFairVolModelPredictRejectsFeatureSpanSizeMismatch) {
  const std::array<double, kFairVolFeatureCount> coefs{};
  const ScopedTempFile coef_file("predict_mismatch",
                                 make_coef_tsv(kFairVolFeatureSchemaV1, 0.0, coefs));
  auto model = load_linear_fair_vol_model(coef_file.path_string());
  ASSERT_TRUE(model.has_value()) << model.error().to_string();

  std::array<double, kFairVolFeatureCount - 1> too_few_features{}; // one short of one row
  std::array<double, 1> log_ratio_out{};
  const Status st = (*model)->predict(too_few_features, 1, log_ratio_out);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

// M (review fix round 1): FairVolModelOverlay::adjust sub-chunks its own
// input span on the kTheoMaxBatch cap (theo.cpp's `while (begin <
// queries.size())` loop) -- untested above that boundary via a DIRECT
// overlay call, since routing through TheoEngine::value_into would never
// hand the overlay more than kTheoMaxBatch queries at once (the engine does
// its own chunking first), so the overlay's own loop would only ever run
// one iteration either way. Calls overlay->adjust() directly with > one
// chunk's worth of queries and checks per-index correctness at the seam
// (mirrors Task 8's BatchAboveChunkCapKeepsPerQueryIdentityAtTheSeam /
// MultiOverlayAccumulationHoldsAcrossChunkBoundary chunk-seam pattern).
TEST_F(TheoEngineTest, FairVolModelOverlayAdjustSubChunksAboveMaxBatchBoundary) {
  const std::array<double, kFairVolFeatureCount> coefs{};
  const double b0 = std::log(0.9); // nonzero model: dvol == market_vol * (0.9 - 1)
  const ScopedTempFile coef_file("chunk_boundary",
                                 make_coef_tsv(kFairVolFeatureSchemaV1, b0, coefs));
  auto loaded = load_linear_fair_vol_model(coef_file.path_string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  const std::shared_ptr<const IFairVolModel> model = std::move(*loaded);
  auto overlay_result = make_fair_vol_model_overlay(model);
  ASSERT_TRUE(overlay_result.has_value()) << overlay_result.error().to_string();
  const std::unique_ptr<ITheoOverlay> overlay = std::move(*overlay_result);

  RvPanel rv{};
  rv.vol = {0.20, 0.22, 0.24, 0.26};
  EventSchedule events(std::vector<std::int64_t>{});
  const TheoContext ctx{.surface = &*surface_, .events = &events, .rv = &rv};

  const std::size_t n = kTheoMaxBatch + 5; // > one chunk -- drives the while-loop twice
  const std::array<double, 3> strikes{580.0, 600.0, 620.0};
  const std::array<double, 2> tenors{surface_->context()[1].T, surface_->context()[3].T};

  std::vector<TheoQuery> qs;
  qs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Side side = (i % 2 == 0) ? Side::Call : Side::Put;
    qs.push_back(TheoQuery{.strike = strikes[i % strikes.size()],
                           .tenor_years = tenors[i % tenors.size()],
                           .side = side});
  }
  std::vector<OverlayAdjust> out(n);
  const Status st = overlay->adjust(ctx, qs, out);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // Seam indices spanning both internal chunks: last index of chunk 0 (255
  // == kTheoMaxBatch - 1), first index of chunk 1 (256 == kTheoMaxBatch),
  // and a spot-check well inside chunk 1 (260) -- plus index 0 itself.
  const std::array<std::size_t, 4> check_indices{0, kTheoMaxBatch - 1, kTheoMaxBatch,
                                                 kTheoMaxBatch + 4};
  static_assert(kTheoMaxBatch == 256, "seam indices below assume kTheoMaxBatch == 256");
  for (const std::size_t i : check_indices) {
    const TheoQuery &q = qs[i];
    const double market_vol = surface_->iv(q.strike, q.tenor_years);
    ASSERT_TRUE(std::isfinite(market_vol) && market_vol > 0.0) << i;
    const double expected_dvol = market_vol * (0.9 - 1.0);
    EXPECT_NEAR(out[i].dvol, expected_dvol, 1e-10) << i;
    EXPECT_EQ(out[i].flags & static_cast<std::uint32_t>(TheoFlagBits::ModelMissing), 0u) << i;
  }
}

} // namespace
} // namespace atx::vol
