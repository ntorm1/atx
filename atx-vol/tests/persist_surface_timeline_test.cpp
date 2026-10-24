// persist_surface_timeline_test.cpp — the uid-major SurfaceTimeline (`.atxstl`) and its
// period-file store (api/persist/surface_timeline.hpp; design wave3-persist D12, spec
// docs/storage/PERSIST_HISTORY_FORMAT.md).
//
// Every surface here is built from parameters (tests/support/risk_synthetic_market.hpp:
// three underliers, uid 1 / 3 on eSSVI, uid 2 on ConvexDense, so both the parametric and
// the lazily-materialized record kinds are covered), written to real ATXVSA2 partitions
// (or a real SurfaceDb), folded into a timeline and read back. The bit-exactness gates
// compare std::bit_cast images, never tolerances.
// PersistSurfaceTimeline.NoAllocationOnRangeLookup lives in
// persist_history_alloc_test.cpp (the focused exe that replaces operator new).

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/backtest/priced_surface_view.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/surface_timeline.hpp"
#include "atx/vol/api/risk/strike_coord.hpp"
#include "atx/vol/api/risk/theo_contract.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "atx/vol/api/storage/surface_db.hpp"
#include "support/risk_synthetic_market.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace rt = atx::vol::risk_test;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file

constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::string_view kSymbols[] = {"AAA", "BBB", "CCC"};

[[nodiscard]] fs::path fresh_dir(std::string_view tag) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-persist-stl-" + std::string(tag);
  if (info != nullptr) name += std::string("-") + info->name();
#if defined(_WIN32)
  name += "-" + std::to_string(_getpid());
#else
  name += "-" + std::to_string(getpid());
#endif
  const fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// State k of a deterministic path at `units` grid units after kT0, every axis moving.
[[nodiscard]] rt::StateMove move_at(std::int64_t units, int k) {
  rt::StateMove m;
  m.time_units = units;
  const double x = static_cast<double>(k);
  m.spot_ret = {0.004 * std::sin(x), -0.003 * std::cos(0.6 * x), 0.002 * x / 10.0};
  m.vol_level = 0.003 * std::sin(0.4 * x);
  m.vol_skew = 0.004 * std::cos(0.3 * x);
  return m;
}

// One snapshot's three owned surfaces (the in-memory reference) and its instant.
struct Snapshot {
  std::int64_t ts_ns{0};
  std::vector<PricedSurface> surfaces;
};

[[nodiscard]] Snapshot make_snapshot(std::int64_t units, int k) {
  Snapshot s;
  const rt::StateMove m = move_at(units, k);
  s.ts_ns = rt::instant_ns(units);
  s.surfaces.reserve(3);
  for (std::size_t ui = 0; ui < 3; ++ui) s.surfaces.push_back(rt::make_surface(ui, m));
  return s;
}

[[nodiscard]] std::vector<SurfaceArchiveItem> items_of(const Snapshot &s) {
  std::vector<SurfaceArchiveItem> items;
  for (std::size_t ui = 0; ui < s.surfaces.size(); ++ui) {
    items.push_back(SurfaceArchiveItem{kSymbols[ui], &s.surfaces[ui], std::nullopt});
  }
  return items;
}

// `n` snapshots `step_units` apart, each written as one ATXVSA2 partition.
struct Corpus {
  fs::path dir;
  std::vector<Snapshot> snaps;
  std::vector<std::string> paths;
  std::vector<TimelineSource> sources;
};

[[nodiscard]] Corpus make_corpus(std::string_view tag, int n, std::int64_t step_units = 7) {
  Corpus c;
  c.dir = fresh_dir(tag);
  for (int k = 0; k < n; ++k) {
    c.snaps.push_back(make_snapshot(static_cast<std::int64_t>(k) * step_units, k));
    const std::string path = (c.dir / ("p" + std::to_string(k) + ".atxvsa")).string();
    const auto items = items_of(c.snaps.back());
    EXPECT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    c.paths.push_back(path);
    c.sources.push_back(TimelineSource{path, 0});
  }
  return c;
}

[[nodiscard]] fs::path publish_timeline(const Corpus &c, std::string_view name) {
  auto image = encode_surface_timeline(c.sources, 0);
  EXPECT_TRUE(image.has_value()) << (image ? "" : to_string(image.error().status));
  const fs::path p = c.dir / std::string(name);
  EXPECT_TRUE(publish_persist_file(p.string(), *image).has_value());
  return p;
}

[[nodiscard]] std::vector<std::byte> slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  const std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::vector<std::byte> out(raw.size());
  if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

void spit(const fs::path &p, std::span<const std::byte> bytes) {
  std::vector<char> raw(bytes.size());
  if (!raw.empty()) std::memcpy(raw.data(), bytes.data(), bytes.size());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
}

// The queries a backtest asks of a surface, bit-compared over a strike / tenor grid.
template <class A, class B> void expect_views_bit_equal(const A &a, const B &b, std::size_t tag) {
  ASSERT_EQ(a.n_slices(), b.n_slices()) << tag;
  ASSERT_EQ(a.uid(), b.uid()) << tag;
  const double S = a.pricing().S;
  for (const double rel : {0.85, 0.97, 1.0, 1.04, 1.2}) {
    for (const double T : {0.03, 0.12, 0.31, 0.55, 0.9}) {
      const double K = S * rel;
      EXPECT_TRUE(same_bits(a.iv(K, T), b.iv(K, T))) << tag << " K " << K << " T " << T;
      EXPECT_TRUE(same_bits(a.forward_at(T), b.forward_at(T))) << tag;
      for (const Side side : {Side::Call, Side::Put}) {
        const auto fa = a.fair_value(K, T, side);
        const auto fb = b.fair_value(K, T, side);
        ASSERT_EQ(fa.has_value(), fb.has_value()) << tag;
        if (fa) EXPECT_TRUE(same_bits(*fa, *fb)) << tag << " K " << K << " T " << T;
        if (T < 0.1 || rel != 1.0) continue; // Greeks: cold AL solves, sample the ATM row
        const auto ga = a.greeks(K, T, side);
        const auto gb = b.greeks(K, T, side);
        ASSERT_EQ(ga.has_value(), gb.has_value()) << tag;
        if (!ga) continue;
        EXPECT_TRUE(same_bits(ga->price, gb->price)) << tag;
        EXPECT_TRUE(same_bits(ga->delta, gb->delta)) << tag;
        EXPECT_TRUE(same_bits(ga->gamma, gb->gamma)) << tag;
        EXPECT_TRUE(same_bits(ga->vega, gb->vega)) << tag;
        EXPECT_TRUE(same_bits(ga->theta, gb->theta)) << tag;
      }
    }
  }
}

} // namespace

TEST(PersistSurfaceTimeline, BuildsFromArchivePartitions) {
  const Corpus c = make_corpus("build", 5);
  const fs::path p = publish_timeline(c, "t.atxstl");
  auto tl = SurfaceTimeline::open(p.string());
  ASSERT_TRUE(tl.has_value()) << to_string(tl.error().status);
  const std::vector<std::uint32_t> want_uids{1, 2, 3};
  ASSERT_TRUE(std::equal(tl->uids().begin(), tl->uids().end(), want_uids.begin(), want_uids.end()));
  for (const std::uint32_t uid : want_uids) {
    const auto r = tl->range(uid, kMin, kMax);
    ASSERT_EQ(r.size(), c.snaps.size()) << uid;
    for (std::size_t k = 0; k < r.size(); ++k) {
      EXPECT_EQ(r[k].uid, uid);
      EXPECT_EQ(r[k].ts_ns, c.snaps[k].ts_ns) << "ts comes from the record's now_ts_ns";
      EXPECT_EQ(r[k].n_slices, c.snaps[k].surfaces[uid - 1].n_slices());
      EXPECT_EQ(r[k].record_offset % 64u, 0u);
      EXPECT_NE(r[k].source_fingerprint, 0u);
      EXPECT_EQ(r[k].reserved, 0u);
    }
  }
  // The header names the kind and the covered market-time range; the frame verifies.
  auto raw = PersistFile::open(p.string(), PersistKind::SurfaceTimeline,
                               surface_timeline_fingerprint());
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->header().ts_min_ns, c.snaps.front().ts_ns);
  EXPECT_EQ(raw->header().ts_max_ns, c.snaps.back().ts_ns);
  EXPECT_EQ(raw->header().file_size % kPersistFileAlign, 0u);
  EXPECT_TRUE(raw->verify_all().has_value());

  // Deterministic: the same sources give the same bytes.
  const auto again = encode_surface_timeline(c.sources, 0);
  ASSERT_TRUE(again.has_value());
  const std::vector<std::byte> disk = slurp(p);
  EXPECT_TRUE(disk.size() == again->size() &&
              std::memcmp(disk.data(), again->data(), disk.size()) == 0);

  // An explicit source instant overrides the records' own clock.
  std::vector<TimelineSource> shifted = c.sources;
  for (std::size_t k = 0; k < shifted.size(); ++k) {
    shifted[k].ts_ns = 1000 + static_cast<std::int64_t>(k);
  }
  auto img = encode_surface_timeline(shifted, 0);
  ASSERT_TRUE(img.has_value());
  const fs::path p2 = c.dir / "t2.atxstl";
  ASSERT_TRUE(publish_persist_file(p2.string(), *img).has_value());
  auto tl2 = SurfaceTimeline::open(p2.string());
  ASSERT_TRUE(tl2.has_value());
  EXPECT_EQ(tl2->range(2, kMin, kMax).front().ts_ns, 1000);

  // Refusals: the same (uid, ts) twice, an absent partition, no sources at all.
  std::vector<TimelineSource> dup = c.sources;
  dup.push_back(c.sources.front());
  auto d = encode_surface_timeline(dup, 0);
  ASSERT_FALSE(d.has_value());
  EXPECT_EQ(d.error().status, PersistStatus::DuplicateKey);
  const TimelineSource missing{(c.dir / "absent.atxvsa").string(), 0};
  auto m = encode_surface_timeline(std::span{&missing, 1}, 0);
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().status, PersistStatus::NotFound);
  EXPECT_EQ(encode_surface_timeline({}, 0).error().status, PersistStatus::InvalidArgument);
  // A reader of another kind refuses a timeline.
  auto wrong = PersistFile::open(p.string(), PersistKind::RiskRun, surface_timeline_fingerprint());
  ASSERT_FALSE(wrong.has_value());
  EXPECT_EQ(wrong.error().status, PersistStatus::WrongKind);
}

TEST(PersistSurfaceTimeline, ViewsBitExactToPartitionViews) {
  const Corpus c = make_corpus("bitexact", 3);
  const fs::path p = publish_timeline(c, "t.atxstl");
  for (const bool mapped : {true, false}) {
    PersistOpenOptions o;
    o.mapped = mapped;
    auto tl = SurfaceTimeline::open(p.string(), o);
    ASSERT_TRUE(tl.has_value());
    for (std::size_t k = 0; k < c.paths.size(); ++k) {
      auto part = SurfaceArchiveV2::open_file(c.paths[k]);
      ASSERT_TRUE(part.has_value());
      for (const std::uint32_t uid : tl->uids()) {
        const auto r = tl->range(uid, c.snaps[k].ts_ns, c.snaps[k].ts_ns);
        ASSERT_EQ(r.size(), 1u);
        auto tv = tl->view(r.front());
        ASSERT_TRUE(tv.has_value()) << to_string(tv.error().status);
        auto pv = part->map_symbol(kSymbols[uid - 1]);
        ASSERT_TRUE(pv.has_value());
        expect_views_bit_equal(*tv, *pv, k * 10 + uid);
        // ... and to the owned in-memory surface the partition was written from.
        expect_views_bit_equal(*tv, c.snaps[k].surfaces[uid - 1], k * 10 + uid);
      }
    }
  }
}

TEST(PersistSurfaceTimeline, RangeIsTsAscendingAndBounded) {
  const Corpus c = make_corpus("range", 7);
  const fs::path p = publish_timeline(c, "t.atxstl");
  auto tl = SurfaceTimeline::open(p.string());
  ASSERT_TRUE(tl.has_value());
  const auto ts = [&](std::size_t k) { return c.snaps[k].ts_ns; };
  for (const std::uint32_t uid : tl->uids()) {
    const auto all = tl->range(uid, kMin, kMax);
    ASSERT_EQ(all.size(), 7u);
    for (std::size_t k = 1; k < all.size(); ++k) EXPECT_LT(all[k - 1].ts_ns, all[k].ts_ns);
    // Inclusive at both ends.
    const auto mid = tl->range(uid, ts(2), ts(4));
    ASSERT_EQ(mid.size(), 3u);
    EXPECT_EQ(mid.front().ts_ns, ts(2));
    EXPECT_EQ(mid.back().ts_ns, ts(4));
    for (const TimelineEntry &e : mid) EXPECT_EQ(e.uid, uid);
    // Strictly inside a gap: nothing; reversed bounds: nothing.
    EXPECT_TRUE(tl->range(uid, ts(2) + 1, ts(3) - 1).empty());
    EXPECT_TRUE(tl->range(uid, ts(4), ts(2)).empty());
    // Open-ended windows clip at the data.
    EXPECT_EQ(tl->range(uid, kMin, ts(0)).size(), 1u);
    EXPECT_EQ(tl->range(uid, ts(6), kMax).size(), 1u);
    EXPECT_TRUE(tl->range(uid, ts(6) + 1, kMax).empty());
  }
}

TEST(PersistSurfaceTimeline, AbsentUidIsEmpty) {
  const Corpus c = make_corpus("absent", 2);
  const fs::path p = publish_timeline(c, "t.atxstl");
  auto tl = SurfaceTimeline::open(p.string());
  ASSERT_TRUE(tl.has_value());
  EXPECT_TRUE(tl->range(0, kMin, kMax).empty());
  EXPECT_TRUE(tl->range(4, kMin, kMax).empty());
  EXPECT_TRUE(tl->range(0xFFFFFFFFu, kMin, kMax).empty());
  // A moved-from timeline answers empty, never a dangling span.
  SurfaceTimeline moved = std::move(*tl);
  EXPECT_EQ(moved.range(1, kMin, kMax).size(), 2u);
  EXPECT_TRUE(tl->range(1, kMin, kMax).empty()); // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(tl->uids().empty());               // NOLINT(bugprone-use-after-move)
  // An absent file is NotFound; a store over an absent root is NotFound.
  EXPECT_EQ(SurfaceTimeline::open((c.dir / "none.atxstl").string()).error().status,
            PersistStatus::NotFound);
  EXPECT_EQ(SurfaceTimelineStore::open((c.dir / "no-store").string()).error().status,
            PersistStatus::NotFound);
}

TEST(PersistSurfaceTimeline, CorruptRecordRefusedOnView) {
  const Corpus c = make_corpus("corrupt", 3);
  const fs::path p = publish_timeline(c, "t.atxstl");
  constexpr auto kRecords = static_cast<std::uint32_t>(SurfaceTimelineSection::Records);
  std::uint64_t records_at = 0;
  TimelineEntry victim{};
  {
    auto raw = PersistFile::open(p.string(), PersistKind::SurfaceTimeline,
                                 surface_timeline_fingerprint(),
                                 PersistOpenOptions{false, PersistVerify::Framing, false, 0});
    ASSERT_TRUE(raw.has_value());
    ASSERT_NE(raw->entry(kRecords), nullptr);
    records_at = raw->entry(kRecords)->offset;
    auto tl = SurfaceTimeline::open(p.string());
    ASSERT_TRUE(tl.has_value());
    victim = tl->range(2, kMin, kMax)[1];
  }
  std::vector<std::byte> bytes = slurp(p);
  const auto at = static_cast<std::size_t>(records_at + victim.record_offset +
                                           victim.record_size - 9u);
  bytes[at] ^= std::byte{0x40};
  const fs::path bad = c.dir / "bad.atxstl";
  spit(bad, bytes);

  // Lazy (the timeline default) opens: the Records section is verified per record.
  auto tl = SurfaceTimeline::open(bad.string());
  ASSERT_TRUE(tl.has_value()) << to_string(tl.error().status);
  const auto r = tl->range(2, kMin, kMax);
  ASSERT_EQ(r.size(), 3u);
  auto v = tl->view(r[1]);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().status, PersistStatus::SectionChecksum);
  EXPECT_EQ(v.error().section, kRecords);
  EXPECT_TRUE(tl->view(r[0]).has_value()); // the neighbours are intact
  EXPECT_TRUE(tl->view(r[2]).has_value());
  EXPECT_TRUE(tl->view(tl->range(1, kMin, kMax)[1]).has_value());

  // Full verification refuses the file at open instead.
  auto full = SurfaceTimeline::open(bad.string(),
                                    PersistOpenOptions{true, PersistVerify::Full, false, 0});
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(full.error().status, PersistStatus::SectionChecksum);

  // A fabricated entry that points outside the Records section (or off the record
  // grid) is refused, never read; a wrong CRC copy is a checksum refusal.
  TimelineEntry forged = r[0];
  forged.record_offset = 1ull << 40;
  EXPECT_EQ(tl->view(forged).error().status, PersistStatus::IndexBounds);
  forged = r[0];
  forged.record_offset += 8;
  EXPECT_EQ(tl->view(forged).error().status, PersistStatus::IndexBounds);
  forged = r[0];
  forged.record_crc32c ^= 1u;
  EXPECT_EQ(tl->view(forged).error().status, PersistStatus::SectionChecksum);
}

TEST(PersistSurfaceTimeline, ProjectSeriesOverTimelineMatchesOwnedSurfaces) {
  const Corpus c = make_corpus("project", 11, 9);
  const fs::path root = c.dir / "store";
  // One period file published by hand (the SurfaceDb path is PeriodRoutingAcrossMonths).
  auto image = encode_surface_timeline(c.sources, 0);
  ASSERT_TRUE(image.has_value());
  auto pub = PersistDirectory::publish(root.string(), "2023-11", *image, 33);
  ASSERT_TRUE(pub.has_value()) << to_string(pub.error().status);
  auto store = SurfaceTimelineStore::open(root.string());
  ASSERT_TRUE(store.has_value()) << to_string(store.error().status);

  for (const std::uint32_t uid : {1u, 2u}) {
    std::vector<PricedSurfaceView> views;
    std::vector<SurfaceRef> refs;
    std::vector<std::int64_t> ts;
    auto n = store->collect(uid, kMin, kMax, views, refs, ts);
    ASSERT_TRUE(n.has_value());
    ASSERT_EQ(*n, c.snaps.size());
    std::vector<SurfaceRef> owned;
    for (std::size_t k = 0; k < c.snaps.size(); ++k) {
      EXPECT_EQ(ts[k], c.snaps[k].ts_ns);
      owned.emplace_back(&c.snaps[k].surfaces[uid - 1]);
    }
    for (const StrikeCoord strike : {StrikeCoord::log_moneyness(-0.05),
                                     StrikeCoord::delta(-0.25, DeltaConvention::Forward)}) {
      const TheoContract spec{uid, MaturitySpec::years_of(0.2), strike, Side::Put, 100.0};
      std::vector<ResolvedTheo> from_timeline(refs.size());
      std::vector<ResolvedTheo> from_owned(refs.size());
      ASSERT_TRUE(project_series(spec, refs, ts, ProjectionPolicy{}, from_timeline).has_value());
      ASSERT_TRUE(project_series(spec, owned, ts, ProjectionPolicy{}, from_owned).has_value());
      for (std::size_t k = 0; k < refs.size(); ++k) {
        const ResolvedTheo &a = from_timeline[k];
        const ResolvedTheo &b = from_owned[k];
        ASSERT_EQ(a.status, b.status) << k;
        EXPECT_EQ(a.status, TheoStatus::Ok) << k;
        EXPECT_TRUE(same_bits(a.leg.K, b.leg.K)) << k;
        EXPECT_TRUE(same_bits(a.T, b.T)) << k;
        EXPECT_TRUE(same_bits(a.forward, b.forward)) << k;
        EXPECT_TRUE(same_bits(a.k_log, b.k_log)) << k;
        EXPECT_TRUE(same_bits(a.iv, b.iv)) << k;
        EXPECT_TRUE(same_bits(a.mark, b.mark)) << k;
        EXPECT_TRUE(same_bits(a.delta, b.delta)) << k;
        EXPECT_TRUE(same_bits(a.greeks.gamma, b.greeks.gamma)) << k;
        EXPECT_TRUE(same_bits(a.greeks.vega, b.greeks.vega)) << k;
        EXPECT_TRUE(same_bits(a.greeks.theta, b.greeks.theta)) << k;
        EXPECT_EQ(a.fingerprint, b.fingerprint) << k;
      }
    }
  }
}

TEST(PersistSurfaceTimeline, PeriodRoutingAcrossMonths) {
  const fs::path dir = fresh_dir("months");
  auto db = SurfaceDb::create((dir / "db").string());
  ASSERT_TRUE(db.has_value());
  // kT0 is 2023-11-14T22:13:20Z; 17 grid days on is December. Keys follow the fit-lane
  // contract YYYY-MM-DDTHHMM (one market ts per partition).
  const std::int64_t day = rt::kDayUnits;
  const std::vector<std::int64_t> units{0, 5 * day, 12 * day, 17 * day, 19 * day};
  const std::vector<std::string> keys{"2023-11-14T2213", "2023-11-19T2213", "2023-11-26T2213",
                                      "2023-12-01T2213", "2023-12-03T2213"};
  std::vector<Snapshot> snaps;
  for (std::size_t i = 0; i < units.size(); ++i) {
    snaps.push_back(make_snapshot(units[i], static_cast<int>(i)));
    const auto items = items_of(snaps.back());
    ASSERT_TRUE(db->write_partition(keys[i], items).has_value());
  }
  const fs::path root = dir / "timeline";
  auto built = SurfaceTimelineStore::build_from_surface_db(*db, keys, root.string(), 0);
  ASSERT_TRUE(built.has_value()) << to_string(built.error().status);
  EXPECT_EQ(*built, 15u); // 5 snapshots x 3 uids
  auto manifest = PersistDirectory::open(root.string());
  ASSERT_TRUE(manifest.has_value());
  ASSERT_EQ(manifest->entries().size(), 2u);
  ASSERT_NE(manifest->find("2023-11"), nullptr);
  ASSERT_NE(manifest->find("2023-12"), nullptr);
  EXPECT_EQ(manifest->find("2023-11")->records, 9u);
  EXPECT_EQ(manifest->find("2023-12")->records, 6u);

  auto store = SurfaceTimelineStore::open(root.string());
  ASSERT_TRUE(store.has_value());
  std::vector<PricedSurfaceView> views;
  std::vector<SurfaceRef> refs;
  std::vector<std::int64_t> ts;
  ASSERT_EQ(store->collect(3, kMin, kMax, views, refs, ts).value(), 5u);
  for (std::size_t i = 0; i < ts.size(); ++i) {
    EXPECT_EQ(ts[i], snaps[i].ts_ns) << "ascending across the month boundary";
    expect_views_bit_equal(views[i], snaps[i].surfaces[2], i);
  }
  // A window straddling the boundary takes from both files.
  views.clear();
  refs.clear();
  ts.clear();
  ASSERT_EQ(store->collect(1, snaps[2].ts_ns, snaps[3].ts_ns, views, refs, ts).value(), 2u);
  EXPECT_EQ(ts.front(), snaps[2].ts_ns);
  EXPECT_EQ(ts.back(), snaps[3].ts_ns);

  // Incremental: a new December partition merges into the existing December file (a
  // new generation); November keeps its generation.
  const std::uint64_t nov_seq = manifest->find("2023-11")->seq;
  const std::uint64_t dec_seq = manifest->find("2023-12")->seq;
  snaps.push_back(make_snapshot(21 * day, 9));
  const auto items = items_of(snaps.back());
  ASSERT_TRUE(db->write_partition("2023-12-05T2213", items).has_value());
  const std::vector<std::string> more{"2023-12-05T2213"};
  ASSERT_EQ(SurfaceTimelineStore::build_from_surface_db(*db, more, root.string(), 0).value(), 9u);
  auto after = PersistDirectory::open(root.string());
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->find("2023-11")->seq, nov_seq);
  EXPECT_GT(after->find("2023-12")->seq, dec_seq);
  EXPECT_EQ(after->find("2023-12")->records, 9u);
  auto store2 = SurfaceTimelineStore::open(root.string());
  ASSERT_TRUE(store2.has_value());
  views.clear();
  refs.clear();
  ts.clear();
  ASSERT_EQ(store2->collect(2, kMin, kMax, views, refs, ts).value(), 6u);
  EXPECT_EQ(ts.back(), snaps.back().ts_ns);
  expect_views_bit_equal(views.back(), snaps.back().surfaces[1], 99);

  // An unknown key is refused before anything is published.
  const std::vector<std::string> bogus{"1999-01-01T0000"};
  auto refused = SurfaceTimelineStore::build_from_surface_db(*db, bogus, root.string(), 0);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().status, PersistStatus::NotFound);
}

TEST(PersistSurfaceTimeline, CollectReservesBeforeTakingRefs) {
  const Corpus c = make_corpus("collect", 6);
  const fs::path root = c.dir / "store";
  auto image = encode_surface_timeline(c.sources, 0);
  ASSERT_TRUE(image.has_value());
  ASSERT_TRUE(PersistDirectory::publish(root.string(), "2023-11", *image, 18).has_value());
  auto store = SurfaceTimelineStore::open(root.string());
  ASSERT_TRUE(store.has_value());

  std::vector<PricedSurfaceView> views;
  std::vector<SurfaceRef> refs;
  std::vector<std::int64_t> ts;
  ASSERT_EQ(store->collect(1, kMin, kMax, views, refs, ts).value(), 6u);
  EXPECT_GE(views.capacity(), 6u);
  views.shrink_to_fit(); // the next collect must grow (move) the view buffer
  ASSERT_EQ(store->collect(2, kMin, kMax, views, refs, ts).value(), 6u);
  ASSERT_EQ(views.size(), 12u);
  ASSERT_EQ(refs.size(), 12u);
  ASSERT_EQ(ts.size(), 12u);
  // Every ref — including the first call's, taken before the buffer moved — names the
  // view at its own index and answers that view's queries.
  for (std::size_t i = 0; i < views.size(); ++i) {
    ASSERT_TRUE(refs[i].is_view());
    EXPECT_EQ(refs[i].view(), &views[i]) << i;
    EXPECT_EQ(refs[i].uid(), i < 6 ? 1u : 2u);
    EXPECT_TRUE(same_bits(refs[i]->iv(100.0, 0.2), views[i].iv(100.0, 0.2))) << i;
  }
  // An absent uid adds nothing and leaves the vectors as they were.
  EXPECT_EQ(store->collect(9, kMin, kMax, views, refs, ts).value(), 0u);
  EXPECT_EQ(views.size(), 12u);
  EXPECT_EQ(refs.size(), 12u);
}
