#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>      // std::ios
#include <iterator> // std::istreambuf_iterator
#include <string>
#include <vector>

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/segment.hpp"
#include "atx/tsdb/segment_reader.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; the
// individual sub-headers (fileapi.h, winnls.h, …) are not self-contained on
// all SDK versions. All Win32 symbol warnings in this block are suppressed.
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#endif

namespace {

// Return a unique temp file path (not yet created).
std::string temp_path(const std::string &name) {
#if defined(_WIN32)
  // NOLINTBEGIN(misc-include-cleaner) — Win32 symbols come via the umbrella above.
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  // Use Win32 secure temp APIs to avoid MSVC CRT deprecation warnings on tmpnam.
  wchar_t tmp_dir[MAX_PATH + 1]{};
  GetTempPathW(MAX_PATH + 1, tmp_dir);
  wchar_t tmp_file[MAX_PATH + 1]{};
  GetTempFileNameW(tmp_dir, L"atx", 0, tmp_file);
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  // Append our test-name suffix so the path is human-identifiable.
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

TEST(Builder_WriteSealedFile_HeaderIsValid, WritesHeader) {
  atx::tsdb::SegmentBuilder b({"close", "volume"}, {"AAA", "BBB"}, {100, 200, 300});
  b.set(/*field=*/0, /*t=*/1, /*inst=*/0, 42.5);
  const std::string path = temp_path("b1");
  ASSERT_TRUE(b.write(path, /*created_at_nanos=*/123).has_value());

  std::ifstream in(path, std::ios::binary);
  atx::tsdb::SegmentHeader h{};
  // SAFETY: reading the raw on-disk POD header back for verification; the file
  // was just written by SegmentBuilder, which guarantees correct layout.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  in.read(reinterpret_cast<char *>(&h), sizeof(h));
  EXPECT_EQ(h.magic, atx::tsdb::kMagic);
  EXPECT_EQ(h.format_version, atx::tsdb::kFormatVersion);
  EXPECT_NE(h.flags & atx::tsdb::kFlagSealed, 0U);
  EXPECT_EQ(h.field_count, 2U);
  EXPECT_EQ(h.instrument_count, 2U);
  EXPECT_EQ(h.time_count, 3U);
  EXPECT_EQ(h.created_at_nanos, 123);
  static_cast<void>(std::remove(path.c_str()));
}

TEST(Builder_BuildTwice_ByteIdentical, Deterministic) {
  const auto build = [](const std::string &p) {
    atx::tsdb::SegmentBuilder b({"close"}, {"AAA"}, {10, 20});
    b.set(0, 0, 0, 1.0);
    b.set(0, 1, 0, 2.0);
    return b.write(p, /*created_at_nanos=*/0).has_value();
  };
  const std::string p1 = temp_path("det1");
  const std::string p2 = temp_path("det2");
  ASSERT_TRUE(build(p1));
  ASSERT_TRUE(build(p2));

  std::ifstream f1(p1, std::ios::binary);
  std::ifstream f2(p2, std::ios::binary);
  const std::string s1((std::istreambuf_iterator<char>(f1)), {});
  const std::string s2((std::istreambuf_iterator<char>(f2)), {});
  EXPECT_EQ(s1, s2); // identical inputs (incl. created_at) => identical bytes
  static_cast<void>(std::remove(p1.c_str()));
  static_cast<void>(std::remove(p2.c_str()));
}

TEST(Builder_V2Layout_AlignsBlocksAndRoundTrips, AlignedValues) {
  // 2 fields x 3 times x 5 instruments -> packed block = 3*5*8 = 120B,
  // padded stride = align_up(120,64) = 128B. The padding must not corrupt reads.
  atx::tsdb::SegmentBuilder b({"a", "b"}, {"S0", "S1", "S2", "S3", "S4"}, {10, 20, 30});
  b.set(/*field=*/0, /*t=*/2, /*inst=*/4, 7.5);  // a @ t2,inst4
  b.set(/*field=*/1, /*t=*/0, /*inst=*/0, 9.25); // b @ t0,inst0
  const std::string path = temp_path("v2lay");
  ASSERT_TRUE(b.write(path, /*created_at_nanos=*/0).has_value());

  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const atx::u32 fa = *r->field_index("a");
  const atx::u32 fb = *r->field_index("b");
  EXPECT_DOUBLE_EQ(r->value(fa, 2, 4), 7.5);
  EXPECT_DOUBLE_EQ(r->value(fb, 0, 0), 9.25);
  // block starts 64B-aligned and one padded stride (128B = 16 f64) apart.
  const auto a0 = reinterpret_cast<std::uintptr_t>(r->field_block_view(fa).data());
  const auto b0 = reinterpret_cast<std::uintptr_t>(r->field_block_view(fb).data());
  EXPECT_EQ(a0 % 64U, 0U);
  EXPECT_EQ(b0 % 64U, 0U);
  EXPECT_EQ(b0 - a0, 128U);
  static_cast<void>(std::remove(path.c_str()));
}
