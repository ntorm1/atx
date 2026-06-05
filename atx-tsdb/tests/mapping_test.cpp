#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <utility>

#include "atx/tsdb/mapping.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; the
// individual sub-headers (fileapi.h, winnls.h, …) are not self-contained on
// all SDK versions. All Win32 symbol warnings in this block are suppressed.
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#endif

namespace {

// Write bytes to a unique temp file; return its path.
std::string write_temp(const std::string &name, const std::string &bytes) {
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
  wpath += wsuffix + L".bin";
  const std::string path(wpath.begin(), wpath.end());
  // NOLINTEND(misc-include-cleaner)
#else
  char buf[L_tmpnam]{};
  // NOLINTNEXTLINE(cert-msc50-cpp,cert-msc30-c)
  std::tmpnam(buf);
  const std::string path = std::string(buf) + name + ".bin";
#endif
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

} // namespace

TEST(Mapping_MapExistingFile_ExposesBytes, MapsContent) {
  const std::string path = write_temp("map_ok", "hello-mmap");
  auto m = atx::tsdb::Mapping::map_file_ro(path);
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());
  ASSERT_EQ(m->size(), 10U);
  // SAFETY: base() points at the mapped bytes; size() bounds the read.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(m->base()), m->size()), "hello-mmap");
  static_cast<void>(std::remove(path.c_str()));
}

TEST(Mapping_MissingFile_ReturnsErr, RejectsMissing) {
  auto m = atx::tsdb::Mapping::map_file_ro("definitely-not-a-real-path.bin");
  EXPECT_FALSE(m.has_value());
}

TEST(Mapping_MoveLeavesSourceEmpty_NoDoubleUnmap, MoveSafe) {
  const std::string path = write_temp("map_move", "abc");
  auto m = atx::tsdb::Mapping::map_file_ro(path);
  ASSERT_TRUE(m.has_value());
  const atx::tsdb::Mapping moved = std::move(*m);
  EXPECT_EQ(moved.size(), 3U);
  // SAFETY: moved-from Mapping is a valid empty object (base_ set to nullptr in
  // move ctor); reading base() is well-defined. dtor is a no-op on nullptr.
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
  EXPECT_EQ(m->base(), nullptr); // moved-from is empty; dtor must be a no-op
  static_cast<void>(std::remove(path.c_str()));
}
