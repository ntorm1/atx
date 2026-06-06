// End-to-end integration test threading the full Databento daily-OHLCV pipeline
// in-process: atx-core loader -> atx-tsdb bridge -> atx-engine feed.
//
// Each seam (loader, bridge, MultiSegmentBarFeed) is unit-tested in isolation
// elsewhere; this is the ONE test that runs all three together and asserts that
// the loader's stored i64 prices, after the bridge's x1e-9 scaling, reproduce the
// original prices as the feed's PUBLISHED bar closes — across a day boundary.
//
// It also exercises the canonical 5-field OHLCV scale list (open/high/low/close at
// 1e-9, volume at 1.0), covering the open/high/low scaling path no other test hits.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/external/databento.hpp"

#include "atx/tsdb/load_parquet.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/data/multi_segment_bar_feed.hpp"
#include "atx/engine/event/event.hpp"

#include "dbn_fixture.hpp" // atx::test::{OhlcvRow, SymMap, build_dbn, zstd_compress, build_zip}

namespace db = atx::external::databento;
namespace fs = std::filesystem;

namespace {

// ts in ns for 2024-01-02 and 2024-01-03 at 00:00:00 UTC (mirrors databento_test).
constexpr std::uint64_t kJan02 = 1'704'153'600'000'000'000ULL;
constexpr std::uint64_t kJan03 = 1'704'240'000'000'000'000ULL;

// Known close prices in Databento 1e-9 fixed point -> exact f64 dollars after
// the bridge's x1e-9 scaling, so the round-trip assertions are not fuzzy.
constexpr std::int64_t k1e9 = 1'000'000'000LL;

fs::path write_zip_file(const std::string &name, const std::vector<std::byte> &bytes) {
  const fs::path p = fs::temp_directory_path() / name;
  std::ofstream os(p, std::ios::binary);
  os.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return p;
}

// Build a 2-day EQUS.SUMMARY zip: one .dbn.zst entry per trading day. AAPL trades
// on BOTH days (stable intern check); GOOG only on day 2 (per-day symbol set).
std::vector<std::byte> build_two_day_zip() {
  using atx::test::OhlcvRow;
  // Day 1: AAPL close 150, MSFT close 250.
  const std::vector<OhlcvRow> day1 = {
      {101, kJan02, 149 * k1e9, 151 * k1e9, 148 * k1e9, 150 * k1e9, 5000, 0x23},
      {102, kJan02, 248 * k1e9, 252 * k1e9, 247 * k1e9, 250 * k1e9, 6000, 0x23},
  };
  // Day 2: AAPL close 155 (same symbol, new day), GOOG close 300 (day-2 only).
  const std::vector<OhlcvRow> day2 = {
      {101, kJan03, 154 * k1e9, 156 * k1e9, 153 * k1e9, 155 * k1e9, 5500, 0x23},
      {103, kJan03, 299 * k1e9, 301 * k1e9, 298 * k1e9, 300 * k1e9, 7000, 0x23},
  };
  const std::vector<atx::test::SymMap> maps = {
      {101, "AAPL", 20240101, 20250101},
      {102, "MSFT", 20240101, 20250101},
      {103, "GOOG", 20240101, 20250101},
  };
  auto d1 = atx::test::zstd_compress(atx::test::build_dbn(maps, day1));
  auto d2 = atx::test::zstd_compress(atx::test::build_dbn(maps, day2));
  return atx::test::build_zip({{"equs-20240102.dbn.zst", d1}, {"equs-20240103.dbn.zst", d2}});
}

// One published bar's observation: (clock ts at publish, interned id, close $).
struct Published {
  atx::i64 ts_nanos{};
  atx::u32 sym_id{};
  double close{};
};

// Discover the bridge's .seg files under `seg_dir`, sorted ascending (date order).
std::vector<std::string> discover_segments(const fs::path &seg_dir) {
  std::vector<std::string> seg_paths;
  for (const auto &e : fs::directory_iterator(seg_dir)) {
    if (e.path().extension() == ".seg") {
      seg_paths.push_back(e.path().string());
    }
  }
  std::sort(seg_paths.begin(), seg_paths.end());
  return seg_paths;
}

// Drive `feed` to EOF, draining the bus after each step and recording every
// published bar (clock ts, interned symbol id, scaled close).
std::vector<Published> drive_feed(atx::engine::data::MultiSegmentBarFeed &feed,
                                  atx::engine::SimClock &clock,
                                  atx::engine::EventBus<> &bus) {
  std::vector<Published> pubs;
  while (feed.step()) {
    const atx::i64 ts = clock.now().unix_nanos();
    bus.drain_in_order([&](atx::usize, const atx::engine::event::Event &e) {
      const auto &mp = e.payload_as<atx::engine::data::MarketPayload>();
      pubs.push_back({ts, mp.symbol.id, mp.as_bar().close.to_decimal().to_double()});
    });
  }
  return pubs;
}

} // namespace

// Loader -> bridge -> feed round-trip. Asserts the published bar closes equal the
// fixture's close_i64 * 1e-9 per (day, symbol), the clock crosses the day
// boundary, and a both-days symbol keeps one stable interned id.
TEST(DatabentoPipelineE2E, LoaderBridgeFeedRoundTrip) {
  // --- (1) fixture zip on disk ---
  const fs::path zip_path = write_zip_file("atx_e2e_load.zip", build_two_day_zip());
  const fs::path dest = fs::temp_directory_path() / "atx_e2e_dest";
  const fs::path seg_dir = fs::temp_directory_path() / "atx_e2e_segs";
  fs::remove_all(dest);
  fs::remove_all(seg_dir);

  // --- (2) real loader: zip -> hive of date= parquet partitions ---
  auto stats = db::load_equs_summary_zip(zip_path.string(), dest.string());
  ASSERT_TRUE(stats.has_value()) << stats.error().to_string();
  EXPECT_EQ(stats->partitions_written, 2);
  EXPECT_EQ(stats->records_decoded, 4);

  // --- (3) real bridge: hive -> per-date sealed segments, canonical 5-field scale ---
  const std::vector<std::pair<std::string, atx::f64>> scales = {
      {"open", 1e-9}, {"high", 1e-9}, {"low", 1e-9}, {"close", 1e-9}, {"volume", 1.0}};
  ASSERT_TRUE(atx::tsdb::build_dated_segments(dest.string(), seg_dir.string(), "ts", "symbol",
                                              scales, /*created_at_nanos=*/0)
                  .has_value());

  // --- (4) discover .seg files, sort ascending so date order is deterministic ---
  const std::vector<std::string> seg_paths = discover_segments(seg_dir);
  ASSERT_EQ(seg_paths.size(), 2U);

  // --- (5) drive the feed to EOF, recording every published bar ---
  atx::core::domain::SymbolTable symbols;
  atx::engine::SimClock clock;
  // Heap-allocate the bus: EventBus<>'s default ring (~MiB) overflows the stack.
  auto bus = std::make_unique<atx::engine::EventBus<>>();
  (void)bus->add_consumer(0);

  // Pre-intern AAPL: its id must survive the day boundary (stable-intern check).
  const atx::u32 aapl_id = symbols.intern("AAPL").id;

  atx::engine::data::MultiSegmentBarFeed feed(seg_paths, symbols, clock, *bus);
  const std::vector<Published> pubs = drive_feed(feed, clock, *bus);

  // --- assertions: 4 published bars across two days ---
  ASSERT_EQ(pubs.size(), 4U);
  const atx::i64 j2 = static_cast<atx::i64>(kJan02);
  const atx::i64 j3 = static_cast<atx::i64>(kJan03);
  EXPECT_LT(j2, j3); // day boundary: day-1 ts strictly precede day-2 ts

  // Build (ts, ticker) -> close. Resolving id via the shared table also confirms
  // the published id round-trips to the right symbol name.
  std::map<std::pair<atx::i64, std::string>, double> closes;
  bool day1_aapl_stable = false;
  bool day2_aapl_stable = false;
  for (const auto &p : pubs) {
    closes[{p.ts_nanos, std::string(symbols.name(atx::core::domain::Symbol{p.sym_id}))}] = p.close;
    day1_aapl_stable = day1_aapl_stable || (p.sym_id == aapl_id && p.ts_nanos == j2);
    day2_aapl_stable = day2_aapl_stable || (p.sym_id == aapl_id && p.ts_nanos == j3);
  }

  // Closes equal close_i64 * 1e-9 — exact, end-to-end through all three layers.
  EXPECT_DOUBLE_EQ(closes.at({j2, "AAPL"}), 150.0);
  EXPECT_DOUBLE_EQ(closes.at({j2, "MSFT"}), 250.0);
  EXPECT_DOUBLE_EQ(closes.at({j3, "AAPL"}), 155.0);
  EXPECT_DOUBLE_EQ(closes.at({j3, "GOOG"}), 300.0);

  // Both-days symbol kept one stable interned id across the day boundary.
  EXPECT_EQ(symbols.intern("AAPL").id, aapl_id);
  EXPECT_TRUE(day1_aapl_stable);
  EXPECT_TRUE(day2_aapl_stable);

  fs::remove_all(dest);
  fs::remove_all(seg_dir);
  static_cast<void>(std::remove(zip_path.string().c_str()));
}
