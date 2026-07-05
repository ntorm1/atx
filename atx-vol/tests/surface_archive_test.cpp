#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_surface.hpp"

// Mirrors the C ats-vol archive suites (test_surface_archive_format.c,
// _lookup.c, _roundtrip.c, _parallel.c): format/corruption rejection, symbol
// lookup, full write->read->slice-bit-identical round-trip, and concurrent-read
// safety against a const parsed archive.

namespace {

using atx::vol::ArchiveHeader;
using atx::vol::EssviParams;
using atx::vol::ErrorCode;
using atx::vol::Parametrization;
using atx::vol::SurfaceArchive;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveWriteOpts;
using atx::vol::SviParams;
using atx::vol::VolSurface;
using atx::vol::write_surface_archive;

// ── Builders ─────────────────────────────────────────────────────────────

[[nodiscard]] VolSurface make_essvi(std::uint32_t uid, std::uint16_t n) {
  auto created = VolSurface::create(uid, Parametrization::Essvi, n);
  EXPECT_TRUE(created.has_value());
  VolSurface s = std::move(*created);
  for (std::uint16_t i = 0; i < n; ++i) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = 0.05 + 0.10 * static_cast<double>(i);
    e.F = 100.0;
    e.expiry_ns = 1700000000000000000LL + static_cast<std::int64_t>(i) * 86400LL * 1000000000LL;
    e.expiry_id = i;
    const auto st = s.set_slice_essvi(i, e);
    EXPECT_TRUE(st.has_value());
  }
  s.set_fit_ts_ns(1234567890123LL);
  VolSurface::Diagnostics d;
  d.rmse_vol = 0.005;
  d.max_residual_vol = 0.012;
  d.n_quotes_used = 250;
  d.n_quotes_dropped = 7;
  s.set_diagnostics(d);
  return s;
}

[[nodiscard]] VolSurface make_svi(std::uint32_t uid, std::uint16_t n) {
  auto created = VolSurface::create(uid, Parametrization::Svi, n);
  EXPECT_TRUE(created.has_value());
  VolSurface s = std::move(*created);
  for (std::uint16_t i = 0; i < n; ++i) {
    SviParams v{};
    v.a = 0.02 + 0.001 * static_cast<double>(i);
    v.b = 0.10;
    v.rho = -0.3;
    v.m = 0.0;
    v.sigma = 0.15;
    v.T = 0.05 + 0.10 * static_cast<double>(i);
    v.F = 100.0;
    v.expiry_id = i;
    const auto st = s.set_slice_svi(i, v);
    EXPECT_TRUE(st.has_value());
  }
  return s;
}

// Bit-exact double comparison via the object representation.
[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

void flip_byte(std::vector<std::byte>& b, std::size_t off) {
  ASSERT_LT(off, b.size());
  b[off] ^= std::byte{0xFF};
}

[[nodiscard]] std::vector<std::byte> build_one_essvi(std::string_view symbol,
                                                     std::uint32_t uid, std::uint16_t n) {
  const VolSurface s = make_essvi(uid, n);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{symbol, &s}};
  auto built = write_surface_archive(items);
  EXPECT_TRUE(built.has_value());
  return built.has_value() ? std::move(*built) : std::vector<std::byte>{};
}

}  // namespace

// ── Round-trip (mirrors test_surface_archive_roundtrip.c) ────────────────

TEST(SurfaceArchive, WriteOpenMapEssvi_RoundTrip_SlicesBitIdentical) {
  const VolSurface orig = make_essvi(42, 5);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"spy", &orig}};  // lowercase

  auto built = write_surface_archive(items);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();

  auto opened = SurfaceArchive::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);
  EXPECT_EQ(archive.count(), 1u);

  auto mapped = archive.map_symbol("SPY");  // case-insensitive
  ASSERT_TRUE(mapped.has_value()) << mapped.error().to_string();
  const VolSurface& got = *mapped;

  EXPECT_EQ(got.uid(), 42u);
  EXPECT_EQ(got.param(), Parametrization::Essvi);
  EXPECT_EQ(got.n_slices(), 5u);
  EXPECT_EQ(got.fit_ts_ns(), 1234567890123LL);
  EXPECT_EQ(got.diagnostics().n_quotes_used, 250u);
  EXPECT_EQ(got.diagnostics().n_quotes_dropped, 7u);
  EXPECT_TRUE(bits_equal(got.diagnostics().rmse_vol, 0.005));
  EXPECT_TRUE(bits_equal(got.diagnostics().max_residual_vol, 0.012));

  // Slices bit-identical (whole-struct memcmp, matching the C's memcmp check).
  const std::span<const EssviParams> a = orig.essvi_slices();
  const std::span<const EssviParams> b = got.essvi_slices();
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::memcmp(&a[i], &b[i], sizeof(EssviParams)), 0) << "slice " << i;
  }

  // w-eval bit-identical on a fixed (k, T) grid within the slice range.
  const std::array<double, 3> t_grid{0.05, 0.20, 0.40};
  const std::array<double, 3> k_grid{-0.10, 0.0, 0.05};
  for (const double t : t_grid) {
    for (const double k : k_grid) {
      EXPECT_TRUE(bits_equal(orig.w(k, t), got.w(k, t))) << "k=" << k << " T=" << t;
    }
  }
}

TEST(SurfaceArchive, WriteOpenMapSvi_RoundTrip_SlicesBitIdentical) {
  const VolSurface orig = make_svi(7, 4);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"AAPL", &orig}};

  auto built = write_surface_archive(items);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  auto opened = SurfaceArchive::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);

  auto mapped = archive.map_symbol("AAPL");
  ASSERT_TRUE(mapped.has_value()) << mapped.error().to_string();
  const VolSurface& got = *mapped;
  EXPECT_EQ(got.param(), Parametrization::Svi);
  EXPECT_EQ(got.uid(), 7u);

  const std::span<const SviParams> a = orig.svi_slices();
  const std::span<const SviParams> b = got.svi_slices();
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::memcmp(&a[i], &b[i], sizeof(SviParams)), 0) << "slice " << i;
  }
}

// ── Lookup (mirrors test_surface_archive_lookup.c) ───────────────────────

TEST(SurfaceArchive, Lookup_ManySymbols_ResolveEachToUid) {
  constexpr int kN = 500;
  std::vector<std::string> names;
  names.reserve(kN);
  std::vector<VolSurface> surfs;
  surfs.reserve(kN);
  std::vector<SurfaceArchiveItem> items;
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

  // Case-insensitive lookup of the lowercase variant of S00042 -> uid 43.
  auto de = archive.find("s00042");
  ASSERT_TRUE(de.has_value());
  EXPECT_EQ(de->uid, 43u);

  // Missing symbol -> NotFound.
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
  names.reserve(kN);
  std::vector<VolSurface> surfs;
  surfs.reserve(kN);
  std::vector<SurfaceArchiveItem> items;
  items.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    names.emplace_back("GROW" + std::to_string(i));
    surfs.push_back(make_essvi(static_cast<std::uint32_t>(100 + i), 2));
  }
  for (int i = 0; i < kN; ++i) {
    items.push_back(SurfaceArchiveItem{names[static_cast<std::size_t>(i)],
                                       &surfs[static_cast<std::size_t>(i)]});
  }

  SurfaceArchiveWriteOpts opts;
  opts.lookup_load_pct = 15;  // 8 / 0.15 ~= 54 -> next pow2 = 64

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

// ── Format / corruption (mirrors test_surface_archive_format.c) ──────────

TEST(SurfaceArchive, Format_RejectsBadMagic) {
  std::vector<std::byte> buf = build_one_essvi("TEST", 1, 3);
  ASSERT_FALSE(buf.empty());
  flip_byte(buf, 0);  // corrupt the first magic byte

  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
  EXPECT_NE(opened.error().message().find("magic"), std::string::npos);
}

TEST(SurfaceArchive, Format_RejectsCorruptedHeaderCrc) {
  std::vector<std::byte> buf = build_one_essvi("TEST", 1, 3);
  ASSERT_FALSE(buf.empty());
  flip_byte(buf, 120);  // inside the header reserved area (covered by header CRC)

  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
  EXPECT_NE(opened.error().message().find("checksum"), std::string::npos);
}

TEST(SurfaceArchive, Format_RejectsSchemaHashMismatch) {
  std::vector<std::byte> buf = build_one_essvi("TEST", 1, 3);
  ASSERT_FALSE(buf.empty());
  flip_byte(buf, 40);  // schema_hash field -> checked before the header CRC

  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::ParseError);
  EXPECT_NE(opened.error().message().find("schema"), std::string::npos);
}

TEST(SurfaceArchive, Format_RejectsCorruptedBlobPayload) {
  std::vector<std::byte> buf = build_one_essvi("TEST", 1, 3);
  ASSERT_FALSE(buf.empty());

  // Locate the data region and corrupt a byte inside the first blob's slices.
  ArchiveHeader h{};
  std::memcpy(&h, buf.data(), sizeof h);
  const std::size_t target = static_cast<std::size_t>(h.data_offset) + 256;
  flip_byte(buf, target);

  // Header + metadata still validate (only a blob byte changed), so open() ok.
  auto opened = SurfaceArchive::open(std::move(buf));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);

  auto mapped = archive.map_symbol("TEST");
  ASSERT_FALSE(mapped.has_value());
  EXPECT_EQ(mapped.error().code(), ErrorCode::ParseError);
  EXPECT_NE(mapped.error().message().find("checksum"), std::string::npos);
}

TEST(SurfaceArchive, Write_RejectsDuplicateCanonicalSymbol) {
  const VolSurface s1 = make_essvi(1, 3);
  const VolSurface s2 = make_essvi(2, 3);
  const std::array<SurfaceArchiveItem, 2> items{
      SurfaceArchiveItem{"AAA", &s1},
      SurfaceArchiveItem{"aaa", &s2},  // same canonical symbol
  };

  auto built = write_surface_archive(items);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::AlreadyExists);
}

TEST(SurfaceArchive, Write_RejectsEmptyAndUnsupported) {
  // Empty item list.
  auto empty = write_surface_archive(std::span<const SurfaceArchiveItem>{});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);

  // Null surface pointer.
  const std::array<SurfaceArchiveItem, 1> nulls{SurfaceArchiveItem{"X", nullptr}};
  auto bad = write_surface_archive(nulls);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
}

// ── Multi-symbol + map_all + capacity guard ──────────────────────────────

TEST(SurfaceArchive, MultiSymbol_MapAll_And_CapacityGuard) {
  const std::array<const char*, 6> raw{"SPY", "QQQ", "IWM", "AAPL", "MSFT", "TSLA"};
  const int kN = static_cast<int>(raw.size());
  std::vector<std::string> names;
  names.reserve(kN);
  std::vector<VolSurface> surfs;
  surfs.reserve(kN);
  std::vector<SurfaceArchiveItem> items;
  items.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    names.emplace_back(raw[static_cast<std::size_t>(i)]);
    surfs.push_back(make_essvi(static_cast<std::uint32_t>(i + 1), static_cast<std::uint16_t>(3 + i % 3)));
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
  ASSERT_EQ(archive.count(), static_cast<std::uint32_t>(kN));

  auto all = archive.map_all();
  ASSERT_TRUE(all.has_value()) << all.error().to_string();
  EXPECT_EQ(all->size(), static_cast<std::size_t>(kN));

  // Directory is symbol-sorted; every uid 1..N appears exactly once.
  std::array<int, 7> seen{};
  for (const VolSurface& s : *all) {
    ASSERT_GE(s.uid(), 1u);
    ASSERT_LE(s.uid(), static_cast<std::uint32_t>(kN));
    seen[s.uid()]++;
  }
  for (int u = 1; u <= kN; ++u) {
    EXPECT_EQ(seen[static_cast<std::size_t>(u)], 1) << "uid " << u;
  }

  // map_all_into: too-small output -> OutOfRange (the CAPACITY guard).
  std::vector<std::optional<VolSurface>> too_small(static_cast<std::size_t>(kN - 1));
  auto capped = archive.map_all_into(too_small);
  ASSERT_FALSE(capped.has_value());
  EXPECT_EQ(capped.error().code(), ErrorCode::OutOfRange);

  // Correctly sized output -> fills every slot.
  std::vector<std::optional<VolSurface>> outs(static_cast<std::size_t>(kN));
  auto wrote = archive.map_all_into(outs);
  ASSERT_TRUE(wrote.has_value()) << wrote.error().to_string();
  EXPECT_EQ(*wrote, static_cast<std::size_t>(kN));
  for (const auto& o : outs) {
    EXPECT_TRUE(o.has_value());
  }
}

// ── Concurrent reads (mirrors test_surface_archive_parallel.c) ───────────

TEST(SurfaceArchive, ConcurrentReads_ConstArchive_AreSafe) {
  constexpr int kN = 256;
  std::vector<std::string> names;
  names.reserve(kN);
  std::vector<VolSurface> surfs;
  surfs.reserve(kN);
  std::vector<SurfaceArchiveItem> items;
  items.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    char buf[8] = {};
    std::snprintf(buf, sizeof buf, "P%05d", i);
    names.emplace_back(buf);
    surfs.push_back(make_essvi(static_cast<std::uint32_t>(i + 1), 3));
  }
  for (int i = 0; i < kN; ++i) {
    items.push_back(SurfaceArchiveItem{names[static_cast<std::size_t>(i)],
                                       &surfs[static_cast<std::size_t>(i)]});
  }

  auto built = write_surface_archive(items);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  auto opened = SurfaceArchive::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const SurfaceArchive archive = std::move(*opened);  // const, shared across threads

  constexpr int kThreads = 4;
  std::array<int, kThreads> ok{};
  std::vector<std::thread> pool;
  pool.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    pool.emplace_back([&archive, &names, &ok, t]() {
      int local_ok = 0;
      // Every thread maps every symbol -> maximal concurrent read pressure on
      // the shared const lookup table + byte buffer.
      for (int i = 0; i < kN; ++i) {
        auto m = archive.map_symbol(names[static_cast<std::size_t>(i)]);
        if (m.has_value() && m->uid() == static_cast<std::uint32_t>(i + 1) &&
            m->n_slices() == 3u) {
          ++local_ok;
        }
      }
      ok[static_cast<std::size_t>(t)] = local_ok;
    });
  }
  for (std::thread& th : pool) {
    th.join();
  }
  for (int t = 0; t < kThreads; ++t) {
    EXPECT_EQ(ok[static_cast<std::size_t>(t)], kN) << "thread " << t;
  }
}
