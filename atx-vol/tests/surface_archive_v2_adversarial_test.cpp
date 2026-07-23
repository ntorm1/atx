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
using atx::vol::PricedSurface;
using atx::vol::PricedSurfaceView;
using atx::vol::PricingContext;
using atx::vol::SliceContext;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveV2;
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
