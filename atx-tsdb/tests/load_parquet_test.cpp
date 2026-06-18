#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/io/parquet_writer.hpp"
#include "atx/tsdb/load_parquet.hpp"
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

} // namespace

TEST(LoadPivot_LongRows_BuildsDenseGrid, Pivots) {
  // Long rows: (t, symbol, close). Two timestamps x two symbols, one gap.
  atx::tsdb::LongColumns cols;
  cols.field_names = {"close"};
  cols.times = {100, 100, 200};         // row time
  cols.symbols = {"AAA", "BBB", "AAA"}; // row symbol
  cols.values = {{1.0, 2.0, 3.0}};      // values[field][row]

  const std::string path = temp_path("pivot1");
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, path, /*created_at_nanos=*/0).has_value());

  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->time_count(), 2U);       // {100, 200} deduped + sorted
  EXPECT_EQ(r->instrument_count(), 2U); // {AAA, BBB}
  const atx::u32 close = *r->field_index("close");
  // AAA is interned first (row 0) => inst 0; BBB => inst 1.
  EXPECT_DOUBLE_EQ(r->value(close, 0, 0), 1.0); // t=100, AAA
  EXPECT_DOUBLE_EQ(r->value(close, 0, 1), 2.0); // t=100, BBB
  EXPECT_DOUBLE_EQ(r->value(close, 1, 0), 3.0); // t=200, AAA
  EXPECT_FALSE(r->present(1, 1));               // t=200, BBB absent (the gap)
  static_cast<void>(std::remove(path.c_str()));
}

TEST(BuildFromLong, SingleTimestampAxisCollapsesToOneRow) {
  atx::tsdb::LongColumns cols;
  cols.field_names = {"close"};
  cols.times = {1000, 1000, 1000};
  cols.symbols = {"A", "B", "C"};
  cols.values = {{10.0, 20.0, 30.0}};
  const std::string path = (std::filesystem::temp_directory_path() / "axis1.seg").string();
  auto st = atx::tsdb::build_from_long(cols, path, 0);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  auto rdr = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(rdr.has_value());
  EXPECT_EQ(rdr->time_count(), 1u);
  EXPECT_EQ(rdr->instrument_count(), 3u);
  static_cast<void>(std::remove(path.c_str()));
}

TEST(BuildDatedSegments, ScalesI64FieldsAndPartitions) {
  namespace io = atx::core::io;
  namespace fs = std::filesystem;
  using atx::core::time::Timestamp;

  const fs::path root = fs::temp_directory_path() / "atx_dated_root";
  const fs::path segs = fs::temp_directory_path() / "atx_dated_segs";
  fs::remove_all(root);
  fs::remove_all(segs);

  // Two date partitions, prices as int64 1e-9 (2.0 and 3.0), volume int64 (1500).
  auto write_day = [&](const std::string &date, std::int64_t close_i64) {
    const std::vector<Timestamp> ts = {Timestamp::from_unix_nanos(100)};
    const std::vector<std::string> sym = {"AAA"};
    const std::vector<atx::i64> close = {close_i64};
    const std::vector<atx::i64> vol = {1500};
    const std::vector<io::WriteColumn> cols = {
        {"ts", std::span<const Timestamp>(ts)},
        {"symbol", std::span<const std::string>(sym)},
        {"close", std::span<const atx::i64>(close)},
        {"volume", std::span<const atx::i64>(vol)},
    };
    const fs::path p = root / ("date=" + date) / "data.parquet";
    ASSERT_TRUE(io::write_parquet(cols, p.string()).has_value());
  };
  write_day("2024-01-02", 2'000'000'000); // -> 2.0
  write_day("2024-01-03", 3'000'000'000); // -> 3.0

  const std::vector<std::pair<std::string, atx::f64>> scales = {{"close", 1e-9}, {"volume", 1.0}};
  ASSERT_TRUE(
      atx::tsdb::build_dated_segments(root.string(), segs.string(), "ts", "symbol", scales, 0)
          .has_value());

  auto r2 = atx::tsdb::SegmentReader::attach((segs / "2024-01-02.seg").string());
  ASSERT_TRUE(r2.has_value());
  const auto fc = r2->field_index("close");
  ASSERT_TRUE(fc.has_value());
  EXPECT_DOUBLE_EQ(r2->value(*fc, 0, 0), 2.0); // i64 2e9 * 1e-9
  const auto fv = r2->field_index("volume");
  ASSERT_TRUE(fv.has_value());
  EXPECT_DOUBLE_EQ(r2->value(*fv, 0, 0), 1500.0);

  auto r3 = atx::tsdb::SegmentReader::attach((segs / "2024-01-03.seg").string());
  ASSERT_TRUE(r3.has_value());
  EXPECT_DOUBLE_EQ(r3->value(*r3->field_index("close"), 0, 0), 3.0);
}
