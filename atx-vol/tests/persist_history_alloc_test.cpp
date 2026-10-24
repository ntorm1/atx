// persist_history_alloc_test.cpp — the allocation contracts of the lane-P2 history
// formats (design wave3-persist D12 / D13 / T8), counted with tests/support/
// alloc_counter.* in the focused atx-vol-persist-alloc-tests exe (it replaces global
// operator new, so it must never link into atx-vol-tests or a slice). Registered under
// the "persist/" prefix so the lane's anchored
// -R "^persist/(PersistSurfaceTimeline|PersistRiskRun)\." runs it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/surface_timeline.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "support/alloc_counter.hpp"
#include "support/risk_synthetic_market.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace rt = atx::vol::risk_test;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file

[[nodiscard]] fs::path fresh_dir(const std::string &tag) {
#if defined(_WIN32)
  const std::string pid = std::to_string(_getpid());
#else
  const std::string pid = std::to_string(getpid());
#endif
  const fs::path dir = fs::temp_directory_path() / ("atx-persist-history-alloc-" + tag + "-" + pid);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

} // namespace

TEST(PersistSurfaceTimeline, NoAllocationOnRangeLookup) {
  const fs::path dir = fresh_dir("range");
  std::vector<TimelineSource> sources;
  std::vector<std::int64_t> instants;
  for (int k = 0; k < 6; ++k) {
    rt::StateMove m;
    m.time_units = 11 * static_cast<std::int64_t>(k);
    m.vol_level = 0.001 * static_cast<double>(k);
    std::vector<PricedSurface> surfaces;
    for (std::size_t ui = 0; ui < 3; ++ui) surfaces.push_back(rt::make_surface(ui, m));
    const SurfaceArchiveItem items[] = {{"AAA", &surfaces[0], std::nullopt},
                                        {"BBB", &surfaces[1], std::nullopt},
                                        {"CCC", &surfaces[2], std::nullopt}};
    const std::string path = (dir / ("p" + std::to_string(k) + ".atxvsa")).string();
    ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    sources.push_back(TimelineSource{path, 0});
    instants.push_back(rt::instant_ns(m.time_units));
  }
  auto image = encode_surface_timeline(sources, 0);
  ASSERT_TRUE(image.has_value());
  const fs::path p = dir / "t.atxstl";
  ASSERT_TRUE(publish_persist_file(p.string(), *image).has_value());

  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  for (const bool mapped : {true, false}) {
    for (const PersistVerify v : {PersistVerify::Framing, PersistVerify::Lazy, PersistVerify::Full}) {
      PersistOpenOptions o;
      o.mapped = mapped;
      o.verify = v;
      auto tl = SurfaceTimeline::open(p.string(), o);
      ASSERT_TRUE(tl.has_value());
      std::size_t sink = 0;
      const std::uint64_t n = atx::vol::testkit::count_allocations([&] {
        for (const std::uint32_t uid : tl->uids()) {
          sink += tl->range(uid, kMin, kMax).size();
          for (std::size_t k = 0; k < instants.size(); ++k) {
            sink += tl->range(uid, instants[k], instants[k]).size();
            sink += tl->range(uid, instants[k] + 1, kMax).size();
          }
        }
        sink += tl->range(0, kMin, kMax).size();
        sink += tl->range(77, kMin, kMax).size();
        sink += tl->range(1, kMax, kMin).size();
      });
      EXPECT_EQ(n, 0u) << "mapped=" << mapped << " verify=" << static_cast<int>(v);
      EXPECT_EQ(sink, 3u * (6u + 6u + 15u));
    }
  }
}
