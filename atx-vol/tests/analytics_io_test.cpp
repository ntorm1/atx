// Tests for the CSV serializers (analytics.hpp).
//
// For each writer we build a struct by hand, serialize it to a temp path, then
// read the bytes back and assert on structure: the `# key=` meta lines are
// present, the header row is present, and the data-row count matches the number
// of tenors / grid points. We also assert an un-creatable path yields an error
// Status. Temp files are removed at the end of each test.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "atx/vol/analytics.hpp"

namespace atx::vol {
namespace {

// ── Read-back helpers ───────────────────────────────────────────────────────

// Slurp a file as raw bytes (binary, so the writer's `\n` survives verbatim).
std::string slurp(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Split on `\n`, dropping a trailing `\r` (defensive) and any final empty line.
std::vector<std::string> lines_of(const std::string &text) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : text) {
    if (c == '\n') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

// Is there a line that begins with `prefix`?
bool has_line_prefix(const std::string &text, const std::string &prefix) {
  for (const auto &ln : lines_of(text)) {
    if (ln.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

// Non-comment, non-empty lines after the first (which is the header row).
std::size_t count_data_rows(const std::string &text) {
  std::size_t data = 0;
  bool seen_header = false;
  for (const auto &ln : lines_of(text)) {
    if (ln.empty() || ln[0] == '#') {
      continue;
    }
    if (!seen_header) {
      seen_header = true; // first non-comment line == header row
      continue;
    }
    ++data;
  }
  return data;
}

std::filesystem::path temp_path(const char *name) {
  return std::filesystem::temp_directory_path() / name;
}

// A path whose parent directory does not exist — std::fopen("wb") must fail on
// it (ENOENT), giving a portable "un-creatable" target without depending on a
// particular drive letter being absent.
std::filesystem::path uncreatable_path() {
  return std::filesystem::temp_directory_path() / "atx-analytics-io-nonexistent-dir-xyz" /
         "out.csv";
}

void remove_quietly(const std::filesystem::path &p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

// ── Struct builders ─────────────────────────────────────────────────────────

RiskNeutralDensity make_rnd() {
  RiskNeutralDensity r;
  r.T = 90.0 / 365.25;
  r.forward = 101.5;
  r.df = 0.998;
  r.strikes = {90.0, 100.0, 110.0};
  r.pdf = {0.1, 0.6, 0.3};
  r.cdf = {0.1, 0.7, 1.0};
  r.mean = 101.4;
  r.variance = 25.0;
  r.skewness = -0.4;
  r.kurtosis = 3.6;
  r.bkm_variance = 0.05;
  r.bkm_skew = -0.5;
  r.bkm_kurt = 3.7;
  r.skew_index = 105.0;
  r.mass_before_norm = 0.987;
  r.quantile_p = {0.05, 0.50, 0.95};
  r.quantile_k = {88.0, 101.0, 116.0};
  r.prob_below_forward = 0.53;
  r.valid = true;
  return r;
}

// ── Kept scaffold assertion ────────────────────────────────────────────────

TEST(AnalyticsIo, RndStructDefaults) {
  const RiskNeutralDensity r{};
  EXPECT_FALSE(r.valid);
  EXPECT_TRUE(r.strikes.empty());
}

// ── write_surface_analytics_csv ────────────────────────────────────────────

TEST(AnalyticsIo, WriteSurfaceAnalytics) {
  SurfaceAnalytics a;
  a.uid = 42;
  a.as_of_ts_ns = 1700000000000000000LL;
  a.spot = 100.5;
  a.implied_emove = 0.03;
  a.ts_slope_1m_3m = 0.01;
  a.ts_slope_3m_1y = -0.005;
  a.ts_ratio_1m_3m = 1.05;
  a.backwardation = true;
  a.valid = true;

  TenorAnalytics t0;
  t0.tenor_years = 0.0833;
  t0.label = "1m";
  t0.forward = 101.0;
  t0.df = 0.999;
  t0.atm_vol = 0.25;
  t0.atm_vol_ex_earn = 0.22;
  t0.n_earnings = 1;
  t0.put_delta_vol = {0.28, 0.30};
  t0.call_delta_vol = {0.24, 0.23};
  t0.risk_reversal = {0.04, 0.07};
  t0.butterfly = {0.01, 0.02};
  t0.skew_slope = -0.5;
  t0.curvature = 1.2;
  t0.moneyness_vol = {0.30, 0.27, 0.25, 0.24, 0.26};
  t0.skew_90_110 = 0.04;
  t0.var_swap_vol = 0.26;
  t0.convexity_premium = 0.01;
  t0.expected_move = 0.02;
  t0.rnd_skewness = -0.3;
  t0.rnd_kurtosis = 3.5;
  t0.prob_below_forward = 0.52;
  t0.valid = true;

  // Second tenor intentionally leaves the aligned wing/moneyness vectors empty
  // (and a shorter-than-4/5 set on t0) to exercise the bounds guard.
  TenorAnalytics t1;
  t1.tenor_years = 0.25;
  t1.label = "3m";
  t1.forward = 102.0;
  t1.df = 0.997;
  t1.atm_vol = 0.24;
  t1.valid = true;

  a.tenors = {t0, t1};

  const auto path = temp_path("atx-analytics-io-surface.csv");
  const auto st = write_surface_analytics_csv(a, path.string());
  ASSERT_TRUE(st.has_value());

  const std::string text = slurp(path);
  EXPECT_TRUE(has_line_prefix(text, "# uid="));
  EXPECT_TRUE(has_line_prefix(text, "# as_of_ts_ns="));
  EXPECT_TRUE(has_line_prefix(text, "# spot="));
  EXPECT_TRUE(has_line_prefix(text, "# implied_emove="));
  EXPECT_TRUE(has_line_prefix(text, "# ts_slope_1m_3m="));
  EXPECT_TRUE(has_line_prefix(text, "# ts_slope_3m_1y="));
  EXPECT_TRUE(has_line_prefix(text, "# ts_ratio_1m_3m="));
  EXPECT_TRUE(has_line_prefix(text, "# backwardation="));
  EXPECT_TRUE(has_line_prefix(text, "# valid="));
  EXPECT_TRUE(has_line_prefix(text, "tenor_years,label,forward,df,"));
  // Wing + moneyness columns must be part of the header.
  EXPECT_NE(text.find("put_delta_vol_0"), std::string::npos);
  EXPECT_NE(text.find("call_delta_vol_3"), std::string::npos);
  EXPECT_NE(text.find("rr_0"), std::string::npos);
  EXPECT_NE(text.find("bf_3"), std::string::npos);
  EXPECT_NE(text.find("mvol_4"), std::string::npos);
  EXPECT_EQ(count_data_rows(text), a.tenors.size());

  remove_quietly(path);
}

TEST(AnalyticsIo, WriteSurfaceAnalyticsBadPathFails) {
  SurfaceAnalytics a;
  a.valid = true;
  const auto st = write_surface_analytics_csv(a, uncreatable_path().string());
  EXPECT_FALSE(st.has_value());
}

// ── write_surface_diff_csv ─────────────────────────────────────────────────

TEST(AnalyticsIo, WriteSurfaceDiff) {
  SurfaceDiff d;
  d.ts1_ns = 1700000000000000000LL;
  d.ts2_ns = 1700000086400000000LL;
  d.spot1 = 100.0;
  d.spot2 = 101.0;
  d.d_spot = 1.0;
  d.log_return = 0.00995;
  d.sticky_strike_atm_pred = -0.004;
  d.sticky_delta_atm_pred = 0.0;
  d.residual_atm_move = 0.002;
  d.valid = true;

  TenorDiff a;
  a.tenor_years = 0.0833;
  a.label = "1m";
  a.d_forward = 1.0;
  a.d_atm_vol = -0.002;
  a.d_vol_fixed_strike = -0.003;
  a.d_vol_fixed_delta = -0.001;
  a.d_skew_slope = 0.01;
  a.d_risk_reversal_25 = 0.005;
  a.d_butterfly_25 = -0.002;
  a.valid = true;

  TenorDiff b;
  b.tenor_years = 0.25;
  b.label = "3m";
  b.d_atm_vol = -0.0015;
  b.valid = true;

  d.tenors = {a, b};

  const auto path = temp_path("atx-analytics-io-diff.csv");
  const auto st = write_surface_diff_csv(d, path.string());
  ASSERT_TRUE(st.has_value());

  const std::string text = slurp(path);
  EXPECT_TRUE(has_line_prefix(text, "# ts1_ns="));
  EXPECT_TRUE(has_line_prefix(text, "# ts2_ns="));
  EXPECT_TRUE(has_line_prefix(text, "# spot1="));
  EXPECT_TRUE(has_line_prefix(text, "# spot2="));
  EXPECT_TRUE(has_line_prefix(text, "# d_spot="));
  EXPECT_TRUE(has_line_prefix(text, "# log_return="));
  EXPECT_TRUE(has_line_prefix(text, "# sticky_strike_atm_pred="));
  EXPECT_TRUE(has_line_prefix(text, "# sticky_delta_atm_pred="));
  EXPECT_TRUE(has_line_prefix(text, "# residual_atm_move="));
  EXPECT_TRUE(has_line_prefix(text, "# valid="));
  EXPECT_TRUE(has_line_prefix(text, "tenor_years,label,d_forward,d_atm_vol,"));
  EXPECT_EQ(count_data_rows(text), d.tenors.size());

  remove_quietly(path);
}

TEST(AnalyticsIo, WriteSurfaceDiffBadPathFails) {
  SurfaceDiff d;
  d.valid = true;
  const auto st = write_surface_diff_csv(d, uncreatable_path().string());
  EXPECT_FALSE(st.has_value());
}

// ── write_rnd_csv ──────────────────────────────────────────────────────────

TEST(AnalyticsIo, WriteRnd) {
  const RiskNeutralDensity r = make_rnd();

  const auto path = temp_path("atx-analytics-io-rnd.csv");
  const auto st = write_rnd_csv(r, path.string());
  ASSERT_TRUE(st.has_value());

  const std::string text = slurp(path);
  EXPECT_TRUE(has_line_prefix(text, "# T="));
  EXPECT_TRUE(has_line_prefix(text, "# forward="));
  EXPECT_TRUE(has_line_prefix(text, "# df="));
  EXPECT_TRUE(has_line_prefix(text, "# mean="));
  EXPECT_TRUE(has_line_prefix(text, "# variance="));
  EXPECT_TRUE(has_line_prefix(text, "# skewness="));
  EXPECT_TRUE(has_line_prefix(text, "# kurtosis="));
  EXPECT_TRUE(has_line_prefix(text, "# bkm_variance="));
  EXPECT_TRUE(has_line_prefix(text, "# bkm_skew="));
  EXPECT_TRUE(has_line_prefix(text, "# bkm_kurt="));
  EXPECT_TRUE(has_line_prefix(text, "# skew_index="));
  EXPECT_TRUE(has_line_prefix(text, "# mass_before_norm="));
  EXPECT_TRUE(has_line_prefix(text, "# prob_below_forward="));
  EXPECT_TRUE(has_line_prefix(text, "# valid="));
  // One quantile meta line per (p, k) pair.
  EXPECT_TRUE(has_line_prefix(text, "# quantile_"));
  EXPECT_TRUE(has_line_prefix(text, "strike,pdf,cdf"));
  EXPECT_EQ(count_data_rows(text), r.strikes.size());

  remove_quietly(path);
}

TEST(AnalyticsIo, WriteRndBadPathFails) {
  const RiskNeutralDensity r = make_rnd();
  const auto st = write_rnd_csv(r, uncreatable_path().string());
  EXPECT_FALSE(st.has_value());
}

} // namespace
} // namespace atx::vol
