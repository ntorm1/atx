#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

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
