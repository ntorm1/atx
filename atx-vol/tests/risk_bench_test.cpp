// risk_bench_test.cpp — the ENGINE-FREE core of atx-vol-risk-bench
// (tools/risk_bench.hpp / risk_bench_core.cpp): CLI parsing, book selection over a
// loaded SpiderRock-shaped bucket, the explain-quality statistics and the versioned
// JSON report. The engine driver (run_risk_bench) is covered by
// risk_bench_e2e_test.cpp, compiled only under ATX_VOL_RISK_E2E (it needs the risk
// engine linked).
//
// The book fixture is one bucket of the shipped synthetic store, written once per
// suite into a temp dir. No licensed data is read.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "risk_bench.hpp"

namespace {

namespace fs = std::filesystem;
namespace rb = atx::vol::riskbench;
using atx::vol::ErrorCode;
using atx::vol::Side;

[[nodiscard]] atx::vol::Result<rb::RiskBenchArgs> parse(std::vector<const char *> argv) {
  argv.insert(argv.begin(), "atx-vol-risk-bench");
  return rb::parse_risk_bench_args(argv);
}

// ── RiskBenchArgs ───────────────────────────────────────────────────────────

TEST(RiskBenchArgs, DefaultsWithStore) {
  const auto a = parse({"--store", "C:/store"});
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  EXPECT_EQ(a->store, "C:/store");
  EXPECT_EQ(a->store_root(), "C:/store");
  EXPECT_EQ(a->date, "2026-10-14");
  EXPECT_EQ(a->from, "1000");
  EXPECT_EQ(a->to, "1500");
  EXPECT_EQ(a->underliers, 100u);
  EXPECT_EQ(a->options, 10000u);
  EXPECT_EQ(a->shares, 100u);
  EXPECT_EQ(a->kernel, rb::BenchKernel::Family);
  EXPECT_EQ(a->threads, 8u);
  EXPECT_FALSE(a->needs_rho);
  EXPECT_FALSE(a->trapezoid);
  EXPECT_FALSE(a->determinism);
  EXPECT_EQ(a->seed, 1u);
  EXPECT_TRUE(a->out_json.empty());
  EXPECT_FALSE(a->help);
}

TEST(RiskBenchArgs, AllFlags) {
  const auto a = parse({"--synthetic", "D:/syn", "--date", "2026-08-14", "--from", "1030", "--to",
                        "1200", "--underliers", "10", "--options", "400", "--shares", "7",
                        "--kernel", "both", "--threads", "2", "--needs", "rho", "--trapezoid",
                        "--determinism", "--out", "r.json", "--md", "r.md", "--seed", "42"});
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  EXPECT_EQ(a->synthetic_dir, "D:/syn");
  EXPECT_EQ(a->store_root(), "D:/syn");
  EXPECT_EQ(a->date, "2026-08-14");
  EXPECT_EQ(a->from, "1030");
  EXPECT_EQ(a->to, "1200");
  EXPECT_EQ(a->underliers, 10u);
  EXPECT_EQ(a->options, 400u);
  EXPECT_EQ(a->shares, 7u);
  EXPECT_EQ(a->kernel, rb::BenchKernel::Both);
  EXPECT_EQ(a->threads, 2u);
  EXPECT_TRUE(a->needs_rho);
  EXPECT_TRUE(a->trapezoid);
  EXPECT_TRUE(a->determinism);
  EXPECT_EQ(a->out_json, "r.json");
  EXPECT_EQ(a->out_md, "r.md");
  EXPECT_EQ(a->seed, 42u);
  EXPECT_EQ(parse({"--store", "s", "--kernel", "cold"}).value().kernel, rb::BenchKernel::Cold);
}

TEST(RiskBenchArgs, RejectsUsageErrors) {
  const std::vector<std::vector<const char *>> bad = {
      {},                                      // neither --store nor --synthetic
      {"--store", "s", "--bogus"},             // unknown flag
      {"--store"},                             // missing value
      {"--store", "s", "--kernel", "fast"},    // bad kernel
      {"--store", "s", "--needs", "vega"},     // bad needs
      {"--store", "s", "--options", "-5"},     // negative
      {"--store", "s", "--options", "12x"},    // trailing garbage
      {"--store", "s", "--options", "0"},      // zero
      {"--store", "s", "--from", "1500", "--to", "1000"}, // reversed window
      {"--store", "s", "--from", "1060"},      // not HHMM
      {"--store", "s", "--date", "2026/10/14"},
      {"--store", "a", "--synthetic", "b"},    // two different roots
  };
  for (const auto &argv : bad) {
    const auto a = parse(argv);
    ASSERT_FALSE(a.has_value()) << "accepted case #" << (&argv - bad.data());
    EXPECT_EQ(a.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(RiskBenchArgs, HelpSkipsValidation) {
  const auto a = parse({"--help"});
  ASSERT_TRUE(a.has_value());
  EXPECT_TRUE(a->help);
  EXPECT_NE(rb::risk_bench_usage().find("--synthetic"), std::string::npos);
}

// ── RiskBenchBook ───────────────────────────────────────────────────────────

class RiskBenchBook : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    root_ = std::make_unique<fs::path>(fs::temp_directory_path() / "atx-risk-bench-book");
    std::error_code ec;
    fs::remove_all(*root_, ec);
    atx::vol::SyntheticSpiderRockSpec spec;
    spec.date = "2026-10-14";
    spec.buckets_et = {"1000"};
    spec.n_underliers = 6;
    spec.n_expiries = 8;
    spec.n_strikes = 15;
    spec.seed = 3;
    ASSERT_TRUE(atx::vol::write_synthetic_spiderrock_store(spec, root_->string()).has_value());
    atx::vol::SpiderRockStoreSpec rspec;
    rspec.store_root = root_->string();
    rspec.date = spec.date;
    atx::vol::UnderlierTable table;
    auto b = atx::vol::load_spiderrock_bucket(rspec, "1000", table);
    ASSERT_TRUE(b.has_value()) << b.error().to_string();
    bucket_ = std::make_unique<atx::vol::SpiderRockBucket>(std::move(*b));
  }
  static void TearDownTestSuite() {
    bucket_.reset();
    std::error_code ec;
    fs::remove_all(*root_, ec);
    root_.reset();
  }
  [[nodiscard]] static rb::RiskBenchArgs args(std::size_t underliers, std::size_t options,
                                              std::size_t shares) {
    rb::RiskBenchArgs a;
    a.store = "unused";
    a.underliers = underliers;
    a.options = options;
    a.shares = shares;
    return a;
  }
  // Suite-scoped fixture data owned by SetUp/TearDownTestSuite (gtest's documented
  // static-member pattern).
  static inline std::unique_ptr<fs::path> root_;
  static inline std::unique_ptr<atx::vol::SpiderRockBucket> bucket_;
};

TEST_F(RiskBenchBook, SelectsRequestedCounts) {
  ASSERT_NE(bucket_, nullptr);
  std::mt19937_64 rng{1};
  const auto book = rb::select_bench_book(*bucket_, args(5, 100, 5), rng);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  EXPECT_EQ(book->uids.size(), 5u);
  EXPECT_EQ(book->options.size(), 100u);
  EXPECT_EQ(book->shares.size(), 5u);
  EXPECT_EQ(book->shortfall, 0u);
  EXPECT_GE(book->min_expiries_per_uid, 4u);
  EXPECT_EQ(book->uids_with_both_sides, 5u);

  std::map<std::uint32_t, std::size_t> per_uid;
  std::map<std::uint32_t, std::set<std::int64_t>> expiries;
  std::set<std::uint64_t> ids;
  for (const atx::vol::OptionLeg &leg : book->options) {
    ++per_uid[leg.uid];
    expiries[leg.uid].insert(leg.expiry_ns);
    EXPECT_TRUE(ids.insert(leg.id).second) << "duplicate id " << leg.id;
    EXPECT_NE(leg.qty, 0.0);
    EXPECT_LE(std::abs(leg.qty), 10.0);
    EXPECT_EQ(leg.qty, std::round(leg.qty));
    EXPECT_EQ(leg.multiplier, 100.0);
    const auto row = rb::find_bucket_row(*bucket_, leg.uid, leg.expiry_ns, leg.side, leg.K);
    ASSERT_TRUE(row.has_value());
    const double ad = std::abs(bucket_->vendor_delta[*row]);
    EXPECT_GE(ad, 0.05);
    EXPECT_LE(ad, 0.95);
    EXPECT_GT(bucket_->bid[*row], 0.0);
    EXPECT_GE(bucket_->ask[*row], bucket_->bid[*row]);
    EXPECT_GT(bucket_->sigma[*row], 0.01);
    EXPECT_LE(bucket_->sigma[*row], 3.0);
  }
  for (const auto &[uid, n] : per_uid) {
    EXPECT_EQ(n, 20u) << "uid " << uid;
    EXPECT_GE(expiries[uid].size(), 4u) << "uid " << uid;
  }
  for (const atx::vol::ShareLeg &s : book->shares) {
    EXPECT_TRUE(ids.insert(s.id).second) << "share id collides " << s.id;
  }
  // An uneven split puts the remainder on the first uids; asking for more uids than
  // exist caps at the bucket's uid count.
  std::mt19937_64 rng2{1};
  const auto uneven = rb::select_bench_book(*bucket_, args(10, 13, 0), rng2);
  ASSERT_TRUE(uneven.has_value());
  EXPECT_EQ(uneven->uids.size(), 6u);
  EXPECT_EQ(uneven->options.size(), 13u);
  EXPECT_TRUE(uneven->shares.empty());
}

TEST_F(RiskBenchBook, SharesHedgeVendorDelta) {
  ASSERT_NE(bucket_, nullptr);
  std::mt19937_64 rng{7};
  const auto book = rb::select_bench_book(*bucket_, args(4, 60, 3), rng);
  ASSERT_TRUE(book.has_value());
  ASSERT_EQ(book->shares.size(), 3u); // first 3 uids only
  for (const atx::vol::ShareLeg &s : book->shares) {
    double net = 0.0;
    for (const atx::vol::OptionLeg &leg : book->options) {
      if (leg.uid != s.uid) {
        continue;
      }
      const auto row = rb::find_bucket_row(*bucket_, leg.uid, leg.expiry_ns, leg.side, leg.K);
      ASSERT_TRUE(row.has_value());
      net += leg.qty * leg.multiplier * bucket_->vendor_delta[*row];
    }
    double want = -std::round(net / 100.0) * 100.0;
    want = want == 0.0 ? 100.0 : want;
    EXPECT_EQ(s.qty, want) << "uid " << s.uid << " net delta " << net;
    EXPECT_EQ(std::fmod(s.qty, 100.0), 0.0);
  }
  EXPECT_EQ(book->shares[0].uid, book->uids[0]);
}

TEST_F(RiskBenchBook, DeterministicBySeed) {
  ASSERT_NE(bucket_, nullptr);
  std::mt19937_64 a{11};
  std::mt19937_64 b{11};
  std::mt19937_64 c{12};
  const auto ba = rb::select_bench_book(*bucket_, args(6, 120, 6), a);
  const auto bb = rb::select_bench_book(*bucket_, args(6, 120, 6), b);
  const auto bc = rb::select_bench_book(*bucket_, args(6, 120, 6), c);
  ASSERT_TRUE(ba.has_value() && bb.has_value() && bc.has_value());
  EXPECT_EQ(ba->options, bb->options);
  EXPECT_EQ(ba->shares, bb->shares);
  EXPECT_NE(ba->options, bc->options);
}

// ── RiskBenchStats (engine-free statistics) ─────────────────────────────────

TEST(RiskBenchStats, SpearmanKsZonesAndParity) {
  const std::vector<double> x{1.0, 2.0, 3.0, 4.0, 5.0};
  const std::vector<double> up{10.0, 20.0, 30.0, 40.0, 50.0};
  const std::vector<double> down{5.0, 4.0, 3.0, 2.0, 1.0};
  EXPECT_DOUBLE_EQ(rb::spearman_rho(x, up), 1.0);
  EXPECT_DOUBLE_EQ(rb::spearman_rho(x, down), -1.0);
  // Ties take average ranks: y ranks {1.5, 1.5, 3.5, 3.5, 5} -> rho = 9 / sqrt(90).
  EXPECT_NEAR(rb::spearman_rho(x, std::vector<double>{1, 1, 2, 2, 3}), 9.0 / std::sqrt(90.0),
              1e-15);
  EXPECT_TRUE(std::isnan(rb::spearman_rho(x, std::vector<double>{2, 2, 2, 2, 2})));
  EXPECT_DOUBLE_EQ(rb::ks_statistic(x, x), 0.0);
  EXPECT_DOUBLE_EQ(rb::ks_statistic(x, std::vector<double>{6, 7, 8, 9, 10}), 1.0);
  EXPECT_DOUBLE_EQ(rb::ks_statistic(std::vector<double>{1, 2}, std::vector<double>{2, 3}), 0.5);
  EXPECT_TRUE(std::isnan(rb::ks_statistic(x, std::vector<double>{})));
  EXPECT_EQ(rb::pla_zone(0.9, 0.05), rb::PlaZone::Green);
  EXPECT_EQ(rb::pla_zone(0.75, 0.10), rb::PlaZone::Amber);
  EXPECT_EQ(rb::pla_zone(0.65, 0.05), rb::PlaZone::Red);
  EXPECT_EQ(rb::pla_zone(0.9, 0.13), rb::PlaZone::Red);
  EXPECT_EQ(rb::pla_zone(std::numeric_limits<double>::quiet_NaN(), 0.0), rb::PlaZone::Undefined);
  const auto p = rb::parity(std::vector<double>{1.0, 2.0, std::nan("")},
                            std::vector<double>{1.5, 2.0, 1.0}, 0.01);
  EXPECT_EQ(p.n, 2u);
  EXPECT_DOUBLE_EQ(p.mae, 0.25);
  EXPECT_DOUBLE_EQ(p.sym_rel, (0.5 / 1.5) / 2.0);
  EXPECT_DOUBLE_EQ(p.max_abs, 0.5);
  EXPECT_DOUBLE_EQ(rb::nearest_rank(std::vector<double>{4, 1, 3, 2}, 0.5), 2.0);
  EXPECT_DOUBLE_EQ(rb::nearest_rank(std::vector<double>{4, 1, 3, 2}, 0.95), 4.0);
  const std::vector<std::string> all{"0930", "1000", "1030", "1500", "1530"};
  EXPECT_EQ(rb::select_buckets(all, "1000", "1500"),
            (std::vector<std::string>{"1000", "1030", "1500"}));
}

// ── RiskBenchReport ─────────────────────────────────────────────────────────

[[nodiscard]] rb::RiskBenchReport fabricated_report() {
  rb::RiskBenchArgs args;
  args.synthetic_dir = "C:/tmp/syn";
  args.kernel = rb::BenchKernel::Both;
  args.determinism = true;
  rb::RiskBenchReport r = rb::report_skeleton(args);
  r.synthetic_generated = true;
  r.synthetic_seconds = 14.6;
  r.synthetic_rows = 440000;
  r.n_option_legs = 10000;
  r.n_unique_options = 9876;
  r.n_share_legs = 100;
  r.n_underliers = 100;
  r.book_shortfall = 3;
  r.buckets = {"1000", "1030", "1100"};
  rb::BucketLoad bl;
  bl.bucket = "1000";
  bl.rows_read = 2082476;
  bl.rows_kept = 367664;
  bl.dropped_non_american = 114149;
  bl.dropped_nonstandard_root = 31329;
  bl.rows_null_vendor = 12;
  bl.n_uids = 100;
  bl.max_ddiv_spread = 0.125;
  r.bucket_loads = {bl, bl};
  r.start_state_seconds = 0.033;
  r.start_evaluate_seconds = 0.15;
  for (int k = 0; k < 2; ++k) {
    rb::StepReport t;
    t.bucket_from = r.buckets[static_cast<std::size_t>(k)];
    t.bucket_to = r.buckets[static_cast<std::size_t>(k) + 1];
    t.ts_from = 1'791'000'000'000'000'000 + k;
    t.ts_to = t.ts_from + 1'800'000'000'000;
    t.dt_years = 5.7e-5;
    t.load_seconds = 1.3 + k;
    t.state_seconds = 0.03;
    t.evaluate_seconds = 0.12 + 0.01 * k;
    t.family_build_seconds = 0.05;
    t.explain_seconds = 0.004;
    t.step_seconds = 0.125;
    t.families_built = 1600;
    t.family_rows = 9800;
    t.cold_rows = 76;
    t.routes[1] = 9800;
    t.routes[2] = 76;
    t.flags[8] = 9876;
    t.n_legs = 10100;
    t.n_ok = 10090;
    t.n_excluded = 10;
    t.excluded_premium = 1234.5;
    t.excluded_by_reason[4] = 10;
    t.pnl_total = -5432.1 * (k + 1);
    t.explained = -5400.0 * (k + 1);
    t.sum_abs_unexplained = 32.1;
    t.sum_abs_pnl_total = 99999.0;
    t.explained_ratio = 32.1 / 99999.0;
    t.axes.delta = 0.1 + k;
    t.axes.vol_resid = -0.25;
    t.axis_share.delta = 0.5;
    t.spearman = 0.97;
    t.ks = 0.02;
    t.zone = rb::PlaZone::Green;
    t.parity_delta = rb::ParityStat{100, 1e-7, 2e-6, 3e-7};
    t.determinism_checked = true;
    t.determinism_bit_equal = true;
    t.cold.ran = true;
    t.cold.evaluate_seconds = 12.0;
    t.cold.speedup = 100.0;
    t.cold.n_compared = 9800;
    t.cold.max_abs_price = 1e-6;
    r.steps.push_back(t);
  }
  rb::summarize(r);
  return r;
}

TEST(RiskBenchReport, JsonSchemaV1RoundTrip) {
  const rb::RiskBenchReport r = fabricated_report();
  // NaN-valued fields (never-set parity / cold maxima) must survive as null.
  ASSERT_TRUE(std::isnan(r.steps[0].parity_price.mae));
  const std::string text = rb::report_to_json(r);
  EXPECT_NE(text.find("\"schema_version\": 1"), std::string::npos);
  EXPECT_NE(text.find("\"mae\": null"), std::string::npos);

  const auto back = rb::report_from_json(text);
  ASSERT_TRUE(back.has_value()) << back.error().to_string();
  // Every written field is read back: re-serializing the parsed report reproduces
  // the original text byte for byte (doubles round-trip exactly through JSON).
  EXPECT_EQ(rb::report_to_json(*back), text);
  EXPECT_EQ(back->schema_version, rb::kReportSchemaVersion);
  EXPECT_EQ(back->kernel, "both");
  EXPECT_TRUE(back->synthetic);
  EXPECT_EQ(back->steps.size(), 2u);
  EXPECT_EQ(back->steps[1].ts_from, r.steps[1].ts_from);
  EXPECT_EQ(back->steps[0].routes, r.steps[0].routes);
  EXPECT_EQ(back->steps[0].zone, rb::PlaZone::Green);
  EXPECT_TRUE(std::isnan(back->steps[0].parity_price.mae));
  EXPECT_EQ(back->steps[0].parity_delta.mae, 1e-7);
  EXPECT_EQ(back->bucket_loads[0].dropped_nonstandard_root, 31329);
  EXPECT_TRUE(back->summary.determinism_bit_equal);
  EXPECT_EQ(back->summary.cold_speedup_p50, 100.0);

  // The summary is computed from the steps.
  EXPECT_DOUBLE_EQ(r.summary.explained_ratio, (2 * 32.1) / (2 * 99999.0));
  EXPECT_DOUBLE_EQ(r.summary.cold_lane_fraction, (2.0 * 76.0) / (2.0 * 9876.0));
  EXPECT_NEAR(r.summary.run_seconds_excl_io, 0.033 + 0.15 + 2 * (0.03 + 0.125), 1e-12);

  // A wrong schema version or a missing field is a ParseError, never a guess.
  std::string v2 = text;
  v2.replace(v2.find("\"schema_version\": 1"), 19, "\"schema_version\": 2");
  EXPECT_EQ(rb::report_from_json(v2).error().code(), ErrorCode::ParseError);
  std::string missing = text;
  missing.replace(missing.find("\"tool\""), 6, "\"tooX\"");
  EXPECT_EQ(rb::report_from_json(missing).error().code(), ErrorCode::ParseError);
  EXPECT_EQ(rb::report_from_json("not json").error().code(), ErrorCode::ParseError);

  const std::string md = rb::report_to_markdown(r);
  EXPECT_NE(md.find("# atx-vol-risk-bench"), std::string::npos);
  EXPECT_NE(md.find("1000->1030"), std::string::npos);
  EXPECT_NE(md.find("diagnostic"), std::string::npos);
}

} // namespace
