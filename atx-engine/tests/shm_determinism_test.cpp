// Determinism cross-check (tsdb-9): a backtest fed from a shared-memory segment
// must produce the SAME event stream as one fed from the in-memory k-way-merge
// feed on identical data. Folds every drained event's salient fields into a
// running hash; identical streams => identical digest. Mirrors the hashing style
// of replay_determinism_test.cpp.

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
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

// Fold every drained event's salient fields into a running hash. Identical
// streams -> identical digest.
atx::u64 drain_hash(atx::engine::EventBus<> &bus) {
  std::size_t h = 0;
  bus.drain_in_order([&](atx::usize, const atx::engine::event::Event &e) {
    const auto payload = e.payload_as<atx::engine::data::MarketPayload>();
    h = atx::core::hash_combine(h, e.knowledge_ts.unix_nanos(), e.event_ts.unix_nanos(),
                                payload.symbol.id, payload.as_bar().close.to_decimal().to_string());
  });
  return static_cast<atx::u64>(h);
}

} // namespace

TEST(ShmDeterminism_ShmFeedEqualsInMemory_ByteStream, SameDigest) {
  // --- shared data: two symbols, two timestamps, dense ----------------------
  atx::core::domain::SymbolTable shm_syms;
  atx::core::domain::SymbolTable mem_syms;

  // Build the segment (shm side).
  atx::tsdb::SegmentBuilder b({"open", "high", "low", "close", "volume"}, {"AAA", "BBB"},
                              {100, 200});
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  const double px[2][2] = {{11.0, 21.0}, {12.0, 22.0}}; // [t][inst] close
  for (atx::u64 t = 0; t < 2; ++t) {
    for (atx::u32 i = 0; i < 2; ++i) {
      b.set(0, t, i, px[t][i]);       // open
      b.set(1, t, i, px[t][i] + 1.0); // high
      b.set(2, t, i, px[t][i] - 1.0); // low
      b.set(3, t, i, px[t][i]);       // close
      b.set(4, t, i, 100.0);          // volume
    }
  }
  const std::string path = temp_path("det");
  ASSERT_TRUE(b.write(path, 0).has_value());
  auto reader = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(reader.has_value());

  // --- run the shm feed -----------------------------------------------------
  atx::engine::SimClock shm_clock;
  auto shm_bus = std::make_unique<atx::engine::EventBus<>>();
  (void)shm_bus->add_consumer(0);
  atx::engine::data::ShmBarFeed shm_feed(*reader, shm_syms, shm_clock, *shm_bus);
  std::size_t shm_digest = 0;
  while (shm_feed.step()) {
    shm_digest = atx::core::hash_combine(shm_digest, drain_hash(*shm_bus));
  }

  // --- run the in-memory feed over the SAME logical data ---------------------
  using atx::engine::data::BarRow;
  const auto ts100 = atx::core::time::Timestamp::from_unix_nanos(100);
  const auto ts200 = atx::core::time::Timestamp::from_unix_nanos(200);
  const auto sym_aaa = mem_syms.intern("AAA");
  const auto sym_bbb = mem_syms.intern("BBB");
  const auto mk = [](atx::core::domain::Symbol s, atx::core::time::Timestamp ts, double close) {
    atx::core::domain::Bar bar{};
    bar.ts = ts;
    bar.open = atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close));
    bar.high =
        atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close + 1.0));
    bar.low = atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close - 1.0));
    bar.close = atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close));
    bar.volume = atx::core::domain::Quantity::from_decimal(*atx::core::Decimal::from_double(100.0));
    return BarRow{s, bar, ts, false};
  };
  // Each source must be knowledge_ts-sorted; one source per symbol.
  const std::vector<BarRow> src_aaa{mk(sym_aaa, ts100, 11.0), mk(sym_aaa, ts200, 12.0)};
  const std::vector<BarRow> src_bbb{mk(sym_bbb, ts100, 21.0), mk(sym_bbb, ts200, 22.0)};
  const std::vector<std::span<const BarRow>> sources{src_aaa, src_bbb};

  atx::engine::SimClock mem_clock;
  auto mem_bus = std::make_unique<atx::engine::EventBus<>>();
  (void)mem_bus->add_consumer(0);
  atx::engine::data::InMemoryBarFeed mem_feed(sources, mem_clock, *mem_bus);
  std::size_t mem_digest = 0;
  while (mem_feed.step()) {
    mem_digest = atx::core::hash_combine(mem_digest, drain_hash(*mem_bus));
  }

  EXPECT_EQ(shm_digest, mem_digest);
  static_cast<void>(std::remove(path.c_str()));
}
