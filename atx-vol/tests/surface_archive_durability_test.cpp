#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "atx/vol/american.hpp"
#include "atx/vol/detail/archive_util.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

// WS-C C3: durable atomic publish (SE-P2-1 fsync-before-rename, SE-P2-2 Windows
// rename-under-reader retry + preserve-temp-on-failure). fsync PRESENCE is
// code-review-verifiable and documented as a crash-consistency note in
// docs/atxvsa2-format.md (no automated power-loss harness is possible in a unit
// test). The reader-held retry / temp-preservation behavior IS automatable on
// Windows via a FILE_SHARE_READ holder (the exact mode MSVC std::ifstream uses),
// which is what these tests exercise.

namespace {

using atx::vol::AlOpts;
using atx::vol::AmericanMethod;
using atx::vol::CurveSurface;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::PricedSurface;
using atx::vol::PricingContext;
using atx::vol::SliceContext;
using atx::vol::SurfaceArchiveItem;
using atx::vol::SurfaceArchiveV2;
using atx::vol::write_surface_archive_v2_file;

constexpr double kS = 100.0;
constexpr double kR = 0.043;

[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = kS;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 250, 7});
  }
  PricingContext pc;
  pc.S = kS;
  pc.r = kR;
  pc.now_ts_ns = 1700000000000000000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = AlOpts{};
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

std::filesystem::path test_dir(std::string_view name) {
  auto p = std::filesystem::temp_directory_path() / ("atx_ws_c_durable_" + std::string(name));
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return p;
}

[[nodiscard]] std::vector<std::filesystem::path>
publish_temps_for(const std::filesystem::path &dst) {
  const std::string prefix = dst.filename().string() + ".tmp.";
  std::vector<std::filesystem::path> found;
  for (const auto &entry : std::filesystem::directory_iterator(dst.parent_path())) {
    if (entry.path().filename().string().starts_with(prefix)) {
      found.push_back(entry.path());
    }
  }
  return found;
}

} // namespace

TEST(DurablePublish, ReservesUniqueExclusiveTempsBesideDestination) {
  const auto dir = test_dir("unique");
  const auto dst = dir / "live.bin";
  std::set<std::filesystem::path> unique;
  for (int i = 0; i < 32; ++i) {
    auto reserved = atx::vol::detail::reserve_unique_publish_temp_file(dst.string());
    ASSERT_TRUE(reserved.has_value())
        << (reserved.has_value() ? "" : reserved.error().to_string());
    const std::filesystem::path temp{*reserved};
    EXPECT_EQ(temp.parent_path(), dst.parent_path());
    EXPECT_NE(temp, dst);
    EXPECT_TRUE(std::filesystem::exists(temp));
    EXPECT_TRUE(unique.insert(temp).second);
  }
  EXPECT_EQ(publish_temps_for(dst).size(), unique.size());
  std::filesystem::remove_all(dir);
}

TEST(DurablePublish, SerializesConcurrentSameDestinationPublications) {
  const auto dir = test_dir("concurrent");
  const auto dst = dir / "live.bin";
  constexpr std::size_t kWriters = 8u;
  std::array<std::filesystem::path, kWriters> temps;
  std::array<std::string, kWriters> payloads;
  std::array<bool, kWriters> published{};

  for (std::size_t i = 0; i < kWriters; ++i) {
    auto reserved = atx::vol::detail::reserve_unique_publish_temp_file(dst.string());
    ASSERT_TRUE(reserved.has_value());
    temps[i] = std::filesystem::path{*reserved};
    payloads[i] = "writer-" + std::to_string(i) + ":" + std::string(4096u, char('A' + i));
    std::ofstream out(temps[i], std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out.write(payloads[i].data(), static_cast<std::streamsize>(payloads[i].size()));
    ASSERT_TRUE(out.good());
  }

  std::barrier rendezvous{static_cast<std::ptrdiff_t>(kWriters)};
  std::array<std::thread, kWriters> writers;
  for (std::size_t i = 0; i < kWriters; ++i) {
    writers[i] = std::thread([&, i] {
      rendezvous.arrive_and_wait();
      published[i] =
          atx::vol::detail::flush_and_publish_file(temps[i].string(), dst.string()).has_value();
    });
  }
  for (auto &writer : writers) {
    writer.join();
  }
  for (bool ok : published) {
    EXPECT_TRUE(ok);
  }

  std::ifstream in(dst, std::ios::binary);
  ASSERT_TRUE(in.good());
  const std::string observed((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  EXPECT_NE(std::find(payloads.begin(), payloads.end(), observed), payloads.end());
  EXPECT_TRUE(publish_temps_for(dst).empty());
  in.close();
  std::filesystem::remove_all(dir);
}

TEST(DurablePublish, ReservationFailureLeavesNoTemp) {
  const auto dir = test_dir("reserve_failure");
  const auto blocked_parent = dir / "not_a_directory";
  {
    std::ofstream out(blocked_parent, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << "sentinel";
  }
  auto reserved =
      atx::vol::detail::reserve_unique_publish_temp_file((blocked_parent / "live.bin").string());
  EXPECT_FALSE(reserved.has_value());
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir),
                          std::filesystem::directory_iterator{}),
            1);
  std::filesystem::remove_all(dir);
}

#if defined(_WIN32)

// SE-P2-2: a republish whose destination is held open FILE_SHARE_READ (no
// FILE_SHARE_DELETE) throughout cannot rename — but the freshly written temp must
// be PRESERVED (recoverable) and the prior good destination left intact. Before
// C3 the writer deleted the temp on the first rename failure (data loss).
TEST(SurfaceArchiveV2Durable, PreservesTempWhenDestinationHeldThroughout) {
  const auto dir = test_dir("preserve");
  const auto dst = dir / "part.atxvsa2";
  const PricedSurface s = make_essvi(1, 4);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &s, std::nullopt}};

  // First publish (no reader) establishes the destination.
  ASSERT_TRUE(write_surface_archive_v2_file(dst.string(), items).has_value());
  const auto first_size = std::filesystem::file_size(dst);

  // Hold the destination open FILE_SHARE_READ only, for the entire republish.
  HANDLE holder = ::CreateFileW(dst.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(holder, INVALID_HANDLE_VALUE);

  const auto republish = write_surface_archive_v2_file(dst.string(), items);
  EXPECT_FALSE(republish.has_value()); // rename can never succeed while dst is held

  // C3 fix: the temp is preserved on final failure (RED today: it was deleted).
  const auto preserved = publish_temps_for(dst);
  ASSERT_EQ(preserved.size(), 1u);
  EXPECT_GT(std::filesystem::file_size(preserved.front()), 0u);

  ::CloseHandle(holder);
  // The prior good destination was never replaced.
  EXPECT_EQ(std::filesystem::file_size(dst), first_size);
  EXPECT_TRUE(SurfaceArchiveV2::open_file(dst.string()).has_value());

  std::filesystem::remove_all(dir);
}

// SE-P2-2: a republish whose destination is held only briefly succeeds via the
// bounded retry + backoff once the reader releases — where the pre-C3 single-shot
// rename failed on the first (and only) attempt.
TEST(SurfaceArchiveV2Durable, RetriesUntilReaderReleases) {
  const auto dir = test_dir("retry");
  const auto dst = dir / "part.atxvsa2";
  const PricedSurface s = make_essvi(2, 5);
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &s, std::nullopt}};
  ASSERT_TRUE(write_surface_archive_v2_file(dst.string(), items).has_value());

  std::atomic<bool> holding{false};
  std::atomic<bool> open_ok{false};
  std::thread reader([&] {
    HANDLE h = ::CreateFileW(dst.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    open_ok.store(h != INVALID_HANDLE_VALUE);
    holding.store(true);
    if (h != INVALID_HANDLE_VALUE) {
      // Hold well under the writer's ~635 ms cumulative retry budget, then release.
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      ::CloseHandle(h);
    }
  });
  while (!holding.load()) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(open_ok.load());

  // Republish while the reader still holds: the first rename attempt fails, but the
  // backed-off retries outlast the ~40 ms hold and the publish succeeds.
  const auto republish = write_surface_archive_v2_file(dst.string(), items);
  reader.join();
  EXPECT_TRUE(republish.has_value())
      << (republish.has_value() ? "" : republish.error().to_string());

  std::filesystem::remove_all(dir);
}

#endif // _WIN32
