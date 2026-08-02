#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/detail/archive_util.hpp" // crc32c / crc32c_update (metadata restamp)
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
using atx::vol::ArchiveV2DirEntry;
using atx::vol::ArchiveV2Header;
using atx::vol::ArchiveV2LookupSlot;
using atx::vol::ArchiveV2SurfaceHeader;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::LinearVarianceCurve;
using atx::vol::PricedSurface;
using atx::vol::PricedSurfaceView;
using atx::vol::PricingContext;
using atx::vol::SliceContext;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveV2;
using atx::vol::SurfaceProvenance;
using atx::vol::SurfaceState;
using atx::vol::ValidationFailure;
using atx::vol::write_surface_archive_v2;
using atx::vol::write_surface_archive_v2_file;

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

// A LinearVariance surface: `n` slices, each carrying `nodes` STRICTLY ASCENDING
// k[] node keys (the layout `PricedSurfaceView::slice_w` binary-searches).
// Mirrors surface_archive_v2_test.cpp's make_linear.
[[nodiscard]] PricedSurface make_linear(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    std::vector<double> k(static_cast<std::size_t>(nodes));
    std::vector<double> w(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double x = -0.4 + 0.8 * static_cast<double>(j) / static_cast<double>(nodes - 1);
      k[static_cast<std::size_t>(j)] = x;
      w[static_cast<std::size_t>(j)] = (0.20 * 0.20 + 0.01 * x + 0.02 * x * x) * T;
    }
    cs.push(std::make_unique<LinearVarianceCurve>(T, kS, std::exp(-kR * T), std::move(k),
                                                  std::move(w)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, static_cast<std::size_t>(nodes), 2});
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

[[nodiscard]] std::uint64_t peek_u64(const std::vector<std::byte> &rec, std::uint64_t off) {
  std::uint64_t v = 0;
  std::memcpy(&v, rec.data() + off, sizeof v);
  return v;
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

// ── S2-2.1: the LinearVariance node axis is a validated binary-search key ─────
//
// Both parsers over a v2 record (`PricedSurfaceView::create_over_record`, the
// zero-copy one, and `reconstruct_v2_record` behind `reconstruct_*`) validated
// each slice payload's EXTENT but never its CONTENT. The LinearVariance k[] node
// array is a binary-search key: `PricedSurfaceView::slice_w` /
// `LinearVarianceCurve::w` locate a bracket with `std::lower_bound` and then
// index at `lo = hi - 1`, so `hi == 0` makes that subscript `SIZE_MAX`.
//
// The two wing guards (`k_log <= k[0]`, `k_log >= k[nc-1]`) pin the returned
// index into [1, nc-1] for any ORDERED k[] — but a NaN node compares false in
// every direction, so both guards fall through AND lower_bound walks to the
// front and returns 0. `k[SIZE_MAX]` / `w[SIZE_MAX]` is then an out-of-bounds
// read inside a `noexcept` concurrent query. An unordered (finite) k[] is not
// UB but silently breaks lower_bound's precondition and interpolates across a
// bracket that does not contain the query.
//
// Validation belongs at record/view creation (O(nodes) once, off the hot query
// path), not per query.

namespace {

// Whole-archive corruption seam for the OWNED reconstruct path. A record BODY is
// not covered by metadata_crc32c and `open` never reads it (see
// MapAllRejectsUnknownKindByte), so a poke at a record-relative offset needs no
// CRC restamp.
struct ArchiveWithRecord {
  std::vector<std::byte> bytes;
  std::uint64_t record_offset{0};
  ArchiveV2SurfaceHeader header{};
};

[[nodiscard]] ArchiveWithRecord build_archive_with_record(const PricedSurface &ps,
                                                          std::string_view sym) {
  ArchiveWithRecord out;
  out.bytes = build_v2_single(ps, sym);
  auto arch = SurfaceArchiveV2::open(std::vector<std::byte>(out.bytes));
  EXPECT_TRUE(arch.has_value());
  const std::span<const ArchiveV2DirEntry> dir = arch->directory();
  EXPECT_EQ(dir.size(), 1u);
  out.record_offset = dir.empty() ? 0u : dir[0].surface_offset;
  std::memcpy(&out.header, out.bytes.data() + out.record_offset, sizeof out.header);
  return out;
}

} // namespace

// Non-vacuity for every LinearVariance case below: the untouched record parses on
// BOTH parsers, so each rejection is attributable to the single poked node.
TEST(SurfaceArchiveV2Adversarial, BaselineLinearVarianceRecordParses) {
  const ExtractedRecord r = extract_record(make_linear(1, 3, 9), "sym");
  EXPECT_TRUE(PricedSurfaceView::create_over_record(r.bytes).has_value());
  auto arch = SurfaceArchiveV2::open(build_v2_single(make_linear(1, 3, 9), "sym"));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  EXPECT_TRUE(arch->reconstruct_all().has_value());
}

TEST(SurfaceArchiveV2Adversarial, RejectsNonAscendingLinearVarianceNodes) {
  ExtractedRecord r = extract_record(make_linear(1, 3, 9), "sym");
  const std::uint64_t payload = peek_u64(r.bytes, r.header.col_payload_off_off);
  double k0 = 0.0;
  std::memcpy(&k0, r.bytes.data() + payload, sizeof k0);
  poke_f64(r.bytes, payload + 8u, k0 - 0.1); // k[1] < k[0]
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

// The concrete `lo = hi - 1` underflow: a NaN node compares false in every
// direction, so both wing guards fall through AND lower_bound returns index 0.
TEST(SurfaceArchiveV2Adversarial, RejectsNonFiniteLinearVarianceNode) {
  ExtractedRecord r = extract_record(make_linear(1, 3, 9), "sym");
  const std::uint64_t payload = peek_u64(r.bytes, r.header.col_payload_off_off);
  poke_f64(r.bytes, payload, kNaN); // k[0] = NaN
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

// The owned parser builds a `LinearVarianceCurve` over the same bytes and its
// `w()` has the identical `lo = hi - 1` shape, so it must reject the same record.
TEST(SurfaceArchiveV2Adversarial, ReconstructRejectsNonFiniteLinearVarianceNode) {
  ArchiveWithRecord a = build_archive_with_record(make_linear(1, 3, 9), "sym");
  const std::uint64_t payload = peek_u64(a.bytes, a.record_offset + a.header.col_payload_off_off);
  poke_f64(a.bytes, a.record_offset + payload, kNaN);
  auto arch = SurfaceArchiveV2::open(std::move(a.bytes));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string(); // lazy: body untouched at open
  const auto rec = arch->reconstruct_all();
  ASSERT_FALSE(rec.has_value());
  EXPECT_EQ(rec.error().code(), ErrorCode::ParseError);
}

// ── S2-2.2: per-slice payload extents must be monotone and disjoint ───────────
//
// Every slice's payload was bounds-checked in ISOLATION — `need <= record_size -
// payload_off` — and nothing tied the extents to each other. Alias all n slices
// onto ONE offset and every check still passes, against the SAME bytes, so the
// record's own size stops bounding what it can demand: each slice may then claim
// a node count worth the whole remaining record, and the parsers allocate that
// per slice. A ~1 MB record has room for ~19k slice columns (~53 B of columns
// per slice), each able to ask for ~1 MB of node vectors — ~19 GB out of 1 MB of
// input, before any CRC is checked.
//
// The writer lays payloads out in slice order, each `align_up`'d past the
// previous extent's end, so requiring `payload_off[i] >= end(payload[i-1])`
// re-imposes exactly the writer's own geometry and makes the record size the
// allocation bound again.

TEST(SurfaceArchiveV2Adversarial, RejectsOverlappingSlicePayloadExtents) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  const std::uint64_t p0 = peek_u64(r.bytes, r.header.col_payload_off_off);
  poke_u64(r.bytes, r.header.col_payload_off_off + 8u, p0); // slice 1 aliases slice 0
  const auto v = PricedSurfaceView::create_over_record(r.bytes);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), ErrorCode::ParseError);
}

TEST(SurfaceArchiveV2Adversarial, ReconstructRejectsOverlappingSlicePayloadExtents) {
  ArchiveWithRecord a = build_archive_with_record(make_essvi(1, 4), "sym");
  const std::uint64_t off = a.record_offset + a.header.col_payload_off_off;
  const std::uint64_t p0 = peek_u64(a.bytes, off);
  poke_u64(a.bytes, off + 8u, p0);
  auto arch = SurfaceArchiveV2::open(std::move(a.bytes));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string(); // lazy: body untouched at open
  const auto rec = arch->reconstruct_all();
  ASSERT_FALSE(rec.has_value());
  EXPECT_EQ(rec.error().code(), ErrorCode::ParseError);
}

// A DESCENDING pair is the same violation seen from the other side, and equally
// unreachable from any writer output.
TEST(SurfaceArchiveV2Adversarial, RejectsDescendingSlicePayloadOffsets) {
  ExtractedRecord r = extract_record(make_essvi(1, 4), "sym");
  const std::uint64_t p0 = peek_u64(r.bytes, r.header.col_payload_off_off);
  const std::uint64_t p1 = peek_u64(r.bytes, r.header.col_payload_off_off + 8u);
  poke_u64(r.bytes, r.header.col_payload_off_off, p1);
  poke_u64(r.bytes, r.header.col_payload_off_off + 8u, p0);
  EXPECT_FALSE(PricedSurfaceView::create_over_record(r.bytes).has_value());
}

// The variable-length kinds are where the amplification actually bites (a node
// count, not a fixed sizeof, sets the allocation), so pin one of those too.
TEST(SurfaceArchiveV2Adversarial, RejectsOverlappingLinearVariancePayloadExtents) {
  ExtractedRecord r = extract_record(make_linear(1, 3, 9), "sym");
  const std::uint64_t p0 = peek_u64(r.bytes, r.header.col_payload_off_off);
  poke_u64(r.bytes, r.header.col_payload_off_off + 8u, p0);
  EXPECT_FALSE(PricedSurfaceView::create_over_record(r.bytes).has_value());
}

// ── C4 (SE-P2-7): v1 "repaired blob" corruption corpus ported to v2 ────────────
// The metadata CRC covers the lookup ‖ directory span, so a metadata tamper that
// stays undetected (a buggy writer or a deliberate forge) must RECOMPUTE both
// CRCs — otherwise open fails at the checksum, not at the integrity cross-check we
// are exercising. restamp_v2_crcs mirrors the writer's CRC algorithm exactly.

namespace {

[[nodiscard]] ArchiveV2Header read_header(const std::vector<std::byte> &b) {
  ArchiveV2Header h;
  std::memcpy(&h, b.data(), sizeof h);
  return h;
}

// Recompute metadata_crc32c (over lookup ‖ directory) then header_crc32c (over the
// header with its own crc field zeroed) — the two fields a metadata edit invalidates.
void restamp_v2_crcs(std::vector<std::byte> &b) {
  ArchiveV2Header h = read_header(b);
  const std::size_t lookup_bytes =
      static_cast<std::size_t>(h.lookup_slot_count) * h.lookup_slot_size;
  const std::size_t dir_bytes = static_cast<std::size_t>(h.surface_count) * h.dir_entry_size;
  std::uint32_t meta = atx::vol::detail::crc32c_update(0xFFFFFFFFu, b.data() + h.lookup_offset,
                                                       lookup_bytes);
  meta = atx::vol::detail::crc32c_update(meta, b.data() + h.directory_offset, dir_bytes) ^
         0xFFFFFFFFu;
  h.metadata_crc32c = meta;
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveV2Header)> hb{};
  std::memcpy(hb.data(), &h, sizeof h);
  h.header_crc32c = atx::vol::detail::crc32c(hb.data(), hb.size());
  std::memcpy(b.data(), &h, sizeof h);
}

[[nodiscard]] ArchiveV2LookupSlot read_slot(const std::vector<std::byte> &b, const ArchiveV2Header &h,
                                            std::size_t i) {
  ArchiveV2LookupSlot s;
  std::memcpy(&s, b.data() + h.lookup_offset + i * sizeof(s), sizeof s);
  return s;
}
void write_slot(std::vector<std::byte> &b, const ArchiveV2Header &h, std::size_t i,
                const ArchiveV2LookupSlot &s) {
  std::memcpy(b.data() + h.lookup_offset + i * sizeof(s), &s, sizeof s);
}
[[nodiscard]] ArchiveV2DirEntry read_dir(const std::vector<std::byte> &b, const ArchiveV2Header &h,
                                         std::size_t i) {
  ArchiveV2DirEntry d;
  std::memcpy(&d, b.data() + h.directory_offset + i * sizeof(d), sizeof d);
  return d;
}
void write_dir(std::vector<std::byte> &b, const ArchiveV2Header &h, std::size_t i,
               const ArchiveV2DirEntry &d) {
  std::memcpy(b.data() + h.directory_offset + i * sizeof(d), &d, sizeof d);
}

// Index of the OCCUPIED lookup slot whose stored (already-canonical) symbol equals
// `canon`. -1 if none.
[[nodiscard]] std::ptrdiff_t slot_index_for(const std::vector<std::byte> &b,
                                            const ArchiveV2Header &h, std::string_view canon) {
  for (std::size_t i = 0; i < h.lookup_slot_count; ++i) {
    const ArchiveV2LookupSlot s = read_slot(b, h, i);
    if (s.flags == atx::vol::kArchiveV2SlotOccupied && s.symbol_len == canon.size() &&
        std::memcmp(s.symbol, canon.data(), canon.size()) == 0) {
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

// A 2-surface archive (AAA: 4 slices uid 10, BBB: 5 slices uid 20).
[[nodiscard]] std::vector<std::byte> build_two() {
  const PricedSurface a = make_essvi(10, 4);
  const PricedSurface b = make_essvi(20, 5);
  const std::array<SurfaceArchiveItem, 2> items{SurfaceArchiveItem{"AAA", &a, std::nullopt},
                                                SurfaceArchiveItem{"BBB", &b, std::nullopt}};
  auto built = write_surface_archive_v2(items);
  EXPECT_TRUE(built.has_value()) << (built.has_value() ? "" : built.error().to_string());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}

} // namespace

// A lookup slot cross-linked to ANOTHER symbol's (valid) record: pre-C4 the reader
// never checked slot↔directory agreement, so map_symbol("AAA") served BBB's surface
// with no error. The open-time cross-check rejects it.
TEST(SurfaceArchiveV2Adversarial, RejectsCrossLinkedLookupAndDirectory) {
  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  // Directory is sorted by symbol: [0]=AAA, [1]=BBB.
  const ArchiveV2DirEntry dirB = read_dir(bytes, h, 1);
  const std::ptrdiff_t ia = slot_index_for(bytes, h, "AAA");
  ASSERT_GE(ia, 0);
  ArchiveV2LookupSlot sa = read_slot(bytes, h, static_cast<std::size_t>(ia));
  sa.surface_offset = dirB.surface_offset; // point AAA's slot at BBB's record
  sa.surface_size = dirB.surface_size;
  write_slot(bytes, h, static_cast<std::size_t>(ia), sa);
  restamp_v2_crcs(bytes);

  const auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_FALSE(arch.has_value());
  EXPECT_EQ(arch.error().code(), ErrorCode::ParseError);
}

// FIX-1 / F4 (rev-ws-c I-1): C4's cross-check claimed to "force a bijection" between
// occupied slots and directory entries. It did not. It resolves each DIRECTORY entry to
// a slot, but never checks that every OCCUPIED slot is referenced, and never checks that
// directory symbols are unique — and `occupied == surface_count` only yields a bijection
// if they are.
//
// The forgery below duplicates a directory symbol so both entries resolve to the SAME
// slot. Every pre-F4 check still passes: 2 occupied slots == surface_count 2, and both
// directory entries resolve to a slot with identical (offset, size, uid, symbol_hash).
// Yet BBB's occupied slot is now referenced by nothing and goes entirely unvalidated —
// so it can be cross-linked to an arbitrary record and map_symbol("BBB") serves the
// wrong surface. That is exactly the failure SE-P2-7 describes.
//
// Not reachable from any writer output (the writer rejects duplicate canonical symbols),
// so this closes an overstated guarantee rather than a live defect. The directory is
// already written sorted by canonical symbol, so requiring STRICTLY ascending symbols
// gives uniqueness, and uniqueness + the existing count check + the existing per-entry
// resolution is the real bijection.
TEST(SurfaceArchiveV2Adversarial, RejectsDuplicateDirectorySymbols) {
  // Non-vacuity: the untampered archive opens, so the rejection below is attributable
  // to the forgery alone and not to a broken restamp.
  ASSERT_TRUE(SurfaceArchiveV2::open(build_two()).has_value());

  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  // Directory is sorted by symbol: [0]=AAA, [1]=BBB. Duplicate AAA over BBB's entry so
  // BOTH entries resolve to AAA's slot and BBB's slot loses its only referent.
  const ArchiveV2DirEntry dirA = read_dir(bytes, h, 0);
  write_dir(bytes, h, 1, dirA);
  // Cross-link the now-unreferenced BBB slot to AAA's record: the harm the missing
  // uniqueness check lets through.
  const std::ptrdiff_t ib = slot_index_for(bytes, h, "BBB");
  ASSERT_GE(ib, 0);
  ArchiveV2LookupSlot sb = read_slot(bytes, h, static_cast<std::size_t>(ib));
  sb.surface_offset = dirA.surface_offset;
  sb.surface_size = dirA.surface_size;
  write_slot(bytes, h, static_cast<std::size_t>(ib), sb);
  restamp_v2_crcs(bytes);

  const auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_FALSE(arch.has_value());
  EXPECT_EQ(arch.error().code(), ErrorCode::ParseError);
  // Attribute the rejection to the new ordering check, not to the CRC gate (which also
  // returns ParseError) — otherwise a future restamp regression would keep this green
  // for the wrong reason.
  EXPECT_NE(arch.error().to_string().find("directory symbols not strictly ascending"),
            std::string::npos)
      << arch.error().to_string();
}

// An occupied-slot count that disagrees with surface_count (here: an occupied slot
// flipped to empty). Pre-C4 open ignored slot flags entirely.
TEST(SurfaceArchiveV2Adversarial, RejectsOccupiedSlotCountMismatch) {
  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  const std::ptrdiff_t ia = slot_index_for(bytes, h, "AAA");
  ASSERT_GE(ia, 0);
  ArchiveV2LookupSlot sa = read_slot(bytes, h, static_cast<std::size_t>(ia));
  sa.flags = atx::vol::kArchiveV2SlotEmpty; // now 1 occupied slot vs surface_count 2
  write_slot(bytes, h, static_cast<std::size_t>(ia), sa);
  restamp_v2_crcs(bytes);

  EXPECT_FALSE(SurfaceArchiveV2::open(std::move(bytes)).has_value());
}

// A lookup slot whose surface_offset was tampered out of bounds. Pre-C4 the slot
// offset was unvalidated at open (only caught later at map_symbol); the cross-check
// rejects it at open (slot.offset != directory.offset).
TEST(SurfaceArchiveV2Adversarial, RejectsTamperedLookupOffsetOutOfBounds) {
  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  const std::ptrdiff_t ia = slot_index_for(bytes, h, "AAA");
  ASSERT_GE(ia, 0);
  ArchiveV2LookupSlot sa = read_slot(bytes, h, static_cast<std::size_t>(ia));
  sa.surface_offset = h.file_size + 4096ull; // far past EOF
  write_slot(bytes, h, static_cast<std::size_t>(ia), sa);
  restamp_v2_crcs(bytes);

  EXPECT_FALSE(SurfaceArchiveV2::open(std::move(bytes)).has_value());
}

// A directory entry's n_slices (u16) tampered to disagree with the record header's
// n_slices (u32). open stays lazy (does not read record bodies), so it succeeds;
// map_all cross-checks the pair and rejects it (v1 checks the analogous pair).
TEST(SurfaceArchiveV2Adversarial, MapAllRejectsDirectoryRecordNSlicesMismatch) {
  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  ArchiveV2DirEntry dirA = read_dir(bytes, h, 0);
  dirA.n_slices = static_cast<std::uint16_t>(dirA.n_slices + 3u); // record header still says 4
  write_dir(bytes, h, 0, dirA);
  restamp_v2_crcs(bytes);

  auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string(); // lazy: bodies untouched
  EXPECT_FALSE(arch->map_all().has_value());                 // whole-board scan catches it
}

// Coverage (already handled by create_over_record's kind dispatch): an unknown kind
// byte in a record body. The body is NOT covered by the metadata CRC, so open stays
// lazy and succeeds; map_all rejects the unknown kind.
TEST(SurfaceArchiveV2Adversarial, MapAllRejectsUnknownKindByte) {
  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  const ArchiveV2DirEntry dirA = read_dir(bytes, h, 0);
  ArchiveV2SurfaceHeader rh;
  std::memcpy(&rh, bytes.data() + dirA.surface_offset, sizeof rh);
  // col_kind[0] lives at record_offset + col_kind_off; set it to an invalid kind.
  bytes[static_cast<std::size_t>(dirA.surface_offset + rh.col_kind_off)] = std::byte{200};

  auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  EXPECT_FALSE(arch->map_all().has_value());
}

// Coverage: open_mapped (previously untested by any TU) must open a real file and
// serve a subset map identically to the owned open path.
TEST(SurfaceArchiveV2Adversarial, OpenMappedServesSubsetMap) {
  const auto dir =
      std::filesystem::temp_directory_path() / "atx_ws_c_c4_openmapped";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "part.atxvsa2";
  const PricedSurface a = make_essvi(10, 4);
  const PricedSurface b = make_essvi(20, 5);
  const std::array<SurfaceArchiveItem, 2> items{SurfaceArchiveItem{"AAA", &a, std::nullopt},
                                                SurfaceArchiveItem{"BBB", &b, std::nullopt}};
  ASSERT_TRUE(write_surface_archive_v2_file(path.string(), items).has_value());

  {
    auto mapped = SurfaceArchiveV2::open_mapped(path.string());
    ASSERT_TRUE(mapped.has_value()) << mapped.error().to_string();
    EXPECT_EQ(mapped->count(), 2u);
    auto vb = mapped->map_symbol("BBB");
    ASSERT_TRUE(vb.has_value()) << vb.error().to_string();
    EXPECT_EQ(vb->uid(), 20u);
    EXPECT_EQ(vb->n_slices(), 5u);
  } // view + mapped archive destroyed here -> file mapping released before cleanup

  std::error_code ec;
  std::filesystem::remove_all(dir, ec); // best-effort
}

// ── S3-T15 F-1: `open`'s inline-symbol length bounds ──────────────────────────
//
// `symbol_len` bounds the fixed 32-byte inline `symbol[]` carried by BOTH the
// lookup slot and the directory entry, and every consumer that materializes the
// name does `std::string(e.symbol, e.symbol_len)`. A length past
// `kArchiveSymbolMax` is therefore an out-of-bounds read straight off the end of
// the record. The writer can never emit one (`canonicalize_symbol` truncates at
// `kArchiveSymbolMax`), so these two guards fire only on a forged or corrupt
// file — which is exactly why they need a pin rather than none.
//
// Both edits land inside the metadata CRC span, so each restamps; without that
// `open` would fail at the checksum and the test would pass for the wrong
// reason. The message assertions attribute the rejection to the intended guard
// (every framing rejection in `open` returns ParseError).
//
// Ported from the deleted v1 suite: SurfaceArchive.Open_RejectsOversized-
// LookupSymbolLength / Open_RejectsOversizedDirectorySymbolLength.

TEST(SurfaceArchiveV2Adversarial, OpenRejectsOversizedLookupSymbolLength) {
  // Non-vacuity: the untampered archive opens, so the rejection is attributable
  // to the poked field and not to a broken restamp.
  ASSERT_TRUE(SurfaceArchiveV2::open(build_two()).has_value());

  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  const std::ptrdiff_t ia = slot_index_for(bytes, h, "AAA");
  ASSERT_GE(ia, 0);
  ArchiveV2LookupSlot sa = read_slot(bytes, h, static_cast<std::size_t>(ia));
  sa.symbol_len = static_cast<std::uint16_t>(atx::vol::kArchiveSymbolMax + 1u);
  write_slot(bytes, h, static_cast<std::size_t>(ia), sa);
  restamp_v2_crcs(bytes);

  const auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_FALSE(arch.has_value());
  EXPECT_EQ(arch.error().code(), ErrorCode::ParseError);
  EXPECT_NE(arch.error().to_string().find("lookup symbol length out of bounds"), std::string::npos)
      << arch.error().to_string();
}

TEST(SurfaceArchiveV2Adversarial, OpenRejectsOversizedDirectorySymbolLength) {
  ASSERT_TRUE(SurfaceArchiveV2::open(build_two()).has_value());

  std::vector<std::byte> bytes = build_two();
  const ArchiveV2Header h = read_header(bytes);
  // Only the directory entry is poked; every lookup slot keeps a legal length,
  // so the earlier lookup-side loop cannot be what rejects this file.
  ArchiveV2DirEntry dirA = read_dir(bytes, h, 0);
  dirA.symbol_len = static_cast<std::uint16_t>(atx::vol::kArchiveSymbolMax + 1u);
  write_dir(bytes, h, 0, dirA);
  restamp_v2_crcs(bytes);

  const auto arch = SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_FALSE(arch.has_value());
  EXPECT_EQ(arch.error().code(), ErrorCode::ParseError);
  EXPECT_NE(arch.error().to_string().find("directory symbol length out of bounds"),
            std::string::npos)
      << arch.error().to_string();
}

// ── S3-T15 F-2: `write_surface_archive_v2`'s input validation ─────────────────
//
// The writer is the format's only trusted producer: its rejection set is what
// stops an unrepresentable item from becoming a file every later reader has to
// defend against. All but one of these guards return the SAME code
// (InvalidArgument), so each case also pins the message — otherwise any single
// guard firing would keep the whole group green and the group would pin nothing.
//
// Ported from the deleted v1 suite: SurfaceArchive.Write_RejectsEmptyAndNull,
// Write_RejectsDuplicateCanonicalSymbol, Write_RejectsExplicitLegacyProvenance-
// Record, WriteRejectsHealthyProvenanceWithValidationFailures.
//
// NOT pinned, deliberately: the "empty canonical symbol" guard. `it.symbol` is
// already rejected when empty, and `canonicalize_symbol(s, kArchiveSymbolMax)`
// returns `min(s.size(), 32)` bytes — never zero for a non-empty input. The
// guard is unreachable defence-in-depth against a future canonicalizer that can
// erase characters, and no input through the public API can construct it.

TEST(SurfaceArchiveV2Adversarial, WriteRejectsNoItems) {
  const auto built = write_surface_archive_v2(std::span<const SurfaceArchiveItem>{});
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(built.error().to_string().find("no items"), std::string::npos)
      << built.error().to_string();
}

TEST(SurfaceArchiveV2Adversarial, WriteRejectsNullSurface) {
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", nullptr, std::nullopt}};
  const auto built = write_surface_archive_v2(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(built.error().to_string().find("null surface"), std::string::npos)
      << built.error().to_string();
}

TEST(SurfaceArchiveV2Adversarial, WriteRejectsEmptySymbol) {
  const PricedSurface ps = make_essvi(1, 3);
  // Non-vacuity: the same surface under a real symbol writes cleanly, so the
  // rejection is attributable to the empty name alone.
  const std::array<SurfaceArchiveItem, 1> good{SurfaceArchiveItem{"SPY", &ps, std::nullopt}};
  ASSERT_TRUE(write_surface_archive_v2(good).has_value());

  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"", &ps, std::nullopt}};
  const auto built = write_surface_archive_v2(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(built.error().to_string().find("empty symbol"), std::string::npos)
      << built.error().to_string();
}

// Two items whose CANONICAL symbols collide. `canonicalize_symbol` upper-cases,
// so "AAA"/"aaa" are byte-equal after canonicalization: the second insert meets
// an occupied slot with an EQUAL key (not merely an equal hash), which is the
// precise condition the AlreadyExists guard names. Left unguarded, the archive
// would carry two directory entries for one symbol and `map_symbol` would serve
// whichever landed first — silently the wrong surface.
TEST(SurfaceArchiveV2Adversarial, WriteRejectsDuplicateCanonicalSymbol) {
  const PricedSurface a = make_essvi(10, 4);
  const PricedSurface b = make_essvi(20, 5);
  // Non-vacuity: the same two surfaces under DISTINCT symbols write cleanly.
  const std::array<SurfaceArchiveItem, 2> distinct{SurfaceArchiveItem{"AAA", &a, std::nullopt},
                                                   SurfaceArchiveItem{"BBB", &b, std::nullopt}};
  ASSERT_TRUE(write_surface_archive_v2(distinct).has_value());

  const std::array<SurfaceArchiveItem, 2> items{SurfaceArchiveItem{"AAA", &a, std::nullopt},
                                                SurfaceArchiveItem{"aaa", &b, std::nullopt}};
  const auto built = write_surface_archive_v2(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::AlreadyExists);
  EXPECT_NE(built.error().to_string().find("duplicate canonical symbol"), std::string::npos)
      << built.error().to_string();
}

// v2 has no legacy representation to write into: `legacy_format` provenance only
// ever comes OUT of a v1-era zero-provenance record. Accepting it would mint a
// v2 record claiming a lineage the format cannot express.
TEST(SurfaceArchiveV2Adversarial, WriteRejectsExplicitLegacyProvenance) {
  const PricedSurface ps = make_essvi(1, 3);
  const SurfaceProvenance legacy = atx::vol::legacy_surface_provenance();
  ASSERT_TRUE(legacy.legacy_format);

  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &ps, legacy}};
  const auto built = write_surface_archive_v2(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(built.error().to_string().find("explicit legacy provenance is unsupported"),
            std::string::npos)
      << built.error().to_string();
}

// Healthy-with-failures is the self-contradictory provenance the record-validity
// predicate exists to refuse: a surface that failed a validation gate must not
// be published claiming a clean bill of health.
TEST(SurfaceArchiveV2Adversarial, WriteRejectsHealthyProvenanceWithValidationFailures) {
  const PricedSurface ps = make_essvi(1, 3);
  SurfaceProvenance ok;
  ASSERT_EQ(ok.state, SurfaceState::Healthy);
  ASSERT_FALSE(ok.legacy_format); // isolates this from the legacy-provenance guard
  // Non-vacuity: identical provenance minus the failure bit is accepted, so the
  // rejection is attributable to the inconsistency and not to provenance per se.
  const std::array<SurfaceArchiveItem, 1> good{SurfaceArchiveItem{"SPY", &ps, ok}};
  ASSERT_TRUE(write_surface_archive_v2(good).has_value());

  SurfaceProvenance inconsistent = ok;
  inconsistent.validation.failures = ValidationFailure::Butterfly;
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &ps, inconsistent}};
  const auto built = write_surface_archive_v2(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(built.error().to_string().find("invalid surface provenance"), std::string::npos)
      << built.error().to_string();
}
