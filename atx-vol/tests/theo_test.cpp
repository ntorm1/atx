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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/panel.hpp"    // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/query_pricing.hpp"  // QueryPricingTier
#include "atx/vol/session.hpp"        // VolaSession, SessionInputs, FitPreset
#include "atx/vol/spy_fixture.hpp"    // make_spy_synthetic_spec, make_spy_session_inputs
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/types.hpp"          // Side
#include "atx/vol/vol_curve.hpp"      // CurveSurface, EssviParams, EssviCurve

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

} // namespace
} // namespace atx::vol
