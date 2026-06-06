#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios> // std::ios
#include <span>
#include <string>

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

std::string make_segment(const std::string &name) {
  atx::tsdb::SegmentBuilder b({"close", "volume"}, {"AAA", "BBB"}, {100, 200, 300});
  b.set(/*field=*/0, /*t=*/0, /*inst=*/0, 10.0); // close AAA @ t0
  b.set(/*field=*/0, /*t=*/2, /*inst=*/1, 20.0); // close BBB @ t2
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}

} // namespace

TEST(Reader_AttachValid_ExposesGrid, ReadsCells) {
  const std::string path = make_segment("r1");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_EQ(r->time_count(), 3U);
  EXPECT_EQ(r->instrument_count(), 2U);
  EXPECT_EQ(r->symbol_name(1), "BBB");
  ASSERT_TRUE(r->field_index("close").has_value());
  const atx::u32 close = *r->field_index("close");
  EXPECT_TRUE(r->present(0, 0));
  EXPECT_DOUBLE_EQ(r->value(close, 0, 0), 10.0);
  EXPECT_FALSE(r->present(1, 0)); // unset cell
  static_cast<void>(std::remove(path.c_str()));
}

TEST(Reader_CutoffIndex_RespectsVisibility, NoLookAhead) {
  const std::string path = make_segment("r2");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r->cutoff_index(99).has_value()); // before first row
  EXPECT_EQ(r->cutoff_index(100), 0U);           // exact hit on t0
  EXPECT_EQ(r->cutoff_index(250), 1U);           // between t1 and t2
  EXPECT_EQ(r->cutoff_index(10'000), 2U);        // after last row
  static_cast<void>(std::remove(path.c_str()));
}

TEST(Reader_BadMagic_Rejected, ValidatesMagic) {
  const std::string path = make_segment("r3");
  { // corrupt the first byte of the magic
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0);
    const char junk = 'Z';
    f.write(&junk, 1);
  }
  EXPECT_FALSE(atx::tsdb::SegmentReader::attach(path).has_value());
  static_cast<void>(std::remove(path.c_str()));
}

TEST(Reader_TwoIndependentMappings_SameContent, SharedReadProxy) {
  // In-process proxy for the cross-process value prop: two independent mappings
  // of the same file (possibly at different addresses) must agree on content.
  const std::string path = make_segment("r4");
  auto a = atx::tsdb::SegmentReader::attach(path);
  auto b = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_EQ(a->content_hash(), b->content_hash());
  EXPECT_DOUBLE_EQ(a->value(*a->field_index("close"), 2, 1),
                   b->value(*b->field_index("close"), 2, 1));
  static_cast<void>(std::remove(path.c_str()));
}

TEST(Reader_FieldName_RoundTrips, NamesByIndex) {
  const std::string path = make_segment("fn1");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_EQ(r->field_name(0), "close");
  EXPECT_EQ(r->field_name(1), "volume");
  std::remove(path.c_str());
}

TEST(Reader_FieldBlockView_ExposesWholeBlock, BlockView) {
  const std::string path = make_segment("fb1");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value());
  const atx::u32 close = *r->field_index("close");
  const std::span<const atx::f64> block = r->field_block_view(close);
  ASSERT_EQ(block.size(), r->time_count() * r->instrument_count()); // T*N = 3*2
  // date-major [t][inst]: close AAA@t0 (inst 0, t 0) == 10.0
  EXPECT_DOUBLE_EQ(block[0 * r->instrument_count() + 0], 10.0);
  // close BBB@t2 (inst 1, t 2)
  EXPECT_DOUBLE_EQ(block[2 * r->instrument_count() + 1], 20.0);
  std::remove(path.c_str());
}
