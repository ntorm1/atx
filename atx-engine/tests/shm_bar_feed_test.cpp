#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/data/shm_bar_feed.hpp"
#include "atx/engine/event/event.hpp"

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/segment_reader.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; the
// individual sub-headers are not self-contained on all SDK versions, so the
// umbrella is required. All Win32 symbol warnings in this file are suppressed.
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#endif

namespace {

// Return a unique temp file path (not yet created). Uses Win32 secure temp APIs
// on Windows to avoid the MSVC CRT tmpnam deprecation (which is fatal under /WX).
std::string temp_path(const std::string &name) {
#if defined(_WIN32)
  // NOLINTBEGIN(misc-include-cleaner) — Win32 symbols come via the umbrella above.
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  wchar_t tmp_dir[MAX_PATH + 1]{};
  GetTempPathW(MAX_PATH + 1, tmp_dir);
  wchar_t tmp_file[MAX_PATH + 1]{};
  GetTempFileNameW(tmp_dir, L"atx", 0, tmp_file);
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::wstring wpath(tmp_file);
  const std::wstring wsuffix(name.begin(), name.end());
  wpath += wsuffix + L".atxseg";
  const std::string path(wpath.begin(), wpath.end());
  // NOLINTEND(misc-include-cleaner)
#else
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  char buf[L_tmpnam]{};
  // NOLINTNEXTLINE(cert-msc50-cpp,cert-msc30-c)
  std::tmpnam(buf);
  const std::string path = std::string(buf) + name + ".atxseg";
#endif
  return path;
}

// Build a 2-time x 1-instrument OHLCV segment: AAA has a bar at t=100 only.
std::string make_ohlcv(const std::string &name) {
  atx::tsdb::SegmentBuilder b({"open", "high", "low", "close", "volume"}, {"AAA"}, {100, 200});
  b.set(0, 0, 0, 10.0);   // open
  b.set(1, 0, 0, 12.0);   // high
  b.set(2, 0, 0, 9.0);    // low
  b.set(3, 0, 0, 11.0);   // close
  b.set(4, 0, 0, 1000.0); // volume
  // t=200 left absent for AAA (present bit clear) -> no event there.
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}

} // namespace

TEST(ShmBarFeed_StepPublishesPresentBars_NoLookAhead, PublishesBars) {
  const std::string path = make_ohlcv("feed1");
  auto reader = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(reader.has_value());

  atx::core::domain::SymbolTable symbols;
  atx::engine::SimClock clock;
  auto bus = std::make_unique<atx::engine::EventBus<>>();
  (void)bus->add_consumer(0); // register the drain consumer (drain_in_order gate)

  atx::engine::data::ShmBarFeed feed(*reader, symbols, clock, *bus);

  // Step 1: frontier t=100, AAA present -> exactly one Market event, clock@100.
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 100);
  int seen = 0;
  atx::core::domain::Bar got{};
  bus->drain_in_order([&](atx::usize, const atx::engine::event::Event &e) {
    ++seen;
    EXPECT_LE(e.knowledge_ts.unix_nanos(), clock.now().unix_nanos()); // no look-ahead
    got = e.payload_as<atx::engine::data::MarketPayload>().as_bar();
  });
  EXPECT_EQ(seen, 1);
  EXPECT_DOUBLE_EQ(got.close.to_decimal().to_double(), 11.0);

  // Step 2: frontier t=200, AAA absent -> clock advances, zero events.
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 200);
  int seen2 = 0;
  bus->drain_in_order([&](atx::usize, const atx::engine::event::Event &) { ++seen2; });
  EXPECT_EQ(seen2, 0);

  // Step 3: EOF.
  EXPECT_FALSE(feed.step());
  static_cast<void>(std::remove(path.c_str()));
}
