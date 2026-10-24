// risk_bench_e2e_test.cpp — END-TO-END atx-vol-risk-bench run on the synthetic
// SpiderRock-shaped store: run_risk_bench (tools/risk_bench.cpp) drives RiskStream
// over 3 buckets of a 10-underlier store and the report's identities are checked.
//
// Compiled ONLY under -DATX_VOL_RISK_E2E=ON (tests/CMakeLists.txt): it links the risk
// engine (lane A's src/risk/*), which the lane-C worktree does not have. The
// integration stage flips the option once the engine is merged.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include "risk_bench.hpp"

#if defined(_WIN32)
#include <process.h> // _getpid: a private scratch dir per test process
#else
#include <unistd.h> // getpid
#endif

namespace {

namespace fs = std::filesystem;
namespace rb = atx::vol::riskbench;

// Per-process suffix for the scratch store (see risk_bench_test.cpp process_tag).
[[nodiscard]] std::string process_tag() {
#if defined(_WIN32)
  return std::to_string(::_getpid());
#else
  return std::to_string(::getpid());
#endif
}

[[nodiscard]] double rel_gap(double a, double b) {
  return std::abs(a - b) / std::max({1.0, std::abs(a), std::abs(b)});
}

TEST(RiskBenchE2E, SyntheticRunProducesReport) {
  const fs::path dir = fs::temp_directory_path() / ("atx-risk-bench-e2e-" + process_tag());
  std::error_code ec;
  fs::remove_all(dir, ec);

  rb::RiskBenchArgs args;
  args.synthetic_dir = dir.string();
  args.from = "1000";
  args.to = "1100"; // 3 buckets, 2 explain steps
  args.underliers = 10;
  args.options = 400; // 40 per underlier
  args.shares = 10;
  args.kernel = rb::BenchKernel::Family;
  args.threads = 4;
  args.determinism = true;
  std::ostringstream log;
  const auto rep = rb::run_risk_bench(args, log);
  ASSERT_TRUE(rep.has_value()) << rep.error().to_string() << "\n" << log.str();
  std::printf("%s", log.str().c_str());

  EXPECT_TRUE(rep->synthetic_generated);
  EXPECT_GT(rep->synthetic_rows, 0u);
  EXPECT_EQ(rep->buckets.size(), 3u);
  ASSERT_EQ(rep->steps.size(), 2u);
  EXPECT_EQ(rep->n_option_legs, 400u);
  EXPECT_EQ(rep->n_share_legs, 10u);
  EXPECT_EQ(rep->n_underliers, 10u);
  EXPECT_EQ(rep->book_shortfall, 0u);
  EXPECT_EQ(rep->bucket_loads.size(), 3u);

  for (const rb::StepReport &t : rep->steps) {
    EXPECT_EQ(t.n_legs, 410u);
    EXPECT_GT(static_cast<double>(t.n_ok), 0.9 * static_cast<double>(t.n_legs))
        << t.bucket_from << "->" << t.bucket_to;
    EXPECT_EQ(t.n_ok + t.n_excluded, t.n_legs);
    // Eight Taylor terms + unexplained reproduce the total (totals are Neumaier sums
    // of per-leg exact remainders, so the identity holds to rounding).
    const rb::AxisTotals &a = t.axes;
    const double sum = a.delta + a.gamma + a.vega + a.volga + a.vanna + a.theta + a.rho +
                       a.charm + a.unexplained;
    EXPECT_LT(rel_gap(sum, t.pnl_total), 1e-9);
    EXPECT_LT(rel_gap(a.vol_level + a.vol_skew + a.vol_curv + a.vol_resid, a.vega), 1e-9);
    EXPECT_TRUE(std::isfinite(t.explained_ratio));
    EXPECT_TRUE(std::isfinite(t.spearman));
    EXPECT_GT(t.evaluate_seconds, 0.0);
    // Synthetic vendor columns are the library's own American Greeks.
    EXPECT_GT(t.parity_delta.n, 0u);
    EXPECT_LT(t.parity_delta.sym_rel, 1e-3);
    EXPECT_TRUE(t.determinism_checked);
    EXPECT_TRUE(t.determinism_bit_equal) << t.bucket_from << "->" << t.bucket_to;
  }
  EXPECT_TRUE(rep->summary.determinism_bit_equal);
  EXPECT_TRUE(std::isfinite(rep->summary.explained_ratio));

  // The report round-trips through the versioned JSON schema.
  const std::string text = rb::report_to_json(*rep);
  const auto back = rb::report_from_json(text);
  ASSERT_TRUE(back.has_value()) << back.error().to_string();
  EXPECT_EQ(rb::report_to_json(*back), text);

  // A second run reuses the store instead of rewriting it.
  std::ostringstream log2;
  const auto again = rb::run_risk_bench(args, log2);
  ASSERT_TRUE(again.has_value()) << again.error().to_string();
  EXPECT_FALSE(again->synthetic_generated);
  fs::remove_all(dir, ec);
}

} // namespace
