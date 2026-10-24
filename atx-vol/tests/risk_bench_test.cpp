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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
using atx::vol::ErrorCode;
using atx::vol::Side;

// Per-process suffix for scratch directories: ctest runs every test of a suite in its
// own process, so a fixed name would let one process's remove_all() delete another's
// store mid-read (atx-test-scratch already gives each process a private TEMP; this
// keeps the fixture safe in a binary that does not link it).
[[nodiscard]] std::string process_tag() {
#if defined(_WIN32)
  return std::to_string(::_getpid());
#else
  return std::to_string(::getpid());
#endif
}

[[nodiscard]] atx::vol::Result<rb::RiskBenchArgs> parse(std::vector<const char *> argv) {
  argv.insert(argv.begin(), "atx-vol-risk-bench");
  return rb::parse_risk_bench_args(argv);
}

// ── RiskBenchArgs ───────────────────────────────────────────────────────────

TEST(RiskBenchArgs, DefaultsWithStore) {
  const auto a = parse({"--store", "store-root"});
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  EXPECT_EQ(a->store, "store-root");
  EXPECT_EQ(a->store_root(), "store-root");
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
  const auto a = parse({"--synthetic", "syn-root", "--date", "2026-08-14", "--from", "1030", "--to",
                        "1200", "--underliers", "10", "--options", "400", "--shares", "7",
                        "--kernel", "both", "--threads", "2", "--needs", "rho", "--trapezoid",
                        "--determinism", "--out", "r.json", "--md", "r.md", "--seed", "42"});
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  EXPECT_EQ(a->synthetic_dir, "syn-root");
  EXPECT_EQ(a->store_root(), "syn-root");
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

TEST(RiskBenchArgs, DumpLegsFlags) {
  const auto d = parse({"--store", "store-root"});
  ASSERT_TRUE(d.has_value()) << d.error().to_string();
  EXPECT_TRUE(d->dump_legs.empty());
  EXPECT_EQ(d->dump_steps, 1u);
  const auto a = parse({"--store", "store-root", "--dump-legs", "legs.csv", "--dump-steps", "3"});
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  EXPECT_EQ(a->dump_legs, "legs.csv");
  EXPECT_EQ(a->dump_steps, 3u);
  const auto all = parse({"--store", "s", "--dump-legs", "l.csv", "--dump-steps", "0"});
  ASSERT_TRUE(all.has_value()) << all.error().to_string();
  EXPECT_EQ(all->dump_steps, 0u); // 0 = every step
  EXPECT_EQ(parse({"--store", "s", "--dump-legs"}).error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(parse({"--store", "s", "--dump-steps", "x"}).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_NE(rb::risk_bench_usage().find("--dump-legs"), std::string::npos);
  EXPECT_NE(rb::risk_bench_usage().find("--dump-steps"), std::string::npos);
}

// --time: Calendar365 is the default under BOTH stores. VolTime reproduces the vendor's
// `years` only up to the first NYSE early close (SpiderRockStoreReal.LoadsBucket1000
// prints the table; docs/risk-bench.md "Clock"), and the synthetic store's vendor columns
// are priced on the calendar clock.
TEST(RiskBenchArgs, TimeFlag) {
  using atx::vol::TimeConvention;
  EXPECT_EQ(parse({"--store", "s"}).value().time, TimeConvention::Calendar365);
  EXPECT_EQ(parse({"--synthetic", "s"}).value().time, TimeConvention::Calendar365);
  EXPECT_EQ(parse({"--store", "s", "--time", "voltime"}).value().time, TimeConvention::VolTime);
  EXPECT_EQ(parse({"--synthetic", "s", "--time", "voltime"}).value().time,
            TimeConvention::VolTime);
  EXPECT_EQ(parse({"--store", "s", "--time", "calendar365"}).value().time,
            TimeConvention::Calendar365);
  EXPECT_EQ(parse({"--store", "s", "--time", "vol"}).error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(parse({"--store", "s", "--time"}).error().code(), ErrorCode::InvalidArgument);
  EXPECT_STREQ(rb::time_name(TimeConvention::VolTime), "voltime");
  EXPECT_STREQ(rb::time_name(TimeConvention::Calendar365), "calendar365");
  EXPECT_NE(rb::risk_bench_usage().find("--time voltime|calendar365"), std::string::npos);
}

// ── RiskBenchBook ───────────────────────────────────────────────────────────

class RiskBenchBook : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    root_ = std::make_unique<fs::path>(fs::temp_directory_path() /
                                       ("atx-risk-bench-book-" + process_tag()));
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
  // Sides alternate inside each expiry: with a quota of 20 over 8 expiries every
  // expiry is visited at least twice, so each one holds a call and a put (each |delta|
  // target is taken on both sides, not twice on one).
  std::map<std::pair<std::uint32_t, std::int64_t>, std::set<Side>> sides;
  for (const atx::vol::OptionLeg &leg : book->options) {
    sides[{leg.uid, leg.expiry_ns}].insert(leg.side);
  }
  for (const auto &[key, s] : sides) {
    EXPECT_EQ(s.size(), 2u) << "uid " << key.first << " expiry " << key.second
                            << " holds one side only";
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
  // A quota equal to the expiry count (8) still spans both sides: the first pass over
  // the expiries alternates the side from one expiry to the next.
  std::mt19937_64 rng3{5};
  const auto thin = rb::select_bench_book(*bucket_, args(6, 48, 0), rng3);
  ASSERT_TRUE(thin.has_value());
  EXPECT_EQ(thin->options.size(), 48u);
  EXPECT_EQ(thin->uids_with_both_sides, 6u);
  EXPECT_GE(thin->min_expiries_per_uid, 4u);
}

TEST_F(RiskBenchBook, RejectsZeroUnderliers) {
  ASSERT_NE(bucket_, nullptr);
  std::mt19937_64 rng{1};
  const auto book = rb::select_bench_book(*bucket_, args(0, 100, 5), rng);
  ASSERT_FALSE(book.has_value());
  EXPECT_EQ(book.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(RiskBenchBook, RejectsShortColumns) {
  ASSERT_NE(bucket_, nullptr);
  // market_state_from_bucket accepts empty vendor columns, so such a bucket is legal
  // input elsewhere; the book selector reads them and must refuse, not read past them.
  const auto refused = [&](const atx::vol::SpiderRockBucket &b) {
    std::mt19937_64 rng{1};
    const auto book = rb::select_bench_book(b, args(5, 100, 5), rng);
    return !book.has_value() && book.error().code() == ErrorCode::InvalidArgument;
  };
  atx::vol::SpiderRockBucket no_delta = *bucket_;
  no_delta.vendor_delta.clear();
  EXPECT_TRUE(refused(no_delta));
  atx::vol::SpiderRockBucket short_bid = *bucket_;
  short_bid.bid.pop_back();
  EXPECT_TRUE(refused(short_bid));
  atx::vol::SpiderRockBucket short_ask = *bucket_;
  short_ask.ask.resize(3);
  EXPECT_TRUE(refused(short_ask));
  atx::vol::SpiderRockBucket short_strike = *bucket_;
  short_strike.strike.pop_back();
  EXPECT_TRUE(refused(short_strike));
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

// The licensed store root from ATX_VOL_SPIDERROCK_STORE; empty when unset.
[[nodiscard]] std::string real_store_root() {
#if defined(_WIN32)
  char *value = nullptr;
  std::size_t size = 0U;
  std::string root;
  if (::_dupenv_s(&value, &size, "ATX_VOL_SPIDERROCK_STORE") == 0 && value != nullptr) {
    root = value;
  }
  std::free(value);
  return root;
#else
  const char *value = std::getenv("ATX_VOL_SPIDERROCK_STORE");
  return value != nullptr ? std::string(value) : std::string();
#endif
}

// The full benchmark book (100 underliers x 10,000 options + 100 shares) on bucket
// 1000 of the licensed 2026-10-14 store. Env-gated like SpiderRockStoreReal.
TEST(RiskBenchBookReal, SelectsFullBookOnBucket1000) {
  const std::string root = real_store_root();
  if (root.empty()) {
    GTEST_SKIP() << "ATX_VOL_SPIDERROCK_STORE unset (licensed store not configured)";
  }
  atx::vol::SpiderRockStoreSpec spec;
  spec.store_root = root;
  spec.date = "2026-10-14";
  spec.max_underliers = 100;
  atx::vol::UnderlierTable table;
  const auto bucket = atx::vol::load_spiderrock_bucket(spec, "1000", table);
  ASSERT_TRUE(bucket.has_value()) << bucket.error().to_string();
  rb::RiskBenchArgs a;
  a.store = root;
  std::mt19937_64 rng{a.seed};
  const auto book = rb::select_bench_book(*bucket, a, rng);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  std::printf("[real book 1000] uids=%zu options=%zu shares=%zu candidates=%zu shortfall=%zu "
              "min_expiries=%zu both_sides=%zu\n",
              book->uids.size(), book->options.size(), book->shares.size(), book->n_candidates,
              book->shortfall, book->min_expiries_per_uid, book->uids_with_both_sides);
  EXPECT_EQ(book->uids.size(), 100u);
  EXPECT_EQ(book->options.size(), 10000u);
  EXPECT_EQ(book->shares.size(), 100u);
  EXPECT_EQ(book->shortfall, 0u);
  EXPECT_GE(book->min_expiries_per_uid, 4u);
  EXPECT_EQ(book->uids_with_both_sides, 100u);
  for (const std::string &tk : {"SPX", "NDX", "XSP", "RUT"}) {
    EXPECT_EQ(table.find(tk), 0u) << tk << " is European: american_only keeps it out";
  }
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
  args.synthetic_dir = "syn-store";
  args.kernel = rb::BenchKernel::Both;
  args.determinism = true;
  args.time = atx::vol::TimeConvention::VolTime;
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

TEST(RiskBenchReport, JsonSchemaV2RoundTrip) {
  const rb::RiskBenchReport r = fabricated_report();
  // NaN-valued fields (never-set parity / cold maxima) must survive as null.
  ASSERT_TRUE(std::isnan(r.steps[0].parity_price.mae));
  const std::string text = rb::report_to_json(r);
  EXPECT_EQ(rb::kReportSchemaVersion, 2);
  EXPECT_NE(text.find("\"schema_version\": 2"), std::string::npos);
  EXPECT_NE(text.find("\"time\": \"voltime\""), std::string::npos);
  EXPECT_NE(text.find("\"mae\": null"), std::string::npos);

  const auto back = rb::report_from_json(text);
  ASSERT_TRUE(back.has_value()) << back.error().to_string();
  // Every written field is read back: re-serializing the parsed report reproduces
  // the original text byte for byte (doubles round-trip exactly through JSON).
  EXPECT_EQ(rb::report_to_json(*back), text);
  EXPECT_EQ(back->schema_version, rb::kReportSchemaVersion);
  EXPECT_EQ(back->kernel, "both");
  EXPECT_EQ(back->time, "voltime");
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

  // A wrong schema version (a v1 report: it lacks the v2 fields) or a missing field is a
  // ParseError, never a guess.
  std::string v1 = text;
  v1.replace(v1.find("\"schema_version\": 2"), 19, "\"schema_version\": 1");
  EXPECT_EQ(rb::report_from_json(v1).error().code(), ErrorCode::ParseError);
  std::string missing = text;
  missing.replace(missing.find("\"tool\""), 6, "\"tooX\"");
  EXPECT_EQ(rb::report_from_json(missing).error().code(), ErrorCode::ParseError);
  EXPECT_EQ(rb::report_from_json("not json").error().code(), ErrorCode::ParseError);

  const std::string md = rb::report_to_markdown(r);
  EXPECT_NE(md.find("# atx-vol-risk-bench"), std::string::npos);
  EXPECT_NE(md.find("1000->1030"), std::string::npos);
  EXPECT_NE(md.find("diagnostic"), std::string::npos);
}

TEST(RiskBenchReport, MarkdownListsFrameCountsPerStep) {
  rb::RiskBenchReport r = fabricated_report();
  r.steps[1].flags = {};
  r.steps[1].carry_sets_built = 12;
  const std::string md = rb::report_to_markdown(r);
  EXPECT_NE(md.find("- 1000->1030: families_built 1600, carry_sets_built 0, family_rows 9800, "
                    "cold_rows 76, exercised_rows 0, refused_rows 0; routes Family 9800, "
                    "ColdAnalytic 76; flags VendorParityAvailable 9876\n"),
            std::string::npos)
      << md;
  EXPECT_NE(md.find("- 1030->1100: families_built 1600, carry_sets_built 12, family_rows 9800, "
                    "cold_rows 76, exercised_rows 0, refused_rows 0; routes Family 9800, "
                    "ColdAnalytic 76; flags none\n"),
            std::string::npos)
      << md;
}

// One CSV line (its LF stripped) split into cells, RFC 4180 quotes honored.
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

[[nodiscard]] std::vector<std::string> csv_line_cells(const std::string &text) {
  return csv_cells(std::string_view{text}.substr(0, text.empty() ? 0 : text.size() - 1));
}

[[nodiscard]] atx::vol::SpiderRockBucket one_row_bucket(std::int64_t expiry_ns) {
  atx::vol::SpiderRockBucket b;
  b.uid = {7};
  b.strike = {450.5};
  b.expiry_ns = {expiry_ns};
  b.side = {Side::Put};
  b.sigma = {0.25};
  b.vendor_price = {10.5};
  b.vendor_delta = {-0.4};
  b.vendor_gamma = {0.02};
  b.vendor_vega = {0.25};
  b.vendor_theta = {-0.01};
  b.vendor_T = {0.1};
  b.uprc = {455.0};
  b.rate = {0.04};
  b.sdiv = {0.01};
  b.ddiv = {1.5};
  b.bid = {10.4};
  b.ask = {10.6};
  return b;
}

TEST(RiskBenchReport, LegDumpCsvFormat) {
  const std::string header = rb::leg_dump_csv_header();
  ASSERT_FALSE(header.empty());
  EXPECT_EQ(header.back(), '\n');
  EXPECT_EQ(header.find('\r'), std::string::npos);
  const std::vector<std::string> names = csv_line_cells(header);
  ASSERT_FALSE(names.empty());
  EXPECT_EQ(names.front(), "step");
  EXPECT_EQ(std::set<std::string>(names.begin(), names.end()).size(), names.size());

  const auto expiry = atx::vol::spiderrock_bucket_instant_ns("2026-11-20", "1600");
  ASSERT_TRUE(expiry.has_value());
  rb::LegDumpRow row;
  row.step = "1000->1030";
  row.leg_index = 3;
  row.leg_id = 4;
  row.prepared_row = 2;
  row.ticker = "BRK,B";
  row.uid = 7;
  row.uid_slot = 1;
  row.strike = 450.5;
  row.expiry_ns = *expiry;
  row.side = Side::Put;
  row.qty = -3.0;
  row.multiplier = 100.0;
  row.T = 0.1;
  row.sigma = 0.25;
  row.spot = 455.0;
  row.forward = 454.0;
  row.rate = 0.04;
  row.q_eff = 0.02;
  row.carry_source = rb::CarrySource::TermCarry;
  row.carry_rate = 0.04;
  row.carry_q = 0.01;
  row.pv_divs = 1.5;
  row.route = "Family";
  row.flags = "VendorParityAvailable|ColdFallback";
  row.status = "Ok";
  row.leg_status = "Ok";
  row.price = 10.0;
  row.delta = -0.41;
  row.gamma = 0.021;
  row.vega = 26.0;
  row.theta = -2.6; // vanna stays NaN: an empty cell
  row.volga = 1.0;
  row.charm = 0.1;
  row.vendor_price = 10.5;
  row.vendor_delta = -0.4;
  rb::set_leg_dump_vendor(row, one_row_bucket(*expiry), 0);
  row.pnl_total = 12.5;
  row.pnl_delta = 10.0;
  row.pnl_unexplained = 0.25;

  std::string out;
  rb::append_leg_dump_csv(out, row);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out.back(), '\n');
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1);
  EXPECT_EQ(out.find('\r'), std::string::npos);
  EXPECT_NE(out.find("\"BRK,B\""), std::string::npos); // quoted on write
  const std::vector<std::string> cells = csv_line_cells(out);
  ASSERT_EQ(cells.size(), names.size());
  const auto cell = [&](std::string_view name) -> std::string {
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? "<no column " + std::string{name} + ">"
                             : cells[static_cast<std::size_t>(it - names.begin())];
  };
  EXPECT_EQ(cell("step"), "1000->1030");
  EXPECT_EQ(cell("leg_index"), "3");
  EXPECT_EQ(cell("leg_id"), "4");
  EXPECT_EQ(cell("prepared_row"), "2");
  EXPECT_EQ(cell("ticker"), "BRK,B");
  EXPECT_EQ(cell("uid"), "7");
  EXPECT_EQ(cell("uid_slot"), "1");
  EXPECT_EQ(cell("strike"), "450.5");
  EXPECT_EQ(cell("expiry"), "2026-11-20");
  EXPECT_EQ(cell("side"), "put");
  EXPECT_EQ(cell("qty"), "-3");
  EXPECT_EQ(cell("multiplier"), "100");
  EXPECT_EQ(cell("T"), "0.1");
  EXPECT_EQ(cell("q_eff"), "0.02");
  EXPECT_EQ(cell("carry_source"), "term_carry");
  EXPECT_EQ(cell("pv_divs"), "1.5");
  EXPECT_EQ(cell("route"), "Family");
  EXPECT_EQ(cell("flags"), "VendorParityAvailable|ColdFallback");
  EXPECT_EQ(cell("status"), "Ok");
  EXPECT_EQ(cell("leg_status"), "Ok");
  EXPECT_EQ(cell("price"), "10");
  EXPECT_EQ(cell("vanna"), ""); // non-finite => empty
  EXPECT_EQ(cell("charm"), "0.1");
  EXPECT_EQ(cell("vendor_price"), "10.5");
  EXPECT_EQ(cell("vendor_delta"), "-0.4");
  EXPECT_EQ(cell("sr_prc"), "10.5");
  EXPECT_EQ(cell("sr_de"), "-0.4");
  EXPECT_EQ(cell("sr_ga"), "0.02");
  EXPECT_EQ(cell("sr_ve"), "0.25");
  EXPECT_EQ(cell("sr_th"), "-0.01");
  EXPECT_EQ(cell("sr_years"), "0.1");
  EXPECT_EQ(cell("sr_uprc"), "455");
  EXPECT_EQ(cell("sr_rate"), "0.04");
  EXPECT_EQ(cell("sr_sdiv"), "0.01");
  EXPECT_EQ(cell("sr_ddiv"), "1.5");
  EXPECT_EQ(cell("sr_bid"), "10.4");
  EXPECT_EQ(cell("sr_ask"), "10.6");
  // Derived columns: exact doubles through the shortest round-trip text.
  EXPECT_EQ(std::stod(cell("vendor_vega")), 0.25 / atx::vol::kSpiderRockVegaScale);
  EXPECT_EQ(std::stod(cell("vendor_theta")), -0.01 / atx::vol::kSpiderRockThetaScale);
  EXPECT_EQ(std::stod(cell("q_eff_d4")), atx::vol::spiderrock_q_eff(455.0, 1.5, 0.04, 0.01, 0.1));
  EXPECT_EQ(std::stod(cell("price_abs_err")), 0.5);
  EXPECT_EQ(std::stod(cell("price_sym_rel")), 0.5 / 10.5);
  EXPECT_EQ(cell("pnl_total"), "12.5");
  EXPECT_EQ(cell("pnl_delta"), "10");
  EXPECT_EQ(cell("pnl_gamma"), "");
  EXPECT_EQ(cell("pnl_unexplained"), "0.25");

  // No vendor mark => no price error; flat carry labelled as such.
  row.vendor_price = std::numeric_limits<double>::quiet_NaN();
  row.carry_source = rb::CarrySource::Flat;
  row.ticker = "SPY";
  std::string flat;
  rb::append_leg_dump_csv(flat, row);
  const std::vector<std::string> fc = csv_line_cells(flat);
  ASSERT_EQ(fc.size(), names.size());
  const auto at = [&](std::string_view name) {
    return fc[static_cast<std::size_t>(std::find(names.begin(), names.end(), name) -
                                       names.begin())];
  };
  EXPECT_EQ(at("price_abs_err"), "");
  EXPECT_EQ(at("price_sym_rel"), "");
  EXPECT_EQ(at("carry_source"), "flat");
  EXPECT_EQ(flat.find('"'), std::string::npos);

  // Prepared-row order (ties by leg index) and the --dump-steps selection.
  EXPECT_EQ(rb::leg_dump_order(std::vector<std::uint32_t>{2, 0, 2, 1}),
            (std::vector<std::size_t>{1, 3, 0, 2}));
  EXPECT_TRUE(rb::dump_step_selected(1, 1));
  EXPECT_FALSE(rb::dump_step_selected(1, 2));
  EXPECT_TRUE(rb::dump_step_selected(3, 3));
  EXPECT_FALSE(rb::dump_step_selected(3, 4));
  EXPECT_TRUE(rb::dump_step_selected(0, 9));
}

} // namespace
