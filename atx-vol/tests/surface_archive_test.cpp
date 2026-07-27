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
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/dense_slice.hpp"
#include "atx/vol/detail/archive_util.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

#include "../src/slice_payload_padding.hpp" // detail::normalize_c8_payload_padding

// ATXVSA v3 archive suite: full write -> open -> map round-trip with
// BIT-IDENTICAL served theo (iv / fair_value) across all five curve kinds
// (ConvexDense / eSSVI / SVI / LinearVariance / C8), symbol lookup, rejection, and
// concurrent-read safety against a const parsed archive. The design guarantee is
// that a fitted surface of ANY kind reproduces the same prices after a
// serialize/deserialize round-trip.

namespace {

using atx::vol::AlOpts;
using atx::vol::AmericanMethod;
using atx::vol::ArchiveDirEntry;
using atx::vol::ArchivedSurface;
using atx::vol::ArchiveHeader;
using atx::vol::ArchiveIndexSlot;
using atx::vol::ArchivePricingRecord;
using atx::vol::ArchiveSurfaceProvenanceRecord;
using atx::vol::C8Curve;
using atx::vol::C8Params;
using atx::vol::ConvexDenseCurve;
using atx::vol::ConvexSliceFit;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::fit_spline_vol_slice;
using atx::vol::FitObs;
using atx::vol::FitQualityMode;
using atx::vol::LinearVarianceCurve;
using atx::vol::PricedSurface;
using atx::vol::PricingContext;
using atx::vol::Side;
using atx::vol::SliceContext;
using atx::vol::SplineVolCurve;
using atx::vol::SplineVolParams;
using atx::vol::SurfaceArchive;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveWriteOpts;
using atx::vol::SurfaceBlobHeader;
using atx::vol::SurfaceProvenance;
using atx::vol::SurfacePurpose;
using atx::vol::SurfaceState;
using atx::vol::svi_total_w;
using atx::vol::SviCurve;
using atx::vol::SviParams;
using atx::vol::ValidationFailure;
using atx::vol::VolCurveKind;
using atx::vol::write_surface_archive;

static_assert(!std::is_copy_constructible_v<ArchivedSurface>);
static_assert(!std::is_copy_assignable_v<ArchivedSurface>);
static_assert(std::is_nothrow_move_constructible_v<ArchivedSurface>);
static_assert(std::is_nothrow_move_assignable_v<ArchivedSurface>);

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

// `C8Params`'s object representation is WIDER than its value: a gap between
// `expiry_id` and `v`, and a tail gap after `bumps_active`. Padding is not part
// of the value — nothing ever writes it, so a live in-memory struct carries the
// producing thread's stack residue there — and the archive writer deliberately
// canonicalizes it to zero so a record cannot inherit that residue
// (src/slice_payload_padding.hpp). A round-trip assertion must therefore compare
// the VALUE representation: blit the struct, canonicalize the pad, compare. Every
// value byte is still covered, including any field added later.
[[nodiscard]] std::array<std::byte, sizeof(C8Params)> c8_value_bytes(const C8Params &p) noexcept {
  std::array<std::byte, sizeof(C8Params)> bytes{};
  std::memcpy(bytes.data(), &p, sizeof p);
  atx::vol::detail::normalize_c8_payload_padding(bytes.data());
  return bytes;
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

// Raw-SVI-generated observations (copied from spline_curve_test.cpp's
// svi_smile_obs): iv_i = sqrt(svi_total_w(params, k_i) / T), uniform tight
// weights -- deterministic, hand-checkable smile data for the SplineVol fitter.
[[nodiscard]] std::vector<FitObs> svi_smile_obs(const SviParams &p, double T, int n,
                                                double k_half_width) {
  std::vector<FitObs> obs(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = (n > 1) ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.5;
    const double k = -k_half_width + t * (2.0 * k_half_width);
    const double w = svi_total_w(p, k);
    FitObs o;
    o.k = k;
    o.sigma_mkt = std::sqrt(w / T);
    o.weight_w = 1.0;
    obs[static_cast<std::size_t>(i)] = o;
  }
  return obs;
}

// SplineVol priced surface, `n` ascending-T slices, each fit_spline_vol_slice'd
// from an SVI-generated smile (the exact fixture spline_curve_test.cpp uses for
// SplineVol, RecoversSviSmile) -- genuine fitted params, not hand-rolled.
[[nodiscard]] PricedSurface make_spline(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    const double df = std::exp(-kR * T);
    SviParams svi{};
    svi.a = 0.02 + 0.001 * static_cast<double>(i);
    svi.b = 0.4;
    svi.rho = -0.3;
    svi.m = 0.0;
    svi.sigma = 0.4;
    const std::vector<FitObs> obs = svi_smile_obs(svi, T, 25, 0.6);
    auto fitted = fit_spline_vol_slice(obs, F, T, df);
    EXPECT_TRUE(fitted.has_value()) << (fitted.has_value() ? "" : fitted.error().to_string());
    cs.push(std::move(*fitted));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 25, 0});
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

namespace {

void expect_provenance_equal(const SurfaceProvenance &expected, const SurfaceProvenance &actual) {
  EXPECT_EQ(actual.purpose, expected.purpose);
  EXPECT_EQ(actual.quality_mode, expected.quality_mode);
  EXPECT_EQ(actual.state, expected.state);
  EXPECT_EQ(actual.validation.failures, expected.validation.failures);
  EXPECT_EQ(actual.validation.validation_id, expected.validation.validation_id);
  EXPECT_EQ(actual.source_generation, expected.source_generation);
  EXPECT_EQ(actual.served_generation, expected.served_generation);
  EXPECT_EQ(actual.legacy_format, expected.legacy_format);
}

void repair_archive_metadata_and_header_crcs(std::vector<std::byte> &bytes) {
  ArchiveHeader header{};
  ASSERT_GE(bytes.size(), sizeof header);
  std::memcpy(&header, bytes.data(), sizeof header);
  const std::size_t lookup_offset = static_cast<std::size_t>(header.lookup_offset);
  const std::size_t lookup_bytes =
      static_cast<std::size_t>(header.lookup_slot_count) * sizeof(ArchiveIndexSlot);
  const std::size_t directory_offset = static_cast<std::size_t>(header.directory_offset);
  const std::size_t directory_bytes =
      static_cast<std::size_t>(header.surface_count) * sizeof(atx::vol::ArchiveDirEntry);
  std::uint32_t metadata =
      atx::vol::detail::crc32c_update(0xFFFF'FFFFu, bytes.data() + lookup_offset, lookup_bytes);
  metadata =
      atx::vol::detail::crc32c_update(metadata, bytes.data() + directory_offset, directory_bytes) ^
      0xFFFF'FFFFu;
  header.metadata_crc32c = metadata;
  header.header_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveHeader)> header_bytes{};
  std::memcpy(header_bytes.data(), &header, sizeof header);
  header.header_crc32c = atx::vol::detail::crc32c(header_bytes.data(), header_bytes.size());
  std::memcpy(bytes.data(), &header, sizeof header);
}

void replace_first_directory_symbol_length(std::vector<std::byte> &bytes,
                                           std::uint16_t symbol_length) {
  ArchiveHeader header{};
  ASSERT_GE(bytes.size(), sizeof header);
  std::memcpy(&header, bytes.data(), sizeof header);
  ASSERT_GT(header.surface_count, 0u);
  const std::size_t offset = static_cast<std::size_t>(header.directory_offset);
  ArchiveDirEntry entry{};
  ASSERT_LE(offset + sizeof entry, bytes.size());
  std::memcpy(&entry, bytes.data() + offset, sizeof entry);
  entry.symbol_len = symbol_length;
  std::memcpy(bytes.data() + offset, &entry, sizeof entry);
  repair_archive_metadata_and_header_crcs(bytes);
}

void replace_first_lookup_symbol_length(std::vector<std::byte> &bytes,
                                        std::uint16_t symbol_length) {
  ArchiveHeader header{};
  ASSERT_GE(bytes.size(), sizeof header);
  std::memcpy(&header, bytes.data(), sizeof header);
  const std::size_t slot_count = header.lookup_slot_count;
  const std::size_t lookup_offset = static_cast<std::size_t>(header.lookup_offset);
  ArchiveIndexSlot slot{};
  std::size_t occupied_index = slot_count;
  for (std::size_t i = 0; i < slot_count; ++i) {
    std::memcpy(&slot, bytes.data() + lookup_offset + i * sizeof slot, sizeof slot);
    if (slot.flags == atx::vol::kArchiveSlotOccupied) {
      occupied_index = i;
      break;
    }
  }
  ASSERT_LT(occupied_index, slot_count);
  slot.symbol_len = symbol_length;
  std::memcpy(bytes.data() + lookup_offset + occupied_index * sizeof slot, &slot, sizeof slot);
  repair_archive_metadata_and_header_crcs(bytes);
}

void cross_link_first_two_directory_entries(std::vector<std::byte> &bytes) {
  ArchiveHeader header{};
  ASSERT_GE(bytes.size(), sizeof header);
  std::memcpy(&header, bytes.data(), sizeof header);
  ASSERT_GE(header.surface_count, 2u);
  const std::size_t offset = static_cast<std::size_t>(header.directory_offset);
  std::array<ArchiveDirEntry, 2> entries{};
  ASSERT_LE(offset + sizeof entries, bytes.size());
  std::memcpy(entries.data(), bytes.data() + offset, sizeof entries);
  std::swap(entries[0].surface_offset, entries[1].surface_offset);
  std::swap(entries[0].surface_size, entries[1].surface_size);
  std::swap(entries[0].uid, entries[1].uid);
  std::memcpy(bytes.data() + offset, entries.data(), sizeof entries);
  repair_archive_metadata_and_header_crcs(bytes);
}

void replace_first_provenance_state_and_repair_crcs(std::vector<std::byte> &bytes,
                                                    std::uint8_t state) {
  ArchiveHeader header{};
  ASSERT_GE(bytes.size(), sizeof header);
  std::memcpy(&header, bytes.data(), sizeof header);

  const std::size_t slot_count = header.lookup_slot_count;
  const std::size_t lookup_offset = static_cast<std::size_t>(header.lookup_offset);
  ArchiveIndexSlot slot{};
  std::size_t occupied_index = slot_count;
  for (std::size_t i = 0; i < slot_count; ++i) {
    std::memcpy(&slot, bytes.data() + lookup_offset + i * sizeof slot, sizeof slot);
    if (slot.flags == atx::vol::kArchiveSlotOccupied) {
      occupied_index = i;
      break;
    }
  }
  ASSERT_LT(occupied_index, slot_count);

  const std::size_t blob_offset = static_cast<std::size_t>(slot.surface_offset);
  SurfaceBlobHeader blob_header{};
  ASSERT_LE(blob_offset + sizeof blob_header, bytes.size());
  std::memcpy(&blob_header, bytes.data() + blob_offset, sizeof blob_header);
  ArchiveSurfaceProvenanceRecord record{};
  std::memcpy(&record, blob_header.reserved, sizeof record);
  ASSERT_EQ(record.marker, atx::vol::kArchiveProvenanceMarker);
  record.state = state;
  std::memcpy(blob_header.reserved, &record, sizeof record);
  std::memcpy(bytes.data() + blob_offset, &blob_header, sizeof blob_header);

  slot.surface_crc32c = atx::vol::detail::crc32c(bytes.data() + blob_offset,
                                                 static_cast<std::size_t>(slot.surface_size));
  std::memcpy(bytes.data() + lookup_offset + occupied_index * sizeof slot, &slot, sizeof slot);
  repair_archive_metadata_and_header_crcs(bytes);
}

struct FirstBlobLocation {
  ArchiveHeader archive{};
  ArchiveIndexSlot slot{};
  std::size_t slot_offset{};
  std::size_t blob_offset{};
};

[[nodiscard]] FirstBlobLocation first_blob_location(const std::vector<std::byte> &bytes) {
  FirstBlobLocation location;
  EXPECT_GE(bytes.size(), sizeof location.archive);
  if (bytes.size() < sizeof location.archive) {
    return location;
  }
  std::memcpy(&location.archive, bytes.data(), sizeof location.archive);
  const std::size_t lookup_offset = static_cast<std::size_t>(location.archive.lookup_offset);
  const std::size_t slot_count = location.archive.lookup_slot_count;
  for (std::size_t i = 0; i < slot_count; ++i) {
    const std::size_t slot_offset = lookup_offset + i * sizeof(ArchiveIndexSlot);
    EXPECT_LE(slot_offset + sizeof(ArchiveIndexSlot), bytes.size());
    if (slot_offset + sizeof(ArchiveIndexSlot) > bytes.size()) {
      return location;
    }
    ArchiveIndexSlot slot{};
    std::memcpy(&slot, bytes.data() + slot_offset, sizeof slot);
    if (slot.flags == atx::vol::kArchiveSlotOccupied) {
      location.slot = slot;
      location.slot_offset = slot_offset;
      location.blob_offset = static_cast<std::size_t>(slot.surface_offset);
      return location;
    }
  }
  ADD_FAILURE() << "archive has no occupied lookup slot";
  return location;
}

void repair_first_blob_and_archive_crcs(std::vector<std::byte> &bytes) {
  FirstBlobLocation location = first_blob_location(bytes);
  ASSERT_GT(location.slot.surface_size, 0u);
  ASSERT_LE(location.blob_offset + static_cast<std::size_t>(location.slot.surface_size),
            bytes.size());
  location.slot.surface_crc32c = atx::vol::detail::crc32c(
      bytes.data() + location.blob_offset, static_cast<std::size_t>(location.slot.surface_size));
  std::memcpy(bytes.data() + location.slot_offset, &location.slot, sizeof location.slot);
  repair_archive_metadata_and_header_crcs(bytes);
}

template <typename Mutator>
void mutate_first_blob_header(std::vector<std::byte> &bytes, Mutator mutator) {
  const FirstBlobLocation location = first_blob_location(bytes);
  SurfaceBlobHeader blob{};
  ASSERT_LE(location.blob_offset + sizeof blob, bytes.size());
  std::memcpy(&blob, bytes.data() + location.blob_offset, sizeof blob);
  mutator(blob);
  std::memcpy(bytes.data() + location.blob_offset, &blob, sizeof blob);
  repair_first_blob_and_archive_crcs(bytes);
}

void replace_first_pricing_uid(std::vector<std::byte> &bytes, std::uint32_t uid) {
  const FirstBlobLocation location = first_blob_location(bytes);
  SurfaceBlobHeader blob{};
  ASSERT_LE(location.blob_offset + sizeof blob, bytes.size());
  std::memcpy(&blob, bytes.data() + location.blob_offset, sizeof blob);
  const std::size_t pricing_offset =
      location.blob_offset + static_cast<std::size_t>(blob.pricing_offset);
  ArchivePricingRecord pricing{};
  ASSERT_LE(pricing_offset + sizeof pricing, bytes.size());
  std::memcpy(&pricing, bytes.data() + pricing_offset, sizeof pricing);
  pricing.uid = uid;
  std::memcpy(bytes.data() + pricing_offset, &pricing, sizeof pricing);
  repair_first_blob_and_archive_crcs(bytes);
}

void replace_first_blob_symbol_byte(std::vector<std::byte> &bytes, char value) {
  const FirstBlobLocation location = first_blob_location(bytes);
  SurfaceBlobHeader blob{};
  ASSERT_LE(location.blob_offset + sizeof blob, bytes.size());
  std::memcpy(&blob, bytes.data() + location.blob_offset, sizeof blob);
  const std::size_t symbol_offset =
      location.blob_offset + static_cast<std::size_t>(blob.symbol_offset);
  ASSERT_GT(blob.symbol_size, 0u);
  ASSERT_LT(symbol_offset, bytes.size());
  bytes[symbol_offset] = static_cast<std::byte>(value);
  repair_first_blob_and_archive_crcs(bytes);
}

void shift_first_pricing_section_to_unaligned_offset(std::vector<std::byte> &bytes) {
  const FirstBlobLocation location = first_blob_location(bytes);
  SurfaceBlobHeader blob{};
  ASSERT_LE(location.blob_offset + sizeof blob, bytes.size());
  std::memcpy(&blob, bytes.data() + location.blob_offset, sizeof blob);
  const std::size_t old_offset =
      location.blob_offset + static_cast<std::size_t>(blob.pricing_offset);
  const std::size_t new_offset = old_offset + 1u;
  std::array<std::byte, sizeof(ArchivePricingRecord)> pricing{};
  ASSERT_LE(new_offset + pricing.size(), bytes.size());
  std::memcpy(pricing.data(), bytes.data() + old_offset, pricing.size());
  std::memcpy(bytes.data() + new_offset, pricing.data(), pricing.size());
  ++blob.pricing_offset;
  std::memcpy(bytes.data() + location.blob_offset, &blob, sizeof blob);
  repair_first_blob_and_archive_crcs(bytes);
}

void replace_first_directory_summary(std::vector<std::byte> &bytes,
                                     std::optional<std::uint16_t> n_slices,
                                     std::optional<std::uint16_t> kind_bits) {
  ArchiveHeader header{};
  ASSERT_GE(bytes.size(), sizeof header);
  std::memcpy(&header, bytes.data(), sizeof header);
  ASSERT_GT(header.surface_count, 0u);
  const std::size_t offset = static_cast<std::size_t>(header.directory_offset);
  ArchiveDirEntry entry{};
  ASSERT_LE(offset + sizeof entry, bytes.size());
  std::memcpy(&entry, bytes.data() + offset, sizeof entry);
  if (n_slices.has_value()) {
    entry.n_slices = *n_slices;
  }
  if (kind_bits.has_value()) {
    entry.kind_bits = *kind_bits;
  }
  std::memcpy(bytes.data() + offset, &entry, sizeof entry);
  repair_archive_metadata_and_header_crcs(bytes);
}

void replace_first_lookup_and_directory_uid(std::vector<std::byte> &bytes, std::uint32_t uid) {
  FirstBlobLocation location = first_blob_location(bytes);
  location.slot.uid = uid;
  std::memcpy(bytes.data() + location.slot_offset, &location.slot, sizeof location.slot);
  const std::size_t directory_offset = static_cast<std::size_t>(location.archive.directory_offset);
  ArchiveDirEntry entry{};
  ASSERT_LE(directory_offset + sizeof entry, bytes.size());
  std::memcpy(&entry, bytes.data() + directory_offset, sizeof entry);
  entry.uid = uid;
  std::memcpy(bytes.data() + directory_offset, &entry, sizeof entry);
  repair_archive_metadata_and_header_crcs(bytes);
}

void replace_first_provenance_reserved_and_repair_crcs(std::vector<std::byte> &bytes,
                                                       std::uint8_t reserved0,
                                                       std::uint32_t reserved1) {
  mutate_first_blob_header(bytes, [reserved0, reserved1](SurfaceBlobHeader &blob) {
    ArchiveSurfaceProvenanceRecord record{};
    std::memcpy(&record, blob.reserved, sizeof record);
    EXPECT_EQ(record.marker, atx::vol::kArchiveProvenanceMarker);
    record.reserved0 = reserved0;
    record.reserved1 = reserved1;
    std::memcpy(blob.reserved, &record, sizeof record);
  });
}

void expect_map_symbol_parse_error(std::vector<std::byte> bytes, std::string_view symbol) {
  auto archive = SurfaceArchive::open(std::move(bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  const auto mapped = archive->map_symbol(symbol);
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().code(), ErrorCode::ParseError);
}

void expect_bulk_map_parse_error(std::vector<std::byte> bytes) {
  auto archive = SurfaceArchive::open(std::move(bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  const auto plain = archive->map_all();
  ASSERT_FALSE(plain.has_value());
  EXPECT_EQ(plain.error().code(), ErrorCode::ParseError);
  const auto paired = archive->map_all_with_provenance();
  ASSERT_FALSE(paired.has_value());
  EXPECT_EQ(paired.error().code(), ErrorCode::ParseError);
}

void expect_all_maps_parse_error_containing(std::vector<std::byte> bytes, std::string_view needle) {
  auto archive = SurfaceArchive::open(std::move(bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();
  const auto symbol = archive->map_symbol("TEST");
  ASSERT_FALSE(symbol.has_value());
  EXPECT_EQ(symbol.error().code(), ErrorCode::ParseError);
  EXPECT_NE(symbol.error().message().find(needle), std::string::npos);
  const auto plain = archive->map_all();
  ASSERT_FALSE(plain.has_value());
  EXPECT_EQ(plain.error().code(), ErrorCode::ParseError);
  EXPECT_NE(plain.error().message().find(needle), std::string::npos);
  const auto paired = archive->map_all_with_provenance();
  ASSERT_FALSE(paired.has_value());
  EXPECT_EQ(paired.error().code(), ErrorCode::ParseError);
  EXPECT_NE(paired.error().message().find(needle), std::string::npos);
}

} // namespace

TEST(SurfaceArchive, RoundTrip_OffPillarCarryPreservesForwardIdentity) {
  const PricedSurface orig = make_essvi(42, 5);
  auto got = round_trip(orig, "SPY", "spy");
  ASSERT_TRUE(got.has_value());

  const std::span<const SliceContext> pillars = orig.context();
  ASSERT_GE(pillars.size(), std::size_t{2});
  const double probes[] = {
      pillars.front().T * 0.5,
      0.5 * (pillars[1].T + pillars[2].T),
      pillars.back().T * 1.5,
  };
  for (const double T : probes) {
    const double forward = got->forward_at(T);
    EXPECT_NEAR(got->pricing().S * std::exp((got->rate_at(T) - got->q_eff_at(T)) * T), forward,
                2.0e-13 * forward)
        << "T=" << T;
    EXPECT_DOUBLE_EQ(forward, orig.forward_at(T));
    EXPECT_NEAR(got->q_eff_at(T), orig.q_eff_at(T), 2.0e-13) << "T=" << T;
  }
  EXPECT_DOUBLE_EQ(got->q_eff_at(probes[0]), pillars.front().q_eff);
  EXPECT_DOUBLE_EQ(got->q_eff_at(probes[2]), pillars.back().q_eff);

  for (const SliceContext &pillar : pillars) {
    EXPECT_DOUBLE_EQ(got->forward_at(pillar.T), pillar.forward);
    EXPECT_DOUBLE_EQ(got->q_eff_at(pillar.T), pillar.q_eff);
  }
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
    const auto a_bytes = c8_value_bytes(a->slice());
    const auto b_bytes = c8_value_bytes(b->slice());
    EXPECT_EQ(std::memcmp(a_bytes.data(), b_bytes.data(), sizeof(C8Params)), 0);
  }
}

TEST(SurfaceArchive, SplineVolRoundTripBitExact) {
  const PricedSurface orig = make_spline(21, 4);
  auto got = round_trip(orig, "SPLN", "spln");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->kind_at(0), VolCurveKind::SplineVol);
  expect_theo_bit_identical(orig, *got);

  for (std::size_t i = 0; i < orig.n_slices(); ++i) {
    const auto *a = static_cast<const SplineVolCurve *>(orig.surface().slices()[i].get());
    const auto *b = static_cast<const SplineVolCurve *>(got->surface().slices()[i].get());
    const SplineVolParams &pa = a->params();
    const SplineVolParams &pb = b->params();
    // Byte-equality of the re-serialized payload: every scalar + array field
    // that write_surface_archive packs into the ATXVSA payload compares
    // byte-for-byte (memcmp on the arrays, bit-exact on the scalars) --
    // mirrors RoundTrip_LinearVariance_TheoAndNodesBitIdentical's node-array
    // memcmp and RoundTrip_C8_TheoAndParamsBitIdentical's whole-struct memcmp.
    EXPECT_TRUE(bits_equal(pa.atm_vol, pb.atm_vol));
    EXPECT_TRUE(bits_equal(pa.z_lo_valid, pb.z_lo_valid));
    EXPECT_TRUE(bits_equal(pa.z_hi_valid, pb.z_hi_valid));
    ASSERT_EQ(pa.z.size(), pb.z.size());
    ASSERT_EQ(pa.mult.size(), pb.mult.size());
    EXPECT_EQ(std::memcmp(pa.z.data(), pb.z.data(), pa.z.size() * sizeof(double)), 0);
    EXPECT_EQ(std::memcmp(pa.mult.data(), pb.mult.data(), pa.mult.size() * sizeof(double)), 0);
    EXPECT_EQ(pa.n_butterfly_viol, pb.n_butterfly_viol);
  }

  // w() equality on a 64-pt k-grid, bit-exact (==, not NEAR).
  for (std::size_t i = 0; i < orig.n_slices(); ++i) {
    const auto *a = orig.surface().slices()[i].get();
    const auto *b = got->surface().slices()[i].get();
    for (int g = -32; g <= 31; ++g) {
      const double k = 0.01 * static_cast<double>(g);
      EXPECT_TRUE(bits_equal(a->w(k), b->w(k))) << "slice " << i << " k=" << k;
    }
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

TEST(SurfaceArchive, MapAllWithProvenance_PairsExplicitMetadataInDirectoryOrder) {
  const PricedSurface zzz = make_essvi(31, 3);
  const PricedSurface aaa = make_svi(11, 2);
  const PricedSurface mmm = make_linear(21, 4, 9);

  SurfaceProvenance zzz_provenance;
  zzz_provenance.purpose = SurfacePurpose::MarketMark;
  zzz_provenance.quality_mode = FitQualityMode::Latency;
  zzz_provenance.state = SurfaceState::Stale;
  zzz_provenance.validation.failures = ValidationFailure::StaleInput;
  zzz_provenance.validation.validation_id = 301;
  zzz_provenance.source_generation = 31;
  zzz_provenance.served_generation = 30;

  SurfaceProvenance aaa_provenance;
  aaa_provenance.purpose = SurfacePurpose::Risk;
  aaa_provenance.quality_mode = FitQualityMode::Accuracy;
  aaa_provenance.validation.validation_id = 101;
  aaa_provenance.source_generation = 11;
  aaa_provenance.served_generation = 11;

  SurfaceProvenance mmm_provenance;
  mmm_provenance.purpose = SurfacePurpose::MarketMark;
  mmm_provenance.quality_mode = FitQualityMode::Balanced;
  mmm_provenance.state = SurfaceState::Degraded;
  mmm_provenance.validation.failures = ValidationFailure::CarryGap;
  mmm_provenance.validation.validation_id = 201;
  mmm_provenance.source_generation = 21;
  mmm_provenance.served_generation = 21;

  const std::array<SurfaceArchiveItem, 3> items{
      SurfaceArchiveItem{"ZZZ", &zzz, zzz_provenance},
      SurfaceArchiveItem{"AAA", &aaa, aaa_provenance},
      SurfaceArchiveItem{"MMM", &mmm, mmm_provenance},
  };
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();

  auto mapped = archive->map_all_with_provenance();
  ASSERT_TRUE(mapped.has_value()) << mapped.error().to_string();
  ASSERT_EQ(mapped->size(), 3u);
  EXPECT_EQ((*mapped)[0].surface.uid(), 11u);
  expect_provenance_equal(aaa_provenance, (*mapped)[0].provenance);
  EXPECT_EQ((*mapped)[1].surface.uid(), 21u);
  expect_provenance_equal(mmm_provenance, (*mapped)[1].provenance);
  EXPECT_EQ((*mapped)[2].surface.uid(), 31u);
  expect_provenance_equal(zzz_provenance, (*mapped)[2].provenance);
}

TEST(SurfaceArchive, MapAllWithProvenance_LegacyZeroRecordUsesSafeLegacyProvenance) {
  const PricedSurface original = make_linear(92, 3, 9);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &original}};
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();

  auto mapped = archive->map_all_with_provenance();
  ASSERT_TRUE(mapped.has_value()) << mapped.error().to_string();
  ASSERT_EQ(mapped->size(), 1u);
  expect_provenance_equal(atx::vol::legacy_surface_provenance(), mapped->front().provenance);
  expect_theo_bit_identical(original, mapped->front().surface);
}

TEST(SurfaceArchive, MapAllWithProvenance_MalformedTaggedRecordIsParseError) {
  const PricedSurface original = make_essvi(93, 3);
  SurfaceProvenance provenance;
  provenance.validation.validation_id = 77;
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &original, provenance}};
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  replace_first_provenance_state_and_repair_crcs(*bytes, 0xFFu);
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();

  auto mapped = archive->map_all_with_provenance();
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchive, MapAllWithProvenance_RepairedReservedProvenanceFlagsAreParseErrors) {
  const PricedSurface original = make_essvi(93, 3);
  SurfaceProvenance provenance;
  provenance.validation.validation_id = 77;
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &original, provenance}};

  for (const auto [reserved0, reserved1] : {std::pair<std::uint8_t, std::uint32_t>{1u, 0u},
                                            std::pair<std::uint8_t, std::uint32_t>{0u, 1u}}) {
    SCOPED_TRACE(testing::Message()
                 << "reserved0=" << static_cast<unsigned>(reserved0) << " reserved1=" << reserved1);
    auto bytes = write_surface_archive(items);
    ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
    replace_first_provenance_reserved_and_repair_crcs(*bytes, reserved0, reserved1);
    expect_bulk_map_parse_error(std::move(*bytes));
  }
}

TEST(SurfaceArchive, WriteRejectsHealthyProvenanceWithValidationFailures) {
  const PricedSurface orig = make_essvi(93, 2);
  SurfaceProvenance inconsistent;
  inconsistent.state = SurfaceState::Healthy;
  inconsistent.validation.failures = ValidationFailure::Butterfly;
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &orig, inconsistent}};
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

TEST(SurfaceArchive, MapSymbol_RejectsRepairedBlobHeaderUidMismatch) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  mutate_first_blob_header(bytes, [](SurfaceBlobHeader &blob) { blob.uid = 99u; });
  expect_map_symbol_parse_error(std::move(bytes), "TEST");
}

TEST(SurfaceArchive, MapSymbol_RejectsRepairedPricingUidMismatch) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  replace_first_pricing_uid(bytes, 99u);
  expect_map_symbol_parse_error(std::move(bytes), "TEST");
}

TEST(SurfaceArchive, MapSymbol_RejectsRepairedSymbolSectionOutOfBounds) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  mutate_first_blob_header(bytes,
                           [](SurfaceBlobHeader &blob) { blob.symbol_offset = blob.blob_size; });
  expect_map_symbol_parse_error(std::move(bytes), "TEST");
}

TEST(SurfaceArchive, MapSymbol_RejectsRepairedSymbolLengthMismatch) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  mutate_first_blob_header(bytes, [](SurfaceBlobHeader &blob) { ++blob.symbol_size; });
  expect_map_symbol_parse_error(std::move(bytes), "TEST");
}

TEST(SurfaceArchive, MapSymbol_RejectsRepairedSymbolContentMismatch) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  replace_first_blob_symbol_byte(bytes, 'X');
  expect_map_symbol_parse_error(std::move(bytes), "TEST");
}

TEST(SurfaceArchive, MapAll_RejectsRepairedDirectorySliceCountMismatch) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  replace_first_directory_summary(bytes, 4u, std::nullopt);
  expect_bulk_map_parse_error(std::move(bytes));
}

TEST(SurfaceArchive, MapAll_RejectsRepairedDirectoryKindBitsMismatch) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  replace_first_directory_summary(bytes, std::nullopt, 0u);
  expect_bulk_map_parse_error(std::move(bytes));
}

TEST(SurfaceArchive, MapAll_RejectsLookupUidThatWouldBeAbsentFromMappedSurfaceSet) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  replace_first_lookup_and_directory_uid(bytes, 99u);
  expect_bulk_map_parse_error(std::move(bytes));
}

TEST(SurfaceArchive, MappingRejectsRepairedOverlappingPricingAndSlicesSections) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  mutate_first_blob_header(bytes, [](SurfaceBlobHeader &blob) {
    blob.pricing_size = blob.slices_offset - blob.pricing_offset + atx::vol::kArchiveArrayAlign;
  });
  expect_all_maps_parse_error_containing(std::move(bytes), "topology");
}

TEST(SurfaceArchive, MappingRejectsRepairedOutOfOrderSections) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  mutate_first_blob_header(
      bytes, [](SurfaceBlobHeader &blob) { blob.slices_offset = blob.symbol_offset; });
  expect_all_maps_parse_error_containing(std::move(bytes), "topology");
}

TEST(SurfaceArchive, MappingRejectsRepairedUnalignedPricingSection) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  shift_first_pricing_section_to_unaligned_offset(bytes);
  expect_all_maps_parse_error_containing(std::move(bytes), "alignment");
}

TEST(SurfaceArchive, MappingRejectsRepairedUnconsumedSliceTail) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  mutate_first_blob_header(bytes, [](SurfaceBlobHeader &blob) { --blob.n_slices; });
  replace_first_directory_summary(bytes, 2u, std::nullopt);
  expect_all_maps_parse_error_containing(std::move(bytes), "consumption");
}

TEST(SurfaceArchive, Open_RejectsOversizedDirectorySymbolLength) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  ASSERT_FALSE(bytes.empty());
  replace_first_directory_symbol_length(
      bytes, static_cast<std::uint16_t>(atx::vol::kArchiveSymbolMax + 1u));

  auto opened = SurfaceArchive::open(std::move(bytes));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchive, Open_RejectsOversizedLookupSymbolLength) {
  const PricedSurface surface = make_essvi(1, 3);
  std::vector<std::byte> bytes = build_one(surface, "TEST");
  ASSERT_FALSE(bytes.empty());
  replace_first_lookup_symbol_length(bytes,
                                     static_cast<std::uint16_t>(atx::vol::kArchiveSymbolMax + 1u));

  auto opened = SurfaceArchive::open(std::move(bytes));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchive, MapAll_RejectsCrossLinkedDirectoryAndLookupRecords) {
  const PricedSurface aaa = make_essvi(1, 3);
  const PricedSurface bbb = make_essvi(2, 3);
  const std::array<SurfaceArchiveItem, 2> items{
      SurfaceArchiveItem{"AAA", &aaa},
      SurfaceArchiveItem{"BBB", &bbb},
  };
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  cross_link_first_two_directory_entries(*bytes);
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();

  auto plain = archive->map_all();
  ASSERT_FALSE(plain.has_value());
  EXPECT_EQ(plain.error().code(), ErrorCode::ParseError);
  EXPECT_NE(plain.error().message().find("directory/lookup mismatch"), std::string::npos);

  auto paired = archive->map_all_with_provenance();
  ASSERT_FALSE(paired.has_value());
  EXPECT_EQ(paired.error().code(), ErrorCode::ParseError);
  EXPECT_NE(paired.error().message().find("directory/lookup mismatch"), std::string::npos);
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

TEST(SurfaceArchive, Write_RejectsExplicitLegacyProvenanceRecord) {
  const PricedSurface surface = make_essvi(1, 3);
  const SurfaceProvenance legacy = atx::vol::legacy_surface_provenance();
  ASSERT_TRUE(legacy.legacy_format);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &surface, legacy}};

  auto built = write_surface_archive(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::InvalidArgument);
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

TEST(SurfaceArchive, MapAllWithProvenance_NumericallyMatchesMapAll) {
  const PricedSurface spy = make_convex(1, 4, 32);
  const PricedSurface aapl = make_essvi(2, 5);
  const PricedSurface xom = make_svi(3, 3);
  const std::array<SurfaceArchiveItem, 3> items{
      SurfaceArchiveItem{"XOM", &xom},
      SurfaceArchiveItem{"SPY", &spy},
      SurfaceArchiveItem{"AAPL", &aapl},
  };
  auto bytes = write_surface_archive(items);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto archive = SurfaceArchive::open(std::move(*bytes));
  ASSERT_TRUE(archive.has_value()) << archive.error().to_string();

  auto legacy = archive->map_all();
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  auto paired = archive->map_all_with_provenance();
  ASSERT_TRUE(paired.has_value()) << paired.error().to_string();
  ASSERT_EQ(legacy->size(), paired->size());
  for (std::size_t i = 0; i < legacy->size(); ++i) {
    EXPECT_EQ((*legacy)[i].uid(), (*paired)[i].surface.uid());
    expect_theo_bit_identical((*legacy)[i], (*paired)[i].surface);
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
