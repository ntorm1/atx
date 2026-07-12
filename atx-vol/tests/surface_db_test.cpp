#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/dense_slice.hpp"
#include "atx/vol/detail/archive_util.hpp" // crc32c (test-side CRC repair)
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/vol_curve.hpp"

// ATXVDB v1 manifest suite. First: on-disk record layout pinning,
// writer/parser round-trip (every SymbolFitConfig field preserved
// bit-for-bit), duplicate/malformed-input rejection, and corruption
// detection (magic, header CRC, payload CRC, truncation, out-of-range enum
// wire values) — pure in-memory, no file IO. Then: SurfaceDb itself
// (create/open/upsert/refresh, concurrent readers), the partition store
// (write/open/load/drop), the apply_symbol_config pipeline binding, and an
// end-to-end configure-store-reload-serve test against a real directory on
// disk.

namespace atx::vol {
namespace {

// A config where EVERY field differs from its default, so a round-trip that
// drops or transposes any field fails the equality sweep below.
SymbolFitConfig make_full_config() {
  SymbolFitConfig c;
  c.enabled = false;
  c.preset = FitPreset::Hft;
  c.pin_curve = true;
  c.curve.kind = VolCurveKind::ConvexDense;
  c.curve.convex.lambda = 7.5e-4;
  c.curve.convex.bound_slope_below = true;
  c.curve.convex.node_cap = 56;
  c.curve.convex.max_iter = 123;
  c.curve.convex.loss = CalibLossKind::Interval;
  auto &p = c.curve.parametric;
  p.max_outer_iter = 5;
  p.max_inner_iter = 13;
  p.tol_param = 2e-9;
  p.tol_residual = 3e-10;
  p.huber_k = 1.75;
  p.min_vega_weight = 2e-6;
  p.max_spread_vol = 0.07;
  p.max_weight = 500.0;
  p.max_obs_per_slice = 96;
  p.max_otm_shortcut_premium_spread_frac = 0.25;
  p.prior_strength = 0.5;
  p.essvi_rho_mode = EssviRhoMode::Shared;
  p.optimization_level = OptimizationLevel::Risk;
  p.essvi_fallback_rmse_threshold = 0.02;
  p.n_butterfly_grid = 128;
  p.max_iter_quick_mark = 9;
  p.max_iter_trading = 36;
  p.max_iter_risk = 101;
  p.max_iter_reference = 251;
  p.max_iter_cold_fast = 11;
  p.wing_floor_alpha = 0.05;
  p.lee_bound_project = false;
  p.morozov_stop = true;
  p.morozov_tau = 1.3;
  p.validate_no_arb = false;
  p.residual_disable = false;
  p.residual_basis_kind = ResidualBasisKind::C2Bspline;
  p.residual_n_basis_terms = 8;
  p.residual_ridge_factor = 2e-3;
  p.loss_kind = CalibLossKind::Interval;
  p.anchor_kind = CalibAnchorKind::Ask;
  p.essvi_asymmetric_rho = true;
  p.min_obs_per_slice = 6;
  p.max_post_fit_sigma = 3.0;
  p.max_spread_to_mid_pct = 0.4;
  c.al_override = true;
  c.al = AlOpts{9, 20, 6, 1e-9};
  c.band_k = 1.25;
  c.calendar_repair = CalendarRepair::Project;
  c.use_correction_cache = false;
  c.score_parity = false;
  c.enforce_calendar_floor = false;
  c.use_deam_cache_for_fit = true;
  c.surface_policy.quality_mode = FitQualityMode::Accuracy;
  c.surface_policy.outputs = SurfaceOutputs::Risk;
  c.surface_policy.risk_admission = RiskAdmission::Required;
  c.surface_policy.fallback = SurfaceFallback::None;
  return c;
}

void expect_config_eq(const SymbolFitConfig &a, const SymbolFitConfig &b) {
  EXPECT_EQ(a.enabled, b.enabled);
  EXPECT_EQ(a.preset, b.preset);
  EXPECT_EQ(a.pin_curve, b.pin_curve);
  EXPECT_EQ(a.curve.kind, b.curve.kind);
  EXPECT_EQ(a.curve.convex.lambda, b.curve.convex.lambda);
  EXPECT_EQ(a.curve.convex.bound_slope_below, b.curve.convex.bound_slope_below);
  EXPECT_EQ(a.curve.convex.node_cap, b.curve.convex.node_cap);
  EXPECT_EQ(a.curve.convex.max_iter, b.curve.convex.max_iter);
  EXPECT_EQ(a.curve.convex.loss, b.curve.convex.loss);
  const auto &x = a.curve.parametric;
  const auto &y = b.curve.parametric;
  EXPECT_EQ(x.max_outer_iter, y.max_outer_iter);
  EXPECT_EQ(x.max_inner_iter, y.max_inner_iter);
  EXPECT_EQ(x.tol_param, y.tol_param);
  EXPECT_EQ(x.tol_residual, y.tol_residual);
  EXPECT_EQ(x.huber_k, y.huber_k);
  EXPECT_EQ(x.min_vega_weight, y.min_vega_weight);
  EXPECT_EQ(x.max_spread_vol, y.max_spread_vol);
  EXPECT_EQ(x.max_weight, y.max_weight);
  EXPECT_EQ(x.max_obs_per_slice, y.max_obs_per_slice);
  EXPECT_EQ(x.max_otm_shortcut_premium_spread_frac, y.max_otm_shortcut_premium_spread_frac);
  EXPECT_EQ(x.prior_strength, y.prior_strength);
  EXPECT_EQ(x.essvi_rho_mode, y.essvi_rho_mode);
  EXPECT_EQ(x.optimization_level, y.optimization_level);
  EXPECT_EQ(x.essvi_fallback_rmse_threshold, y.essvi_fallback_rmse_threshold);
  EXPECT_EQ(x.n_butterfly_grid, y.n_butterfly_grid);
  EXPECT_EQ(x.max_iter_quick_mark, y.max_iter_quick_mark);
  EXPECT_EQ(x.max_iter_trading, y.max_iter_trading);
  EXPECT_EQ(x.max_iter_risk, y.max_iter_risk);
  EXPECT_EQ(x.max_iter_reference, y.max_iter_reference);
  EXPECT_EQ(x.max_iter_cold_fast, y.max_iter_cold_fast);
  EXPECT_EQ(x.wing_floor_alpha, y.wing_floor_alpha);
  EXPECT_EQ(x.lee_bound_project, y.lee_bound_project);
  EXPECT_EQ(x.morozov_stop, y.morozov_stop);
  EXPECT_EQ(x.morozov_tau, y.morozov_tau);
  EXPECT_EQ(x.validate_no_arb, y.validate_no_arb);
  EXPECT_EQ(x.residual_disable, y.residual_disable);
  EXPECT_EQ(x.residual_basis_kind, y.residual_basis_kind);
  EXPECT_EQ(x.residual_n_basis_terms, y.residual_n_basis_terms);
  EXPECT_EQ(x.residual_ridge_factor, y.residual_ridge_factor);
  EXPECT_EQ(x.loss_kind, y.loss_kind);
  EXPECT_EQ(x.anchor_kind, y.anchor_kind);
  EXPECT_EQ(x.essvi_asymmetric_rho, y.essvi_asymmetric_rho);
  EXPECT_EQ(x.min_obs_per_slice, y.min_obs_per_slice);
  EXPECT_EQ(x.max_post_fit_sigma, y.max_post_fit_sigma);
  EXPECT_EQ(x.max_spread_to_mid_pct, y.max_spread_to_mid_pct);
  EXPECT_EQ(a.al_override, b.al_override);
  EXPECT_EQ(a.al.n_collocation, b.al.n_collocation);
  EXPECT_EQ(a.al.n_quadrature, b.al.n_quadrature);
  EXPECT_EQ(a.al.max_newton_iter, b.al.max_newton_iter);
  EXPECT_EQ(a.al.tol, b.al.tol);
  EXPECT_EQ(a.band_k, b.band_k);
  EXPECT_EQ(a.calendar_repair, b.calendar_repair);
  EXPECT_EQ(a.use_correction_cache, b.use_correction_cache);
  EXPECT_EQ(a.score_parity, b.score_parity);
  EXPECT_EQ(a.enforce_calendar_floor, b.enforce_calendar_floor);
  EXPECT_EQ(a.use_deam_cache_for_fit, b.use_deam_cache_for_fit);
  EXPECT_EQ(a.surface_policy.quality_mode, b.surface_policy.quality_mode);
  EXPECT_EQ(a.surface_policy.outputs, b.surface_policy.outputs);
  EXPECT_EQ(a.surface_policy.risk_admission, b.surface_policy.risk_admission);
  EXPECT_EQ(a.surface_policy.fallback, b.surface_policy.fallback);
}

void restamp_crcs(std::vector<std::byte> &bytes);

// Fresh per-test temp dir under the system temp root, self-cleaning at start
// so a prior crashed run doesn't leak stale manifest/partition files into
// this run. Each SurfaceDb.* test also removes it again at the end.
std::filesystem::path test_root(std::string_view name) {
  auto p = std::filesystem::temp_directory_path() / ("atx_surface_db_" + std::string(name));
  std::filesystem::remove_all(p);
  return p;
}

// ── Partition-store test fixtures ──────────────────────────────────────────
//
// Copied from surface_archive_test.cpp (the binding bit-identity oracle for
// this task): make_essvi/make_convex/make_linear build genuine PricedSurface
// instances of each curve kind, bits_equal + expect_theo_bit_identical are
// the exact assertion primitives used there. Kept self-contained here rather
// than shared so this file has no test-only dependency on another test
// binary's translation unit.

constexpr double kArchS = 100.0;
constexpr double kArchR = 0.043;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kArchS;
  pc.r = kArchR;
  pc.now_ts_ns = 1700000000000000000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = AlOpts{}; // {12, 24, 8, 1e-10}
  pc.uid = uid;
  return pc;
}

// eSSVI priced surface, `n` ascending-T slices with a realistic mild smile.
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kArchS;
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
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kArchR * T)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// Dense convex priced surface, `n` slices x `nodes` genuine arb-free convex
// node prices (a flat-vol Black-76 call curve -> an invertible, finite smile).
[[nodiscard]] PricedSurface make_convex(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kArchS;
    const double df = std::exp(-kArchR * T);
    const double sigma = 0.20 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.rmse_price = 0.25;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 3;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double K = F * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = black76_price(F, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(nodes), 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_linear(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kArchS;
    std::vector<double> k(static_cast<std::size_t>(nodes));
    std::vector<double> w(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double x = -0.4 + 0.8 * static_cast<double>(j) / static_cast<double>(nodes - 1);
      k[static_cast<std::size_t>(j)] = x;
      w[static_cast<std::size_t>(j)] = (0.20 * 0.20 + 0.01 * x + 0.02 * x * x) * T;
    }
    cs.push(std::make_unique<LinearVarianceCurve>(T, F, std::exp(-kArchR * T), std::move(k),
                                                  std::move(w)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(nodes), 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// Assert a reconstructed surface serves BIT-IDENTICAL theo to the original
// across a (K, T, side) grid: implied vol, total variance, and re-Americanized
// fair value. Copied verbatim (pattern) from surface_archive_test.cpp -- the
// binding bit-identity oracle for this task.
void expect_theo_bit_identical(const PricedSurface &a, const PricedSurface &b) {
  ASSERT_EQ(a.n_slices(), b.n_slices());
  ASSERT_EQ(a.uid(), b.uid());
  const std::array<double, 4> Ks{85.0, 100.0, 108.0, 120.0};
  const std::array<double, 3> Ts{0.06, 0.18, 0.34};
  for (const double K : Ks) {
    for (const double T : Ts) {
      EXPECT_TRUE(bits_equal(a.iv(K, T), b.iv(K, T))) << "iv K=" << K << " T=" << T;
      EXPECT_TRUE(bits_equal(a.total_variance(K, T), b.total_variance(K, T)))
          << "w K=" << K << " T=" << T;
      for (const Side side : {Side::Call, Side::Put}) {
        const auto fa = a.fair_value(K, T, side);
        const auto fb = b.fair_value(K, T, side);
        ASSERT_EQ(fa.has_value(), fb.has_value()) << "fv K=" << K << " T=" << T;
        if (fa.has_value()) {
          EXPECT_TRUE(bits_equal(*fa, *fb)) << "fv K=" << K << " T=" << T;
        }
      }
    }
  }
}

TEST(SurfaceDbManifest, RoundTrip_FullConfig_EveryFieldPreserved) {
  const auto cfg = make_full_config();
  SurfaceProvenance provenance;
  provenance.purpose = SurfacePurpose::Risk;
  provenance.quality_mode = FitQualityMode::Accuracy;
  provenance.state = SurfaceState::Degraded;
  provenance.validation.failures = ValidationFailure::Calendar;
  provenance.validation.validation_id = 0xA11CE55u;
  provenance.served_generation = 17;
  const std::vector<DbSymbolEntry> syms{{"aapl", cfg, provenance},
                                        {"SPY", SymbolFitConfig{}}};
  const std::vector<DbPartitionInfo> parts{{"2026-07-10", 123, 456789, 1720569600000000000LL}};
  auto bytes = write_db_manifest(syms, parts, {.generation = 7});
  ASSERT_TRUE(bytes.has_value());
  auto m = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->generation(), 7u);
  ASSERT_EQ(m->symbols().size(), 2u);
  ASSERT_EQ(m->partitions().size(), 1u);
  // canonical sort: AAPL < SPY
  auto got = m->find_symbol("AaPl"); // case-insensitive
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);
  auto got_provenance = m->find_symbol_provenance("aapl");
  ASSERT_TRUE(got_provenance.has_value());
  ASSERT_TRUE(got_provenance->has_value());
  EXPECT_EQ((*got_provenance)->purpose, provenance.purpose);
  EXPECT_EQ((*got_provenance)->quality_mode, provenance.quality_mode);
  EXPECT_EQ((*got_provenance)->state, provenance.state);
  EXPECT_EQ((*got_provenance)->validation.failures,
            provenance.validation.failures);
  EXPECT_EQ((*got_provenance)->validation.validation_id,
            provenance.validation.validation_id);
  EXPECT_EQ((*got_provenance)->served_generation,
            provenance.served_generation);
  auto dflt = m->find_symbol("spy");
  ASSERT_TRUE(dflt.has_value());
  expect_config_eq(*dflt, SymbolFitConfig{});
  const auto *p = m->find_partition("2026-07-10");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->surface_count, 123u);
  EXPECT_EQ(p->file_size, 456789u);
  EXPECT_EQ(m->find_partition("2026-07-11"), nullptr);
  EXPECT_EQ(m->find_symbol("MSFT").error().code(), ErrorCode::NotFound);
}

// Review C-1: the db record's known-failures allowlist must accept the
// CarryGap bit — a Degraded+CarryGap provenance is a routinely SERVED state
// and has to survive a manifest round-trip (record_valid on load).
TEST(SurfaceDbManifest, RoundTrip_DegradedCarryGapProvenancePreserved) {
  SurfaceProvenance provenance;
  provenance.purpose = SurfacePurpose::Risk;
  provenance.quality_mode = FitQualityMode::Balanced;
  provenance.state = SurfaceState::Degraded;
  provenance.validation.failures = ValidationFailure::CarryGap;
  provenance.validation.validation_id = 0xCA44'76A9u;
  provenance.served_generation = 9;
  const std::vector<DbSymbolEntry> syms{{"spy", SymbolFitConfig{}, provenance}};
  auto bytes = write_db_manifest(syms, {});
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto m = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(m.has_value());
  auto got_provenance = m->find_symbol_provenance("SPY");
  ASSERT_TRUE(got_provenance.has_value());
  ASSERT_TRUE(got_provenance->has_value());
  EXPECT_EQ((*got_provenance)->state, SurfaceState::Degraded);
  EXPECT_EQ((*got_provenance)->validation.failures, ValidationFailure::CarryGap);
  EXPECT_EQ((*got_provenance)->validation.validation_id,
            provenance.validation.validation_id);
  EXPECT_EQ((*got_provenance)->served_generation, provenance.served_generation);
}

TEST(SurfaceDbManifest, RoundTrip_Empty) {
  auto bytes = write_db_manifest({}, {});
  ASSERT_TRUE(bytes.has_value());
  auto m = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->symbols().size(), 0u);
  EXPECT_EQ(m->partitions().size(), 0u);
  EXPECT_EQ(m->header().symbol_count, 0u);
  EXPECT_EQ(m->header().partition_count, 0u);
}

TEST(SurfaceDbManifest, LegacyV1ZeroReservedPolicyUsesSafeV2Defaults) {
  SymbolFitConfig configured;
  configured.surface_policy.quality_mode = FitQualityMode::Accuracy;
  configured.surface_policy.outputs = SurfaceOutputs::Risk;
  configured.surface_policy.fallback = SurfaceFallback::None;
  auto bytes = write_db_manifest({{DbSymbolEntry{"SPY", configured}}}, {});
  ASSERT_TRUE(bytes.has_value());

  DbManifestHeader header{};
  std::memcpy(&header, bytes->data(), sizeof header);
  const std::size_t reserved_offset =
      static_cast<std::size_t>(header.symbols_offset) + offsetof(DbSymbolRecord, reserved);
  std::memset(bytes->data() + reserved_offset, 0, 32);
  restamp_crcs(*bytes);

  auto manifest = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(manifest.has_value());
  auto legacy = manifest->find_symbol("SPY");
  ASSERT_TRUE(legacy.has_value());
  EXPECT_EQ(legacy->surface_policy.quality_mode, FitQualityMode::Balanced);
  EXPECT_EQ(legacy->surface_policy.outputs, SurfaceOutputs::MarketMarkAndRisk);
  EXPECT_EQ(legacy->surface_policy.risk_admission, RiskAdmission::Required);
  EXPECT_EQ(legacy->surface_policy.fallback, SurfaceFallback::LastKnownGood);
  auto legacy_provenance = manifest->find_symbol_provenance("SPY");
  ASSERT_TRUE(legacy_provenance.has_value());
  EXPECT_FALSE(legacy_provenance->has_value());
}

TEST(SurfaceDbManifest, Write_RejectsDuplicateAndInvalid) {
  const std::vector<DbSymbolEntry> dup{{"AAPL", {}}, {"aapl", {}}};
  EXPECT_EQ(write_db_manifest(dup, {}).error().code(), ErrorCode::AlreadyExists);
  const std::vector<DbSymbolEntry> empty_sym{{"", {}}};
  EXPECT_EQ(write_db_manifest(empty_sym, {}).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> bad_key{{"bad/key", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, bad_key).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> dotdot{{"..", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, dotdot).error().code(), ErrorCode::InvalidArgument);
  SymbolFitConfig unsafe_risk;
  unsafe_risk.surface_policy.outputs = SurfaceOutputs::Risk;
  unsafe_risk.surface_policy.risk_admission = RiskAdmission::NotApplicable;
  EXPECT_EQ(write_db_manifest({{DbSymbolEntry{"SPY", unsafe_risk}}}, {}).error().code(),
            ErrorCode::InvalidArgument);
}

TEST(SurfaceDbManifest, Open_RejectsCorruption) {
  auto bytes = write_db_manifest({{DbSymbolEntry{"AAPL", {}}}}, {});
  ASSERT_TRUE(bytes.has_value());
  {
    auto bad = *bytes;
    bad[0] ^= std::byte{0xFF}; // magic
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes;
    bad[100] ^= std::byte{0x01}; // header reserved => header CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes;
    bad[200] ^= std::byte{0x01}; // symbol record => payload CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes;
    bad.resize(bad.size() - 1); // truncation
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
}

// Re-stamp both CRCs after a deliberate payload mutation so DbManifest::open
// gets PAST the checksum gates — the record-level (enum wire-range) validation
// must then be what rejects. Mirrors the writer's discipline: payload CRC over
// [symbols_offset, end), then header CRC over the header with its own field
// zeroed, computed last (so it covers the fresh payload_crc32c).
void restamp_crcs(std::vector<std::byte> &bytes) {
  DbManifestHeader h{};
  std::memcpy(&h, bytes.data(), sizeof h);
  const auto symbols_offset = static_cast<std::size_t>(h.symbols_offset);
  const std::uint32_t payload =
      detail::crc32c(bytes.data() + symbols_offset, bytes.size() - symbols_offset);
  std::memcpy(bytes.data() + offsetof(DbManifestHeader, payload_crc32c), &payload, sizeof payload);
  const std::uint32_t zero = 0;
  std::memcpy(bytes.data() + offsetof(DbManifestHeader, header_crc32c), &zero, sizeof zero);
  const std::uint32_t hcrc = detail::crc32c(bytes.data(), sizeof(DbManifestHeader));
  std::memcpy(bytes.data() + offsetof(DbManifestHeader, header_crc32c), &hcrc, sizeof hcrc);
}

TEST(SurfaceDbManifest, Open_RejectsOutOfRangeEnum) {
  auto bytes = write_db_manifest({{DbSymbolEntry{"AAPL", {}}}}, {});
  ASSERT_TRUE(bytes.has_value());

  // Sanity: a restamp with NO mutation must still open — proves the helper
  // reproduces the writer's CRCs, so the rejections below are the enum check.
  {
    auto same = *bytes;
    restamp_crcs(same);
    EXPECT_TRUE(DbManifest::open(std::move(same)).has_value());
  }

  DbManifestHeader h{};
  std::memcpy(&h, bytes->data(), sizeof h);
  // DbSymbolRecord layout: symbol[32], symbol_len (u16 @32), flags (u16 @34),
  // then the uint8 enum run — preset @ +36, curve_kind @ +37.
  const auto preset_off = static_cast<std::size_t>(h.symbols_offset) + 36;
  for (const std::size_t off : {preset_off, preset_off + 1}) {
    auto bad = *bytes;
    bad[off] = std::byte{0xFF}; // outside every enum's wire range
    restamp_crcs(bad);
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError)
        << "enum byte at offset " << off;
  }
}

// ── SurfaceDb: create/open, atomic manifest persistence, symbol CRUD,
// refresh() ─────────────────────────────────────────────────────────────

TEST(SurfaceDb, CreateOpenUpsertReopen_ConfigPersists) {
  const auto root = test_root("create_open"); // helper: fresh temp dir
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->generation(), 1u);
  EXPECT_TRUE(db->symbols().empty());

  const auto cfg = make_full_config();
  SurfaceProvenance provenance;
  provenance.purpose = SurfacePurpose::Risk;
  provenance.quality_mode = FitQualityMode::Accuracy;
  provenance.state = SurfaceState::Degraded;
  provenance.validation.failures = ValidationFailure::Calendar;
  provenance.validation.validation_id = 71211;
  provenance.served_generation = 6;
  ASSERT_TRUE(db->upsert_symbol("aapl", cfg, provenance).has_value());
  EXPECT_EQ(db->generation(), 2u);
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  EXPECT_EQ(db->generation(), 3u);

  auto db2 = SurfaceDb::open(root.string()); // fresh process simulation
  ASSERT_TRUE(db2.has_value());
  EXPECT_EQ(db2->generation(), 3u);
  EXPECT_EQ(db2->symbols(), (std::vector<std::string>{"AAPL", "SPY"}));
  auto got = db2->symbol_config("AAPL");
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);
  auto got_provenance = db2->surface_provenance("AAPL");
  ASSERT_TRUE(got_provenance.has_value());
  ASSERT_TRUE(got_provenance->has_value());
  EXPECT_EQ((*got_provenance)->validation.validation_id, 71211u);
  EXPECT_EQ((*got_provenance)->served_generation, 6u);

  ASSERT_TRUE(db2->remove_symbol("aapl").has_value());
  EXPECT_EQ(db2->symbol_config("AAPL").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db2->remove_symbol("AAPL").error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, UpsertBadEnum_FailsCleanly_DbStillOpens) {
  // Regression for the writer/reader enum-validation asymmetry: a config
  // carrying an out-of-range enum wire value must be rejected by the
  // mutation itself (InvalidArgument), never reach disk, and leave the
  // database fully usable afterward -- not brick every future
  // SurfaceDb::open/refresh in every process.
  const auto root = test_root("upsert_bad_enum");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->generation(), 1u);

  SymbolFitConfig bad;
  bad.preset = static_cast<FitPreset>(250); // outside every enum's wire range
  const auto result = db->upsert_symbol("AAPL", bad);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(db->generation(), 1u); // rejected mutation must not advance generation
  EXPECT_TRUE(db->symbols().empty());

  // A subsequent valid upsert on the same handle still succeeds:
  ASSERT_TRUE(db->upsert_symbol("AAPL", SymbolFitConfig{}).has_value());
  EXPECT_EQ(db->generation(), 2u);

  // And the db still opens cleanly from disk -- the rejected mutation never
  // touched the on-disk manifest.
  auto reopened = SurfaceDb::open(root.string());
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->generation(), 2u);
  EXPECT_EQ(reopened->symbols(), (std::vector<std::string>{"AAPL"}));
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Upsert_SplineVolKind_RejectedCleanly) {
  // SplineVol (= 5) sits above symbol_record_enums_valid's curve_kind <= 4
  // cap by design: it has no ATXVDB v1 wire format (see surface_db.cpp's
  // comment on the cap, mirroring surface_archive.cpp's SplineVol
  // rejection). A config pinning it must fail upsert cleanly -- same
  // InvalidArgument path as UpsertBadEnum_FailsCleanly_DbStillOpens -- and
  // leave the database fully usable afterward.
  const auto root = test_root("upsert_splinevol");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->generation(), 1u);

  SymbolFitConfig bad;
  bad.curve.kind = VolCurveKind::SplineVol;
  const auto result = db->upsert_symbol("AAPL", bad);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(db->generation(), 1u);   // rejected mutation must not advance generation
  EXPECT_TRUE(db->symbols().empty());

  // A subsequent valid upsert on the same handle still succeeds:
  ASSERT_TRUE(db->upsert_symbol("AAPL", SymbolFitConfig{}).has_value());
  EXPECT_EQ(db->generation(), 2u);

  // And the db still opens cleanly from disk -- the rejected mutation never
  // touched the on-disk manifest.
  auto reopened = SurfaceDb::open(root.string());
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->generation(), 2u);
  EXPECT_EQ(reopened->symbols(), (std::vector<std::string>{"AAPL"}));
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Create_RejectsExisting_Open_RejectsMissing) {
  const auto root = test_root("create_guard");
  ASSERT_TRUE(SurfaceDb::create(root.string()).has_value());
  EXPECT_EQ(SurfaceDb::create(root.string()).error().code(), ErrorCode::AlreadyExists);
  const auto missing = test_root("no_such_db");
  EXPECT_EQ(SurfaceDb::open(missing.string()).error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Refresh_SeesExternalWriterUpdate) {
  const auto root = test_root("refresh");
  auto writer = SurfaceDb::create(root.string());
  ASSERT_TRUE(writer.has_value());
  auto reader = SurfaceDb::open(root.string());
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->generation(), 1u);

  ASSERT_TRUE(writer->upsert_symbol("QQQ", SymbolFitConfig{}).has_value());
  // Reader still on its old snapshot until refresh:
  EXPECT_EQ(reader->generation(), 1u);
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  EXPECT_TRUE(reader->symbol_config("QQQ").has_value());
  // Idempotent when current:
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, ConcurrentReaders_DuringUpserts_AreSafe) {
  const auto root = test_root("concurrent");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        auto snap = db->manifest();
        auto cfg = db->symbol_config("SPY");
        EXPECT_TRUE(cfg.has_value());
        (void)snap;
      }
    });
  }
  for (int i = 0; i < 50; ++i) {
    SymbolFitConfig c;
    c.band_k = 1.0 + 0.01 * i;
    ASSERT_TRUE(db->upsert_symbol("SPY", c).has_value());
  }
  stop.store(true);
  for (auto &th : readers)
    th.join();
  auto final_cfg = db->symbol_config("SPY");
  ASSERT_TRUE(final_cfg.has_value());
  EXPECT_DOUBLE_EQ(final_cfg->band_k, 1.0 + 0.01 * 49);
  std::filesystem::remove_all(root);
}

// ── SurfaceDb: partition store ──────────────────────────────────────────────

TEST(SurfaceDbPartition, WriteOpenLoad_TheoBitIdentical) {
  const auto root = test_root("part_roundtrip");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const auto s1 = make_essvi(/*uid=*/1, /*n_slices=*/3);
  const auto s2 = make_essvi(/*uid=*/2, /*n_slices=*/2);
  const std::vector<SurfaceArchiveItem> items{{"AAPL", &s1}, {"MSFT", &s2}};
  ASSERT_TRUE(db->write_partition("2026-07-10", items).has_value());
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->partitions()[0].key, "2026-07-10");
  EXPECT_EQ(db->partitions()[0].surface_count, 2u);

  auto loaded = db->load_surface("2026-07-10", "aapl");
  ASSERT_TRUE(loaded.has_value());
  // Bit-identity assertion block copied from surface_archive_test.cpp's
  // RoundTrip_Essvi_TheoBitIdentical -- same probe points, same bits_equal
  // oracle, no tolerance comparisons.
  EXPECT_EQ(loaded->kind_at(0), VolCurveKind::Essvi);
  expect_theo_bit_identical(s1, *loaded);

  // reopen db cold and load through the fresh instance:
  auto db2 = SurfaceDb::open(root.string());
  ASSERT_TRUE(db2.has_value());
  auto arch = db2->open_partition("2026-07-10");
  ASSERT_TRUE(arch.has_value());
  EXPECT_EQ(arch->count(), 2u);
  ASSERT_TRUE(arch->map_symbol("MSFT").has_value());
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, MixedKinds_ConvexDenseAndLinearVariance_RoundTripBitIdentical) {
  // Explicit requirement: ConvexDense + LinearVariance surfaces fully
  // supported through the db's binary path. One partition holding all three
  // kinds; each loads back with the SAME assertions the archive suite uses.
  const auto root = test_root("part_mixed_kinds");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto sc = make_convex(/*uid=*/11, /*n_slices=*/2, /*n_nodes=*/40);
  const auto sl = make_linear(/*uid=*/12, /*n_slices=*/2, /*n_nodes=*/17);
  const auto se = make_essvi(/*uid=*/13, /*n_slices=*/2);
  const std::vector<SurfaceArchiveItem> items{{"CVX", &sc}, {"LIN", &sl}, {"ESS", &se}};
  ASSERT_TRUE(db->write_partition("2026-07-10", items).has_value());
  auto db2 = SurfaceDb::open(root.string());
  ASSERT_TRUE(db2.has_value());

  auto c = db2->load_surface("2026-07-10", "CVX");
  ASSERT_TRUE(c.has_value());
  // ConvexDense: theo bit-identical AND node arrays byte-equal, copied from
  // RoundTrip_ConvexDense_TheoBitIdentical_AndNodesByteEqual.
  EXPECT_EQ(c->kind_at(0), VolCurveKind::ConvexDense);
  expect_theo_bit_identical(sc, *c);
  for (std::size_t i = 0; i < sc.n_slices(); ++i) {
    const auto *ca = static_cast<const ConvexDenseCurve *>(sc.surface().slices()[i].get());
    const auto *cb = static_cast<const ConvexDenseCurve *>(c->surface().slices()[i].get());
    ASSERT_EQ(ca->fit().u.size(), cb->fit().u.size());
    ASSERT_EQ(ca->fit().C.size(), cb->fit().C.size());
    EXPECT_EQ(
        std::memcmp(ca->fit().u.data(), cb->fit().u.data(), ca->fit().u.size() * sizeof(double)),
        0);
    EXPECT_EQ(
        std::memcmp(ca->fit().C.data(), cb->fit().C.data(), ca->fit().C.size() * sizeof(double)),
        0);
    EXPECT_TRUE(bits_equal(ca->fit().rmse_price, cb->fit().rmse_price));
    EXPECT_EQ(ca->fit().n_obs, cb->fit().n_obs);
    EXPECT_EQ(ca->fit().n_active, cb->fit().n_active);
  }

  auto l = db2->load_surface("2026-07-10", "LIN");
  ASSERT_TRUE(l.has_value());
  // LinearVariance: theo + nodes bit-identical, copied from
  // RoundTrip_LinearVariance_TheoAndNodesBitIdentical.
  EXPECT_EQ(l->kind_at(0), VolCurveKind::LinearVariance);
  expect_theo_bit_identical(sl, *l);
  for (std::size_t i = 0; i < sl.n_slices(); ++i) {
    const auto *a = static_cast<const LinearVarianceCurve *>(sl.surface().slices()[i].get());
    const auto *b = static_cast<const LinearVarianceCurve *>(l->surface().slices()[i].get());
    ASSERT_EQ(a->k_nodes().size(), b->k_nodes().size());
    EXPECT_EQ(
        std::memcmp(a->k_nodes().data(), b->k_nodes().data(), a->k_nodes().size() * sizeof(double)),
        0);
    EXPECT_EQ(
        std::memcmp(a->w_nodes().data(), b->w_nodes().data(), a->w_nodes().size() * sizeof(double)),
        0);
  }

  auto e = db2->load_surface("2026-07-10", "ESS");
  ASSERT_TRUE(e.has_value());
  // Essvi: theo bit-identity.
  EXPECT_EQ(e->kind_at(0), VolCurveKind::Essvi);
  expect_theo_bit_identical(se, *e);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, RewriteReplaces_DropRemoves) {
  const auto root = test_root("part_lifecycle");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto s1 = make_essvi(1, 2);
  const auto s2 = make_essvi(2, 2);
  const std::vector<SurfaceArchiveItem> one{{"AAPL", &s1}};
  const std::vector<SurfaceArchiveItem> two{{"AAPL", &s1}, {"MSFT", &s2}};
  ASSERT_TRUE(db->write_partition("2026-07-10", one).has_value());
  ASSERT_TRUE(db->write_partition("2026-07-10", two).has_value()); // rewrite
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->partitions()[0].surface_count, 2u);

  ASSERT_TRUE(db->drop_partition("2026-07-10").has_value());
  EXPECT_TRUE(db->partitions().empty());
  EXPECT_EQ(db->open_partition("2026-07-10").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db->drop_partition("2026-07-10").error().code(), ErrorCode::NotFound);
  // file physically gone:
  EXPECT_FALSE(
      std::filesystem::exists(std::filesystem::path(root) / "partitions" / "2026-07-10.atxvsa"));
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, ManySymbols_ManyPartitions_SingleSurfaceLookup) {
  const auto root = test_root("part_scale");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::vector<PricedSurface> pool;
  pool.reserve(64);
  for (int i = 0; i < 64; ++i)
    pool.push_back(make_essvi(100 + i, 2));
  for (int p = 0; p < 4; ++p) {
    // NOTE: symbol strings must outlive the write_partition call --
    // SurfaceArchiveItem::symbol is a non-owning string_view, and a
    // temporary std::string built inline in the items initializer would
    // dangle by the time the archive writer reads it. Build an owning
    // std::vector<std::string> holder first, then string_views into it.
    std::vector<std::string> names;
    names.reserve(64);
    for (int i = 0; i < 64; ++i) {
      names.push_back(std::string("SYM") + std::to_string(i));
    }
    std::vector<SurfaceArchiveItem> items;
    items.reserve(64);
    for (int i = 0; i < 64; ++i) {
      items.push_back({names[static_cast<std::size_t>(i)], &pool[static_cast<std::size_t>(i)]});
    }
    ASSERT_TRUE(db->write_partition("2026-07-1" + std::to_string(p), items).has_value());
  }
  EXPECT_EQ(db->partitions().size(), 4u);
  auto s = db->load_surface("2026-07-12", "SYM42");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(db->load_surface("2026-07-12", "NOPE").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db->load_surface("2026-99-99", "SYM1").error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, BadKey_Rejected) {
  const auto root = test_root("part_badkey");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto s1 = make_essvi(1, 2);
  const std::vector<SurfaceArchiveItem> items{{"AAPL", &s1}};
  for (const char *bad : {"", "a/b", "a\\b", "..", "x..y", "0123456789012345678901234567890123"}) {
    EXPECT_EQ(db->write_partition(bad, items).error().code(), ErrorCode::InvalidArgument) << bad;
  }
  std::filesystem::remove_all(root);
}

// ── Fitting-pipeline binding ─────────────────────────────────────────────────

TEST(SurfaceDbApply, PinnedConfig_OverridesPreset) {
  auto cfg = make_full_config(); // pin_curve=true, al_override=true, Hft
  SessionInputs in;
  in.S = 100.0;
  in.r = 0.04;
  in.now_ts_ns = 42; // market snapshot
  apply_symbol_config(cfg, in);
  // market snapshot untouched:
  EXPECT_DOUBLE_EQ(in.S, 100.0);
  EXPECT_DOUBLE_EQ(in.r, 0.04);
  EXPECT_EQ(in.now_ts_ns, 42);
  // explicit fields won over the Hft preset:
  EXPECT_EQ(in.curve.kind, VolCurveKind::ConvexDense);
  EXPECT_EQ(in.curve.convex.node_cap, 56);
  EXPECT_EQ(in.calib.optimization_level, OptimizationLevel::Risk);
  EXPECT_DOUBLE_EQ(in.band_k, 1.25);
  EXPECT_EQ(in.calendar_repair, CalendarRepair::Project);
  EXPECT_FALSE(in.use_correction_cache);
  EXPECT_FALSE(in.score_parity);
  EXPECT_FALSE(in.enforce_calendar_floor);
  EXPECT_TRUE(in.use_deam_cache_for_fit);
  ASSERT_TRUE(in.deam.al_opts.has_value());
  EXPECT_EQ(in.deam.al_opts->n_collocation, 9);
  EXPECT_DOUBLE_EQ(in.deam.al_opts->tol, 1e-9);
}

TEST(SurfaceDbApply, UnpinnedConfig_PresetCurveStands) {
  SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Robust);
  cfg.pin_curve = false;
  SessionInputs via_apply;
  apply_symbol_config(cfg, via_apply);
  SessionInputs via_preset;
  apply_fit_preset(via_preset, FitPreset::Robust);
  // Identity: a config captured from a preset and applied unpinned reproduces
  // apply_fit_preset exactly on the fields SymbolFitConfig carries.
  EXPECT_EQ(via_apply.curve.kind, via_preset.curve.kind);
  EXPECT_DOUBLE_EQ(via_apply.band_k, via_preset.band_k);
  EXPECT_EQ(via_apply.calendar_repair, via_preset.calendar_repair);
  EXPECT_EQ(via_apply.use_correction_cache, via_preset.use_correction_cache);
  EXPECT_EQ(via_apply.score_parity, via_preset.score_parity);
  EXPECT_EQ(via_apply.enforce_calendar_floor, via_preset.enforce_calendar_floor);
  EXPECT_EQ(via_apply.use_deam_cache_for_fit, via_preset.use_deam_cache_for_fit);
  EXPECT_EQ(via_apply.deam.al_opts.has_value(), via_preset.deam.al_opts.has_value());
  if (via_preset.deam.al_opts.has_value()) {
    EXPECT_EQ(via_apply.deam.al_opts->n_collocation, via_preset.deam.al_opts->n_collocation);
    EXPECT_DOUBLE_EQ(via_apply.deam.al_opts->tol, via_preset.deam.al_opts->tol);
  }
}

TEST(SurfaceDbApply, LegacyHftPresetMapsToMarketMarkNotRisk) {
  const SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Hft);
  EXPECT_EQ(cfg.surface_policy.quality_mode, FitQualityMode::Latency);
  EXPECT_EQ(cfg.surface_policy.outputs, SurfaceOutputs::MarketMark);
  EXPECT_EQ(cfg.surface_policy.risk_admission, RiskAdmission::NotApplicable);
  EXPECT_EQ(cfg.surface_policy.fallback, SurfaceFallback::None);
}

// Unwired C-1: apply_symbol_config previously ignored cfg.surface_policy
// entirely, so a persisted per-symbol policy could never reach a live
// PricerConfig/session pipeline. Store a config whose policy deliberately
// diverges from what its own preset would produce (Robust's
// map_legacy_fit_preset default is Balanced + Risk-required), round-trip it
// through a real SurfaceDb, and confirm apply_symbol_config's three-argument
// overload surfaces the STORED policy, not the preset-derived one.
TEST(SurfaceDbApply, StoredPolicyAppliedOverPresetDefault) {
  const auto root = test_root("apply_policy");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Robust);
  ASSERT_EQ(cfg.surface_policy.quality_mode, FitQualityMode::Balanced);
  ASSERT_EQ(cfg.surface_policy.outputs, SurfaceOutputs::Risk);
  // Accuracy + mark-only: differs from the Robust preset default on every
  // SurfacePolicy field.
  cfg.surface_policy.quality_mode = FitQualityMode::Accuracy;
  cfg.surface_policy.outputs = SurfaceOutputs::MarketMark;
  cfg.surface_policy.risk_admission = RiskAdmission::NotApplicable;
  cfg.surface_policy.fallback = SurfaceFallback::None;
  ASSERT_TRUE(db->upsert_symbol("SPY", cfg).has_value());

  auto db2 = SurfaceDb::open(root.string()); // fresh handle: pipeline startup
  ASSERT_TRUE(db2.has_value());
  auto stored = db2->symbol_config("SPY");
  ASSERT_TRUE(stored.has_value());

  SessionInputs in;
  SurfacePolicy policy;
  apply_symbol_config(*stored, in, policy);
  EXPECT_EQ(policy.quality_mode, FitQualityMode::Accuracy);
  EXPECT_EQ(policy.outputs, SurfaceOutputs::MarketMark);
  EXPECT_EQ(policy.risk_admission, RiskAdmission::NotApplicable);
  EXPECT_EQ(policy.fallback, SurfaceFallback::None);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbEndToEnd, ConfigureStoreReloadServe) {
  const auto root = test_root("e2e");
  // Session 1: operator configures the universe + pipeline stores fits.
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    auto spy = symbol_config_from_preset(FitPreset::Robust);
    spy.pin_curve = true;
    spy.curve.kind = VolCurveKind::ConvexDense;
    spy.curve.convex.node_cap = 48;
    ASSERT_TRUE(db->upsert_symbol("SPY", spy).has_value());
    auto aapl = symbol_config_from_preset(FitPreset::Fast);
    aapl.enabled = false;
    ASSERT_TRUE(db->upsert_symbol("AAPL", aapl).has_value());

    // SPY stored as ConvexDense (matches its pinned config; exercises the
    // variable-length-node kind end-to-end), AAPL as Essvi.
    const auto s1 = make_convex(1, 3, 40);
    const auto s2 = make_essvi(2, 3);
    SurfaceProvenance spy_provenance;
    spy_provenance.purpose = SurfacePurpose::Risk;
    spy_provenance.quality_mode = FitQualityMode::Balanced;
    spy_provenance.state = SurfaceState::Healthy;
    spy_provenance.validation.validation_id = 20260711;
    spy_provenance.source_generation = 9;
    spy_provenance.served_generation = 9;
    const std::vector<SurfaceArchiveItem> items{
        {"SPY", &s1, spy_provenance}, {"AAPL", &s2}};
    ASSERT_TRUE(db->write_partition("2026-07-11", items).has_value());
  }
  // Session 2 (fresh open — the fitting pipeline at startup):
  auto db = SurfaceDb::open(root.string());
  ASSERT_TRUE(db.has_value());
  auto spy_cfg = db->symbol_config("SPY");
  ASSERT_TRUE(spy_cfg.has_value());
  EXPECT_TRUE(spy_cfg->enabled);
  auto manifest_provenance = db->surface_provenance("SPY");
  ASSERT_TRUE(manifest_provenance.has_value());
  ASSERT_TRUE(manifest_provenance->has_value());
  EXPECT_EQ((*manifest_provenance)->validation.validation_id, 20260711u);
  auto partition = db->open_partition("2026-07-11");
  ASSERT_TRUE(partition.has_value());
  auto archive_provenance = partition->provenance("SPY");
  ASSERT_TRUE(archive_provenance.has_value());
  EXPECT_EQ(archive_provenance->source_generation, 9u);
  SessionInputs in;
  in.S = 500.0;
  in.r = 0.05;
  apply_symbol_config(*spy_cfg, in);
  EXPECT_EQ(in.curve.kind, VolCurveKind::ConvexDense);
  EXPECT_EQ(in.curve.convex.node_cap, 48);
  auto aapl_cfg = db->symbol_config("AAPL");
  ASSERT_TRUE(aapl_cfg.has_value());
  EXPECT_FALSE(aapl_cfg->enabled); // pipeline skips disabled names
  // Real-time adjustment: another handle flips node_cap; pipeline refreshes.
  {
    auto ops = SurfaceDb::open(root.string());
    ASSERT_TRUE(ops.has_value());
    auto c = *ops->symbol_config("SPY");
    c.curve.convex.node_cap = 64;
    ASSERT_TRUE(ops->upsert_symbol("SPY", c).has_value());
  }
  ASSERT_TRUE(db->refresh().has_value());
  EXPECT_EQ(db->symbol_config("SPY")->curve.convex.node_cap, 64);
  auto preserved_provenance = db->surface_provenance("SPY");
  ASSERT_TRUE(preserved_provenance.has_value());
  ASSERT_TRUE(preserved_provenance->has_value());
  EXPECT_EQ((*preserved_provenance)->validation.validation_id, 20260711u);
  // Stored surfaces still serve:
  ASSERT_TRUE(db->load_surface("2026-07-11", "SPY").has_value());
  std::filesystem::remove_all(root);
}

} // namespace
} // namespace atx::vol
