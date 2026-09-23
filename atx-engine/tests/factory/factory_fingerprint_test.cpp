// atx::engine::factory — output fingerprint tests (L3, suite FactoryFingerprint).
//
// The fingerprint must collide for monotone-equivalent signals (the same rank-
// traded book) and must separate genuinely different alphas.

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/factory/fingerprint.hpp"

namespace atx_test_l3_search_fingerprint {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::factory::FingerprintIndex;
using atx::engine::factory::FingerprintScratch;
using atx::engine::factory::output_fingerprint;
using atx::engine::factory::probe_rows;
using atx::engine::factory::signal_fingerprint;

constexpr usize kDates = 120;
constexpr usize kInsts = 25;

[[nodiscard]] Panel noise_panel() {
  std::vector<f64> close(kDates * kInsts);
  std::vector<f64> vol(kDates * kInsts);
  std::uint64_t s = 0xF1A9ULL;
  auto next = [&]() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U);
  };
  std::vector<f64> px(kInsts, 100.0);
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kInsts; ++j) {
      px[j] *= 1.0 + 0.02 * (next() - 0.5);
      close[t * kInsts + j] = px[j];
      vol[t * kInsts + j] = 1.0e6 * (0.5 + next());
    }
  }
  auto r = Panel::create(kDates, kInsts, {"close", "volume"}, {close, vol}, {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

[[nodiscard]] u64 fp_of(std::string_view src, const Library &lib, const Panel &panel) {
  auto ast = parse_expr(src, lib);
  EXPECT_TRUE(ast.has_value()) << src;
  auto info = analyze(*ast);
  EXPECT_TRUE(info.has_value()) << src;
  auto prog = compile(*ast, *info);
  EXPECT_TRUE(prog.has_value()) << src;
  Engine engine{panel};
  auto out = engine.evaluate(*prog);
  EXPECT_TRUE(out.has_value()) << src;
  FingerprintScratch scratch;
  const auto rows = probe_rows(kDates, 32);
  return signal_fingerprint(out->alphas.front().values, kInsts, rows, scratch);
}

TEST(FactoryFingerprint, MonotoneEquivalentSignalsCollide) {
  Library lib{};
  const Panel panel = noise_panel();
  const u64 base = fp_of("rank(close)", lib, panel);
  EXPECT_EQ(base, fp_of("rank(close * 1.0001)", lib, panel));
  EXPECT_EQ(base, fp_of("close", lib, panel));
  EXPECT_EQ(base, fp_of("rank(close) * 3 + 1", lib, panel));
  EXPECT_EQ(base, fp_of("zscore(close)", lib, panel));
}

TEST(FactoryFingerprint, DistinctAlphasDoNotCollide) {
  Library lib{};
  const Panel panel = noise_panel();
  const std::vector<std::string> alphas = {
      "rank(close)",          "-rank(close)",       "rank(volume)",
      "rank(ts_mean(close, 5))", "rank(delta(close, 1))", "rank(close / volume)",
      "ts_rank(close, 10)",   "rank(ts_std(close, 10))"};
  std::set<u64> fps;
  for (const std::string &a : alphas) {
    fps.insert(fp_of(a, lib, panel));
  }
  EXPECT_EQ(fps.size(), alphas.size());
}

TEST(FactoryFingerprint, AverageTieRanksAndNaNsAreOrderIndependent) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  // A constant row fingerprints the same whatever the constant.
  EXPECT_EQ(output_fingerprint(std::vector<f64>{3.0, 3.0, 3.0, 3.0}),
            output_fingerprint(std::vector<f64>{-7.0, -7.0, -7.0, -7.0}));
  // NaN positions matter, monotone transforms do not.
  EXPECT_EQ(output_fingerprint(std::vector<f64>{1.0, nan, 5.0, 2.0}),
            output_fingerprint(std::vector<f64>{10.0, nan, 50.0, 20.0}));
  EXPECT_NE(output_fingerprint(std::vector<f64>{1.0, nan, 5.0, 2.0}),
            output_fingerprint(std::vector<f64>{nan, 1.0, 5.0, 2.0}));
  // A sign flip reverses the book.
  EXPECT_NE(output_fingerprint(std::vector<f64>{1.0, 2.0, 3.0}),
            output_fingerprint(std::vector<f64>{-1.0, -2.0, -3.0}));
  // +0.0 and -0.0 tie.
  EXPECT_EQ(output_fingerprint(std::vector<f64>{0.0, -0.0, 1.0}),
            output_fingerprint(std::vector<f64>{-0.0, 0.0, 1.0}));
  // Empty / all-NaN rows are well-defined.
  EXPECT_EQ(output_fingerprint(std::vector<f64>{nan, nan}),
            output_fingerprint(std::vector<f64>{nan, nan}));
  static_cast<void>(output_fingerprint(std::vector<f64>{}));
}

TEST(FactoryFingerprint, ProbeRowsAreEvenlySpacedAndBounded) {
  const auto r = probe_rows(100, 4);
  ASSERT_EQ(r.size(), 4U);
  EXPECT_EQ(r[0], 20U);
  EXPECT_EQ(r[3], 80U);
  EXPECT_EQ(probe_rows(3, 10).size(), 3U);
  EXPECT_TRUE(probe_rows(0, 10).empty());
}

TEST(FactoryFingerprint, IndexKeepsFirstOwner) {
  FingerprintIndex idx;
  EXPECT_TRUE(idx.insert(7, 100));
  EXPECT_FALSE(idx.insert(7, 200));
  ASSERT_NE(idx.find(7), nullptr);
  EXPECT_EQ(*idx.find(7), 100U);
  EXPECT_EQ(idx.find(8), nullptr);
}

} // namespace atx_test_l3_search_fingerprint
