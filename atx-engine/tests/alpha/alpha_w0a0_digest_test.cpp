// W0-A0 golden-digest battery — old→new re-baseline of the alpha kernel fixes.
//
// One expression group per cited defect, evaluated by the production Engine on
// a fixed synthetic panel (ties, universe gaps, forward-filled blocks) and
// folded into an FNV-1a digest over every output cell (NaN canonicalized).
//
//   * kOld*  — the digests the PRE-W0 engine produced (captured on base
//              458d0bef before any W0-A0 change). KernelPolicy::legacy_v1() — and
//              the single versioned enum of each defect where the group isolates
//              it — must reproduce them BIT-EXACTLY (RULES §2 versioned enums).
//   * kNew*  — the re-baselined digests of the corrected default policy.
//
// Suites: AlphaCsRankTies_Digest (A-01), AlphaHumpWarmup_Digest (A-02),
// AlphaFlatWindow_Digest (A-09), AlphaAuditExactParity_Digest (A-13).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_test_w0_a0_digest {

using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::FlatGuard;
using atx::engine::alpha::HumpNaN;
using atx::engine::alpha::KernelPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::Program;
using atx::engine::alpha::RankTies;
using atx::engine::alpha::TsSumPath;

// Pre-W0 digests (base 458d0bef, default Engine, captured before the change).
constexpr std::uint64_t kOldRank = 0xa50ec3743580856bULL;
constexpr std::uint64_t kOldHump = 0x0a9ce0c6fb27f23eULL;
constexpr std::uint64_t kOldFlat = 0x1c75499cdd019337ULL;
constexpr std::uint64_t kOldSumAuditExact = 0xfbd765ddbfee7d5cULL;
constexpr std::uint64_t kOldSumResearchFast = 0xfbd765ddbfee7d5cULL;

// Re-baselined digests of the corrected default KernelPolicy.
constexpr std::uint64_t kNewRank = 0xdd31545d3a5ad696ULL;
constexpr std::uint64_t kNewHump = 0x3159e020352402f8ULL;
constexpr std::uint64_t kNewFlat = 0xc87fd2b6cc8ceb63ULL;
constexpr std::uint64_t kNewSumAuditExact = 0xda7bd655e7f69444ULL;
constexpr std::uint64_t kNewSumResearchFast = 0xca905bf2dfb89bb4ULL;

const Library &lib() {
  static const Library l;
  return l;
}

Program compile_ok(std::string_view src) {
  auto ast = atx::engine::alpha::parse_expr(src, lib());
  EXPECT_TRUE(ast.has_value()) << src;
  auto ana = atx::engine::alpha::analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << src << ": " << (ana ? "" : ana.error().message());
  auto prog = atx::engine::alpha::compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << src;
  return prog.value_or(Program{});
}

// 80 dates x 16 instruments: rounded prices (sign(close-open) ties), a tier
// column of tied buckets, a forward-filled "fund" column (constant 10-date
// blocks), and three universe gaps (a 16-date exit, a late entry, a 2-date hole).
// THIS FIXTURE IS FROZEN — the kOld* digests were captured on it.
Panel battery_panel() {
  constexpr atx::usize kD = 80;
  constexpr atx::usize kI = 16;
  std::vector<std::vector<atx::f64>> cols(8, std::vector<atx::f64>(kD * kI));
  std::vector<std::uint8_t> uni(kD * kI, 1);
  std::uint64_t s = 0x5EEDA0A0ULL;
  auto u01 = [&s]() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(s >> 11) * (1.0 / 9007199254740992.0);
  };
  std::vector<atx::f64> px(kI);
  for (atx::usize j = 0; j < kI; ++j) {
    px[j] = 20.0 + 5.0 * static_cast<atx::f64>(j);
  }
  for (atx::usize t = 0; t < kD; ++t) {
    for (atx::usize j = 0; j < kI; ++j) {
      const atx::usize c = t * kI + j;
      px[j] *= 1.0 + 0.02 * (u01() - 0.5);
      const atx::f64 close = std::round(px[j] * 100.0) / 100.0;
      const atx::f64 open = std::round(close * (1.0 + 0.01 * (u01() - 0.5)) * 100.0) / 100.0;
      cols[0][c] = close;
      cols[1][c] = open;
      cols[2][c] = std::max(close, open) + 0.1;
      cols[3][c] = std::min(close, open) - 0.1;
      cols[4][c] = 1.0e6 + 1.0e5 * u01();
      cols[5][c] = static_cast<atx::f64>(j % 4);              // IndClass.sector
      cols[6][c] = 0.1 * static_cast<atx::f64>(1 + (j % 3)) + // fund: forward-filled
                   0.05 * static_cast<atx::f64>(t / 10);      // blocks of 10 dates
      cols[7][c] = static_cast<atx::f64>(j / 4);              // tier: tied buckets
    }
  }
  for (atx::usize t = 30; t < 46; ++t) {
    uni[t * kI + 3] = 0; // instrument 3 leaves the universe for 16 dates
  }
  for (atx::usize t = 0; t < 12; ++t) {
    uni[t * kI + 7] = 0; // instrument 7 enters late
  }
  for (atx::usize t = 50; t < 52; ++t) {
    uni[t * kI + 9] = 0; // a two-date gap
  }
  std::vector<std::string> names = {"close",  "open",            "high", "low",
                                    "volume", "IndClass.sector", "fund", "tier"};
  auto p = Panel::create(kD, kI, std::move(names), std::move(cols), std::move(uni));
  EXPECT_TRUE(p.has_value());
  return p.value();
}

std::uint64_t fnv_cells(std::uint64_t h, const std::vector<atx::f64> &v) {
  for (const atx::f64 x : v) {
    std::uint64_t bits = 0;
    if (std::isnan(x)) {
      bits = 0x7ff8000000000000ULL; // canonical NaN: payload/sign noise is not signal
    } else {
      std::memcpy(&bits, &x, sizeof(bits));
    }
    for (int k = 0; k < 8; ++k) {
      h ^= (bits >> (8 * k)) & 0xFFULL;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

std::uint64_t group_digest(const std::vector<std::string_view> &exprs, EvalMode mode,
                           KernelPolicy policy) {
  const Panel panel = battery_panel();
  std::uint64_t h = 1469598103934665603ULL;
  for (const std::string_view e : exprs) {
    const Program prog = compile_ok(e);
    Engine eng{panel};
    eng.set_eval_mode(mode);
    eng.set_kernel_policy(policy);
    auto out = eng.evaluate(prog);
    EXPECT_TRUE(out.has_value()) << e;
    if (out.has_value()) {
      h = fnv_cells(h, out.value().alphas.front().values);
    }
  }
  return h;
}

const std::vector<std::string_view> kRankExprs = {
    "rank(sign(close - open))", "rank(tier)", "group_rank(sign(close - open), IndClass.sector)",
    "quantile(tier, 3)", "rank(close)"};
const std::vector<std::string_view> kHumpExprs = {"hump(ts_mean(close, 5), 0.1)", "hump(close, 0.5)",
                                                  "hump(rank(close), 0.05)"};
const std::vector<std::string_view> kFlatExprs = {
    "ts_zscore(fund, 5)", "correlation(fund, close, 5)", "slope(fund, 5)",
    "ts_std(fund, 5)",    "skew(fund, 6)",               "rsquare(fund, 5)",
    "resid(fund, 5)",     "ts_regression(close, fund, 5)"};
const std::vector<std::string_view> kSumExprs = {"ts_sum(close, 5)", "ts_mean(volume, 7)",
                                                 "ts_sum(fund, 9)"};

void print_row(const char *name, std::uint64_t old_d, std::uint64_t new_d) {
  std::printf("[w0a0-digest] %-18s old=0x%016llx new=0x%016llx\n", name,
              static_cast<unsigned long long>(old_d), static_cast<unsigned long long>(new_d));
}

// ---- A-01 -----------------------------------------------------------------
TEST(AlphaCsRankTies_Digest, OrdinalV1ReproducesPreW0DefaultRebaselines) {
  KernelPolicy ties_only{};
  ties_only.rank_ties = RankTies::OrdinalV1;
  EXPECT_EQ(group_digest(kRankExprs, EvalMode::AuditExact, ties_only), kOldRank);
  EXPECT_EQ(group_digest(kRankExprs, EvalMode::AuditExact, KernelPolicy::legacy_v1()), kOldRank);
  const std::uint64_t now = group_digest(kRankExprs, EvalMode::AuditExact, KernelPolicy{});
  print_row("rank (A-01)", kOldRank, now);
  EXPECT_NE(now, kOldRank);
  EXPECT_EQ(now, kNewRank);
}

// ---- A-02 -----------------------------------------------------------------
TEST(AlphaHumpWarmup_Digest, StickyV1ReproducesPreW0DefaultRebaselines) {
  KernelPolicy hump_legacy{}; // the group nests ts_mean, so its A-13 enum is legacy too
  hump_legacy.hump = HumpNaN::StickyV1;
  hump_legacy.ts_sum = TsSumPath::OnlineV1;
  EXPECT_EQ(group_digest(kHumpExprs, EvalMode::AuditExact, hump_legacy), kOldHump);
  EXPECT_EQ(group_digest(kHumpExprs, EvalMode::AuditExact, KernelPolicy::legacy_v1()), kOldHump);
  const std::uint64_t now = group_digest(kHumpExprs, EvalMode::AuditExact, KernelPolicy{});
  print_row("hump (A-02)", kOldHump, now);
  EXPECT_NE(now, kOldHump);
  EXPECT_EQ(now, kNewHump);
}

// ---- A-09 -----------------------------------------------------------------
TEST(AlphaFlatWindow_Digest, NoneV1ReproducesPreW0DefaultRebaselines) {
  KernelPolicy flat_only{};
  flat_only.flat = FlatGuard::NoneV1;
  EXPECT_EQ(group_digest(kFlatExprs, EvalMode::AuditExact, flat_only), kOldFlat);
  EXPECT_EQ(group_digest(kFlatExprs, EvalMode::AuditExact, KernelPolicy::legacy_v1()), kOldFlat);
  const std::uint64_t now = group_digest(kFlatExprs, EvalMode::AuditExact, KernelPolicy{});
  print_row("flat (A-09)", kOldFlat, now);
  EXPECT_NE(now, kOldFlat);
  EXPECT_EQ(now, kNewFlat);
}

// ---- A-13 -----------------------------------------------------------------
TEST(AlphaAuditExactParity_Digest, OnlineV1ReproducesPreW0DefaultRebaselines) {
  KernelPolicy sum_only{};
  sum_only.ts_sum = TsSumPath::OnlineV1;
  EXPECT_EQ(group_digest(kSumExprs, EvalMode::AuditExact, sum_only), kOldSumAuditExact);
  EXPECT_EQ(group_digest(kSumExprs, EvalMode::ResearchFast, sum_only), kOldSumResearchFast);
  EXPECT_EQ(group_digest(kSumExprs, EvalMode::AuditExact, KernelPolicy::legacy_v1()),
            kOldSumAuditExact);
  const std::uint64_t now_ae = group_digest(kSumExprs, EvalMode::AuditExact, KernelPolicy{});
  const std::uint64_t now_rf = group_digest(kSumExprs, EvalMode::ResearchFast, KernelPolicy{});
  print_row("sum AuditExact", kOldSumAuditExact, now_ae);
  print_row("sum ResearchFast", kOldSumResearchFast, now_rf);
  EXPECT_NE(now_ae, kOldSumAuditExact);
  EXPECT_EQ(now_ae, kNewSumAuditExact);
  EXPECT_EQ(now_rf, kNewSumResearchFast);
}

} // namespace atx_test_w0_a0_digest
