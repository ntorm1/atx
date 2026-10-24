// persist_alloc_test.cpp — PersistFile.NoAllocationAfterOpen (design wave3-persist
// D6 / T8): once `open` returns, every accessor of the container is allocation-free.
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a
// replaced global operator new), linked ONLY into atx-vol-persist-alloc-tests, never
// the monolithic atx-vol-tests or a slice (which must keep the default operator new).
// Registered under the "persist/" prefix so the lane's anchored
// -R "^persist/PersistFile\." runs it.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/persist/persist_file.hpp"
#include "support/alloc_counter.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file

constexpr std::uint64_t kFp = 0x0A110CA7Eull;

[[nodiscard]] fs::path fresh_dir() {
#if defined(_WIN32)
  const std::string pid = std::to_string(_getpid());
#else
  const std::string pid = std::to_string(getpid());
#endif
  const fs::path dir = fs::temp_directory_path() / ("atx-persist-alloc-" + pid);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

} // namespace

TEST(PersistFile, NoAllocationAfterOpen) {
  std::vector<double> plain(4096, 2.5);
  std::vector<double> packed(4096);
  for (std::size_t i = 0; i < packed.size(); ++i) packed[i] = static_cast<double>(i % 17);
  std::vector<std::uint8_t> flags(4096, 1);
  const PersistSectionInput inputs[] = {
      {16, 1, 0, PersistDType::F64, 8, true, {}, {}, 3, std::as_bytes(std::span{plain})},
      {17, 1, 0, PersistDType::F64, 8, true, PersistCodec::Zstd, PersistFilter::ByteShuffle, 3,
       std::as_bytes(std::span{packed})},
      {18, 1, 0, PersistDType::U8, 1, false, {}, {}, 3, std::as_bytes(std::span{flags})},
  };
  PersistFileInfo info;
  info.kind = PersistKind::RiskRun;
  info.schema_fingerprint = kFp;
  auto image = build_persist_file(inputs, info);
  ASSERT_TRUE(image.has_value());
  const fs::path path = fresh_dir() / "alloc.atxrr";
  ASSERT_TRUE(publish_persist_file(path.string(), *image).has_value());

  static constexpr PersistSectionSpec kSchema[] = {
      {16, 1, PersistDType::F64, 8, "plain", {}, true},
      {17, 1, PersistDType::F64, 8, "packed", {}, true},
      {18, 1, PersistDType::U8, 1, "flags", {}, false},
  };
  for (const bool mapped : {true, false}) {
    for (const PersistVerify v : {PersistVerify::Framing, PersistVerify::Lazy, PersistVerify::Full}) {
      PersistOpenOptions o;
      o.mapped = mapped;
      o.verify = v;
      auto f = PersistFile::open(path.string(), PersistKind::RiskRun, kFp, o);
      ASSERT_TRUE(f.has_value());
      double sink = 0.0;
      const std::uint64_t n = atx::vol::testkit::count_allocations([&] {
        sink += static_cast<double>(f->header().file_size + f->footer().file_size);
        sink += static_cast<double>(f->sections().size() + f->bytes().size());
        for (const std::uint32_t t : {16u, 17u, 18u, 99u}) {
          auto s = f->section(t);
          sink += s ? static_cast<double>(s->size()) : 1.0;
          sink += f->entry(t) != nullptr ? 1.0 : 0.0;
        }
        auto d = f->typed<double>(17);
        sink += d ? (*d)[5] : 0.0;
        auto u = f->typed<std::uint8_t>(18);
        sink += u ? (*u)[0] : 0.0;
        sink += f->check_schema(kSchema).has_value() ? 1.0 : 0.0;
        sink += f->verify_all().has_value() ? 1.0 : 0.0;
        const std::shared_ptr<const void> keep = f->keepalive();
        sink += keep != nullptr ? 1.0 : 0.0;
      });
      EXPECT_EQ(n, 0u) << "mapped=" << mapped << " verify=" << static_cast<int>(v);
      EXPECT_GT(sink, 0.0);
    }
  }
}
