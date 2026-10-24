// risk_bench_e2e_test.cpp — END-TO-END atx-vol-risk-bench run on the synthetic
// SpiderRock-shaped store: run_risk_bench (tools/risk_bench.cpp) drives RiskStream
// over 3 buckets of a 10-underlier store and the report's identities are checked.
//
// Compiled ONLY under -DATX_VOL_RISK_E2E=ON (tests/CMakeLists.txt): it links the risk
// engine (src/risk/*) and tools/risk_bench.cpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/vol/api/marketdata/data.hpp" // ns_to_iso_date
#include "atx/vol/api/marketdata/spiderrock_store.hpp"
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

// One CSV line (no LF) split into cells, RFC 4180 quotes honored.
[[nodiscard]] std::vector<std::string> csv_cells(std::string_view line) {
  std::vector<std::string> cells(1);
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted && c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
      cells.back() += '"';
      ++i;
    } else if (c == '"') {
      quoted = !quoted;
    } else if (c == ',' && !quoted) {
      cells.emplace_back();
    } else {
      cells.back() += c;
    }
  }
  return cells;
}

// The book the run selected: the first bucket of the same store read the way
// run_risk_bench reads it (top --underliers), then select_bench_book with the run's seed.
[[nodiscard]] rb::BenchBook rebuilt_book(const rb::RiskBenchArgs &args) {
  atx::vol::SpiderRockStoreSpec spec;
  spec.store_root = args.store_root();
  spec.date = args.date;
  spec.max_underliers = args.underliers;
  atx::vol::UnderlierTable table;
  const auto bucket = atx::vol::load_spiderrock_bucket(spec, args.from, table);
  EXPECT_TRUE(bucket.has_value()) << bucket.error().to_string();
  if (!bucket.has_value()) {
    return {};
  }
  std::mt19937_64 rng{args.seed};
  auto book = rb::select_bench_book(*bucket, args, rng);
  EXPECT_TRUE(book.has_value()) << book.error().to_string();
  return book.has_value() ? std::move(*book) : rb::BenchBook{};
}

// --dump-legs on the synthetic store (lane C review-2 minor 1): every option leg of every
// step, in prepared order, mapped to its book leg, on the D4 carry the store was priced
// with (every row term_carry, q_eff == q_eff_d4).
void check_leg_dump(const rb::RiskBenchArgs &args, std::size_t n_steps) {
  std::ifstream in(args.dump_legs, std::ios::binary);
  ASSERT_TRUE(in.good()) << args.dump_legs;
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  EXPECT_EQ(text.find('\r'), std::string::npos);
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t lf = text.find('\n'); lf != std::string::npos; lf = text.find('\n', start)) {
    lines.push_back(text.substr(start, lf - start));
    start = lf + 1;
  }
  EXPECT_EQ(start, text.size()) << "the file ends with LF";
  const rb::BenchBook book = rebuilt_book(args);
  ASSERT_EQ(book.options.size(), args.options);
  ASSERT_EQ(lines.size(), 1 + n_steps * book.options.size());
  EXPECT_EQ(lines.front() + "\n", rb::leg_dump_csv_header());
  const std::vector<std::string> names = csv_cells(lines.front());
  std::map<std::string, std::size_t> col;
  for (std::size_t i = 0; i < names.size(); ++i) {
    col[names[i]] = i;
  }
  std::map<std::uint64_t, const atx::vol::OptionLeg *> by_id;
  for (const atx::vol::OptionLeg &leg : book.options) {
    by_id[leg.id] = &leg;
  }
  std::string last_step;
  long long last_row = -1;
  for (std::size_t l = 1; l < lines.size(); ++l) {
    const std::vector<std::string> c = csv_cells(lines[l]);
    ASSERT_EQ(c.size(), names.size()) << "line " << l;
    auto at = [&](const char *name) { return c[col.at(name)]; };
    if (at("step") != last_step) { // prepared order restarts per step
      last_step = at("step");
      last_row = -1;
    }
    const long long row = std::stoll(at("prepared_row"));
    EXPECT_GE(row, last_row) << "prepared_row decreases at line " << l;
    last_row = row;
    EXPECT_FALSE(at("ticker").empty()) << "line " << l;
    EXPECT_EQ(at("carry_source"), "term_carry") << "line " << l;
    EXPECT_LE(std::abs(std::stod(at("q_eff")) - std::stod(at("q_eff_d4"))), 1e-12)
        << "line " << l;
    // Follow-up 2 columns: --synthetic builds Published (D4) states, so nothing is
    // implied, and the Published forward is the forward the kernel priced at.
    EXPECT_EQ(at("carry_mode"), "published") << "line " << l;
    EXPECT_EQ(at("implied_forward"), "") << "line " << l;
    EXPECT_LE(rel_gap(std::stod(at("published_forward")), std::stod(at("forward"))), 1e-12)
        << "line " << l;
    EXPECT_EQ(at("sr_synspot"), "") << "the synthetic store has no synSpot";
    const auto it = by_id.find(std::stoull(at("leg_id")));
    ASSERT_NE(it, by_id.end()) << "line " << l;
    const atx::vol::OptionLeg &leg = *it->second;
    EXPECT_EQ(std::stoul(at("uid")), leg.uid) << "line " << l;
    EXPECT_EQ(std::stod(at("strike")), leg.K) << "line " << l;
    EXPECT_EQ(at("expiry"), atx::vol::ns_to_iso_date(leg.expiry_ns)) << "line " << l;
    EXPECT_EQ(at("side"), leg.side == atx::vol::Side::Put ? "put" : "call") << "line " << l;
  }
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
  args.dump_legs = (dir.parent_path() / ("atx-risk-bench-e2e-legs-" + process_tag() + ".csv"))
                       .string();
  args.dump_steps = 0; // every step
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
    // Lane A2: every family is solved at a node count inside the policy's [4, 16] range.
    EXPECT_GE(t.family_nodes, 4u * t.families_built);
    EXPECT_LE(t.family_nodes, 16u * t.families_built);
  }
  EXPECT_TRUE(rep->summary.determinism_bit_equal);
  // Lane A2: the report echoes the effective family policy.
  EXPECT_NE(rep->family_policy.find("n_sigma_min"), std::string::npos) << rep->family_policy;
  EXPECT_NE(rep->family_policy.find("short_box_tau"), std::string::npos) << rep->family_policy;
  EXPECT_TRUE(std::isfinite(rep->summary.explained_ratio));
  EXPECT_EQ(rep->time, "calendar365");
  EXPECT_EQ(rep->carry_mode, "published");
  for (const rb::BucketLoad &b : rep->bucket_loads) {
    EXPECT_EQ(b.n_expiries_implied, 0u) << b.bucket;
    EXPECT_GT(b.n_expiries_published, 0u) << b.bucket;
    EXPECT_FALSE(b.pv_divs_from_ddiv_pv) << b.bucket;
    EXPECT_EQ(b.dropped_forward_framework, 0) << b.bucket;
  }
  check_leg_dump(args, rep->steps.size());

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

  // A directory as --dump-legs is an IoError (the dump opens after the bucket listing).
  rb::RiskBenchArgs bad = args;
  bad.dump_legs = dir.string();
  std::ostringstream log3;
  const auto refused = rb::run_risk_bench(bad, log3);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), atx::vol::ErrorCode::IoError) << refused.error().to_string();
  fs::remove_all(dir, ec);
  fs::remove(args.dump_legs, ec);
}

} // namespace
