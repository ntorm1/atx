// TheoEngine unit suite (THEO-7) -- the identity-semantics gate: with zero
// overlays engaged, every TheoValue this engine produces is bit-for-bit
// identical to what the served PricedSurface itself reports.
//
// Fixture: the deterministic synthetic SPY-like index surface (spy_fixture.hpp
// / panel.hpp), fit once per test suite via VolaSession::from_frame + Fast
// preset (mirrors examples/spy_surface_bench.cpp's build), the same fixture
// tests/breakeven_test.cpp's BevPathLoader section is built on.

#include "atx/vol/theo.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/panel.hpp" // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/session.hpp"     // VolaSession, SessionInputs, FitPreset
#include "atx/vol/spy_fixture.hpp" // make_spy_synthetic_spec, make_spy_session_inputs
#include "atx/vol/types.hpp"       // Side

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

// ── Test-only overlay stubs ──────────────────────────────────────────────

// Adds a fixed vol-space `dvol` to every query, no band contribution.
class ConstantDvolOverlay final : public ITheoOverlay {
public:
  explicit ConstantDvolOverlay(double dvol) : dvol_(dvol) {}

  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_constant_dvol_overlay";
  }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = OverlayAdjust{.dvol = dvol_, .band = 0.0};
    }
    return Ok();
  }

private:
  double dvol_;
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
  overlays.push_back(std::make_unique<ConstantDvolOverlay>(0.02));
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
  overlays.push_back(std::make_unique<ConstantDvolOverlay>(5.0)); // far beyond default 0.15
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
  overlays.push_back(std::make_unique<ConstantDvolOverlay>(0.02));
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

} // namespace
} // namespace atx::vol
