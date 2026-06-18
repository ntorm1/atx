// atx::engine::alpha — cross-sectional VM kernel differential tests (P3-7).
//
// The VM's cross-sectional opcodes (vm.hpp via cs_ops.hpp) are the PRODUCTION
// path; oracle.hpp is the slow, obviously-correct reference. These tests run
// BOTH on identical inputs and assert bit-for-bit agreement (NaN==NaN treated
// as equal) for every Cs* opcode — the differential is exactly what catches a
// kernel that drifts from the pinned semantic contract (ordinal vs average-rank
// ties, sample vs population std, the group-mean edge cases). Plus:
//   * hand-computed known-value checks (exact ranks, mean≈0 zscore, Σ|scale|≈a,
//     per-group demean sums to ~0);
//   * NaN / out-of-universe exclusion from the cross-section;
//   * boundaries (1-instrument singleton -> 0.5, a group with one member,
//     all-NaN date).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/cs_ops.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_cs_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// A process-lifetime Library so any borrowed OpSig stays valid across tests.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN, or exactly value-equal (covers ±inf, ±0).
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Compile a bare expression all the way to a Program (parse -> analyze ->
// compile). On any failure the caller's EXPECT surfaces the message.
[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// Build a Panel with the OHLCV fields plus an `IndClass.sector` Group field
// (integer labels widened to f64). Field order: close/open/high/low/volume,
// then IndClass.sector. An empty universe means all cells in-universe.
[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments,
                               std::vector<std::vector<atx::f64>> cols,
                               std::vector<std::uint8_t> universe = {}) {
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  auto p =
      Panel::create(dates, instruments, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Fill five OHLCV columns + a sector classifier from a fixed-seed RNG. Prices in
// [1,100], volume positive, sector in {0,1,2}. Deterministic.
[[nodiscard]] std::vector<std::vector<atx::f64>> random_cols(atx::usize cells, std::uint64_t seed,
                                                             int num_sectors = 3) {
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{1.0, 100.0};
  std::uniform_real_distribution<atx::f64> vol{0.0, 1.0e6};
  std::uniform_int_distribution<int> sector{0, num_sectors - 1};
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = price(rng); // close
    cols[1][i] = price(rng); // open
    cols[2][i] = price(rng); // high
    cols[3][i] = price(rng); // low
    cols[4][i] = vol(rng);   // volume
    cols[5][i] = static_cast<atx::f64>(sector(rng));
  }
  return cols;
}

// The core differential assertion: VM == oracle, cell by cell.
void expect_vm_matches_oracle(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto vm = engine.evaluate(prog);
  ASSERT_TRUE(vm.has_value()) << "VM: " << (vm ? "" : vm.error().message());
  auto ref = evaluate_reference(prog, panel);
  ASSERT_TRUE(ref.has_value()) << "oracle: " << (ref ? "" : ref.error().message());

  const SignalSet &v = vm.value();
  const SignalSet &r = ref.value();
  ASSERT_EQ(v.alphas.size(), r.alphas.size());
  ASSERT_EQ(v.dates, r.dates);
  ASSERT_EQ(v.instruments, r.instruments);
  for (atx::usize a = 0; a < v.alphas.size(); ++a) {
    ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
    for (atx::usize i = 0; i < v.alphas[a].values.size(); ++i) {
      const atx::f64 vc = v.alphas[a].values[i];
      const atx::f64 rc = r.alphas[a].values[i];
      EXPECT_TRUE(same_cell(vc, rc)) << "expr '" << expr << "' alpha " << a << " cell " << i
                                     << ": VM=" << vc << " oracle=" << rc;
    }
  }
}

// Run the VM and return the single root's date-major values (for known-value
// asserts). The caller's ASSERT surfaces any evaluate() error via an empty vec.
[[nodiscard]] std::vector<atx::f64> vm_values(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  if (!out.has_value() || out.value().alphas.empty()) {
    return {};
  }
  return out.value().alphas[0].values;
}

// ===========================================================================
//  Differential — every Cs* opcode on a seeded-random multi-date panel.
// ===========================================================================

TEST(AlphaCs_Differential, EveryCrossSectionalOp_RandomPanel_MatchesOracle) {
  const atx::usize dates = 11;
  const atx::usize instruments = 9;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0xC5A1FAULL));

  const std::vector<std::string_view> exprs = {
      "rank(close)",
      "zscore(close)",
      "scale(close, 2)",
      "indneutralize(close, IndClass.sector)",
      "group_neutralize(close, IndClass.sector)",
      "group_rank(close, IndClass.sector)",
      "group_zscore(close, IndClass.sector)",
  };
  for (const std::string_view e : exprs) {
    expect_vm_matches_oracle(e, panel);
  }
}

// indneutralize and group_neutralize are pinned IDENTICAL (both per-group
// demean); the differential must hold for the alias too — checked on a panel
// with more sectors so groups are small and the per-group mean varies.
TEST(AlphaCs_Differential, NeutralizeAliasAndManySectors_MatchesOracle) {
  const atx::usize dates = 7;
  const atx::usize instruments = 13;
  const Panel panel = make_panel(dates, instruments,
                                 random_cols(dates * instruments, 0x5EC701ULL, /*num_sectors=*/5));
  expect_vm_matches_oracle("indneutralize(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_neutralize(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_rank(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_zscore(close, IndClass.sector)", panel);
}

// Large panel with many small groups — stresses the fast path's group hash
// table (growth + linear probing across collisions) at a scale where the old
// O(n²) rescans are no longer indistinguishable from O(n). Every grouped op
// must still match the O(n²) oracle bit-for-bit.
TEST(AlphaCs_Differential, LargeManyGroups_MatchesOracle) {
  const atx::usize dates = 5;
  const atx::usize instruments = 96;
  const Panel panel = make_panel(
      dates, instruments, random_cols(dates * instruments, 0xB1664EULL, /*num_sectors=*/17));
  expect_vm_matches_oracle("indneutralize(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_neutralize(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_rank(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_zscore(close, IndClass.sector)", panel);
}

// Signed-zero group labels: -0.0 and +0.0 compare EQUAL under the oracle's
// `g[j] == g[i]`, so they form ONE group. The fast path keys groups on the
// label's bit pattern after `+ 0.0` (which folds -0.0 -> +0.0) precisely so it
// partitions identically. A regression that hashed raw bits would split the
// two zeros and diverge; this differential pins that it does not.
TEST(AlphaCs_Group, SignedZeroLabels_SameGroupAsOracle) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(dates * instruments, 0xDULL);
  cols[0] = {10.0, 20.0, 30.0, 40.0, 50.0};
  cols[5] = {-0.0, 0.0, -0.0, 1.0, 1.0}; // {i0,i1,i2} one group despite ±0.0 bits
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  expect_vm_matches_oracle("indneutralize(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_rank(close, IndClass.sector)", panel);
  expect_vm_matches_oracle("group_zscore(close, IndClass.sector)", panel);

  // And concretely: the ±0.0 cells share group 0's mean (10+20+30)/3 = 20.
  const std::vector<atx::f64> gd = vm_values("group_neutralize(close, IndClass.sector)", panel);
  ASSERT_EQ(gd.size(), instruments);
  EXPECT_DOUBLE_EQ(gd[0], 10.0 - 20.0);
  EXPECT_DOUBLE_EQ(gd[1], 20.0 - 20.0);
  EXPECT_DOUBLE_EQ(gd[2], 30.0 - 20.0);
}

// A nested program where a Cs op feeds an element-wise op and another Cs op:
// proves the cross-sectional result composes correctly inside a larger DAG.
TEST(AlphaCs_Differential, NestedComposition_MatchesOracle) {
  const atx::usize dates = 6;
  const atx::usize instruments = 8;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0xC0FFEEULL));
  expect_vm_matches_oracle("rank(close - open) * scale(volume, 1)", panel);
  expect_vm_matches_oracle("zscore(rank(close))", panel);
}

// ===========================================================================
//  Known-value — hand-computed on a small panel.
// ===========================================================================

// rank on a single date with distinct values -> exact ordinal percentiles.
// One date, 5 instruments, close = {30, 10, 50, 20, 40}. Sorted ascending the
// instrument order is 1,3,0,4,2 -> percentiles 0,.25,.5,.75,1.
TEST(AlphaCs_Rank, DistinctValues_ExactPercentiles) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(dates * instruments, 0x1ULL);
  cols[0] = {30.0, 10.0, 50.0, 20.0, 40.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> v = vm_values("rank(close)", panel);
  ASSERT_EQ(v.size(), instruments);
  EXPECT_DOUBLE_EQ(v[0], 0.50); // 30 -> middle
  EXPECT_DOUBLE_EQ(v[1], 0.00); // 10 -> smallest
  EXPECT_DOUBLE_EQ(v[2], 1.00); // 50 -> largest
  EXPECT_DOUBLE_EQ(v[3], 0.25); // 20
  EXPECT_DOUBLE_EQ(v[4], 0.75); // 40
}

// rank on an all-equal date (tie storm) -> deterministic ordinal output: the
// stable sort keeps instrument order, so ranks are 0, .25, .5, .75, 1.
TEST(AlphaCs_Rank, AllEqual_DeterministicOrdinal) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(dates * instruments, 0x2ULL);
  cols[0] = {7.0, 7.0, 7.0, 7.0, 7.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> v = vm_values("rank(close)", panel);
  ASSERT_EQ(v.size(), instruments);
  EXPECT_DOUBLE_EQ(v[0], 0.00);
  EXPECT_DOUBLE_EQ(v[1], 0.25);
  EXPECT_DOUBLE_EQ(v[2], 0.50);
  EXPECT_DOUBLE_EQ(v[3], 0.75);
  EXPECT_DOUBLE_EQ(v[4], 1.00);
}

// zscore over a known set -> the cross-sectional mean of the output is ~0 and
// the sample-std normalization holds: for {1,2,3,4,5}, mean=3, sample-sd=√2.5.
TEST(AlphaCs_Zscore, KnownSet_MeanZeroSampleStd) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(dates * instruments, 0x3ULL);
  cols[0] = {1.0, 2.0, 3.0, 4.0, 5.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> v = vm_values("zscore(close)", panel);
  ASSERT_EQ(v.size(), instruments);
  const atx::f64 sd = std::sqrt(2.5); // ddof=1 over {1..5}
  atx::f64 sum = 0.0;
  for (const atx::f64 z : v) {
    sum += z;
  }
  EXPECT_NEAR(sum, 0.0, 1e-12); // demeaned -> sums to 0
  EXPECT_DOUBLE_EQ(v[2], 0.0);  // x==mean -> 0
  EXPECT_DOUBLE_EQ(v[0], (1.0 - 3.0) / sd);
  EXPECT_DOUBLE_EQ(v[4], (5.0 - 3.0) / sd);
}

// scale(x, 1) -> the sum of absolute values over the valid set is ~1.
TEST(AlphaCs_Scale, UnitNorm_SumAbsIsOne) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  auto cols = random_cols(dates * instruments, 0x4ULL);
  cols[0] = {1.0, -3.0, 2.0, -4.0}; // Σ|x| = 10
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> v = vm_values("scale(close, 1)", panel);
  ASSERT_EQ(v.size(), instruments);
  atx::f64 l1 = 0.0;
  for (const atx::f64 c : v) {
    l1 += std::fabs(c);
  }
  EXPECT_NEAR(l1, 1.0, 1e-12);
  EXPECT_DOUBLE_EQ(v[0], 0.1);  // 1/10
  EXPECT_DOUBLE_EQ(v[3], -0.4); // -4/10
}

// indneutralize -> each group's valid members sum to ~0 after demean. Two
// sectors: {0,0,1,1}, close {10,20,100,140}. Group 0 mean=15, group 1 mean=120.
TEST(AlphaCs_IndNeutralize, PerGroupDemean_GroupSumsToZero) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  auto cols = random_cols(dates * instruments, 0x5ULL);
  cols[0] = {10.0, 20.0, 100.0, 140.0};
  cols[5] = {0.0, 0.0, 1.0, 1.0}; // IndClass.sector
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> v = vm_values("indneutralize(close, IndClass.sector)", panel);
  ASSERT_EQ(v.size(), instruments);
  EXPECT_DOUBLE_EQ(v[0], 10.0 - 15.0);   // -5
  EXPECT_DOUBLE_EQ(v[1], 20.0 - 15.0);   // +5
  EXPECT_DOUBLE_EQ(v[2], 100.0 - 120.0); // -20
  EXPECT_DOUBLE_EQ(v[3], 140.0 - 120.0); // +20
  EXPECT_NEAR(v[0] + v[1], 0.0, 1e-12);  // group 0 demeaned -> 0
  EXPECT_NEAR(v[2] + v[3], 0.0, 1e-12);  // group 1 demeaned -> 0
}

// ===========================================================================
//  NaN / universe — out-of-universe cells are excluded from the cross-section.
// ===========================================================================

// A date with some out-of-universe (NaN) instruments -> excluded from the
// cross-section: those cells are NaN, the rest ranked among the valid only.
TEST(AlphaCs_Rank, OutOfUniverse_ExcludedAndNaN) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(dates * instruments, 0x6ULL);
  cols[0] = {30.0, 10.0, 50.0, 20.0, 40.0};
  std::vector<std::uint8_t> universe(instruments, std::uint8_t{1});
  universe[2] = 0; // knock out the largest (50) -> valid set {30,10,20,40}
  const Panel panel = make_panel(dates, instruments, std::move(cols), std::move(universe));

  const std::vector<atx::f64> v = vm_values("rank(close)", panel);
  ASSERT_EQ(v.size(), instruments);
  EXPECT_TRUE(std::isnan(v[2])); // out-of-universe -> NaN
  // Valid set sorted ascending: 10(i1),20(i3),30(i0),40(i4) over n=4.
  EXPECT_DOUBLE_EQ(v[1], 0.0 / 3.0);
  EXPECT_DOUBLE_EQ(v[3], 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(v[0], 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(v[4], 3.0 / 3.0);
  // Also confirm bit-identical to the oracle.
  expect_vm_matches_oracle("rank(close)", panel);
}

// A group with a single valid member -> that member's group-rank is the
// singleton 0.5; its group-zscore is NaN (sample-std needs 2+); group-demean
// is 0 (x - x). Sectors {0,1,1}, all valid, group 0 has one member.
TEST(AlphaCs_Group, SingleMember_SingletonRankNaNZscore) {
  const atx::usize dates = 1;
  const atx::usize instruments = 3;
  auto cols = random_cols(dates * instruments, 0x7ULL);
  cols[0] = {42.0, 10.0, 20.0};
  cols[5] = {0.0, 1.0, 1.0}; // group 0 = {i0}, group 1 = {i1,i2}
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> gr = vm_values("group_rank(close, IndClass.sector)", panel);
  const std::vector<atx::f64> gz = vm_values("group_zscore(close, IndClass.sector)", panel);
  const std::vector<atx::f64> gd = vm_values("group_neutralize(close, IndClass.sector)", panel);
  ASSERT_EQ(gr.size(), instruments);
  EXPECT_DOUBLE_EQ(gr[0], 0.5);   // singleton group -> 0.5
  EXPECT_TRUE(std::isnan(gz[0])); // singleton group -> sample-std NaN
  EXPECT_DOUBLE_EQ(gd[0], 0.0);   // x - x == 0
  // Group 1 ({10,20}) ranks: 0 and 1.
  EXPECT_DOUBLE_EQ(gr[1], 0.0);
  EXPECT_DOUBLE_EQ(gr[2], 1.0);
}

// An all-NaN date (out-of-universe everywhere) -> every output cell NaN.
TEST(AlphaCs_Rank, AllNaNDate_AllNaN) {
  const atx::usize dates = 2;
  const atx::usize instruments = 4;
  auto cols = random_cols(dates * instruments, 0x8ULL);
  // First date entirely out-of-universe; second date normal.
  std::vector<std::uint8_t> universe(dates * instruments, std::uint8_t{1});
  for (atx::usize j = 0; j < instruments; ++j) {
    universe[j] = 0; // date 0 all out
  }
  const Panel panel = make_panel(dates, instruments, std::move(cols), std::move(universe));

  const std::vector<atx::f64> v = vm_values("rank(close)", panel);
  ASSERT_EQ(v.size(), dates * instruments);
  for (atx::usize j = 0; j < instruments; ++j) {
    EXPECT_TRUE(std::isnan(v[j])) << "date-0 cell " << j << " must be NaN";
  }
  // Date 1 must have a full valid set (ranks span [0,1]); confirm not all NaN.
  bool any_finite = false;
  for (atx::usize j = 0; j < instruments; ++j) {
    if (std::isfinite(v[instruments + j])) {
      any_finite = true;
    }
  }
  EXPECT_TRUE(any_finite);
  expect_vm_matches_oracle("rank(close)", panel);
}

// A cell with a NaN group label stays NaN even when its value is valid: a
// classifier that is out-of-universe at that cell has no group.
TEST(AlphaCs_Group, NaNGroupLabel_CellStaysNaN) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  auto cols = random_cols(dates * instruments, 0x9ULL);
  cols[0] = {10.0, 20.0, 30.0, 40.0};
  cols[5] = {0.0, 0.0, 1.0, 1.0};
  // Knock instrument 2 out of universe -> both its close AND its sector read
  // NaN through LoadField, so it is excluded everywhere.
  std::vector<std::uint8_t> universe(instruments, std::uint8_t{1});
  universe[2] = 0;
  const Panel panel = make_panel(dates, instruments, std::move(cols), std::move(universe));

  const std::vector<atx::f64> v = vm_values("group_rank(close, IndClass.sector)", panel);
  ASSERT_EQ(v.size(), instruments);
  EXPECT_TRUE(std::isnan(v[2])); // excluded -> NaN
  // Group 1 now has only instrument 3 -> singleton 0.5; group 0 has {i0,i1}.
  EXPECT_DOUBLE_EQ(v[3], 0.5);
  EXPECT_DOUBLE_EQ(v[0], 0.0);
  EXPECT_DOUBLE_EQ(v[1], 1.0);
  expect_vm_matches_oracle("group_rank(close, IndClass.sector)", panel);
}

// ===========================================================================
//  Boundaries.
// ===========================================================================

// 1-instrument universe: a singleton valid set -> rank == 0.5, zscore == NaN
// (no sample-std), scale(x,1) == ±1 (Σ|x| == |x|), demean == 0.
TEST(AlphaCs_Boundary, OneInstrument_SingletonSemantics) {
  const atx::usize dates = 3;
  const atx::usize instruments = 1;
  const Panel panel = make_panel(dates, instruments, random_cols(dates, 0xAULL));

  const std::vector<atx::f64> rk = vm_values("rank(close)", panel);
  const std::vector<atx::f64> zs = vm_values("zscore(close)", panel);
  ASSERT_EQ(rk.size(), dates);
  for (atx::usize d = 0; d < dates; ++d) {
    EXPECT_DOUBLE_EQ(rk[d], 0.5);   // singleton -> 0.5
    EXPECT_TRUE(std::isnan(zs[d])); // singleton -> NaN
  }
  expect_vm_matches_oracle("rank(close)", panel);
  expect_vm_matches_oracle("zscore(close)", panel);
  expect_vm_matches_oracle("scale(close, 1)", panel);
  expect_vm_matches_oracle("indneutralize(close, IndClass.sector)", panel);
}

// 1x1 panel — degenerate shape; the VM and oracle must still agree.
TEST(AlphaCs_Boundary, OneByOne_MatchesOracle) {
  const Panel panel = make_panel(1, 1, random_cols(1, 0xBULL));
  expect_vm_matches_oracle("rank(close)", panel);
  expect_vm_matches_oracle("group_zscore(close, IndClass.sector)", panel);
}

// scale on an all-zero valid set -> the L1 norm is 0; the row stays 0 (no inf).
TEST(AlphaCs_Scale, ZeroL1Norm_StaysZeroNoInf) {
  const atx::usize dates = 1;
  const atx::usize instruments = 3;
  auto cols = random_cols(dates * instruments, 0xCULL);
  cols[0] = {0.0, 0.0, 0.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> v = vm_values("scale(close, 5)", panel);
  ASSERT_EQ(v.size(), instruments);
  for (const atx::f64 c : v) {
    EXPECT_DOUBLE_EQ(c, 0.0);
    EXPECT_TRUE(std::isfinite(c)); // no a/0 == inf
  }
  expect_vm_matches_oracle("scale(close, 5)", panel);
}


}  // namespace atxtest_alpha_cs_test
