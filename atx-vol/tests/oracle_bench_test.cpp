// oracle_bench_test.cpp — correctness GATE for atx-vol-oracle-bench
// (bench/oracle/CHARTER.md stage 2).
//
// ACCESS MECHANISM. The bench DRIVER (BenchArgs/parse_bench_args/ModeRunner/
// run_oracle_bench) is a CLI-only TU with no reuse value beyond the binary, so
// — exactly like bev_label_factory_gate_test.cpp above it in the source list —
// this test reaches it by `#define ATX_ORACLE_BENCH_NO_MAIN` + a TEXTUAL
// #include of tools/oracle_bench_main.cpp (main() compiled out). This file is
// the ONLY place that includes that TU, so there is no ODR hazard. The tool's
// three LIBRARY TUs (oracle_conventions / oracle_scorecard /
// oracle_cohort_reader) have real headers and are compiled into atx-vol-tests
// directly (tests/CMakeLists.txt), so the suite exercises the shipped
// implementation byte-for-byte.
//
// FIXTURE. Self-contained synthetic parquet store written with the Arrow C++
// API directly (the track_store_test.cpp arrangement: the vcpkg Arrow targets
// are SYSTEM includes, /W4 /WX-clean). Arrow is REQUIRED here — the fixture
// must contain genuinely NULL cells (bidIV/askIV/error sentinel rows), which
// atx-core's span-based parquet_writer.hpp cannot produce. No dependency on
// the real oracle store or on lane oracle-s1's cohorts.
//
// COVERAGE (charter stage-2 done criteria):
//   - moneyness band edges at exactly 0.8/0.95/1.05/1.2, cp-aware;
//   - dte band edges at exactly 7/30/90;
//   - within-tolerance accounting + nearest-rank percentiles;
//   - sentinel-null rows counted, never priced; bad-input rows likewise;
//   - reader opens ONLY the cohort-named partition dirs (asserted on
//     partitions_opened) and row-filters by underlier inside them, reading
//     utf8 and large_utf8 (polars-shaped) string columns identically;
//   - end-to-end: synthetic partition + cohort JSON -> scorecard whose cell
//     keys match the charter schema EXACTLY (asserted as a set), per-cell
//     stats and header fields present, rows/s on stderr.

#include <gtest/gtest.h>

#define ATX_ORACLE_BENCH_NO_MAIN
#include "oracle_bench_main.cpp" // textual include, main() suppressed (banner above)

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/io/file.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using atx::vol::AmericanGreeks;
using atx::vol::ErrorCode;
using atx::vol::Side;
using namespace atx::vol::oracle;

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-oracle-bench-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// ── Synthetic store fixture ─────────────────────────────────────────────

struct FixtureRow {
  std::string tk = "AAA";
  std::string cp = "Call";
  double strike = 100.0;
  double uprc = 100.0;
  double rate = 0.05;
  double sdiv = 0.01;
  double ddiv = 0.0;
  double years = 20.0 / 365.0;
  double srVol = 0.25;
  double srPrc = 1.0;
  double de = 0.0, ga = 0.0, th = 0.0, ve = 0.0, rh = 0.0, ph = 0.0;
  double vo = 0.0, va = 0.0, deDecay = 0.0;
  double bidPrc = 1.0;
  double askPrc = 1.1;
  double bidIV = 0.2, askIV = 0.3, error = 0.0;
  bool null_bid_iv = false, null_ask_iv = false, null_error = false;
};

struct NumCol {
  const char *name;
  double FixtureRow::*field;
};

constexpr NumCol kNumCols[] = {
    {"okey_xx", &FixtureRow::strike}, {"uPrc", &FixtureRow::uprc},
    {"rate", &FixtureRow::rate},      {"sdiv", &FixtureRow::sdiv},
    {"ddiv", &FixtureRow::ddiv},      {"years", &FixtureRow::years},
    {"srVol", &FixtureRow::srVol},    {"srPrc", &FixtureRow::srPrc},
    {"de", &FixtureRow::de},          {"ga", &FixtureRow::ga},
    {"th", &FixtureRow::th},          {"ve", &FixtureRow::ve},
    {"rh", &FixtureRow::rh},          {"ph", &FixtureRow::ph},
    {"vo", &FixtureRow::vo},          {"va", &FixtureRow::va},
    {"deDecay", &FixtureRow::deDecay},{"bidPrc", &FixtureRow::bidPrc},
    {"askPrc", &FixtureRow::askPrc},
};

// Writes one partition dir (<dir>/data.parquet) with every column the reader
// selects, real NULLs on the sentinel trio where flagged, plus an EXTRA
// column ("atmVol") the reader must tolerate and ignore. `large_strings`
// defaults to the REAL store's shape: polars round-trips strings as arrow
// LARGE_STRING (large_utf8); the reader must read both encodings, so the
// reader suite writes one partition per shape.
void write_fixture_partition(const fs::path &dir, const std::vector<FixtureRow> &rows,
                             bool large_strings = true) {
  ASSERT_TRUE(fs::create_directories(dir) || fs::is_directory(dir));
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;

  auto finish_string = [&](const char *name, auto get) {
    std::shared_ptr<arrow::Array> arr;
    if (large_strings) {
      arrow::LargeStringBuilder b;
      for (const FixtureRow &r : rows) {
        ASSERT_TRUE(b.Append(get(r)).ok());
      }
      ASSERT_TRUE(b.Finish(&arr).ok());
      fields.push_back(arrow::field(name, arrow::large_utf8()));
    } else {
      arrow::StringBuilder b;
      for (const FixtureRow &r : rows) {
        ASSERT_TRUE(b.Append(get(r)).ok());
      }
      ASSERT_TRUE(b.Finish(&arr).ok());
      fields.push_back(arrow::field(name, arrow::utf8()));
    }
    arrays.push_back(std::move(arr));
  };
  auto finish_double = [&](arrow::DoubleBuilder &b, const char *name) {
    std::shared_ptr<arrow::Array> arr;
    ASSERT_TRUE(b.Finish(&arr).ok());
    fields.push_back(arrow::field(name, arrow::float64()));
    arrays.push_back(std::move(arr));
  };

  finish_string("undSecKey_tk", [](const FixtureRow &r) { return r.tk; });
  if (::testing::Test::HasFatalFailure()) { return; }
  finish_string("okey_cp", [](const FixtureRow &r) { return r.cp; });
  if (::testing::Test::HasFatalFailure()) { return; }
  for (const NumCol &c : kNumCols) {
    arrow::DoubleBuilder b;
    for (const FixtureRow &r : rows) {
      ASSERT_TRUE(b.Append(r.*c.field).ok());
    }
    finish_double(b, c.name);
    if (::testing::Test::HasFatalFailure()) { return; }
  }
  // Sentinel trio: genuinely NULL cells where flagged.
  struct SentCol {
    const char *name;
    double FixtureRow::*field;
    bool FixtureRow::*null_flag;
  };
  const SentCol sent_cols[] = {
      {"bidIV", &FixtureRow::bidIV, &FixtureRow::null_bid_iv},
      {"askIV", &FixtureRow::askIV, &FixtureRow::null_ask_iv},
      {"error", &FixtureRow::error, &FixtureRow::null_error},
  };
  for (const SentCol &c : sent_cols) {
    arrow::DoubleBuilder b;
    for (const FixtureRow &r : rows) {
      if (r.*c.null_flag) {
        ASSERT_TRUE(b.AppendNull().ok());
      } else {
        ASSERT_TRUE(b.Append(r.*c.field).ok());
      }
    }
    finish_double(b, c.name);
    if (::testing::Test::HasFatalFailure()) { return; }
  }
  {
    arrow::DoubleBuilder b; // extra column the reader must ignore
    for (const FixtureRow &r : rows) {
      ASSERT_TRUE(b.Append(r.srVol).ok());
    }
    finish_double(b, "atmVol");
    if (::testing::Test::HasFatalFailure()) { return; }
  }

  const auto table =
      arrow::Table::Make(arrow::schema(fields), arrays, static_cast<std::int64_t>(rows.size()));
  auto out = arrow::io::FileOutputStream::Open((dir / "data.parquet").string());
  ASSERT_TRUE(out.ok());
  const arrow::Status st = parquet::arrow::WriteTable(
      *table, arrow::default_memory_pool(), *out, static_cast<std::int64_t>(rows.size()));
  ASSERT_TRUE(st.ok()) << st.ToString();
  ASSERT_TRUE((*out)->Close().ok());
}

[[nodiscard]] OracleRow to_oracle_row(const FixtureRow &r) {
  OracleRow row;
  row.underlier = r.tk;
  row.side = (r.cp == "Call") ? Side::Call : Side::Put;
  row.strike = r.strike;
  row.uprc = r.uprc;
  row.rate = r.rate;
  row.sdiv = r.sdiv;
  row.ddiv = r.ddiv;
  row.years = r.years;
  row.sr_vol = r.srVol;
  row.sr_prc = r.srPrc;
  row.bid_prc = r.bidPrc;
  row.ask_prc = r.askPrc;
  return row;
}

// Fills the fixture row's ORACLE OUTPUT columns so the tool should reproduce
// them exactly: srPrc from a DIRECT public andersen_lake call (independent of
// the conventions input map — this is the S/K/T/r wiring check), greeks
// through the tool's own conventions chain (so an iteration-0 conventions
// rewrite regenerates the fixture rather than fighting it). The fixture keeps
// ddiv = 0 and passes sdiv as the q of the direct call, matching the stage-2
// hypothesis map.
[[nodiscard]] bool fill_oracle_outputs(FixtureRow &r) {
  const Side side = (r.cp == "Call") ? Side::Call : Side::Put;
  const auto price = atx::vol::andersen_lake(r.uprc, r.strike, r.years, r.srVol, r.rate, r.sdiv,
                                             side, atx::vol::al_fast_opts());
  if (!price.has_value()) {
    return false;
  }
  r.srPrc = *price;
  const EnginePricingInputs in = mode_a_inputs(to_oracle_row(r));
  const auto greeks = atx::vol::american_greeks_al(in.spot, in.strike, in.years, in.sigma,
                                                   in.rate, in.carry, in.side, mode_a_al_opts());
  if (!greeks.has_value()) {
    return false;
  }
  const auto carry = atx::vol::american_carry_greeks_al(in.spot, in.strike, in.years, in.sigma,
                                                        in.rate, in.carry, in.side,
                                                        mode_a_al_opts());
  if (!carry.has_value()) {
    return false;
  }
  const OracleUnitGreeks g = to_oracle_units(*greeks, carry->dP_dq);
  r.de = g.de;
  r.ga = g.ga;
  r.th = g.th;
  r.ve = g.ve;
  r.rh = g.rh;
  r.ph = g.ph;
  r.vo = g.vo;
  r.va = g.va;
  r.deDecay = g.de_decay;
  return true;
}

[[nodiscard]] std::string read_file(const fs::path &p) {
  std::ifstream f{p, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path &p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream f{p, std::ios::binary | std::ios::trunc};
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  ASSERT_TRUE(f.good());
}

// ── Bands ───────────────────────────────────────────────────────────────

TEST(OracleBenchBands, MoneynessCallEdgesAreHalfOpen) {
  EXPECT_EQ(moneyness_band(0.79, Side::Call), MoneynessBand::DeepItm);
  EXPECT_EQ(moneyness_band(0.80, Side::Call), MoneynessBand::Itm);  // edge -> band above
  EXPECT_EQ(moneyness_band(0.9499, Side::Call), MoneynessBand::Itm);
  EXPECT_EQ(moneyness_band(0.95, Side::Call), MoneynessBand::Atm);  // edge -> band above
  EXPECT_EQ(moneyness_band(1.0, Side::Call), MoneynessBand::Atm);
  EXPECT_EQ(moneyness_band(1.0499, Side::Call), MoneynessBand::Atm);
  EXPECT_EQ(moneyness_band(1.05, Side::Call), MoneynessBand::Otm);  // edge -> band above
  EXPECT_EQ(moneyness_band(1.1999, Side::Call), MoneynessBand::Otm);
  EXPECT_EQ(moneyness_band(1.20, Side::Call), MoneynessBand::DeepOtm); // edge -> band above
  EXPECT_EQ(moneyness_band(5.0, Side::Call), MoneynessBand::DeepOtm);
}

TEST(OracleBenchBands, MoneynessPutMirrorsCall) {
  EXPECT_EQ(moneyness_band(0.79, Side::Put), MoneynessBand::DeepOtm);
  EXPECT_EQ(moneyness_band(0.80, Side::Put), MoneynessBand::Otm);
  EXPECT_EQ(moneyness_band(0.95, Side::Put), MoneynessBand::Atm);
  EXPECT_EQ(moneyness_band(1.0, Side::Put), MoneynessBand::Atm);
  EXPECT_EQ(moneyness_band(1.05, Side::Put), MoneynessBand::Itm);
  EXPECT_EQ(moneyness_band(1.20, Side::Put), MoneynessBand::DeepItm);
}

TEST(OracleBenchBands, DteEdgesBelongToTheLowerBand) {
  EXPECT_EQ(dte_band(0.0), DteBand::D0To7);
  EXPECT_EQ(dte_band(7.0), DteBand::D0To7);   // day 7 is in "0-7"
  EXPECT_EQ(dte_band(7.5), DteBand::D8To30);
  EXPECT_EQ(dte_band(30.0), DteBand::D8To30); // day 30 is in "8-30"
  EXPECT_EQ(dte_band(30.5), DteBand::D31To90);
  EXPECT_EQ(dte_band(90.0), DteBand::D31To90); // day 90 is in "31-90"
  EXPECT_EQ(dte_band(90.5), DteBand::D90Plus);
  EXPECT_EQ(dte_band(400.0), DteBand::D90Plus);
  EXPECT_EQ(dte_band(-1.0), DteBand::D0To7); // stale negative collapses, no UB
}

TEST(OracleBenchBands, BandTokensMatchTheCharter) {
  EXPECT_EQ(to_string(MoneynessBand::DeepItm), "deep-itm");
  EXPECT_EQ(to_string(MoneynessBand::Itm), "itm");
  EXPECT_EQ(to_string(MoneynessBand::Atm), "atm");
  EXPECT_EQ(to_string(MoneynessBand::Otm), "otm");
  EXPECT_EQ(to_string(MoneynessBand::DeepOtm), "deep-otm");
  EXPECT_EQ(to_string(DteBand::D0To7), "0-7");
  EXPECT_EQ(to_string(DteBand::D8To30), "8-30");
  EXPECT_EQ(to_string(DteBand::D31To90), "31-90");
  EXPECT_EQ(to_string(DteBand::D90Plus), "90+");
  EXPECT_EQ(cp_token(Side::Call), "c");
  EXPECT_EQ(cp_token(Side::Put), "p");
}

// ── Tolerances ──────────────────────────────────────────────────────────

TEST(OracleBenchTolerance, PriceTickFloorWins) {
  EXPECT_DOUBLE_EQ(price_tolerance(1.00, 1.02), 0.01); // 10% of 0.02 < tick
}

TEST(OracleBenchTolerance, PriceSpreadFractionWins) {
  EXPECT_DOUBLE_EQ(price_tolerance(1.0, 2.0), 0.10);
}

TEST(OracleBenchTolerance, PriceCrossedMarketDegradesToTick) {
  EXPECT_DOUBLE_EQ(price_tolerance(2.0, 1.0), 0.01);
}

TEST(OracleBenchTolerance, VolUsesFiveBpAbsolute) {
  EXPECT_DOUBLE_EQ(vol_tolerance(), 5.0e-4);
}

TEST(OracleBenchTolerance, GreekRelativeWithAbsoluteFloor) {
  EXPECT_DOUBLE_EQ(greek_tolerance(0.0), 1.0e-4);
  EXPECT_DOUBLE_EQ(greek_tolerance(0.005), 1.0e-4); // 1% of 0.005 < floor
  EXPECT_DOUBLE_EQ(greek_tolerance(1.0), 0.01);
  EXPECT_DOUBLE_EQ(greek_tolerance(-2.0), 0.02); // |oracle| drives it
}

// ── Scorecard accounting ────────────────────────────────────────────────

TEST(OracleBenchScorecard, CellKeyMatchesCharterFormat) {
  EXPECT_EQ(cell_key("a", "price", MoneynessBand::Atm, DteBand::D0To7, Side::Call),
            "a.price.atm.0-7.c");
  EXPECT_EQ(cell_key("a", "deDecay", MoneynessBand::DeepItm, DteBand::D90Plus, Side::Put),
            "a.deDecay.deep-itm.90+.p");
}

TEST(OracleBenchScorecard, PercentilesAreNearestRank) {
  std::vector<double> s(100);
  for (std::size_t i = 0; i < s.size(); ++i) {
    s[i] = static_cast<double>(i + 1); // 1..100 sorted
  }
  EXPECT_DOUBLE_EQ(percentile_nearest_rank(s, 0.50), 50.0);
  EXPECT_DOUBLE_EQ(percentile_nearest_rank(s, 0.95), 95.0);
  EXPECT_DOUBLE_EQ(percentile_nearest_rank(s, 0.99), 99.0);
  EXPECT_DOUBLE_EQ(percentile_nearest_rank(s, 1.0), 100.0);
  const std::vector<double> tiny{1.0, 2.0, 3.0, 4.0};
  EXPECT_DOUBLE_EQ(percentile_nearest_rank(tiny, 0.50), 2.0); // ceil(2) -> rank 2
  EXPECT_DOUBLE_EQ(percentile_nearest_rank(tiny, 0.95), 4.0); // ceil(3.8) -> rank 4
  EXPECT_DOUBLE_EQ(percentile_nearest_rank({}, 0.5), 0.0);
}

TEST(OracleBenchScorecard, WithinTolAccountingAndStats) {
  Scorecard card;
  const auto put_obs = [&](double err, bool within) {
    card.observe("a", "price", MoneynessBand::Atm, DteBand::D0To7, Side::Call, err, within);
  };
  put_obs(1.0, true);
  put_obs(-2.0, true);
  put_obs(3.0, false);
  put_obs(-4.0, true);
  ASSERT_EQ(card.n_cells(), 1u);
  const auto stats = card.cell("a.price.atm.0-7.c");
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->n, 4);
  EXPECT_DOUBLE_EQ(stats->mae, 2.5);
  EXPECT_DOUBLE_EQ(stats->rmse, std::sqrt((1.0 + 4.0 + 9.0 + 16.0) / 4.0));
  EXPECT_DOUBLE_EQ(stats->p50, 2.0);
  EXPECT_DOUBLE_EQ(stats->p95, 4.0);
  EXPECT_DOUBLE_EQ(stats->p99, 4.0);
  EXPECT_DOUBLE_EQ(stats->max_abs, 4.0);
  EXPECT_DOUBLE_EQ(stats->within_tol_rate, 0.75);
}

TEST(OracleBenchScorecard, UnknownCellIsNotFound) {
  const Scorecard card;
  const auto stats = card.cell("a.price.atm.0-7.c");
  ASSERT_FALSE(stats.has_value());
  EXPECT_EQ(stats.error().code(), ErrorCode::NotFound);
}

TEST(OracleBenchScorecard, JsonCarriesHeaderModesTolerancesAndCells) {
  Scorecard card;
  card.observe("a", "price", MoneynessBand::Atm, DteBand::D0To7, Side::Call, 0.5, true);
  ModeStats stats;
  stats.rows_total = 10;
  stats.rows_priced = 7;
  stats.rows_null_sentinel = 2;
  stats.rows_bad_input = 1;
  stats.rows_engine_error = 0;
  stats.wall_seconds = 0.5;
  card.set_mode_stats("a", stats);
  const std::string json = card.to_json(ScorecardHeader{7, "deadbeef", "smoke"});
  EXPECT_NE(json.find("\"iter\": 7"), std::string::npos) << json;
  EXPECT_NE(json.find("\"git_sha\": \"deadbeef\""), std::string::npos);
  EXPECT_NE(json.find("\"cohort\": \"smoke\""), std::string::npos);
  EXPECT_NE(json.find("\"modes\""), std::string::npos);
  EXPECT_NE(json.find("\"rows_total\": 10"), std::string::npos);
  EXPECT_NE(json.find("\"rows_priced\": 7"), std::string::npos);
  EXPECT_NE(json.find("\"rows_null_sentinel\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"rows_bad_input\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"rows_engine_error\": 0"), std::string::npos);
  EXPECT_NE(json.find("\"wall_seconds\""), std::string::npos);
  EXPECT_NE(json.find("\"rows_per_second\""), std::string::npos);
  EXPECT_NE(json.find("\"tolerances\""), std::string::npos);
  EXPECT_NE(json.find("\"cells\""), std::string::npos);
  EXPECT_NE(json.find("\"a.price.atm.0-7.c\""), std::string::npos);
  EXPECT_NE(json.find("\"n\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"within_tol_rate\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"max\":"), std::string::npos); // charter field name
}

// ── Cohort JSON ─────────────────────────────────────────────────────────

namespace cohort_json {
constexpr std::string_view kValid = R"({
  "name": "smoke",
  "dates": ["2026-08-14"],
  "underliers": ["SPY", "QQQ"],
  "buckets_et": ["1000", "1330"],
  "notes": "why these were chosen"
})";
} // namespace cohort_json

TEST(OracleBenchCohort, ParsesTheReadmeSchema) {
  const auto cohort = parse_cohort_json(cohort_json::kValid);
  ASSERT_TRUE(cohort.has_value()) << cohort.error().to_string();
  EXPECT_EQ(cohort->name, "smoke");
  EXPECT_EQ(cohort->dates, (std::vector<std::string>{"2026-08-14"}));
  EXPECT_EQ(cohort->underliers, (std::vector<std::string>{"SPY", "QQQ"}));
  EXPECT_EQ(cohort->buckets_et, (std::vector<std::string>{"1000", "1330"}));
  EXPECT_EQ(cohort->notes, "why these were chosen");
}

TEST(OracleBenchCohort, ToleratesUnknownScalarKeys) {
  const auto cohort = parse_cohort_json(R"({
    "name": "x", "dates": ["2026-08-14"], "underliers": ["SPY"],
    "buckets_et": ["1000"], "notes": "n", "extra": 42, "tags": ["a", "b"]
  })");
  ASSERT_TRUE(cohort.has_value()) << cohort.error().to_string();
  EXPECT_EQ(cohort->name, "x");
}

TEST(OracleBenchCohort, RejectsMissingRequiredKey) {
  const auto cohort = parse_cohort_json(
      R"({"name": "x", "dates": ["2026-08-14"], "underliers": ["SPY"]})");
  ASSERT_FALSE(cohort.has_value());
}

TEST(OracleBenchCohort, RejectsWrongTypeForDates) {
  const auto cohort = parse_cohort_json(
      R"({"name": "x", "dates": "2026-08-14", "underliers": ["SPY"], "buckets_et": ["1000"]})");
  ASSERT_FALSE(cohort.has_value());
}

TEST(OracleBenchCohort, RejectsMalformedDate) {
  const auto cohort = parse_cohort_json(
      R"({"name": "x", "dates": ["2026/08/14"], "underliers": ["SPY"], "buckets_et": ["1000"]})");
  ASSERT_FALSE(cohort.has_value());
  EXPECT_EQ(cohort.error().code(), ErrorCode::InvalidArgument);
}

TEST(OracleBenchCohort, RejectsMalformedBucket) {
  const auto cohort = parse_cohort_json(
      R"({"name": "x", "dates": ["2026-08-14"], "underliers": ["SPY"], "buckets_et": ["93a0"]})");
  ASSERT_FALSE(cohort.has_value());
  EXPECT_EQ(cohort.error().code(), ErrorCode::InvalidArgument);
}

TEST(OracleBenchCohort, RejectsEmptyUnderliers) {
  const auto cohort = parse_cohort_json(
      R"({"name": "x", "dates": ["2026-08-14"], "underliers": [], "buckets_et": ["1000"]})");
  ASSERT_FALSE(cohort.has_value());
}

TEST(OracleBenchCohort, RejectsMalformedJson) {
  const auto cohort = parse_cohort_json(R"({"name": "x", )");
  ASSERT_FALSE(cohort.has_value());
  EXPECT_EQ(cohort.error().code(), ErrorCode::ParseError);
}

// ── Bench args ──────────────────────────────────────────────────────────
//
// Every --cohort value below is a PATH (it carries the ".json" suffix or a
// separator), which is the shape the Stage 3 gates pass. Bare NAMES are the
// new list form and are covered by OracleBenchCohortSpec with an injected
// manifest directory — parse_bench_args never probes the real repository.

// No manifest directory: only filesystem paths are admissible.
const fs::path kNoCohortDir{};

TEST(OracleBenchArgs, ParsesAllFlags) {
  const std::vector<std::string> argv{"--cohort", "c.json", "--store", "s",
                                      "--out",    "o.json", "--iter",  "3",
                                      "--git-sha", "abc123"};
  const auto args = parse_bench_args(argv, kNoCohortDir);
  ASSERT_TRUE(args.has_value()) << args.error().to_string();
  EXPECT_EQ(args->cohort_paths, std::vector<std::string>{"c.json"});
  EXPECT_EQ(args->store_root, "s");
  EXPECT_EQ(args->out_path, "o.json");
  EXPECT_EQ(args->iter, 3);
  EXPECT_EQ(args->git_sha, "abc123");
}

TEST(OracleBenchArgs, DefaultsIterZeroShaUnknown) {
  const std::vector<std::string> argv{"--cohort", "c.json", "--store", "s", "--out", "o"};
  const auto args = parse_bench_args(argv, kNoCohortDir);
  ASSERT_TRUE(args.has_value()) << args.error().to_string();
  EXPECT_EQ(args->iter, 0);
  EXPECT_EQ(args->git_sha, "unknown");
  EXPECT_EQ(args->mode, BenchMode::A); // --mode absent defaults to A
  EXPECT_FALSE(args->aggregate_only);
}

TEST(OracleBenchArgs, RejectsMissingRequiredFlag) {
  const std::vector<std::string> argv{"--cohort", "c.json", "--store", "s"};
  ASSERT_FALSE(parse_bench_args(argv, kNoCohortDir).has_value());
}

TEST(OracleBenchArgs, RejectsUnknownFlag) {
  const std::vector<std::string> argv{"--cohort", "c.json", "--store", "s",
                                      "--out",    "o",      "--nope",  "x"};
  ASSERT_FALSE(parse_bench_args(argv, kNoCohortDir).has_value());
}

TEST(OracleBenchArgs, RejectsNonIntegerIter) {
  const std::vector<std::string> argv{"--cohort", "c.json", "--store", "s", "--out", "o",
                                      "--iter",   "7q"};
  ASSERT_FALSE(parse_bench_args(argv, kNoCohortDir).has_value());
}

// ── The FROZEN oracle-loop command lines ────────────────────────────────
//
// These five strings are contract: RATCHET_GATE_COMMANDS and
// READY_MEASURE_GATES in .claude/workflows/vol-oracle-iter.js, plus the
// mode_b_smoke_tune gate in scripts/oracle-targeted-gate.ps1. They are
// asserted here VERBATIM because a parse regression on any of them kills the
// loop at argument parsing, three layers from the cause.
//
// The two `--cohort holdout` lines are asserted at PARSE only, and this is the
// only place holdout appears in the suite: benchmarking frozen holdout data
// outside a ratchet gate is exactly what the charter forbids.

// A manifest directory carrying the three real cohort names, so name
// resolution is exercised without reading the repository's own manifests.
// Resolution only tests for the file's existence, so the contents are inert.
[[nodiscard]] fs::path make_cohort_dir(const char *slug) {
  const fs::path dir = fresh_dir(slug);
  for (const std::string_view name : {"smoke", "tune", "holdout"}) {
    write_file(dir / (std::string{name} + ".json"), "{}");
  }
  return dir;
}

// Splits a frozen command STRING on spaces and drops argv[0], so the tests
// below can quote the workflow's own text instead of a hand-transcribed vector
// (transcription is exactly how a frozen string stops being frozen).
[[nodiscard]] std::vector<std::string> split_argv(std::string_view command) {
  std::vector<std::string> argv;
  std::size_t begin = 0;
  while (begin < command.size()) {
    const std::size_t space = command.find(' ', begin);
    const std::size_t end = space == std::string_view::npos ? command.size() : space;
    if (end > begin) {
      argv.emplace_back(command.substr(begin, end - begin));
    }
    begin = end + 1;
  }
  if (!argv.empty()) {
    argv.erase(argv.begin());
  }
  return argv;
}

TEST(OracleBenchArgs, ParsesFrozenMeasureModeACommand) {
  const fs::path dir = make_cohort_dir("frozen-a");
  const auto args = parse_bench_args(
      split_argv("atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only"),
      dir);
  ASSERT_TRUE(args.has_value()) << args.error().to_string();
  EXPECT_EQ(args->cohort_paths, (std::vector<std::string>{(dir / "smoke.json").string(),
                                                          (dir / "tune.json").string()}));
  EXPECT_EQ(args->mode, BenchMode::A);
  EXPECT_TRUE(args->scorecard);
  EXPECT_TRUE(args->aggregate_only);
  // --store/--out are absent: the store defaults to the licensed root and the
  // aggregate goes to stdout.
  EXPECT_EQ(args->store_root, std::string{kDefaultStoreRoot});
  EXPECT_TRUE(args->out_path.empty());
}

TEST(OracleBenchArgs, ParsesFrozenMeasureModeBCommand) {
  const fs::path dir = make_cohort_dir("frozen-b");
  const auto args = parse_bench_args(
      split_argv("atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only"),
      dir);
  ASSERT_TRUE(args.has_value()) << args.error().to_string();
  EXPECT_EQ(args->mode, BenchMode::B);
  EXPECT_TRUE(args->aggregate_only);
}

TEST(OracleBenchArgs, ParsesFrozenSpeedCommand) {
  const fs::path dir = make_cohort_dir("frozen-speed");
  const auto args = parse_bench_args(
      split_argv("atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 "
                 "--quiet-host --aggregate-only"),
      dir);
  ASSERT_TRUE(args.has_value()) << args.error().to_string();
  EXPECT_EQ(args->cohort_paths, std::vector<std::string>{(dir / "tune.json").string()});
  EXPECT_TRUE(args->benchmark_speed);
  EXPECT_TRUE(args->quiet_host);
  EXPECT_EQ(args->preset, "rel-avx2");
  EXPECT_TRUE(args->aggregate_only);
  // The gate layers name this metric; it is DERIVED from --preset.
  EXPECT_EQ(speed_metric_id(args->preset), "rel_avx2_rows_per_second");
}

TEST(OracleBenchArgs, ParsesFrozenHoldoutCommandsWithoutRunningThem) {
  const fs::path dir = make_cohort_dir("frozen-holdout");
  for (const std::string_view command :
       {std::string_view{"atx-vol-oracle-bench --cohort holdout --mode A --aggregate-only"},
        std::string_view{"atx-vol-oracle-bench --cohort holdout --mode B --aggregate-only"}}) {
    const auto args = parse_bench_args(split_argv(command), dir);
    ASSERT_TRUE(args.has_value()) << command << ": " << args.error().to_string();
    EXPECT_EQ(args->cohort_paths, std::vector<std::string>{(dir / "holdout.json").string()});
    EXPECT_TRUE(args->aggregate_only);
    EXPECT_FALSE(args->scorecard);
  }
}

// ── New flag semantics ──────────────────────────────────────────────────

TEST(OracleBenchArgs, RejectsUnknownModeValue) {
  const auto args = parse_bench_args(
      split_argv("x --cohort c.json --mode C --aggregate-only"), kNoCohortDir);
  ASSERT_FALSE(args.has_value());
  EXPECT_EQ(args.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(args.error().to_string().find("--mode"), std::string::npos)
      << args.error().to_string();
}

TEST(OracleBenchArgs, AggregateOnlyDefaultsStoreAndStdout) {
  const auto args =
      parse_bench_args(split_argv("x --cohort c.json --aggregate-only"), kNoCohortDir);
  ASSERT_TRUE(args.has_value()) << args.error().to_string();
  EXPECT_EQ(args->store_root, std::string{kDefaultStoreRoot});
  EXPECT_TRUE(args->out_path.empty());
}

TEST(OracleBenchArgs, ScorecardPathStillRequiresStoreAndOut) {
  // Without --aggregate-only the LEGACY shape is unchanged: both are required.
  EXPECT_FALSE(
      parse_bench_args(split_argv("x --cohort c.json --scorecard"), kNoCohortDir).has_value());
  EXPECT_FALSE(parse_bench_args(split_argv("x --cohort c.json --scorecard --store s"), kNoCohortDir)
                   .has_value());
  EXPECT_TRUE(parse_bench_args(split_argv("x --cohort c.json --scorecard --store s --out o.json"),
                               kNoCohortDir)
                  .has_value());
}

TEST(OracleBenchArgs, ConventionSweepRejectsTheNewFlags) {
  const std::string base = "x --convention-sweep --smoke s.json --tune t.json --store s --out o";
  ASSERT_TRUE(parse_bench_args(split_argv(base), kNoCohortDir).has_value());
  for (const std::string_view added :
       {" --mode B", " --scorecard", " --aggregate-only", " --benchmark-speed", " --quiet-host",
        " --preset rel-avx2"}) {
    EXPECT_FALSE(parse_bench_args(split_argv(base + std::string{added}), kNoCohortDir).has_value())
        << added;
  }
}

TEST(OracleBenchArgs, RejectsScorecardTogetherWithBenchmarkSpeed) {
  ASSERT_FALSE(parse_bench_args(
                   split_argv("x --cohort c.json --scorecard --benchmark-speed --aggregate-only"),
                   kNoCohortDir)
                   .has_value());
}

TEST(OracleBenchCohortSpec, ResolvesNamesAndKeepsPathsVerbatim) {
  const fs::path dir = make_cohort_dir("spec");
  const auto named =
      parse_bench_args(split_argv("x --cohort smoke --aggregate-only"), dir);
  ASSERT_TRUE(named.has_value()) << named.error().to_string();
  EXPECT_EQ(named->cohort_paths, std::vector<std::string>{(dir / "smoke.json").string()});

  // An absolute path (what the Stage 3 gates pass) is used byte-for-byte, and
  // is NOT looked up in the manifest directory.
  const std::string absolute = (fs::path{"C:/somewhere/else"} / "smoke.json").string();
  const std::vector<std::string> argv{"--cohort", absolute, "--store", "s", "--out", "o.json"};
  const auto by_path = parse_bench_args(argv, dir);
  ASSERT_TRUE(by_path.has_value()) << by_path.error().to_string();
  EXPECT_EQ(by_path->cohort_paths, std::vector<std::string>{absolute});
}

TEST(OracleBenchCohortSpec, RejectsUnresolvableNameAndDuplicates) {
  const fs::path dir = make_cohort_dir("spec-neg");
  const auto missing =
      parse_bench_args(split_argv("x --cohort smoke,nosuch --aggregate-only"), dir);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(missing.error().to_string().find("nosuch"), std::string::npos)
      << missing.error().to_string();

  // A cohort counted twice would double its weight in every aggregate.
  EXPECT_FALSE(
      parse_bench_args(split_argv("x --cohort smoke,smoke --aggregate-only"), dir).has_value());
  // Empty list element.
  EXPECT_FALSE(
      parse_bench_args(split_argv("x --cohort smoke, --aggregate-only"), dir).has_value());
  // A bare name with NO manifest directory says so instead of silently
  // treating it as a relative path.
  EXPECT_FALSE(
      parse_bench_args(split_argv("x --cohort smoke --aggregate-only"), kNoCohortDir).has_value());
}

TEST(OracleBenchCohortSpec, FindsTheManifestDirByWalkingUp) {
  const fs::path root = fresh_dir("repo-walk");
  const fs::path cohorts = root / "atx-vol" / "bench" / "oracle" / "cohorts";
  fs::create_directories(cohorts);
  const fs::path deep = root / "build-rel-avx2" / "bin";
  fs::create_directories(deep);
  EXPECT_EQ(find_cohort_dir(deep).lexically_normal(), cohorts.lexically_normal());
  EXPECT_EQ(find_cohort_dir(root).lexically_normal(), cohorts.lexically_normal());
  // A tree with no manifest directory yields an empty path, never a guess.
  EXPECT_TRUE(find_cohort_dir(fresh_dir("repo-walk-none")).empty());
}

TEST(OracleBenchQuietHost, RefusesWhenACompetingProcessIsRunning) {
  EXPECT_TRUE(require_quiet_host({}).has_value());
  const std::vector<std::string> busy{"ninja", "clang-cl"};
  const auto refused = require_quiet_host(busy);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  const std::string text = refused.error().to_string();
  EXPECT_NE(text.find("ninja"), std::string::npos) << text;
  EXPECT_NE(text.find("clang-cl"), std::string::npos) << text;
  // The registry MIRRORS $busyNames in scripts/oracle-targeted-gate.ps1; a
  // divergence means the in-binary check and the gate disagree about "quiet".
  const std::span<const std::string_view> names = quiet_host_busy_names();
  const std::set<std::string_view> actual(names.begin(), names.end());
  const std::set<std::string_view> expected{"clang-cl", "cl",      "link",
                                            "lld-link", "ninja",   "msbuild",
                                            "atx-vol-oracle-bench"};
  EXPECT_EQ(actual, expected);
  // The bench excludes ITSELF: this process is atx-vol-oracle-bench's TU under
  // test, and a probe that reported the caller could never pass.
  const auto probed = running_busy_processes();
  ASSERT_TRUE(probed.has_value()) << probed.error().to_string();
}

TEST(OracleBenchSpeedMetric, IdIsDerivedFromThePreset) {
  EXPECT_EQ(speed_metric_id("rel-avx2"), "rel_avx2_rows_per_second");
  EXPECT_EQ(speed_metric_id("rel"), "rel_rows_per_second");
  EXPECT_EQ(speed_metric_id(""), "rows_per_second");
}

// ── Cohort reader over a synthetic store ────────────────────────────────

// Store layout used by the reader tests:
//   date=2026-08-14/bucket_et=1000: AAA x3 good (srPrc 11/12/13), BBB x2 good
//     (srPrc 21/22), AAA null-bidIV, AAA null-error, AAA NaN-uPrc, AAA cp "X"
//     — written with LARGE_STRING columns (the polars/real-store shape)
//   date=2026-08-14/bucket_et=1330: AAA x2 good (srPrc 31/32) — written with
//     plain STRING columns, so the cross-bucket test proves both encodings
void build_reader_store(const fs::path &root) {
  std::vector<FixtureRow> b1000;
  for (const double marker : {11.0, 12.0, 13.0}) {
    FixtureRow r;
    r.srPrc = marker;
    b1000.push_back(r);
  }
  for (const double marker : {21.0, 22.0}) {
    FixtureRow r;
    r.tk = "BBB";
    r.srPrc = marker;
    b1000.push_back(r);
  }
  {
    FixtureRow r;
    r.null_bid_iv = true;
    b1000.push_back(r);
  }
  {
    FixtureRow r;
    r.null_error = true;
    b1000.push_back(r);
  }
  {
    FixtureRow r;
    r.uprc = std::numeric_limits<double>::quiet_NaN();
    b1000.push_back(r);
  }
  {
    FixtureRow r;
    r.cp = "X";
    b1000.push_back(r);
  }
  write_fixture_partition(root / "date=2026-08-14" / "bucket_et=1000", b1000);
  if (::testing::Test::HasFatalFailure()) { return; }

  std::vector<FixtureRow> b1330;
  for (const double marker : {31.0, 32.0}) {
    FixtureRow r;
    r.srPrc = marker;
    b1330.push_back(r);
  }
  write_fixture_partition(root / "date=2026-08-14" / "bucket_et=1330", b1330,
                          /*large_strings=*/false);
}

[[nodiscard]] Cohort make_cohort(std::vector<std::string> tks, std::vector<std::string> buckets) {
  Cohort c;
  c.name = "reader";
  c.dates = {"2026-08-14"};
  c.underliers = std::move(tks);
  c.buckets_et = std::move(buckets);
  return c;
}

TEST(OracleBenchReader, OpensOnlyCohortNamedPartitionsAndFiltersUnderlier) {
  const fs::path root = fresh_dir("reader-prune");
  ASSERT_NO_FATAL_FAILURE(build_reader_store(root));
  const auto scan = read_cohort_rows(make_cohort({"AAA"}, {"1000"}), root.string());
  ASSERT_TRUE(scan.has_value()) << scan.error().to_string();
  // No-full-scan evidence: exactly the one cohort-named dir was opened.
  const std::vector<std::string> want{(root / "date=2026-08-14" / "bucket_et=1000").string()};
  EXPECT_EQ(scan->partitions_opened, want);
  // Only AAA's three GOOD rows from that partition survive.
  ASSERT_EQ(scan->rows.size(), 3u);
  std::set<double> markers;
  for (const OracleRow &row : scan->rows) {
    EXPECT_EQ(row.underlier, "AAA");
    markers.insert(row.sr_prc);
  }
  EXPECT_EQ(markers, (std::set<double>{11.0, 12.0, 13.0}));
  // Sentinel-null rows counted, not admitted; bad-input rows likewise.
  EXPECT_EQ(scan->rows_null_sentinel, 2);
  EXPECT_EQ(scan->rows_bad_input, 2);
}

TEST(OracleBenchReader, CrossesUnderliersAndBuckets) {
  const fs::path root = fresh_dir("reader-cross");
  ASSERT_NO_FATAL_FAILURE(build_reader_store(root));
  const auto scan = read_cohort_rows(make_cohort({"AAA", "BBB"}, {"1000", "1330"}), root.string());
  ASSERT_TRUE(scan.has_value()) << scan.error().to_string();
  EXPECT_EQ(scan->partitions_opened.size(), 2u);
  // AAA: 3 + 2 across the buckets; BBB: 2 in bucket 1000, absent from 1330
  // (an underlier missing from a partition yields zero rows, NOT an error).
  EXPECT_EQ(scan->rows.size(), 7u);
  // PARTITION IDENTITY survives the flattening. read_cohort_rows concatenates
  // both partitions into one vector, so without these per-row fields a caller
  // cannot tell a 1000 row from a 1330 row — which is precisely the grouping
  // Mode B fits on. The srPrc markers are the fixture's partition witness:
  // 11/12/13 + 21/22 were written to bucket 1000, 31/32 to bucket 1330.
  std::map<std::string, std::set<double>> markers_by_bucket;
  for (const OracleRow &row : scan->rows) {
    EXPECT_EQ(row.date, "2026-08-14");
    markers_by_bucket[row.bucket_et].insert(row.sr_prc);
  }
  const std::map<std::string, std::set<double>> want{
      {"1000", {11.0, 12.0, 13.0, 21.0, 22.0}},
      {"1330", {31.0, 32.0}}};
  EXPECT_EQ(markers_by_bucket, want);
}

TEST(OracleBenchReader, MissingPartitionDirIsNotFound) {
  const fs::path root = fresh_dir("reader-missing");
  ASSERT_NO_FATAL_FAILURE(build_reader_store(root));
  const auto scan = read_cohort_rows(make_cohort({"AAA"}, {"1500"}), root.string());
  ASSERT_FALSE(scan.has_value());
  EXPECT_EQ(scan.error().code(), ErrorCode::NotFound);
}

// ── End to end ──────────────────────────────────────────────────────────

TEST(OracleBenchE2E, SyntheticCohortProducesCharterScorecard) {
  const fs::path root = fresh_dir("e2e");
  const fs::path out_path = root / "scorecards" / "iter-007.json";

  // 5 strikes x 4 dtes x 2 sides = 40 priced rows covering every moneyness and
  // dte band on both sides.
  const double strikes[] = {70.0, 90.0, 100.0, 110.0, 130.0};
  const double dtes[] = {5.0, 20.0, 60.0, 200.0};
  constexpr std::string_view kMetrics[] = {"price", "vol", "de", "ga", "th", "ve",
                                           "rh", "ph", "vo", "va", "deDecay"};
  std::vector<FixtureRow> rows;
  std::set<std::string> expected_keys;
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double strike : strikes) {
      for (const double dte : dtes) {
        FixtureRow r;
        r.cp = (side == Side::Call) ? "Call" : "Put";
        r.strike = strike;
        r.years = dte / 365.0;
        ASSERT_TRUE(fill_oracle_outputs(r)) << "strike " << strike << " dte " << dte;
        rows.push_back(r);
        const MoneynessBand mband = moneyness_band(strike / r.uprc, side);
        const DteBand dband = dte_band(dte);
        for (const std::string_view metric : kMetrics) {
          expected_keys.insert(cell_key("a", metric, mband, dband, side));
        }
      }
    }
  }
  // Sentinel-null rows: counted, never priced.
  for (int i = 0; i < 3; ++i) {
    FixtureRow r;
    r.null_bid_iv = (i == 0);
    r.null_ask_iv = (i == 1);
    r.null_error = (i == 2);
    rows.push_back(r);
  }
  // Bad-input row: counted, never priced.
  {
    FixtureRow r;
    r.uprc = std::numeric_limits<double>::quiet_NaN();
    rows.push_back(r);
  }
  // Poison decoys: same partition / other underlier, and other bucket. If
  // either leaks past the pushdown, the zero-error assertions below explode.
  for (int i = 0; i < 5; ++i) {
    FixtureRow r;
    r.tk = "ZZZ";
    r.srPrc = 9999.0;
    r.de = 9999.0;
    rows.push_back(r);
  }
  write_fixture_partition(root / "date=2026-08-14" / "bucket_et=1000", rows);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  {
    std::vector<FixtureRow> decoys;
    for (int i = 0; i < 4; ++i) {
      FixtureRow r;
      r.srPrc = 9999.0;
      decoys.push_back(r);
    }
    write_fixture_partition(root / "date=2026-08-14" / "bucket_et=1330", decoys);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }
  const fs::path cohort_path = root / "e2e-cohort.json";
  ASSERT_NO_FATAL_FAILURE(write_file(cohort_path, R"({
    "name": "e2e",
    "dates": ["2026-08-14"],
    "underliers": ["AAA"],
    "buckets_et": ["1000"],
    "notes": "synthetic oracle store fixture"
  })"));

  BenchArgs args;
  args.cohort_paths = {cohort_path.string()};
  args.store_root = root.string();
  args.out_path = out_path.string();
  args.git_sha = "deadbeef";
  args.iter = 7;

  ::testing::internal::CaptureStderr();
  const auto run = run_oracle_bench(args);
  const std::string err_text = ::testing::internal::GetCapturedStderr();
  ASSERT_TRUE(run.has_value()) << run.error().to_string();
  const Scorecard *const card = &run->card;

  // rows/s reported on stderr, plus the opened-partition evidence lines.
  EXPECT_NE(err_text.find("rows/s"), std::string::npos) << err_text;
  EXPECT_NE(err_text.find("bucket_et=1000"), std::string::npos) << err_text;
  EXPECT_EQ(err_text.find("bucket_et=1330"), std::string::npos) << err_text;

  // Cell keys are EXACTLY the charter schema over the fixture's band coverage.
  const std::vector<std::string> keys = card->cell_keys();
  EXPECT_EQ(std::set<std::string>(keys.begin(), keys.end()), expected_keys);
  const std::regex charter_key(
      R"(^a\.(price|vol|de|ga|th|ve|rh|ph|vo|va|deDecay)\.(deep-itm|itm|atm|otm|deep-otm)\.(0-7|8-30|31-90|90\+)\.(c|p)$)");
  for (const std::string &key : keys) {
    EXPECT_TRUE(std::regex_match(key, charter_key)) << key;
  }

  // The fixture's oracle outputs came from the same public engine entry points
  // the tool prices with, so every cell reproduces exactly: zero error, all
  // within tolerance.
  for (const std::string &key : keys) {
    const auto stats = card->cell(key);
    ASSERT_TRUE(stats.has_value()) << key;
    EXPECT_GE(stats->n, 1) << key;
    EXPECT_LE(stats->mae, 1.0e-9) << key;
    EXPECT_LE(stats->max_abs, 1.0e-9) << key;
    EXPECT_DOUBLE_EQ(stats->within_tol_rate, 1.0) << key;
  }

  // Scorecard file: header fields + row accounting, charter names verbatim.
  const std::string json = read_file(out_path);
  ASSERT_FALSE(json.empty());
  EXPECT_NE(json.find("\"iter\": 7"), std::string::npos);
  EXPECT_NE(json.find("\"git_sha\": \"deadbeef\""), std::string::npos);
  EXPECT_NE(json.find("\"cohort\": \"e2e\""), std::string::npos);
  EXPECT_NE(json.find("\"rows_total\": 44"), std::string::npos);
  EXPECT_NE(json.find("\"rows_priced\": 40"), std::string::npos);
  EXPECT_NE(json.find("\"rows_null_sentinel\": 3"), std::string::npos);
  EXPECT_NE(json.find("\"rows_bad_input\": 1"), std::string::npos);
  EXPECT_NE(json.find("\"rows_engine_error\": 0"), std::string::npos);
  EXPECT_NE(json.find("\"wall_seconds\""), std::string::npos);
  EXPECT_NE(json.find("\"rows_per_second\""), std::string::npos);
  EXPECT_NE(json.find("\"tolerances\""), std::string::npos);
  EXPECT_NE(json.find("\"a.price.deep-itm.0-7.c\""), std::string::npos);
}

// ── --aggregate-only, the confidentiality boundary ──────────────────────

// Two named cohorts over one synthetic store: enough to exercise the --cohort
// list, the aggregate metric set, and the leak assertions below. Returns the
// store root; writes <root>/<name>.json manifests beside it.
[[nodiscard]] fs::path build_aggregate_store(const char *tag) {
  const fs::path root = fresh_dir(tag);
  std::vector<FixtureRow> rows;
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double strike : {90.0, 100.0, 110.0}) {
      FixtureRow r;
      r.cp = (side == Side::Call) ? "Call" : "Put";
      r.strike = strike;
      if (!fill_oracle_outputs(r)) {
        return {};
      }
      rows.push_back(r);
    }
  }
  write_fixture_partition(root / "date=2026-08-14" / "bucket_et=1000", rows);
  write_fixture_partition(root / "date=2026-08-14" / "bucket_et=1330", rows);
  write_file(root / "alpha.json", R"({"name": "alpha", "dates": ["2026-08-14"],
    "underliers": ["AAA"], "buckets_et": ["1000"], "notes": "synthetic"})");
  write_file(root / "beta.json", R"({"name": "beta", "dates": ["2026-08-14"],
    "underliers": ["AAA"], "buckets_et": ["1330"], "notes": "synthetic"})");
  return root;
}

TEST(OracleBenchAggregate, PublishesTheElevenTargetsAndNoMembership) {
  const fs::path root = build_aggregate_store("agg");
  ASSERT_FALSE(root.empty());
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  const fs::path out_path = root / "aggregate.json";

  BenchArgs args;
  args.cohort_paths = {(root / "alpha.json").string(), (root / "beta.json").string()};
  args.store_root = root.string();
  args.out_path = out_path.string();
  args.aggregate_only = true;
  args.scorecard = true;
  args.git_sha = "deadbeef";

  ::testing::internal::CaptureStderr();
  const auto run = run_oracle_bench(args);
  const std::string err_text = ::testing::internal::GetCapturedStderr();
  ASSERT_TRUE(run.has_value()) << run.error().to_string();
  // Both cohorts' rows are ONE population.
  EXPECT_EQ(run->stats.rows_priced, 12);
  EXPECT_EQ(run->cohort_names, (std::vector<std::string>{"alpha", "beta"}));

  // MEMBERSHIP NEVER LEAVES: no partition line on stderr under --aggregate-only.
  EXPECT_EQ(err_text.find("bucket_et="), std::string::npos) << err_text;
  EXPECT_EQ(err_text.find("date="), std::string::npos) << err_text;
  EXPECT_EQ(err_text.find("partition"), std::string::npos) << err_text;

  const std::string json = read_file(out_path);
  ASSERT_FALSE(json.empty());
  EXPECT_NE(json.find("\"kind\": \"oracle_aggregate\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"mode\": \"A\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"cohorts\": [\"alpha\", \"beta\"]"), std::string::npos) << json;
  for (const std::string_view metric_id :
       {"mode_a_price_mae", "mode_a_vol_mae", "mode_a_delta_rel", "mode_a_gamma_rel",
        "mode_a_theta_rel", "mode_a_vega_rel", "mode_a_rho_rel", "mode_a_phi_rel",
        "mode_a_volga_rel", "mode_a_vanna_rel", "mode_a_delta_decay_rel"}) {
    EXPECT_NE(json.find(std::string{"\"metric_id\": \""} + std::string{metric_id} + "\""),
              std::string::npos)
        << metric_id << " missing from " << json;
  }
  EXPECT_NE(json.find("\"unit\": \"ticks\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"unit\": \"bp\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"unit\": \"relative\""), std::string::npos) << json;

  // NOTHING row-addressable: no cell key, no band token, no date, no bucket, no
  // underlier, no per-cell stats. This is the assertion the analyst stage's
  // tool-less confidentiality rests on.
  for (const std::string_view leak :
       {"a.price", "deep-itm", "0-7", "date=", "bucket_et=", "AAA", "cells", "within_tol_rate",
        "partitions"}) {
    EXPECT_EQ(json.find(leak), std::string::npos) << leak << " leaked into " << json;
  }
}

TEST(OracleBenchAggregate, WritesToStdoutWhenOutIsAbsent) {
  const fs::path root = build_aggregate_store("agg-stdout");
  ASSERT_FALSE(root.empty());
  ASSERT_FALSE(::testing::Test::HasFatalFailure());

  BenchArgs args;
  args.cohort_paths = {(root / "alpha.json").string()};
  args.store_root = root.string();
  args.aggregate_only = true;

  ::testing::internal::CaptureStdout();
  const auto run = run_oracle_bench(args);
  const std::string out_text = ::testing::internal::GetCapturedStdout();
  ASSERT_TRUE(run.has_value()) << run.error().to_string();
  EXPECT_NE(out_text.find("\"kind\": \"oracle_aggregate\""), std::string::npos) << out_text;
  EXPECT_NE(out_text.find("\"cohorts\": [\"alpha\"]"), std::string::npos) << out_text;
}

TEST(OracleBenchAggregate, BenchmarkSpeedPublishesRowsPerSecondOnly) {
  const fs::path root = build_aggregate_store("agg-speed");
  ASSERT_FALSE(root.empty());
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  const fs::path out_path = root / "speed.json";

  BenchArgs args;
  args.cohort_paths = {(root / "beta.json").string()};
  args.store_root = root.string();
  args.out_path = out_path.string();
  args.aggregate_only = true;
  args.benchmark_speed = true;
  args.preset = "rel-avx2";

  const auto run = run_oracle_bench(args);
  ASSERT_TRUE(run.has_value()) << run.error().to_string();
  const std::string json = read_file(out_path);
  ASSERT_FALSE(json.empty());
  // The SAME metric name and units the existing convention_speed gates read off
  // the scorecard (modes.a.rows_per_second), so the two are comparable.
  EXPECT_NE(json.find("\"metric_id\": \"rel_avx2_rows_per_second\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"unit\": \"rows_per_second\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"preset\": \"rel-avx2\""), std::string::npos) << json;
  // Speed publishes ONLY speed: no accuracy target rides along.
  EXPECT_EQ(json.find("mode_a_price_mae"), std::string::npos) << json;
}

TEST(OracleBenchModeB, FailsAtRunTimeWithADistinctActionableError) {
  BenchArgs args;
  // A store path that does not exist: proves the refusal happens BEFORE any
  // read, so --mode B over holdout can never touch frozen data.
  args.cohort_paths = {"no-such-cohort.json"};
  args.store_root = "C:/no-such-store-root";
  args.aggregate_only = true;
  args.mode = BenchMode::B;

  const auto run = run_oracle_bench(args);
  ASSERT_FALSE(run.has_value());
  EXPECT_EQ(run.error().code(), ErrorCode::NotImplemented);
  const std::string text = run.error().to_string();
  EXPECT_NE(text.find("--mode B"), std::string::npos) << text;
  // DISTINCT from a parse failure: the loop must be able to tell "the flag is
  // wrong" from "the mode is not built yet".
  EXPECT_EQ(text.find("unknown or valueless flag"), std::string::npos) << text;
  EXPECT_EQ(text.find("expected A or B"), std::string::npos) << text;
  // And it is emphatically not a number.
  EXPECT_EQ(text.find("rows_per_second"), std::string::npos) << text;
}

} // namespace
