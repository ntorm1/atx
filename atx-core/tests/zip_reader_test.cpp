#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <miniz.h>

#include "atx/core/io/zip_reader.hpp"

namespace {
namespace fs = std::filesystem;
using atx::core::io::ZipEntryReader;

// Write a one-entry zip with known content to a temp path; return that path.
std::string make_zip(const std::string &entry_name, const std::string &content) {
  const fs::path p = fs::temp_directory_path() / "atx_zip_reader_test.zip";
  fs::remove(p);
  mz_zip_archive zip{};
  EXPECT_TRUE(mz_zip_writer_init_file(&zip, p.string().c_str(), 0));
  EXPECT_TRUE(mz_zip_writer_add_mem(&zip, entry_name.c_str(), content.data(), content.size(),
                                    MZ_BEST_SPEED));
  EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
  EXPECT_TRUE(mz_zip_writer_end(&zip));
  return p.string();
}

std::string drain(ZipEntryReader &r, atx::usize chunk) {
  std::string out;
  std::vector<char> buf(chunk);
  for (;;) {
    auto n = r.read(std::span<char>(buf.data(), buf.size()));
    EXPECT_TRUE(n.has_value()) << n.error().to_string();
    if (!n.has_value() || *n == 0) break;
    out.append(buf.data(), *n);
  }
  return out;
}
} // namespace

TEST(IoZipReader, StreamsEntryContentInSmallChunks) {
  // Content larger than the chunk so the streaming loop iterates several times.
  std::string content;
  for (int i = 0; i < 5000; ++i) content += "line" + std::to_string(i) + "\n";
  const std::string zip = make_zip("history.txt", content);

  auto r = ZipEntryReader::open(zip, "history");
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(r->uncompressed_size(), content.size());
  EXPECT_NE(r->entry_name().find("history"), std::string_view::npos);

  const std::string got = drain(*r, /*chunk=*/64);
  EXPECT_EQ(got, content);
}

TEST(IoZipReader, MissingEntrySubstringIsNotFound) {
  const std::string zip = make_zip("history.txt", "abc");
  auto r = ZipEntryReader::open(zip, "does-not-exist");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::NotFound);
}
