#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/priced_surface_view.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

// WS-C adversarial suite for ATXVSA2 (v2). Under the lazy-CRC design the reader
// never checks a record's payload CRC on the price path, so the zero-copy
// `PricedSurfaceView` is the de-facto untrusted-input parser: it must reject any
// record the owned `PricedSurface::create`/`reconstruct` path would reject, not
// serve it silently (SE-P1-1 semantic validation, SE-P1-3 column bounds). C4
// then ports the v1 "repaired blob" corpus and the lookup<->directory
// cross-check (SE-P2-7) into this file.

namespace {

using atx::vol::AlOpts;
using atx::vol::AmericanMethod;
using atx::vol::ArchiveV2SurfaceHeader;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::PricedSurface;
using atx::vol::PricedSurfaceView;
using atx::vol::PricingContext;
using atx::vol::SliceContext;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveV2;
using atx::vol::write_surface_archive_v2;

constexpr double kS = 100.0;
constexpr double kR = 0.043;

[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = kS;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 250, 7});
  }
  PricingContext pc;
  pc.S = kS;
  pc.r = kR;
  pc.now_ts_ns = 1700000000000000000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = AlOpts{};
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] std::vector<std::byte> build_v2_single(const PricedSurface &ps, std::string_view sym) {
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{sym, &ps, std::nullopt}};
  auto built = write_surface_archive_v2(items);
  EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}

// The single surface record's exact byte extent, copied into an OWNED buffer.
// std::vector<std::byte> is max_align'd (>= 16 B), so the >= 8-B record-base
// precondition of create_over_record holds — the same guarantee the archive's
// in-file 64-B record alignment gives the mapped path.
struct ExtractedRecord {
  std::vector<std::byte> bytes;
  ArchiveV2SurfaceHeader header{};
};

[[nodiscard]] ExtractedRecord extract_record(const PricedSurface &ps, std::string_view sym) {
  const std::vector<std::byte> archive = build_v2_single(ps, sym);
  ExtractedRecord out;
  auto arch = SurfaceArchiveV2::open(std::vector<std::byte>(archive));
  EXPECT_TRUE(arch.has_value());
  const std::span<const atx::vol::ArchiveV2DirEntry> dir = arch->directory();
  EXPECT_EQ(dir.size(), 1u);
  const std::size_t off = static_cast<std::size_t>(dir[0].surface_offset);
  const std::size_t sz = static_cast<std::size_t>(dir[0].surface_size);
  out.bytes.assign(archive.begin() + static_cast<std::ptrdiff_t>(off),
                   archive.begin() + static_cast<std::ptrdiff_t>(off + sz));
  std::memcpy(&out.header, out.bytes.data(), sizeof out.header);
  return out;
}

void poke_f64(std::vector<std::byte> &rec, std::uint64_t off, double v) {
  ASSERT_LE(off + sizeof v, rec.size());
  std::memcpy(rec.data() + off, &v, sizeof v);
}

void poke_u64(std::vector<std::byte> &rec, std::uint64_t off, std::uint64_t v) {
  ASSERT_LE(off + sizeof v, rec.size());
  std::memcpy(rec.data() + off, &v, sizeof v);
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

} // namespace

// ── SE-P1-1: the view enforces PricedSurface::create's semantic invariants ─────

// Sanity: the UNPATCHED extracted record must parse, so every rejection below is
// attributable to the single field we corrupt (non-vacuous).
TEST(SurfaceArchiveV2Adversarial, BaselineRecordParses) {
  const ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  EXPECT_TRUE(PricedSurfaceView::create_over_record(r.bytes).has_value());
}

// A NaN in the last T slot is the concrete OOB the review found: interp_forward's
// `T <= firstT` / `T >= lastT` are both false against NaN, upper_bound returns
// `end`, and the interior branch reads col_T_[n] — one element past the column.
// The owned reconstruct ParseErrors on this; the view must too.
TEST(SurfaceArchiveV2Adversarial, RejectsNanTColumn) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  const std::uint64_t last = static_cast<std::uint64_t>(r.header.n_slices - 1) * 8u;
  poke_f64(r.bytes, r.header.col_T_off + last, kNaN);
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchiveV2Adversarial, RejectsNonAscendingT) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  double t0 = 0.0;
  std::memcpy(&t0, r.bytes.data() + r.header.col_T_off, sizeof t0);
  poke_f64(r.bytes, r.header.col_T_off + 8, t0 - 0.01); // T[1] < T[0]
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchiveV2Adversarial, RejectsNonPositiveForward) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  poke_f64(r.bytes, r.header.col_forward_off, -1.0);
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchiveV2Adversarial, RejectsNonFiniteForward) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  poke_f64(r.bytes, r.header.col_forward_off + 8, kNaN); // forward[1] = NaN
  EXPECT_FALSE(PricedSurfaceView::create_over_record(r.bytes).has_value());
}

TEST(SurfaceArchiveV2Adversarial, RejectsNonFiniteQEff) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  poke_f64(r.bytes, r.header.col_qeff_off, kNaN);
  EXPECT_FALSE(PricedSurfaceView::create_over_record(r.bytes).has_value());
}

TEST(SurfaceArchiveV2Adversarial, RejectsNonPositiveSpot) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  poke_f64(r.bytes, offsetof(ArchiveV2SurfaceHeader, S), 0.0);
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchiveV2Adversarial, RejectsNonFiniteRate) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  poke_f64(r.bytes, offsetof(ArchiveV2SurfaceHeader, r), kNaN);
  EXPECT_FALSE(PricedSurfaceView::create_over_record(r.bytes).has_value());
}

// ── SE-P1-3: col_borrow/nused/ndropped are in the column bounds conjunction ────
// col_borrow was already validated on this trunk; the review's remaining two
// (nused, ndropped) were not — a corrupt offset past the record extent was
// accepted. reconstruct validates all ten; the view must match.

TEST(SurfaceArchiveV2Adversarial, RejectsOutOfBoundsNusedColumn) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  poke_u64(r.bytes, offsetof(ArchiveV2SurfaceHeader, col_nused_off), r.header.record_size + 8u);
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchiveV2Adversarial, RejectsOutOfBoundsNdroppedColumn) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  poke_u64(r.bytes, offsetof(ArchiveV2SurfaceHeader, col_ndropped_off), r.header.record_size + 8u);
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchiveV2Adversarial, RejectsMisalignedNusedColumn) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  // Non-8-aligned offset (col is u64) — natural-alignment half of the conjunction.
  poke_u64(r.bytes, offsetof(ArchiveV2SurfaceHeader, col_nused_off), r.header.col_nused_off + 4u);
  EXPECT_FALSE(PricedSurfaceView::create_over_record(r.bytes).has_value());
}
