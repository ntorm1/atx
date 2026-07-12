#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/dense_slice.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

// ATXVSA v3 archive suite: full write -> open -> map round-trip with
// BIT-IDENTICAL served theo (iv / fair_value) across all five curve kinds
// (ConvexDense / eSSVI / SVI / LinearVariance / C8), symbol lookup, rejection, and
// concurrent-read safety against a const parsed archive. The design guarantee is
// that a fitted surface of ANY kind reproduces the same prices after a
// serialize/deserialize round-trip.

namespace {

using atx::vol::AlOpts;
using atx::vol::AmericanMethod;
using atx::vol::ArchiveHeader;
using atx::vol::C8Curve;
using atx::vol::C8Params;
using atx::vol::ConvexDenseCurve;
using atx::vol::ConvexSliceFit;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::FitQualityMode;
using atx::vol::LinearVarianceCurve;
using atx::vol::PricedSurface;
using atx::vol::PricingContext;
using atx::vol::Side;
using atx::vol::SliceContext;
using atx::vol::SurfaceArchive;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveWriteOpts;
using atx::vol::SurfaceProvenance;
using atx::vol::SurfacePurpose;
using atx::vol::SurfaceState;
using atx::vol::SviCurve;
using atx::vol::SviParams;
using atx::vol::ValidationFailure;
using atx::vol::VolCurveKind;
using atx::vol::write_surface_archive;

// Bit-exact double comparison via the object representation (NaN-safe: two NaNs
// with the same payload compare equal, which is exactly what a byte round-trip
// must preserve).
[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

void flip_byte(std::vector<std::byte> &b, std::size_t off) {
  ASSERT_LT(off, b.size());
  b[off] ^= std::byte{0xFF};
}

constexpr double kS = 100.0;
constexpr double kR = 0.043;

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kS;
  pc.r = kR;
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
    const double F = kS;
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
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// SVI priced surface, `n` slices.
[[nodiscard]] PricedSurface make_svi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    SviParams v{};
    v.a = 0.02 + 0.001 * static_cast<double>(i);
    v.b = 0.10;
    v.rho = -0.3;
    v.m = 0.0;
    v.sigma = 0.15;
    v.T = T;
    v.F = F;
    v.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<SviCurve>(v, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 180, 4});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// Dense convex priced surface, `n` slices × `nodes` genuine arb-free convex node
// prices (a flat-vol Black-76 call curve → an invertible, finite smile).
[[nodiscard]] PricedSurface make_convex(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    const double df = std::exp(-kR * T);
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
      fit.C[static_cast<std::size_t>(j)] = atx::vol::black76_price(F, K, T, sigma, df, Side::Call);
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
    const double F = kS;
    std::vector<double> k(static_cast<std::size_t>(nodes));
    std::vector<double> w(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double x = -0.4 + 0.8 * static_cast<double>(j) / static_cast<double>(nodes - 1);
      k[static_cast<std::size_t>(j)] = x;
      w[static_cast<std::size_t>(j)] = (0.20 * 0.20 + 0.01 * x + 0.02 * x * x) * T;
    }
    cs.push(
        std::make_unique<LinearVarianceCurve>(T, F, std::exp(-kR * T), std::move(k), std::move(w)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(nodes), 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_c8(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    C8Params c8{};
    c8.T = T;
    c8.F = kS;
    c8.v = (0.22 * 0.22) * T;
    c8.v_min = 0.92 * c8.v;
    c8.psi = -0.01 * T;
    c8.kappa = -0.001 * T;
    c8.q_L = 0.0002 * T;
    c8.q_R = -0.0001 * T;
    c8.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<C8Curve>(c8, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 120, 5});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// Assert a reconstructed surface serves BIT-IDENTICAL theo to the original across
// a (K, T, side) grid: implied vol, total variance, and re-Americanized fair value.
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

[[nodiscard]] std::vector<std::byte> build_one(const PricedSurface &ps, std::string_view symbol) {
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{symbol, &ps}};
  auto built = write_surface_archive(items);
  EXPECT_TRUE(built.has_value());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}

// Full write -> open -> map_symbol, returning the reconstructed surface.
[[nodiscard]] std::optional<PricedSurface>
round_trip(const PricedSurface &ps, std::string_view write_sym, std::string_view read_sym) {
  std::vector<std::byte> buf = build_one(ps, write_sym);
  if (buf.empty()) {
    return std::nullopt;
  }
  auto opened = SurfaceArchive::open(std::move(buf));
  if (!opened.has_value()) {
    return std::nullopt;
  }
  auto mapped = opened->map_symbol(read_sym);
  if (!mapped.has_value()) {
    return std::nullopt;
  }
  return std::move(*mapped);
}

} // namespace

// ── Round-trip: theo bit-identical for every curve kind ───────────────────

TEST(SurfaceArchive, RoundTrip_Essvi_TheoBitIdentical) {
  const PricedSurface orig = make_essvi(42, 5);
  auto got = round_trip(orig, "spy", "SPY"); // case-insensitive
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->kind_at(0), VolCurveKind::Essvi);
  expect_theo_bit_identical(orig, *got);
}

TEST(SurfaceArchive, RoundTrip_Svi_TheoBitIdentical) {
  const PricedSurface orig = make_svi(7, 4);
  auto got = round_trip(orig, "AAPL", "aapl");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->kind_at(0), VolCurveKind::Svi);
  expect_theo_bit_identical(orig, *got);
}

TEST(SurfaceArchive, RoundTrip_ConvexDense_TheoBitIdentical_AndNodesByteEqual) {
  const PricedSurface orig = make_convex(11, 5, 40);
  auto got = round_trip(orig, "QQQ", "QQQ");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->kind_at(0), VolCurveKind::ConvexDense);
  expect_theo_bit_identical(orig, *got);

  // The convex node arrays (variable-length payload) round-trip byte-for-byte.
  for (std::size_t i = 0; i < orig.n_slices(); ++i) {
    const auto *ca = static_cast<const ConvexDenseCurve *>(orig.surface().slices()[i].get());
    const auto *cb = static_cast<const ConvexDenseCurve *>(got->surface().slices()[i].get());
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
}

TEST(SurfaceArchive, RoundTrip_LinearVariance_TheoAndNodesBitIdentical) {
  const PricedSurface orig = make_linear(12, 5, 17);
  auto got = round_trip(orig, "SPY", "spy");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->kind_at(0), VolCurveKind::LinearVariance);
  expect_theo_bit_identical(orig, *got);
  for (std::size_t i = 0; i < orig.n_slices(); ++i) {
    const auto *a = static_cast<const LinearVarianceCurve *>(orig.surface().slices()[i].get());
    const auto *b = static_cast<const LinearVarianceCurve *>(got->surface().slices()[i].get());
    ASSERT_EQ(a->k_nodes().size(), b->k_nodes().size());
    EXPECT_EQ(
        std::memcmp(a->k_nodes().data(), b->k_nodes().data(), a->k_nodes().size() * sizeof(double)),
        0);
    EXPECT_EQ(
        std::memcmp(a->w_nodes().data(), b->w_nodes().data(), a->w_nodes().size() * sizeof(double)),
        0);
  }
}

TEST(SurfaceArchive, RoundTrip_C8_TheoAndParamsBitIdentical) {
  const PricedSurface orig = make_c8(13, 5);
  auto got = round_trip(orig, "AAPL", "aapl");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->kind_at(0), VolCurveKind::C8);
  expect_theo_bit_identical(orig, *got);
  for (std::size_t i = 0; i < orig.n_slices(); ++i) {
    const auto *a = static_cast<const C8Curve *>(orig.surface().slices()[i].get());
    const auto *b = static_cast<const C8Curve *>(got->surface().slices()[i].get());
    EXPECT_EQ(std::memcmp(&a->slice(), &b->slice(), sizeof(C8Params)), 0);
  }
}

// Per-slice re-pricing context (T / forward / q_eff / borrow / counts) round-trips.
TEST(SurfaceArchive, RoundTrip_SliceContext_Preserved) {
  const PricedSurface orig = make_essvi(3, 4);
  auto got = round_trip(orig, "IWM", "IWM");
  ASSERT_TRUE(got.has_value());
  const std::span<const SliceContext> a = orig.context();
  const std::span<const SliceContext> b = got->context();
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_TRUE(bits_equal(a[i].T, b[i].T));
    EXPECT_TRUE(bits_equal(a[i].forward, b[i].forward));
    EXPECT_TRUE(bits_equal(a[i].borrow, b[i].borrow));
    EXPECT_TRUE(bits_equal(a[i].q_eff, b[i].q_eff));
    EXPECT_EQ(a[i].n_used, b[i].n_used);
    EXPECT_EQ(a[i].n_dropped, b[i].n_dropped);
  }
  // Pricing scalars survive.
  EXPECT_TRUE(bits_equal(orig.pricing().S, got->pricing().S));
  EXPECT_TRUE(bits_equal(orig.pricing().r, got->pricing().r));
  EXPECT_EQ(orig.pricing().al_opts.n_collocation, got->pricing().al_opts.n_collocation);
  EXPECT_TRUE(bits_equal(orig.pricing().al_opts.tol, got->pricing().al_opts.tol));
}

TEST(SurfaceArchive, RoundTrip_SurfaceProvenancePreserved) {
  const PricedSurface orig = make_essvi(91, 3);
  SurfaceProvenance provenance;
  provenance.purpose = SurfacePurpose::Risk;
  provenance.quality_mode = FitQualityMode::Accuracy;
  provenance.state = SurfaceState::Stale;
  provenance.validation.failures = ValidationFailure::Calendar | ValidationFailure::StaleInput;
  provenance.validation.validation_id = 0x1234'5678'90AB'CDEFull;
  provenance.source_generation = 81;
  provenance.served_generation = 80;

  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &orig, provenance}};
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value());
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value());
  auto got = archive->provenance("spy");
  ASSERT_TRUE(got.has_value());
  EXPECT_FALSE(got->legacy_format);
  EXPECT_EQ(got->purpose, provenance.purpose);
  EXPECT_EQ(got->quality_mode, provenance.quality_mode);
  EXPECT_EQ(got->state, provenance.state);
  EXPECT_EQ(got->validation.failures, provenance.validation.failures);
  EXPECT_EQ(got->validation.validation_id, provenance.validation.validation_id);
  EXPECT_EQ(got->source_generation, provenance.source_generation);
  EXPECT_EQ(got->served_generation, provenance.served_generation);
}

// Review C-1: Degraded+CarryGap is a routinely SERVED admission state (the
// one publish-with-Degraded reason, produced for carry-gapped boards); it must
// round-trip the archive rather than be refused as an unknown failure bit by
// the known-failures allowlist mask.
TEST(SurfaceArchive, RoundTrip_DegradedCarryGapProvenancePreserved) {
  const PricedSurface orig = make_essvi(94, 3);
  SurfaceProvenance provenance;
  provenance.purpose = SurfacePurpose::Risk;
  provenance.quality_mode = FitQualityMode::Balanced;
  provenance.state = SurfaceState::Degraded;
  provenance.validation.failures = ValidationFailure::CarryGap;
  provenance.validation.validation_id = 0x0FED'CBA9'8765'4321ull;
  provenance.source_generation = 7;
  provenance.served_generation = 7;

  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &orig, provenance}};
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value());
  auto got = archive->provenance("SPY");
  ASSERT_TRUE(got.has_value());
  EXPECT_FALSE(got->legacy_format);
  EXPECT_EQ(got->state, SurfaceState::Degraded);
  EXPECT_EQ(got->validation.failures, ValidationFailure::CarryGap);
  EXPECT_EQ(got->validation.validation_id, provenance.validation.validation_id);
  EXPECT_EQ(got->served_generation, provenance.served_generation);
}

TEST(SurfaceArchive, LegacyV3ZeroReservedBytesDecodeAsUnadmittedMarketMark) {
  const PricedSurface orig = make_linear(92, 3, 9);
  // Two-field aggregate is the pre-provenance writer API and deliberately
  // leaves SurfaceBlobHeader::reserved zero-filled.
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &orig}};
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value());
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value());
  auto got = archive->provenance("SPY");
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got->legacy_format);
  EXPECT_EQ(got->purpose, SurfacePurpose::MarketMark);
  EXPECT_EQ(got->quality_mode, FitQualityMode::Balanced);
  EXPECT_EQ(got->state, SurfaceState::Degraded);
  EXPECT_TRUE(atx::vol::has_validation_failure(got->validation.failures,
                                               ValidationFailure::InsufficientData));
  // Legacy metadata interpretation must not affect price reconstruction.
  auto surface = archive->map_symbol("SPY");
  ASSERT_TRUE(surface.has_value());
  expect_theo_bit_identical(orig, *surface);
}

TEST(SurfaceArchive, WriteRejectsHealthyProvenanceWithValidationFailures) {
  const PricedSurface orig = make_essvi(93, 2);
  SurfaceProvenance inconsistent;
  inconsistent.state = SurfaceState::Healthy;
  inconsistent.validation.failures = ValidationFailure::Butterfly;
  const std::array<SurfaceArchiveItem, 1> items{
      SurfaceArchiveItem{"SPY", &orig, inconsistent}};
  auto result = write_surface_archive(items);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

// ── Lookup ─────────────────────────────────────────────────────────────────

TEST(SurfaceArchive, Lookup_ManySymbols_ResolveEachToUid) {
  constexpr int kN = 500;
  std::vector<std::string> names;
  std::vector<PricedSurface> surfs;
  std::vector<SurfaceArchiveItem> items;
  names.reserve(kN);
  surfs.reserve(kN);
  items.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    char buf[8] = {};
    std::snprintf(buf, sizeof buf, "S%05d", i);
    names.emplace_back(buf);
    surfs.push_back(make_essvi(static_cast<std::uint32_t>(i + 1), 2));
  }
  for (int i = 0; i < kN; ++i) {
    items.push_back(SurfaceArchiveItem{names[static_cast<std::size_t>(i)],
                                       &surfs[static_cast<std::size_t>(i)]});
  }

  auto built = write_surface_archive(items);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  auto opened = SurfaceArchive::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);
  EXPECT_EQ(archive.count(), static_cast<std::uint32_t>(kN));

  int hits = 0;
  for (int i = 0; i < kN; ++i) {
    auto de = archive.find(names[static_cast<std::size_t>(i)]);
    if (de.has_value() && de->uid == static_cast<std::uint32_t>(i + 1)) {
      ++hits;
    }
  }
  EXPECT_EQ(hits, kN);

  auto de = archive.find("s00042"); // lowercase of S00042 -> uid 43
  ASSERT_TRUE(de.has_value());
  EXPECT_EQ(de->uid, 43u);

  auto miss = archive.find("ZZZZZ9");
  ASSERT_FALSE(miss.has_value());
  EXPECT_EQ(miss.error().code(), ErrorCode::NotFound);
  auto miss_map = archive.map_symbol("ZZZZZ9");
  ASSERT_FALSE(miss_map.has_value());
  EXPECT_EQ(miss_map.error().code(), ErrorCode::NotFound);
}

TEST(SurfaceArchive, Lookup_LowLoadFactor_ReservesGrowthRoom) {
  constexpr int kN = 8;
  std::vector<std::string> names;
  std::vector<PricedSurface> surfs;
  std::vector<SurfaceArchiveItem> items;
  for (int i = 0; i < kN; ++i) {
    names.emplace_back("GROW" + std::to_string(i));
    surfs.push_back(make_essvi(static_cast<std::uint32_t>(100 + i), 2));
  }
  for (int i = 0; i < kN; ++i) {
    items.push_back(SurfaceArchiveItem{names[static_cast<std::size_t>(i)],
                                       &surfs[static_cast<std::size_t>(i)]});
  }
  SurfaceArchiveWriteOpts opts;
  opts.lookup_load_pct = 15; // 8 / 0.15 ~= 54 -> next pow2 = 64
  auto built = write_surface_archive(items, opts);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  auto opened = SurfaceArchive::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);
  EXPECT_GT(archive.header().lookup_slot_count, static_cast<std::uint32_t>(4 * kN));
  for (int i = 0; i < kN; ++i) {
    auto m = archive.map_symbol(names[static_cast<std::size_t>(i)]);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->uid(), static_cast<std::uint32_t>(100 + i));
  }
}

// ── Format / corruption ────────────────────────────────────────────────────

TEST(SurfaceArchive, Format_RejectsBadMagic) {
  const PricedSurface s = make_essvi(1, 3);
  std::vector<std::byte> buf = build_one(s, "TEST");
  ASSERT_FALSE(buf.empty());
  flip_byte(buf, 0);
  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
  EXPECT_NE(opened.error().message().find("magic"), std::string::npos);
}

TEST(SurfaceArchive, Format_RejectsCorruptedHeaderCrc) {
  const PricedSurface s = make_essvi(1, 3);
  std::vector<std::byte> buf = build_one(s, "TEST");
  ASSERT_FALSE(buf.empty());
  flip_byte(buf, 120); // header reserved area (covered by header CRC)
  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
  EXPECT_NE(opened.error().message().find("checksum"), std::string::npos);
}

TEST(SurfaceArchive, Format_RejectsSchemaHashMismatch) {
  const PricedSurface s = make_essvi(1, 3);
  std::vector<std::byte> buf = build_one(s, "TEST");
  ASSERT_FALSE(buf.empty());
  flip_byte(buf, 40); // schema_hash field -> checked before the header CRC
  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
  EXPECT_NE(opened.error().message().find("schema"), std::string::npos);
}

TEST(SurfaceArchive, Format_RejectsCorruptedBlobPayload) {
  const PricedSurface s = make_convex(1, 3, 24);
  std::vector<std::byte> buf = build_one(s, "TEST");
  ASSERT_FALSE(buf.empty());

  ArchiveHeader h{};
  std::memcpy(&h, buf.data(), sizeof h);
  const std::size_t target = static_cast<std::size_t>(h.data_offset) + 256; // inside first blob
  flip_byte(buf, target);

  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string(); // header/metadata still valid
  const SurfaceArchive archive = std::move(*opened);
  auto mapped = archive.map_symbol("TEST");
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().code(), ErrorCode::ParseError);
  EXPECT_NE(mapped.error().message().find("checksum"), std::string::npos);
}

TEST(SurfaceArchive, Write_RejectsDuplicateCanonicalSymbol) {
  const PricedSurface s1 = make_essvi(1, 3);
  const PricedSurface s2 = make_essvi(2, 3);
  const std::array<SurfaceArchiveItem, 2> items{
      SurfaceArchiveItem{"AAA", &s1}, SurfaceArchiveItem{"aaa", &s2}, // same canonical symbol
  };
  auto built = write_surface_archive(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::AlreadyExists);
}

TEST(SurfaceArchive, Write_RejectsEmptyAndNull) {
  auto empty = write_surface_archive(std::span<const SurfaceArchiveItem>{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);

  const std::array<SurfaceArchiveItem, 1> nulls{SurfaceArchiveItem{"X", nullptr}};
  auto bad = write_surface_archive(nulls);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
}

// ── Multi-symbol, mixed kinds, map_all, capacity guard ─────────────────────

TEST(SurfaceArchive, MultiSymbol_MixedKinds_MapAll_And_CapacityGuard) {
  // A heterogeneous book: dense-convex index surfaces + parsimonious single-name
  // eSSVI/SVI surfaces in one archive.
  PricedSurface spy = make_convex(1, 4, 32);
  PricedSurface qqq = make_convex(2, 3, 28);
  PricedSurface aapl = make_essvi(3, 5);
  PricedSurface msft = make_essvi(4, 4);
  PricedSurface tsla = make_svi(5, 3);
  PricedSurface xom = make_svi(6, 4);
  const std::array<SurfaceArchiveItem, 6> items{
      SurfaceArchiveItem{"SPY", &spy},   SurfaceArchiveItem{"QQQ", &qqq},
      SurfaceArchiveItem{"AAPL", &aapl}, SurfaceArchiveItem{"MSFT", &msft},
      SurfaceArchiveItem{"TSLA", &tsla}, SurfaceArchiveItem{"XOM", &xom},
  };

  auto built = write_surface_archive(items);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  auto opened = SurfaceArchive::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);
  ASSERT_EQ(archive.count(), 6u);

  // Directory kind_bits reflect the surface's curve kind.
  for (const auto &de : archive.directory()) {
    const std::string sym(de.symbol, de.symbol_len);
    const std::uint16_t convex_bit = 1u << static_cast<unsigned>(VolCurveKind::ConvexDense);
    const std::uint16_t essvi_bit = 1u << static_cast<unsigned>(VolCurveKind::Essvi);
    const std::uint16_t svi_bit = 1u << static_cast<unsigned>(VolCurveKind::Svi);
    if (sym == "SPY" || sym == "QQQ") {
      EXPECT_EQ(de.kind_bits, convex_bit);
    } else if (sym == "AAPL" || sym == "MSFT") {
      EXPECT_EQ(de.kind_bits, essvi_bit);
    } else {
      EXPECT_EQ(de.kind_bits, svi_bit);
    }
  }

  auto all = archive.map_all();
  ASSERT_TRUE(all.has_value()) << all.error().to_string();
  EXPECT_EQ(all->size(), 6u);
  std::array<int, 7> seen{};
  for (const PricedSurface &s : *all) {
    ASSERT_GE(s.uid(), 1u);
    ASSERT_LE(s.uid(), 6u);
    seen[s.uid()]++;
  }
  for (int u = 1; u <= 6; ++u) {
    EXPECT_EQ(seen[static_cast<std::size_t>(u)], 1) << "uid " << u;
  }

  // The dense SPY surface round-trips its served prices bit-identically.
  auto spy_back = archive.map_symbol("SPY");
  ASSERT_TRUE(spy_back.has_value());
  expect_theo_bit_identical(spy, *spy_back);

  // map_all_into capacity guard.
  std::vector<std::optional<PricedSurface>> too_small(5);
  auto capped = archive.map_all_into(too_small);
  ASSERT_FALSE(capped.has_value());
  EXPECT_EQ(capped.error().code(), ErrorCode::OutOfRange);

  std::vector<std::optional<PricedSurface>> outs(6);
  auto wrote = archive.map_all_into(outs);
  ASSERT_TRUE(wrote.has_value()) << wrote.error().to_string();
  EXPECT_EQ(*wrote, 6u);
  for (const auto &o : outs) {
    EXPECT_TRUE(o.has_value());
  }
}

// ── Concurrent reads ───────────────────────────────────────────────────────

TEST(SurfaceArchive, ConcurrentReads_ConstArchive_AreSafe) {
  constexpr int kN = 256;
  std::vector<std::string> names;
  std::vector<PricedSurface> surfs;
  std::vector<SurfaceArchiveItem> items;
  for (int i = 0; i < kN; ++i) {
    char buf[8] = {};
    std::snprintf(buf, sizeof buf, "P%05d", i);
    names.emplace_back(buf);
    // Alternate kinds so concurrent readers exercise both parse paths.
    surfs.push_back((i & 1) ? make_convex(static_cast<std::uint32_t>(i + 1), 3, 20)
                            : make_essvi(static_cast<std::uint32_t>(i + 1), 3));
  }
  for (int i = 0; i < kN; ++i) {
    items.push_back(SurfaceArchiveItem{names[static_cast<std::size_t>(i)],
                                       &surfs[static_cast<std::size_t>(i)]});
  }
  auto built = write_surface_archive(items);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  auto opened = SurfaceArchive::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);

  constexpr int kThreads = 4;
  std::array<int, kThreads> ok{};
  std::vector<std::thread> pool;
  for (int t = 0; t < kThreads; ++t) {
    pool.emplace_back([&archive, &names, &ok, t]() {
      int local_ok = 0;
      for (int i = 0; i < kN; ++i) {
        auto m = archive.map_symbol(names[static_cast<std::size_t>(i)]);
        if (m.has_value() && m->uid() == static_cast<std::uint32_t>(i + 1) && m->n_slices() == 3u) {
          ++local_ok;
        }
      }
      ok[static_cast<std::size_t>(t)] = local_ok;
    });
  }
  for (std::thread &th : pool) {
    th.join();
  }
  for (int t = 0; t < kThreads; ++t) {
    EXPECT_EQ(ok[static_cast<std::size_t>(t)], kN) << "thread " << t;
  }
}
