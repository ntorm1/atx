// atx::impl — fundamental alpha zoo (lane 10): fixture contract + opt-in real-data IC run.
//
// Suite: FundamentalZoo
//
//  * FixtureParsesTypechecksAndEvaluates — every line of
//    fixtures/fundamental_zoo.txt parses, typechecks, compiles and evaluates on a
//    synthetic panel extended by fundamentals::with_fundamental_fields, yields at
//    least one finite cell, references at least one fundamental field, and ids
//    are unique. 40..80 lines.
//  * RealDataIcReport — OPT-IN (GTEST_SKIP unless ATX_L10_FUNDZOO_OUT is set).
//    Aligns the exported PIT snapshots (points.csv) onto each identified context
//    panel, evaluates every zoo expression plus a 12-1 momentum reference and
//    measures cross-sectional IC with the frozen engine unit
//    eval::compute_cross_section_ic, restricted to each context's evaluation
//    calendar year. Refuses any evaluated session on or after 2019-01-01 (2019
//    is held out; 2020+ is sealed). Writes CSVs into a fresh directory.
//
// Environment for the real run:
//   ATX_L10_FUNDZOO_OUT      fresh output directory (must not exist)
//   ATX_L10_FUND_POINTS      points.csv from atx-engine/tools/export_fundamental_fields.py
//   ATX_L10_CONTEXTS         ';'-separated identified context directories

#define _CRT_SECURE_NO_WARNINGS 1

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/data/fundamental_fields.hpp"
#include "atx/engine/eval/cross_section_ic.hpp"

#include "serialize_panel.hpp"

#ifndef ATX_IMPL_TESTS_DIR
#define ATX_IMPL_TESTS_DIR "."
#endif

namespace atx_test_l10_fundzoo_fundamental_zoo {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::engine::alpha::Panel;
namespace fund = atx::engine::data::fundamentals;
namespace eval = atx::engine::eval;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr i64 kDay = fund::kNanosPerDay;
constexpr i64 kHoldoutBeginNs = 17'897 * kDay; // 2019-01-01T00:00:00Z

struct ZooLine {
  std::string id;
  std::string expr;
};

std::vector<ZooLine> read_zoo(const std::string &path) {
  std::ifstream in(path);
  std::vector<ZooLine> out;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const auto b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto e = line.find_first_not_of(" \t", colon + 1);
    out.push_back({line.substr(b, colon - b), e == std::string::npos ? "" : line.substr(e)});
  }
  return out;
}

const atx::engine::alpha::Library &lib() {
  static const atx::engine::alpha::Library l;
  return l;
}

// Evaluate one expression; empty vector + message on failure.
std::vector<f64> evaluate(const std::string &expr, const Panel &panel, std::string &err) {
  using namespace atx::engine::alpha;
  auto ast = parse_expr(expr, lib());
  if (!ast.has_value()) {
    err = "parse: " + ast.error().message();
    return {};
  }
  auto ana = analyze(ast.value());
  if (!ana.has_value()) {
    err = "analyze: " + ana.error().message();
    return {};
  }
  auto prog = compile(ast.value(), ana.value());
  if (!prog.has_value()) {
    err = "compile: " + prog.error().message();
    return {};
  }
  Engine engine{panel};
  auto out = engine.evaluate(prog.value());
  if (!out.has_value() || out.value().alphas.empty()) {
    err = out.has_value() ? "no alpha" : "evaluate: " + out.error().message();
    return {};
  }
  return std::move(out.value().alphas[0].values);
}

// Synthetic context-shaped panel: 300 dates x 12 instruments, three sectors.
Panel synthetic_panel() {
  constexpr usize D = 300;
  constexpr usize I = 12;
  const std::vector<std::string> names{"close", "raw_close", "volume", "high", "low", "open",
                                       "market_cap", "sector", "earnFlag", "atmCenI_21d",
                                       "atmCenI_126d", "nEarnCnt_5d"};
  std::vector<std::vector<f64>> cols(names.size(), std::vector<f64>(D * I));
  for (usize d = 0; d < D; ++d) {
    for (usize i = 0; i < I; ++i) {
      const usize c = d * I + i;
      const f64 px = 20.0 + static_cast<f64>(i) + 3.0 * std::sin(0.05 * static_cast<f64>(d + 7 * i));
      cols[0][c] = px;
      cols[1][c] = px;
      cols[2][c] = 1.0e6 + 1.0e4 * static_cast<f64>(i);
      cols[3][c] = px * 1.01;
      cols[4][c] = px * 0.99;
      cols[5][c] = px * 0.995;
      cols[6][c] = px * 1.0e7 * (1.0 + static_cast<f64>(i));
      cols[7][c] = static_cast<f64>(i % 3);
      cols[8][c] = 0.0;
      cols[9][c] = 0.05;
      cols[10][c] = 0.07;
      cols[11][c] = 0.0;
    }
  }
  auto p = Panel::create(D, I, names, std::move(cols), {});
  return std::move(p.value());
}

// Quarterly filings with drifting fundamentals so deltas and SUE vary.
std::vector<fund::PitRecord> synthetic_records(usize instruments) {
  std::vector<fund::PitRecord> recs;
  for (usize i = 0; i < instruments; ++i) {
    for (usize q = 0; q < 5; ++q) {
      fund::PitRecord r{};
      r.instrument = i;
      r.period_end_ns = static_cast<i64>(q * 63) * kDay;
      r.available_ns = r.period_end_ns + 20 * kDay;
      const f64 s = 1.0 + 0.1 * static_cast<f64>(i) + 0.03 * static_cast<f64>(q * (i % 4));
      const f64 sign = (i % 5 == 0) ? -1.0 : 1.0;
      r.values = {40.0 * s, 100.0 * s, 60.0 * s, 90.0 * s,     10.0 * s * sign, 200.0 * s,
                  70.0 * s, 12.0 * s,  15.0 * s, 1.0e6 * s,  1.0e6,           0.5 * sign * s};
      recs.push_back(r);
    }
  }
  return recs;
}

std::optional<Panel> extended(const Panel &base, const std::vector<i64> &keys,
                              const std::vector<fund::PitRecord> &recs, std::string &err) {
  fund::AlignConfig cfg{};
  auto aligned = fund::align_pit_records(recs, keys, base.instruments(), cfg);
  if (!aligned.has_value()) {
    err = aligned.error().message();
    return std::nullopt;
  }
  auto mcap_id = base.field_id("market_cap");
  if (!mcap_id.has_value()) {
    err = "no market_cap";
    return std::nullopt;
  }
  auto derived = fund::derive_fields(*aligned, base.field_all(*mcap_id));
  if (!derived.has_value()) {
    err = derived.error().message();
    return std::nullopt;
  }
  auto out = fund::with_fundamental_fields(base, *aligned, *derived);
  if (!out.has_value()) {
    err = out.error().message();
    return std::nullopt;
  }
  return std::move(out.value());
}

bool references_fundamental(const std::string &expr) {
  for (auto n : fund::kRawFieldNames)
    if (expr.find(n) != std::string::npos) return true;
  for (auto n : fund::kDerivedFieldNames)
    if (expr.find(n) != std::string::npos) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Real-data helpers
// ---------------------------------------------------------------------------

// Days since 1970-01-01 of a proleptic Gregorian ISO date (Hinnant's algorithm).
std::optional<i64> epoch_day(std::string_view iso) {
  if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return std::nullopt;
  auto num = [&](usize at, usize n) {
    i64 v = 0;
    for (usize k = at; k < at + n; ++k) {
      if (iso[k] < '0' || iso[k] > '9') return i64{-1};
      v = v * 10 + (iso[k] - '0');
    }
    return v;
  };
  i64 y = num(0, 4);
  const i64 m = num(5, 2);
  const i64 d = num(8, 2);
  if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
  y -= m <= 2 ? 1 : 0;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const i64 yoe = y - era * 400;
  const i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, sep)) out.push_back(cur);
  if (!s.empty() && s.back() == sep) out.emplace_back();
  return out;
}

struct PointRow {
  std::string sr_id;
  fund::PitRecord rec;
};

std::vector<PointRow> read_points(const std::string &path, std::string &err) {
  std::ifstream in(path);
  std::vector<PointRow> out;
  std::string line;
  if (!std::getline(in, line)) {
    err = "empty points file";
    return out;
  }
  const auto header = split(line, ',');
  constexpr usize kKeys = 7;
  if (header.size() != kKeys + fund::kRawFieldCount) {
    err = "points header width";
    return out;
  }
  for (usize f = 0; f < fund::kRawFieldCount; ++f) {
    if (header[kKeys + f] != fund::kRawFieldNames[f]) {
      err = "points column order differs from RawField: " + header[kKeys + f];
      return out;
    }
  }
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto cells = split(line, ',');
    if (cells.size() != header.size()) continue;
    const auto avail = epoch_day(cells[3]);
    const auto pend = epoch_day(cells[4]);
    if (!avail || !pend) continue;
    PointRow row{cells[0], {}};
    row.rec.available_ns = *avail * kDay;
    row.rec.period_end_ns = *pend * kDay;
    for (usize f = 0; f < fund::kRawFieldCount; ++f) {
      const std::string &c = cells[kKeys + f];
      row.rec.values[f] = c.empty() ? kNaN : std::strtod(c.c_str(), nullptr);
    }
    out.push_back(std::move(row));
  }
  return out;
}

// Average-tie ranks of the finite entries (NaN stays NaN).
std::vector<f64> ranks(const std::vector<f64> &x) {
  std::vector<usize> idx;
  for (usize i = 0; i < x.size(); ++i)
    if (std::isfinite(x[i])) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [&](usize a, usize b) { return x[a] < x[b]; });
  std::vector<f64> r(x.size(), kNaN);
  for (usize k = 0; k < idx.size();) {
    usize j = k;
    while (j + 1 < idx.size() && x[idx[j + 1]] == x[idx[k]]) ++j;
    const f64 avg = 0.5 * static_cast<f64>(k + j);
    for (usize m = k; m <= j; ++m) r[idx[m]] = avg;
    k = j + 1;
  }
  return r;
}

f64 spearman(const std::vector<f64> &a, const std::vector<f64> &b) {
  std::vector<f64> x, y;
  for (usize i = 0; i < a.size(); ++i) {
    if (std::isfinite(a[i]) && std::isfinite(b[i])) {
      x.push_back(a[i]);
      y.push_back(b[i]);
    }
  }
  if (x.size() < 30) return kNaN;
  const auto rx = ranks(x);
  const auto ry = ranks(y);
  const f64 n = static_cast<f64>(rx.size());
  const f64 mx = std::accumulate(rx.begin(), rx.end(), 0.0) / n;
  const f64 my = std::accumulate(ry.begin(), ry.end(), 0.0) / n;
  f64 sxy = 0.0, sxx = 0.0, syy = 0.0;
  for (usize i = 0; i < rx.size(); ++i) {
    sxy += (rx[i] - mx) * (ry[i] - my);
    sxx += (rx[i] - mx) * (rx[i] - mx);
    syy += (ry[i] - my) * (ry[i] - my);
  }
  return (sxx > 0.0 && syy > 0.0) ? sxy / std::sqrt(sxx * syy) : kNaN;
}

struct YearResult {
  std::string context;
  i64 year{};
  std::string signal;
  std::array<f64, 2> rank_ic{kNaN, kNaN};
  std::array<f64, 2> rank_icir{kNaN, kNaN};
  std::array<f64, 2> rank_ic_lo{kNaN, kNaN};
  std::array<f64, 2> rank_ic_hi{kNaN, kNaN};
  std::array<f64, 2> spread_gross{kNaN, kNaN};
  std::array<f64, 2> decile_turnover{kNaN, kNaN};
  f64 implied_turnover{kNaN};
  f64 corr_momentum{kNaN};
  f64 coverage{kNaN}; // mean finite-signal names / eligible names
  f64 names_used{kNaN};
  std::vector<f64> ic21_series; // emitted per-date rank IC at h=21, in date order
};

} // namespace atx_test_l10_fundzoo_fundamental_zoo

namespace z = atx_test_l10_fundzoo_fundamental_zoo;

TEST(FundamentalZoo, FixtureParsesTypechecksAndEvaluates) {
  const auto zoo = z::read_zoo(std::string(ATX_IMPL_TESTS_DIR) + "/fixtures/fundamental_zoo.txt");
  ASSERT_GE(zoo.size(), 40U);
  ASSERT_LE(zoo.size(), 80U);
  const auto base = z::synthetic_panel();
  std::vector<atx::i64> keys(base.dates());
  for (atx::usize t = 0; t < keys.size(); ++t) keys[t] = static_cast<atx::i64>(t) * z::kDay;
  std::string err;
  const auto panel = z::extended(base, keys, z::synthetic_records(base.instruments()), err);
  ASSERT_TRUE(panel.has_value()) << err;
  std::set<std::string> ids;
  for (const auto &line : zoo) {
    EXPECT_TRUE(ids.insert(line.id).second) << "duplicate id " << line.id;
    EXPECT_TRUE(z::references_fundamental(line.expr)) << line.id << " uses no fundamental field";
    std::string e;
    const auto v = z::evaluate(line.expr, *panel, e);
    ASSERT_FALSE(v.empty()) << line.id << ": " << e;
    EXPECT_TRUE(std::any_of(v.begin(), v.end(), [](atx::f64 x) { return std::isfinite(x); }))
        << line.id << " produced no finite cell";
  }
}

TEST(FundamentalZoo, EpochDayMatchesKnownDates) {
  EXPECT_EQ(z::epoch_day("1970-01-01"), std::optional<atx::i64>{0});
  EXPECT_EQ(z::epoch_day("2019-01-01"), std::optional<atx::i64>{17'897});
  EXPECT_EQ(z::epoch_day("2012-12-24"), std::optional<atx::i64>{15'698});
  EXPECT_FALSE(z::epoch_day("2019-13-01").has_value());
  EXPECT_FALSE(z::epoch_day("20190101").has_value());
}

namespace atx_test_l10_fundzoo_fundamental_zoo {

// One context: align, extend, evaluate every signal, measure IC in its year.
void run_context(const std::filesystem::path &dir, const std::vector<PointRow> &points,
                 const std::vector<ZooLine> &zoo, std::vector<YearResult> &results,
                 nlohmann::json &align_log) {
  std::ifstream mf(dir / "context.bin.manifest.json");
  ASSERT_TRUE(mf.is_open()) << dir;
  const auto man = nlohmann::json::parse(mf);
  std::vector<std::string> ids;
  for (const auto &v : man["axes"]["instrument_ids"]) ids.push_back(v.get<std::string>());
  std::vector<i64> keys;
  for (const auto &v : man["axes"]["session_keys"]) keys.push_back(std::stoll(v.get<std::string>()));
  auto base = atx::impl::read_panel((dir / "context.bin").string());
  ASSERT_TRUE(base.has_value()) << base.error().message();
  ASSERT_EQ(base->dates(), keys.size());
  ASSERT_EQ(base->instruments(), ids.size());
  ASSERT_LT(keys.back(), kHoldoutBeginNs) << "context reaches the 2019 holdout";

  std::vector<std::string> rec_ids;
  rec_ids.reserve(points.size());
  for (const auto &p : points) rec_ids.push_back(p.sr_id);
  auto map = fund::map_security_ids(ids, rec_ids);
  ASSERT_TRUE(map.has_value());
  std::vector<fund::PitRecord> recs;
  for (usize k = 0; k < points.size(); ++k) {
    if (!(*map)[k].has_value()) continue;
    fund::PitRecord r = points[k].rec;
    r.instrument = *(*map)[k];
    recs.push_back(r);
  }
  const fund::AlignConfig cfg{};
  auto aligned = fund::align_pit_records(recs, keys, ids.size(), cfg);
  ASSERT_TRUE(aligned.has_value()) << aligned.error().message();
  auto mcap = base->field_id("market_cap");
  ASSERT_TRUE(mcap.has_value());
  auto derived = fund::derive_fields(*aligned, base->field_all(*mcap));
  ASSERT_TRUE(derived.has_value());
  auto panel = fund::with_fundamental_fields(*base, *aligned, *derived);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();

  // Evaluation window: the context's final calendar year.
  const i64 last_day = keys.back() / kDay;
  i64 year = 1970 + last_day / 366; // lower bound, then step to the containing year
  while (*epoch_day(std::to_string(year + 1) + "-01-01") <= last_day) ++year;
  const i64 year_start = *epoch_day(std::to_string(year) + "-01-01") * kDay;
  const usize t0 = static_cast<usize>(std::lower_bound(keys.begin(), keys.end(), year_start) - keys.begin());
  const usize D = keys.size() - t0;
  const usize I = ids.size();
  ASSERT_GT(D, 100U);

  auto close_id = panel->field_id("close");
  auto raw_id = panel->field_id("raw_close");
  ASSERT_TRUE(close_id.has_value() && raw_id.has_value());
  const auto close_all = panel->field_all(*close_id);
  const auto raw_all = panel->field_all(*raw_id);
  std::vector<f64> price(close_all.begin() + static_cast<std::ptrdiff_t>(t0 * I), close_all.end());
  std::vector<f64> raw_price(raw_all.begin() + static_cast<std::ptrdiff_t>(t0 * I), raw_all.end());
  std::vector<std::uint8_t> mask(D * I);
  usize eligible = 0;
  for (usize d = 0; d < D; ++d)
    for (usize i = 0; i < I; ++i) {
      mask[d * I + i] = panel->in_universe(t0 + d, i) ? 1U : 0U;
      eligible += mask[d * I + i];
    }
  const std::vector<std::uint8_t> zeros_u8(D * I, 0U);
  const std::vector<f64> zeros_f64(D * I, 0.0);
  const std::vector<i64> eval_keys(keys.begin() + static_cast<std::ptrdiff_t>(t0), keys.end());

  nlohmann::json alog;
  alog["context"] = dir.filename().string();
  alog["evaluation_year"] = year;
  alog["records_mapped"] = recs.size();
  alog["records_after_axis"] = aligned->stats.records_after_axis;
  for (usize f = 0; f < fund::kRawFieldCount; ++f) {
    usize fin = 0;
    for (usize d = 0; d < D; ++d)
      for (usize i = 0; i < I; ++i)
        fin += (mask[d * I + i] != 0U && std::isfinite(aligned->raw[f][(t0 + d) * I + i])) ? 1U : 0U;
    alog["eval_year_coverage"][std::string(fund::kRawFieldNames[f])] =
        eligible ? static_cast<f64>(fin) / static_cast<f64>(eligible) : 0.0;
  }
  align_log.push_back(alog);

  std::string err;
  const std::string mom_expr = "rank(delay(close, 21) / delay(close, 252) - 1)";
  const auto mom_full = evaluate(mom_expr, *panel, err);
  ASSERT_FALSE(mom_full.empty()) << err;

  std::vector<ZooLine> signals = zoo;
  signals.push_back({"ref_momentum_12_1", mom_expr});
  const std::array<usize, 2> horizons{5, 21};
  for (usize s = 0; s < signals.size(); ++s) {
    const auto full = evaluate(signals[s].expr, *panel, err);
    ASSERT_FALSE(full.empty()) << signals[s].id << ": " << err;
    std::vector<f64> sig(full.begin() + static_cast<std::ptrdiff_t>(t0 * I), full.end());
    eval::CrossSectionIcInput in{};
    in.dates = D;
    in.instruments = I;
    in.signal = sig;
    in.price = price;
    in.raw_price = raw_price;
    in.mask = mask;
    in.terminal = zeros_u8;
    in.terminal_evidenced = zeros_u8;
    in.terminal_value = zeros_f64;
    in.excluded_audited = zeros_u8;
    in.session_keys = eval_keys;
    eval::CrossSectionIcConfig icfg{};
    icfg.horizons = horizons;
    icfg.quantiles = 10;
    icfg.min_names_per_date = 30;
    icfg.bootstrap_draws = 1000;
    icfg.bootstrap_seed = 0x10F0'2026'0923ULL;
    icfg.stream_signal_index = s % 256;
    icfg.trade_bps = 10.0;
    icfg.annual_borrow_bps = 365.0;
    icfg.short_leg_gross = 1.0;
    icfg.forward_variant = eval::ForwardReturnVariant::DropMissingForward;
    icfg.ties = eval::IcTieHandling::AverageRanksV1;
    auto scratch = eval::plan_cross_section_ic(in, icfg);
    ASSERT_TRUE(scratch.has_value()) << scratch.error().message();
    auto res = eval::compute_cross_section_ic(in, icfg, *scratch);
    ASSERT_TRUE(res.has_value()) << signals[s].id << ": " << res.error().message();

    YearResult yr{};
    yr.context = dir.filename().string();
    yr.year = year;
    yr.signal = signals[s].id;
    for (usize h = 0; h < horizons.size(); ++h) {
      const auto &hs = res->horizons[h];
      if (hs.full.summary_reportable != 0U) {
        yr.rank_ic[h] = hs.full.rank_ic_mean;
        yr.rank_icir[h] = hs.full.rank_icir;
      }
      if (hs.full.rank_ic_mean_ci.reportable != 0U) {
        yr.rank_ic_lo[h] = hs.full.rank_ic_mean_ci.lo;
        yr.rank_ic_hi[h] = hs.full.rank_ic_mean_ci.hi;
      }
      if (hs.spread_reportable != 0U) yr.spread_gross[h] = hs.full.spread_gross_mean;
      yr.decile_turnover[h] = hs.decile_one_way_turnover;
    }
    yr.implied_turnover = res->autocorr.implied_one_way_turnover;
    f64 used = 0.0, fin_ratio = 0.0;
    usize n_dates = 0;
    for (const auto &p : res->horizons[1].series) {
      if (p.emitted != 0U) yr.ic21_series.push_back(p.rank_ic);
      used += static_cast<f64>(p.n_used);
      fin_ratio += p.n_eligible ? static_cast<f64>(p.n_signal_finite) / static_cast<f64>(p.n_eligible) : 0.0;
      ++n_dates;
    }
    yr.names_used = n_dates ? used / static_cast<f64>(n_dates) : kNaN;
    yr.coverage = n_dates ? fin_ratio / static_cast<f64>(n_dates) : kNaN;
    // Mean per-date Spearman correlation to 12-1 momentum over admitted cells.
    f64 csum = 0.0;
    usize cn = 0;
    for (usize d = 0; d < D; d += 5) {
      std::vector<f64> a(I, kNaN), b(I, kNaN);
      for (usize i = 0; i < I; ++i) {
        if (mask[d * I + i] == 0U) continue;
        a[i] = sig[d * I + i];
        b[i] = mom_full[(t0 + d) * I + i];
      }
      const f64 c = spearman(a, b);
      if (std::isfinite(c)) {
        csum += c;
        ++cn;
      }
    }
    yr.corr_momentum = cn ? csum / static_cast<f64>(cn) : kNaN;
    results.push_back(std::move(yr));
  }
}

std::string num(f64 v) {
  if (!std::isfinite(v)) return "";
  std::ostringstream o;
  o.precision(6);
  o << v;
  return o.str();
}

} // namespace atx_test_l10_fundzoo_fundamental_zoo

TEST(FundamentalZoo, RealDataIcReport) {
  const char *out_env = std::getenv("ATX_L10_FUNDZOO_OUT");
  if (out_env == nullptr || *out_env == '\0') GTEST_SKIP() << "opt-in: set ATX_L10_FUNDZOO_OUT";
  const char *points_env = std::getenv("ATX_L10_FUND_POINTS");
  const char *ctx_env = std::getenv("ATX_L10_CONTEXTS");
  ASSERT_TRUE(points_env != nullptr && ctx_env != nullptr);
  const std::filesystem::path out_dir{out_env};
  ASSERT_FALSE(std::filesystem::exists(out_dir)) << "output directory must be fresh";

  std::string err;
  const auto points = z::read_points(points_env, err);
  ASSERT_FALSE(points.empty()) << err;
  const auto zoo = z::read_zoo(std::string(ATX_IMPL_TESTS_DIR) + "/fixtures/fundamental_zoo.txt");
  ASSERT_FALSE(zoo.empty());

  std::vector<z::YearResult> results;
  nlohmann::json align_log = nlohmann::json::array();
  for (const auto &ctx : z::split(ctx_env, ';')) {
    if (ctx.empty()) continue;
    z::run_context(ctx, points, zoo, results, align_log);
    if (::testing::Test::HasFatalFailure()) return;
  }
  std::filesystem::create_directories(out_dir);
  {
    std::ofstream o(out_dir / "ic_by_year.csv");
    o << "context,year,signal,rank_ic_h5,rank_icir_h5,rank_ic_h21,rank_icir_h21,rank_ic_h21_lo,"
         "rank_ic_h21_hi,spread_gross_h21,decile_turnover_h21,implied_turnover,corr_momentum,"
         "signal_coverage,names_used\n";
    for (const auto &r : results) {
      o << r.context << ',' << r.year << ',' << r.signal << ',' << z::num(r.rank_ic[0]) << ','
        << z::num(r.rank_icir[0]) << ',' << z::num(r.rank_ic[1]) << ',' << z::num(r.rank_icir[1])
        << ',' << z::num(r.rank_ic_lo[1]) << ',' << z::num(r.rank_ic_hi[1]) << ','
        << z::num(r.spread_gross[1]) << ',' << z::num(r.decile_turnover[1]) << ','
        << z::num(r.implied_turnover) << ',' << z::num(r.corr_momentum) << ','
        << z::num(r.coverage) << ',' << z::num(r.names_used) << '\n';
    }
  }
  {
    // Pooled over years (per context cut): mean of the per-date h=21 rank IC,
    // ICIR, a non-overlapping t-statistic (every 21st emitted date) and sign
    // stability of the yearly means.
    std::map<std::pair<std::string, std::string>, std::vector<const z::YearResult *>> groups;
    for (const auto &r : results) {
      const auto cut = r.context.find("t3000") != std::string::npos ? "t3000" : "t1000";
      groups[{cut, r.signal}].push_back(&r);
    }
    std::ofstream o(out_dir / "zoo_pooled.csv");
    o << "cut,signal,years,pooled_rank_ic_h21,pooled_icir_h21,t_nonoverlap_h21,years_positive,"
         "mean_rank_ic_h5,mean_implied_turnover,mean_corr_momentum,mean_coverage\n";
    for (const auto &[key, rows] : groups) {
      std::vector<atx::f64> all, nov;
      atx::usize pos = 0;
      atx::f64 h5 = 0.0, to = 0.0, cm = 0.0, cov = 0.0;
      atx::usize n5 = 0, nto = 0, ncm = 0, ncov = 0;
      for (const auto *r : rows) {
        for (atx::usize k = 0; k < r->ic21_series.size(); ++k) {
          all.push_back(r->ic21_series[k]);
          if (k % 21 == 0) nov.push_back(r->ic21_series[k]);
        }
        pos += (std::isfinite(r->rank_ic[1]) && r->rank_ic[1] > 0.0) ? 1U : 0U;
        if (std::isfinite(r->rank_ic[0])) { h5 += r->rank_ic[0]; ++n5; }
        if (std::isfinite(r->implied_turnover)) { to += r->implied_turnover; ++nto; }
        if (std::isfinite(r->corr_momentum)) { cm += r->corr_momentum; ++ncm; }
        if (std::isfinite(r->coverage)) { cov += r->coverage; ++ncov; }
      }
      auto mean_sd = [](const std::vector<atx::f64> &v) {
        if (v.size() < 2) return std::pair<atx::f64, atx::f64>{z::kNaN, z::kNaN};
        const atx::f64 m = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<atx::f64>(v.size());
        atx::f64 ss = 0.0;
        for (atx::f64 x : v) ss += (x - m) * (x - m);
        return std::pair<atx::f64, atx::f64>{m, std::sqrt(ss / static_cast<atx::f64>(v.size() - 1))};
      };
      const auto [m, sd] = mean_sd(all);
      const auto [mn, sdn] = mean_sd(nov);
      const atx::f64 t = (sdn > 0.0) ? mn / (sdn / std::sqrt(static_cast<atx::f64>(nov.size()))) : z::kNaN;
      o << key.first << ',' << key.second << ',' << rows.size() << ',' << z::num(m) << ','
        << z::num(sd > 0.0 ? m / sd : z::kNaN) << ',' << z::num(t) << ',' << pos << ','
        << z::num(n5 ? h5 / static_cast<atx::f64>(n5) : z::kNaN) << ','
        << z::num(nto ? to / static_cast<atx::f64>(nto) : z::kNaN) << ','
        << z::num(ncm ? cm / static_cast<atx::f64>(ncm) : z::kNaN) << ','
        << z::num(ncov ? cov / static_cast<atx::f64>(ncov) : z::kNaN) << '\n';
    }
  }
  std::ofstream(out_dir / "alignment.json") << align_log.dump(2);
  nlohmann::json man;
  man["schema"] = "atx.fundamental-zoo-ic/v1";
  man["points"] = points_env;
  man["contexts"] = ctx_env;
  man["zoo_lines"] = zoo.size();
  man["trials_declared"] = zoo.size() * 2;
  man["horizons"] = {5, 21};
  man["align"] = {{"lag_sessions", 1}, {"max_days_since_available", 400},
                  {"max_days_since_period_end", 550}};
  man["ic_engine"] = "atx::engine::eval::compute_cross_section_ic (DropMissingForward, "
                     "AverageRanksV1, Q=10, min_names 30, 1000 draws, trade 10 bps, borrow 365 bps)";
  man["holdout"] = "2019 held out, >=2020 sealed; every evaluated session < 2019-01-01";
  std::ofstream(out_dir / "manifest.json") << man.dump(2);
}
