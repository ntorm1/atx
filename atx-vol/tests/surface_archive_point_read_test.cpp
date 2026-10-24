#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/backtest/priced_surface_view.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "surface_archive_v2_patch.hpp" // slot / record / header CRC restamps

// ATXVSA2 revision 1 point read (docs/atxvsa2-format.md §4): one surface from a
// partition FILE through positional reads of the header, the probed slot run and the
// record extent; every touched byte verified; never a wrong surface.

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
using atx::vol::point_read_archive_reconstruct;
using atx::vol::point_read_archive_surface;
using atx::vol::PricedSurface;
using atx::vol::PricedSurfaceView;
using atx::vol::PricingContext;
using atx::vol::Side;
using atx::vol::SliceContext;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveV2;
using atx::vol::write_surface_archive_v2;
using atx::vol::write_surface_archive_v2_file;

constexpr double kS = 100.0;
constexpr double kR = 0.043;

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kS;
  pc.r = kR;
  pc.now_ts_ns = 1700000000000000000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = AlOpts{};
  pc.uid = uid;
  return pc;
}

[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, double theta0 = 0.04) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    EssviParams e{};
    e.theta = theta0 + 0.005 * static_cast<double>(i);
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
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

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
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// iv + fair value over a (K, T, side) grid, bit for bit.
template <class A, class B> void expect_theo_bit_identical(const A &a, const B &b) {
  ASSERT_EQ(a.n_slices(), b.n_slices());
  ASSERT_EQ(a.uid(), b.uid());
  for (const double T : {0.03, 0.06, 0.18, 0.40}) {
    for (const double K : {82.0, 100.0, 113.0, 128.0}) {
      EXPECT_TRUE(bits_equal(a.iv(K, T), b.iv(K, T))) << "iv K=" << K << " T=" << T;
      for (const Side side : {Side::Call, Side::Put}) {
        const auto fa = a.fair_value(K, T, side);
        const auto fb = b.fair_value(K, T, side);
        ASSERT_EQ(fa.has_value(), fb.has_value());
        if (fa.has_value()) {
          EXPECT_TRUE(bits_equal(*fa, *fb)) << "fv K=" << K << " T=" << T;
        }
      }
    }
  }
}

// A per-test scratch directory under the system temp dir, removed on destruction.
struct ScratchDir {
  std::filesystem::path root;
  explicit ScratchDir(std::string_view name)
      : root(std::filesystem::temp_directory_path() / ("atx_ser_point_read_" + std::string(name))) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
  }
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  ScratchDir(const ScratchDir &) = delete;
  ScratchDir &operator=(const ScratchDir &) = delete;
  [[nodiscard]] std::string file(std::string_view leaf) const { return (root / leaf).string(); }
};

[[nodiscard]] std::vector<std::byte> read_all(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

void write_all(const std::string &path, std::span<const std::byte> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good());
}

// Three surfaces of two kinds: AAPL (Essvi), MSFT (LinearVariance), SPY (Essvi).
struct Board {
  PricedSurface aapl = make_essvi(1, 3);
  PricedSurface msft = make_linear(2, 2, 12);
  PricedSurface spy = make_essvi(3, 5, 0.03);
  [[nodiscard]] std::vector<SurfaceArchiveItem> items() const {
    return {{"AAPL", &aapl, std::nullopt}, {"MSFT", &msft, std::nullopt}, {"SPY", &spy, std::nullopt}};
  }
};

void write_board(const Board &b, const std::string &path) {
  const auto items = b.items();
  ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
}

} // namespace

// Hit: the point-read view and the point-read reconstruct are bit-identical to the
// source surfaces, the symbol is case-insensitive, and the I/O is exactly the header,
// one slot run and the record (three positional reads).
TEST(SurfaceArchivePointRead, HitServesBitIdenticalViewAndReconstruct) {
  const ScratchDir dir("hit");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  const auto file_size = std::filesystem::file_size(path);

  auto a = point_read_archive_surface(path, "aapl");
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  expect_theo_bit_identical(b.aapl, a->view);
  EXPECT_TRUE(a->provenance.legacy_format) << "written without provenance -> Unattested";
  EXPECT_EQ(a->stats.read_calls, 3u);
  EXPECT_GE(a->stats.slots_probed, 1u);
  EXPECT_LT(a->stats.bytes_read, file_size);

  auto m = point_read_archive_reconstruct(path, "MSFT");
  ASSERT_TRUE(m.has_value()) << m.error().to_string();
  expect_theo_bit_identical(b.msft, m->archived.surface);

  // The served view equals the whole-board view of the same record.
  auto arch = SurfaceArchiveV2::open_copied(path);
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto whole = arch->map_symbol("SPY");
  ASSERT_TRUE(whole.has_value());
  auto s = point_read_archive_surface(path, "SpY");
  ASSERT_TRUE(s.has_value()) << s.error().to_string();
  expect_theo_bit_identical(*whole, s->view);
  const auto e = arch->find("SPY");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(s->stats.bytes_read,
            sizeof(ArchiveV2Header) + e->surface_size +
                std::min<std::uint64_t>(atx::vol::kArchiveV2SlotReadRun,
                                        arch->header().lookup_slot_count -
                                            (atx::vol::archive_v2_symbol_hash("SPY") &
                                             (arch->header().lookup_slot_count - 1u))) *
                    sizeof(ArchiveV2LookupSlot));
}

// O(1): on a 2,000-surface partition every point read issues at most four positional
// reads and fetches < 4 KiB, while the file is ~1 MB.
TEST(SurfaceArchivePointRead, IoIsIndependentOfPartitionSize) {
  const ScratchDir dir("o1");
  constexpr int kN = 2000;
  std::vector<PricedSurface> surfaces;
  surfaces.reserve(kN);
  std::vector<std::string> names;
  names.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    surfaces.push_back(make_essvi(static_cast<std::uint32_t>(i + 1), 4));
    names.push_back("SYM" + std::to_string(i));
  }
  std::vector<SurfaceArchiveItem> items;
  for (int i = 0; i < kN; ++i) {
    items.push_back({names[static_cast<std::size_t>(i)], &surfaces[static_cast<std::size_t>(i)],
                     std::nullopt});
  }
  const std::string path = dir.file("big.atxvsa");
  ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
  const auto file_size = std::filesystem::file_size(path);
  ASSERT_GT(file_size, 1'000'000u);
  for (int i = 0; i < kN; i += 97) {
    auto got = point_read_archive_surface(path, names[static_cast<std::size_t>(i)]);
    ASSERT_TRUE(got.has_value()) << got.error().to_string();
    EXPECT_EQ(got->view.uid(), static_cast<std::uint32_t>(i + 1));
    EXPECT_LE(got->stats.read_calls, 4u);
    EXPECT_LT(got->stats.bytes_read, 4096u);
  }
}

TEST(SurfaceArchivePointRead, AbsentSymbolIsNotFound) {
  const ScratchDir dir("absent_symbol");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  auto got = point_read_archive_surface(path, "QQQ");
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::NotFound);
  EXPECT_EQ(point_read_archive_reconstruct(path, "QQQ").error().code(), ErrorCode::NotFound);
}

TEST(SurfaceArchivePointRead, AbsentFileIsNotFound) {
  const ScratchDir dir("absent_file");
  auto got = point_read_archive_surface(dir.file("missing.atxvsa"), "AAPL");
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::NotFound);
}

TEST(SurfaceArchivePointRead, MalformedSymbolIsInvalidArgument) {
  const ScratchDir dir("bad_symbol");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  EXPECT_EQ(point_read_archive_surface(path, "").error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(point_read_archive_surface(path, std::string(33, 'A')).error().code(),
            ErrorCode::InvalidArgument);
}

// A bit flip inside the probed slot fails that slot's own CRC.
TEST(SurfaceArchivePointRead, CorruptSlotIsParseError) {
  const ScratchDir dir("corrupt_slot");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  std::vector<std::byte> bytes = read_all(path);
  const auto idx = atxvsa_patch::slot_index_for(bytes, "AAPL");
  ASSERT_GE(idx, 0);
  const ArchiveV2Header h = atxvsa_patch::read_header(bytes);
  const auto off = atxvsa_patch::slot_offset(h, static_cast<std::size_t>(idx)) +
                   offsetof(ArchiveV2LookupSlot, surface_offset);
  bytes[off] ^= std::byte{0x40};
  write_all(path, bytes);
  auto got = point_read_archive_surface(path, "AAPL");
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::ParseError);
  EXPECT_NE(got.error().to_string().find("slot checksum"), std::string::npos)
      << got.error().to_string();
}

// A SELF-CONSISTENT forged slot (CRCs restamped) that points AAPL at MSFT's intact,
// CRC-valid record must be refused by the record's symbol binding — never served as
// AAPL's surface.
TEST(SurfaceArchivePointRead, ForgedSlotPointingAtASiblingIsRefusedByTheBinding) {
  const ScratchDir dir("forged_slot");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  std::vector<std::byte> bytes = read_all(path);
  const ArchiveV2Header h = atxvsa_patch::read_header(bytes);
  const auto si = atxvsa_patch::slot_index_for(bytes, "AAPL");
  const auto di = atxvsa_patch::dir_index_for(bytes, "MSFT");
  ASSERT_GE(si, 0);
  ASSERT_GE(di, 0);
  const ArchiveV2DirEntry msft = atxvsa_patch::read_dir(bytes, h, static_cast<std::size_t>(di));
  ArchiveV2LookupSlot s = atxvsa_patch::read_slot(bytes, h, static_cast<std::size_t>(si));
  s.surface_offset = msft.surface_offset;
  s.surface_size = static_cast<std::uint32_t>(msft.surface_size);
  s.payload_crc32c = msft.payload_crc32c;
  atxvsa_patch::write_slot(bytes, h, static_cast<std::size_t>(si), s);
  atxvsa_patch::restamp_header(bytes);
  write_all(path, bytes);

  auto got = point_read_archive_surface(path, "AAPL");
  ASSERT_FALSE(got.has_value()) << "served uid " << got->view.uid() << " for AAPL";
  EXPECT_EQ(got.error().code(), ErrorCode::ParseError);
  EXPECT_NE(got.error().to_string().find("does not belong"), std::string::npos)
      << got.error().to_string();
  // The sibling itself is still served.
  auto sib = point_read_archive_surface(path, "MSFT");
  ASSERT_TRUE(sib.has_value()) << sib.error().to_string();
  expect_theo_bit_identical(b.msft, sib->view);
}

// A bit flip anywhere in the record extent fails the record CRC the point read
// recomputes; the sibling records are untouched and still served.
TEST(SurfaceArchivePointRead, CorruptRecordIsParseError) {
  const ScratchDir dir("corrupt_record");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  std::vector<std::byte> bytes = read_all(path);
  const ArchiveV2Header h = atxvsa_patch::read_header(bytes);
  const auto di = atxvsa_patch::dir_index_for(bytes, "SPY");
  ASSERT_GE(di, 0);
  const ArchiveV2DirEntry e = atxvsa_patch::read_dir(bytes, h, static_cast<std::size_t>(di));
  for (const std::uint64_t rel :
       {std::uint64_t{17}, std::uint64_t{sizeof(ArchiveV2SurfaceHeader) + 3}, e.surface_size - 1}) {
    std::vector<std::byte> bad = bytes;
    bad[e.surface_offset + rel] ^= std::byte{0x01};
    write_all(path, bad);
    auto got = point_read_archive_surface(path, "SPY");
    ASSERT_FALSE(got.has_value()) << "record byte " << rel;
    EXPECT_EQ(got.error().code(), ErrorCode::ParseError);
    EXPECT_EQ(point_read_archive_reconstruct(path, "SPY").error().code(), ErrorCode::ParseError);
    auto sib = point_read_archive_surface(path, "AAPL");
    ASSERT_TRUE(sib.has_value()) << sib.error().to_string();
  }
}

// Truncation (and an appended byte) is refused at the header: file_size must equal the
// real size. A file shorter than the header is refused too.
TEST(SurfaceArchivePointRead, TruncatedFileIsParseError) {
  const ScratchDir dir("truncated");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  const std::vector<std::byte> bytes = read_all(path);
  for (const std::size_t keep : {bytes.size() - 64u, bytes.size() / 2u, std::size_t{100},
                                 std::size_t{8}}) {
    write_all(path, std::span<const std::byte>(bytes.data(), keep));
    auto got = point_read_archive_surface(path, "AAPL");
    ASSERT_FALSE(got.has_value()) << "keep=" << keep;
    EXPECT_EQ(got.error().code(), ErrorCode::ParseError) << "keep=" << keep;
  }
  std::vector<std::byte> appended = bytes;
  appended.push_back(std::byte{0});
  write_all(path, appended);
  EXPECT_EQ(point_read_archive_surface(path, "AAPL").error().code(), ErrorCode::ParseError);
}

// A revision-0 file is refused by name, on the point read as on the whole-board open.
TEST(SurfaceArchivePointRead, RefusesRevisionZeroMagicByName) {
  const ScratchDir dir("rev0");
  const Board b;
  const std::string path = dir.file("p.atxvsa");
  write_board(b, path);
  std::vector<std::byte> bytes = read_all(path);
  std::memcpy(bytes.data(), "ATXVSA20", 8);
  write_all(path, bytes);
  auto got = point_read_archive_surface(path, "AAPL");
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), ErrorCode::ParseError);
  EXPECT_NE(got.error().to_string().find("ATXVSA20"), std::string::npos)
      << got.error().to_string();
}

// The densest legal table (load 100 %: S = next_pow2(N + 1)) forms long probe runs that
// wrap past the table end; the slot reads never wrap inside one read, every present
// symbol is found and every absent one terminates at an empty slot as NotFound.
TEST(SurfaceArchivePointRead, DenseTableProbeRunsWrapAndTerminate) {
  const ScratchDir dir("wrap");
  std::vector<PricedSurface> surfaces;
  std::vector<std::string> names;
  constexpr int kN = 40;
  for (int i = 0; i < kN; ++i) {
    surfaces.push_back(make_essvi(static_cast<std::uint32_t>(i + 1), 2));
    names.push_back("W" + std::to_string(i));
  }
  std::vector<SurfaceArchiveItem> items;
  for (int i = 0; i < kN; ++i) {
    items.push_back({names[static_cast<std::size_t>(i)], &surfaces[static_cast<std::size_t>(i)],
                     std::nullopt});
  }
  atx::vol::ArchiveV2WriteOpts opts;
  opts.lookup_load_pct = 100; // densest legal table: S = next_pow2(N + 1)
  const std::string path = dir.file("dense.atxvsa");
  ASSERT_TRUE(write_surface_archive_v2_file(path, items, opts).has_value());
  for (int i = 0; i < kN; ++i) {
    auto got = point_read_archive_surface(path, names[static_cast<std::size_t>(i)]);
    ASSERT_TRUE(got.has_value()) << names[static_cast<std::size_t>(i)] << ": "
                                 << got.error().to_string();
    EXPECT_EQ(got->view.uid(), static_cast<std::uint32_t>(i + 1));
  }
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(point_read_archive_surface(path, "ABSENT" + std::to_string(i)).error().code(),
              ErrorCode::NotFound);
  }
}

// Concurrent republish: readers point-read while a writer atomically republishes the
// partition, alternating two versions. Every read is served whole from exactly one
// version (uid and prices agree), and no publish is refused by a reader's handle.
TEST(SurfaceArchivePointRead, ConcurrentRepublishServesOneWholeVersion) {
  const ScratchDir dir("republish");
  const std::string path = dir.file("p.atxvsa");
  const PricedSurface v1 = make_essvi(101, 3, 0.04);
  const PricedSurface v2 = make_essvi(202, 3, 0.09);
  const std::array<SurfaceArchiveItem, 1> items1{SurfaceArchiveItem{"AAPL", &v1, std::nullopt}};
  const std::array<SurfaceArchiveItem, 1> items2{SurfaceArchiveItem{"AAPL", &v2, std::nullopt}};
  ASSERT_TRUE(write_surface_archive_v2_file(path, items1).has_value());

  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  std::atomic<int> reads{0};
  const auto reader = [&]() {
    while (!done.load()) {
      auto got = point_read_archive_surface(path, "AAPL");
      if (!got.has_value()) {
        failures.fetch_add(1);
        continue;
      }
      const PricedSurface &want = got->view.uid() == 101u ? v1 : v2;
      if ((got->view.uid() != 101u && got->view.uid() != 202u) ||
          !bits_equal(want.iv(100.0, 0.2), got->view.iv(100.0, 0.2))) {
        failures.fetch_add(1);
      }
      reads.fetch_add(1);
    }
  };
  std::thread r1(reader);
  std::thread r2(reader);
  int publish_failures = 0;
  for (int k = 0; k < 40; ++k) {
    const auto &items = (k % 2 == 0) ? items2 : items1;
    if (!write_surface_archive_v2_file(path, items).has_value()) {
      ++publish_failures;
    }
  }
  done.store(true);
  r1.join();
  r2.join();
  EXPECT_EQ(publish_failures, 0);
  EXPECT_EQ(failures.load(), 0);
  EXPECT_GT(reads.load(), 0);
}
