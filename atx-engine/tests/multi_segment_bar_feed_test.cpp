#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/data/multi_segment_bar_feed.hpp"
#include "atx/engine/event/event.hpp"

#include "atx/tsdb/builder.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; the
// individual sub-headers are not self-contained on all SDK versions, so the
// umbrella is required. All Win32 symbol warnings in this file are suppressed.
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace atxtest_multi_segment_bar_feed_test {

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

// One-row OHLCV segment: `sym` present at time `t`, close = `close`.
std::string make_seg(const std::string &name, const std::string &sym, atx::i64 t, double close) {
  atx::tsdb::SegmentBuilder b({"open", "high", "low", "close", "volume"}, {sym}, {t});
  // SegmentBuilder::set arg order is (field, t, inst, value).
  b.set(0, 0, 0, close);  // open
  b.set(1, 0, 0, close);  // high
  b.set(2, 0, 0, close);  // low
  b.set(3, 0, 0, close);  // close
  b.set(4, 0, 0, 1000.0); // volume
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}


TEST(MultiSegmentBarFeed, StreamsSegmentsInDateOrder) {
  const std::string s1 = make_seg("ms1", "AAA", 100, 11.0);
  const std::string s2 = make_seg("ms2", "BBB", 200, 22.0);

  atx::core::domain::SymbolTable symbols;
  atx::engine::SimClock clock;
  // Heap-allocate the bus: EventBus<>'s default 64Ki-slot ring (~8 MiB) is far too
  // large for the stack (mirrors shm_bar_feed_test.cpp).
  auto bus = std::make_unique<atx::engine::EventBus<>>();
  (void)bus->add_consumer(0);

  atx::engine::data::MultiSegmentBarFeed feed({s1, s2}, symbols, clock, *bus);

  // Day 1: AAA @ 100.
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 100);
  int seen1 = 0;
  double close1 = 0.0;
  bus->drain_in_order([&](atx::usize, const atx::engine::event::Event &e) {
    ++seen1;
    close1 =
        e.payload_as<atx::engine::data::MarketPayload>().as_bar().close.to_decimal().to_double();
  });
  EXPECT_EQ(seen1, 1);
  EXPECT_DOUBLE_EQ(close1, 11.0);

  // Day 2: BBB @ 200 (next segment opened lazily, clock advanced forward).
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 200);
  int seen2 = 0;
  double close2 = 0.0;
  bus->drain_in_order([&](atx::usize, const atx::engine::event::Event &e) {
    ++seen2;
    close2 =
        e.payload_as<atx::engine::data::MarketPayload>().as_bar().close.to_decimal().to_double();
  });
  EXPECT_EQ(seen2, 1);
  EXPECT_DOUBLE_EQ(close2, 22.0);

  // EOF after the last segment.
  EXPECT_FALSE(feed.step());

  static_cast<void>(std::remove(s1.c_str()));
  static_cast<void>(std::remove(s2.c_str()));
}

TEST(MultiSegmentBarFeed, SameSymbolAcrossDaysInternsOnce) {
  const std::string s1 = make_seg("msa", "AAA", 100, 10.0);
  const std::string s2 = make_seg("msb", "AAA", 200, 20.0);

  atx::core::domain::SymbolTable symbols;
  atx::engine::SimClock clock;
  auto bus = std::make_unique<atx::engine::EventBus<>>();
  (void)bus->add_consumer(0);

  const atx::core::domain::Symbol pre = symbols.intern("AAA");
  atx::engine::data::MultiSegmentBarFeed feed({s1, s2}, symbols, clock, *bus);

  int total = 0;
  while (feed.step()) {
    bus->drain_in_order([&](atx::usize, const atx::engine::event::Event &) { ++total; });
  }
  EXPECT_EQ(total, 2);                         // AAA on both days
  EXPECT_EQ(symbols.intern("AAA").id, pre.id); // interning stayed stable

  static_cast<void>(std::remove(s1.c_str()));
  static_cast<void>(std::remove(s2.c_str()));
}


}  // namespace atxtest_multi_segment_bar_feed_test
