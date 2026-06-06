// Bridge mechanics: attach_segment_panel produces a borrowed alpha::Panel over a
// sealed segment (window slice, field projection, universe policy, move safety).
// The VM(shm)==VM(owned)==oracle golden differential is added in Task 6.

#include <gtest/gtest.h>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/segment_panel.hpp"

#include "atx/tsdb/builder.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner)
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#endif

namespace {

std::string temp_path(const std::string &name) {
#if defined(_WIN32)
  // NOLINTBEGIN(misc-include-cleaner)
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

// 3 dates x 2 instruments, fields close/volume; close = date*10+inst, all present
// except (t2, inst1) which is left absent (present bit clear).
std::string make_seg(const std::string &name) {
  atx::tsdb::SegmentBuilder b({"close", "volume"}, {"AAA", "BBB"}, {100, 200, 300});
  for (atx::u64 t = 0; t < 3; ++t) {
    for (atx::u32 i = 0; i < 2; ++i) {
      if (t == 2 && i == 1) {
        continue; // absent cell
      }
      b.set(0, t, i, static_cast<atx::f64>(t * 10 + i)); // close
      b.set(1, t, i, 100.0);                             // volume
    }
  }
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}

using atx::engine::alpha::attach_segment_panel;
using atx::engine::alpha::TimeWindow;
using atx::engine::alpha::UniverseKind;
using atx::engine::alpha::UniversePolicy;

} // namespace

TEST(SegmentPanel_FullWindowAllFields_Mirrors, FullWindow) {
  const std::string path = make_seg("sp1");
  auto mp = attach_segment_panel(path);
  ASSERT_TRUE(mp.has_value()) << (mp ? "" : mp.error().to_string());
  const auto &panel = mp->panel();
  EXPECT_EQ(panel.dates(), 3U);
  EXPECT_EQ(panel.instruments(), 2U);
  EXPECT_EQ(panel.num_fields(), 2U);
  const auto cid = panel.field_id("close");
  ASSERT_TRUE(cid.has_value());
  EXPECT_DOUBLE_EQ(panel.field_cross_section(cid.value(), 1)[0], 10.0); // t1,inst0 = 10
  EXPECT_TRUE(panel.in_universe(0, 0));
  EXPECT_FALSE(panel.in_universe(2, 1)); // absent cell -> out of universe
  std::remove(path.c_str());
}

TEST(SegmentPanel_SubWindow_SlicesDates, SubWindow) {
  const std::string path = make_seg("sp2");
  auto mp = attach_segment_panel(path, TimeWindow{/*start*/ 200, /*end*/ 400}); // dates t1,t2
  ASSERT_TRUE(mp.has_value());
  const auto &panel = mp->panel();
  EXPECT_EQ(panel.dates(), 2U); // {200, 300}
  const auto cid = panel.field_id("close");
  ASSERT_TRUE(cid.has_value());
  EXPECT_DOUBLE_EQ(panel.field_cross_section(cid.value(), 0)[0], 10.0); // first row is t1
  std::remove(path.c_str());
}

TEST(SegmentPanel_FieldProjection_SelectsSubset, Projection) {
  const std::string path = make_seg("sp3");
  const std::vector<std::string> fields{"close"};
  auto mp = attach_segment_panel(path, TimeWindow{}, fields);
  ASSERT_TRUE(mp.has_value());
  EXPECT_EQ(mp->panel().num_fields(), 1U);
  EXPECT_TRUE(mp->panel().field_id("close").has_value());
  EXPECT_FALSE(mp->panel().field_id("volume").has_value());
  std::remove(path.c_str());
}

TEST(SegmentPanel_UnknownField_Errs, UnknownField) {
  const std::string path = make_seg("sp4");
  const std::vector<std::string> fields{"nope"};
  auto mp = attach_segment_panel(path, TimeWindow{}, fields);
  EXPECT_FALSE(mp.has_value());
  std::remove(path.c_str());
}

TEST(SegmentPanel_MovePreservesBorrow, MoveSafe) {
  const std::string path = make_seg("sp5");
  auto mp = attach_segment_panel(path);
  ASSERT_TRUE(mp.has_value());
  auto moved = std::move(mp.value()); // mapping address stable -> spans valid
  EXPECT_DOUBLE_EQ(moved.panel().field_cross_section(moved.panel().field_id("close").value(), 1)[1],
                   11.0); // t1, inst1 = 11
  std::remove(path.c_str());
}
