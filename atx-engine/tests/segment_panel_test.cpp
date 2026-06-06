// Bridge mechanics: attach_segment_panel produces a borrowed alpha::Panel over a
// sealed segment (window slice, field projection, universe policy, move safety).
// The VM(shm)==VM(owned)==oracle golden differential is added in Task 6.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/segment_panel.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

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

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

void expect_signalsets_equal(const SignalSet &a, const SignalSet &b) {
  ASSERT_EQ(a.alphas.size(), b.alphas.size());
  ASSERT_EQ(a.dates, b.dates);
  ASSERT_EQ(a.instruments, b.instruments);
  for (atx::usize k = 0; k < a.alphas.size(); ++k) {
    ASSERT_EQ(a.alphas[k].values.size(), b.alphas[k].values.size());
    for (atx::usize i = 0; i < a.alphas[k].values.size(); ++i) {
      EXPECT_TRUE(same_cell(a.alphas[k].values[i], b.alphas[k].values[i]))
          << "alpha " << k << " cell " << i;
    }
  }
}

// Build a dense OHLCV segment AND the equivalent owned Panel from identical
// numbers. Field order matches make_panel convention so field_id resolves the
// same in both. close=date*10+inst+1, others derived; all cells present.
struct PairedData {
  std::string path;
  std::vector<std::vector<atx::f64>> cols; // owned-Panel columns (close,open,high,low,volume)
  atx::usize dates;
  atx::usize instruments;
};

[[nodiscard]] PairedData make_paired(const std::string &name, atx::usize dates,
                                     atx::usize instruments) {
  const std::vector<std::string> fnames{"close", "open", "high", "low", "volume"};
  std::vector<std::string> syms(instruments);
  for (atx::usize i = 0; i < instruments; ++i) {
    syms[i] = "S" + std::to_string(i);
  }
  std::vector<atx::i64> axis(dates);
  for (atx::usize d = 0; d < dates; ++d) {
    axis[d] = static_cast<atx::i64>((d + 1) * 100);
  }
  atx::tsdb::SegmentBuilder b(fnames, syms, axis);

  std::vector<std::vector<atx::f64>> cols(5, std::vector<atx::f64>(dates * instruments));
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      const auto idx = d * instruments + i;
      const atx::f64 close = static_cast<atx::f64>(d * 10 + i + 1);
      const atx::f64 open = close - 0.5;
      const atx::f64 high = close + 1.0;
      const atx::f64 low = close - 1.0;
      const atx::f64 vol = 1000.0 + static_cast<atx::f64>(i);
      cols[0][idx] = close;
      cols[1][idx] = open;
      cols[2][idx] = high;
      cols[3][idx] = low;
      cols[4][idx] = vol;
      b.set(0, d, static_cast<atx::u32>(i), close);
      b.set(1, d, static_cast<atx::u32>(i), open);
      b.set(2, d, static_cast<atx::u32>(i), high);
      b.set(3, d, static_cast<atx::u32>(i), low);
      b.set(4, d, static_cast<atx::u32>(i), vol);
    }
  }
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return PairedData{path, std::move(cols), dates, instruments};
}

} // namespace

TEST(SegmentPanel_Golden_VmShmEqualsVmOwnedEqualsOracle, Differential) {
  const PairedData pd = make_paired("golden", /*dates=*/8, /*instruments=*/5);

  // Owned Panel from the same numbers (universe = all-present).
  auto owned = Panel::create(pd.dates, pd.instruments, {"close", "open", "high", "low", "volume"},
                             pd.cols, {});
  ASSERT_TRUE(owned.has_value()) << (owned ? "" : owned.error().message());

  // Borrowed Panel straight off the segment.
  auto mp = attach_segment_panel(pd.path);
  ASSERT_TRUE(mp.has_value()) << (mp ? "" : mp.error().to_string());

  const std::vector<std::string_view> exprs{"close - open", "ts_mean(close, 3)", "rank(close)",
                                            "(high - low) / close"};
  for (const std::string_view src : exprs) {
    const Program prog = compile_ok(src);

    Engine eng_owned{owned.value()};
    auto so = eng_owned.evaluate(prog);
    ASSERT_TRUE(so.has_value()) << src << " owned: " << (so ? "" : so.error().message());

    Engine eng_shm{mp->panel()};
    auto ss = eng_shm.evaluate(prog);
    ASSERT_TRUE(ss.has_value()) << src << " shm: " << (ss ? "" : ss.error().message());

    auto ref = evaluate_reference(prog, mp->panel());
    ASSERT_TRUE(ref.has_value()) << src << " oracle: " << (ref ? "" : ref.error().message());

    expect_signalsets_equal(ss.value(), so.value());  // shm == owned
    expect_signalsets_equal(ss.value(), ref.value()); // shm == oracle
  }
  std::remove(pd.path.c_str());
}
