// Platform P9 B2 fix round 1 (rulings B2-R1, B2-R2, B2-9): the eval verb on the inputs where a
// port of the Python statistics cannot, or did not, match them bit for bit
// (fixtures/research_eval_edges, written by its generator from today's Python):
//   - paired cells at a stated 1 - rho: with 1 - rho >= 1e-6 every float ties within
//     1e-12 / (1 - rho) and the counts are equal; below 1e-6 the pair is refused as degenerate.
//   - a CSCV grid with a constant nonzero candidate: its Sharpe is 0 in every split, as in Python;
//     and a grid where it wins splits and ties a zero column (re-review 1 S7).
//   - the house DSR rows at N from 2 to 1e6: the bisection quantile, SR* and the DSR tie within
//     1e-12 for every N (the engine's Acklam quantile did not, past N = 1e3).
// The series are regenerated here bit for bit (each step one IEEE double operation), so no series
// file is committed.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/types.hpp"
#include "atx/engine/eval/deflated_sharpe_house.hpp"
#include "atx/engine/eval/stats_ext.hpp"
#include "atx/engine/research/eval/cscv_report.hpp"
#include "atx/engine/research/eval/eval_report.hpp"
#include "atx/engine/research/eval/paired.hpp"

namespace {

namespace fs = std::filesystem;
namespace ev = atx::engine::research::eval;
using atx::f64;
using atx::i64;
using atx::u64;
using atx::usize;
using Json = nlohmann::json;

constexpr u64 kLcgMul = 6364136223846793005ULL;
constexpr u64 kLcgAdd = 1442695040888963407ULL;

std::string read_bytes(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const Json &fixture() {
  static const Json doc =
      Json::parse(read_bytes(fs::path(ATX_EVAL_EDGES_FIXTURE) / "expected.json"));
  return doc;
}

// The generator's LCG uniforms: s <- s * mul + add (mod 2^64), u = (s >> 11) * 2^-53.
std::vector<f64> uniforms(u64 seed, usize n) {
  std::vector<f64> out(n);
  u64 s = seed;
  for (usize k = 0; k < n; ++k) {
    s = s * kLcgMul + kLcgAdd;
    out[k] = static_cast<f64>(s >> 11U) * 0x1p-53;
  }
  return out;
}

// One IEEE operation per statement (no contraction), as the Python generator.
f64 scaled(f64 u, f64 scale, f64 drift) {
  const f64 centred = u - 0.5;
  const f64 spread = centred * scale;
  return spread + drift;
}

std::vector<f64> reference() {
  const Json &lcg = fixture()["lcg"];
  const usize t = fixture()["paired"]["sessions"].get<usize>();
  std::vector<f64> out;
  for (const f64 u : uniforms(lcg["a_seed"].get<u64>(), t)) {
    out.push_back(scaled(u, 0.02, 0.0004));
  }
  return out;
}

std::vector<f64> cell(f64 eps, f64 shift) {
  const Json &lcg = fixture()["lcg"];
  const std::vector<f64> a = reference();
  const std::vector<f64> n = uniforms(lcg["n_seed"].get<u64>(), a.size());
  std::vector<f64> out(a.size());
  for (usize k = 0; k < a.size(); ++k) {
    const f64 noise = n[k] - 0.5;
    const f64 base = a[k] - shift;
    const f64 step = eps * noise;
    out[k] = base + step;
  }
  return out;
}

f64 nan_of(const Json &v) {
  return v.is_null() ? std::numeric_limits<f64>::quiet_NaN() : v.get<f64>();
}

// Python's math.isclose(rel_tol=rel, abs_tol=1e-300), NaN equal to NaN (null).
void expect_tied(f64 got, const Json &want, f64 rel, const std::string &what) {
  const f64 w = nan_of(want);
  if (std::isnan(w) || std::isnan(got)) {
    EXPECT_TRUE(std::isnan(w) && std::isnan(got)) << what << ": " << got << " vs " << w;
    return;
  }
  std::ostringstream s;
  s.precision(17);
  s << what << ": engine " << got << " Python " << w << " rel " << rel;
  EXPECT_LE(std::fabs(got - w), std::fmax(rel * std::fmax(std::fabs(w), std::fabs(got)), 1e-300))
      << s.str();
}

void expect_opt_tied(const std::optional<f64> &got, const Json &want, f64 rel,
                     const std::string &what) {
  if (want.is_null()) {
    EXPECT_FALSE(got.has_value()) << what;
    return;
  }
  ASSERT_TRUE(got.has_value()) << what;
  expect_tied(*got, want, rel, what);
}

ev::PairedStats run_case(const Json &c) {
  const Json &par = fixture()["paired"];
  ev::PairedConfig cfg;
  cfg.draws = par["draws"].get<usize>();
  cfg.block = par["block"].get<usize>();
  cfg.seed = par["seed"].get<u64>();
  cfg.one_sided = par["one_sided"].get<bool>();
  const std::vector<f64> a = reference();
  const std::vector<f64> b = cell(c["eps"].get<f64>(), c["shift"].get<f64>());
  auto r = ev::paired_stats(b, a, cfg);
  EXPECT_TRUE(r.has_value());
  return r.has_value() ? *r : ev::PairedStats{};
}

TEST(EvalEdges, FixtureCoversTheRulingsCases) {
  const Json &cases = fixture()["paired"]["cases"];
  std::vector<std::string> names;
  for (const Json &c : cases) {
    names.push_back(c["name"].get<std::string>());
  }
  for (const char *want : {"omr_5e-4", "omr_5e-5", "omr_5e-6", "omr_1.02e-6", "omr_0.98e-6",
                           "omr_5e-9", "omr_6e-13", "self"}) {
    EXPECT_NE(std::find(names.begin(), names.end(), want), names.end()) << want;
  }
  EXPECT_EQ(fixture()["paired"]["degenerate_below"].get<f64>(), ev::kDegenerateOneMinusRho);
}

// Ruling B2-R1 (b): with 1 - rho >= 1e-6 every paired float within 1e-12 / (1 - rho) of the Python
// and every count (bootstrap validity, both p-values) equal.
TEST(EvalEdges, PairedTiesWithinTheConditioningBound) {
  usize checked = 0;
  for (const Json &c : fixture()["paired"]["cases"]) {
    if (c["degenerate"].get<bool>()) {
      continue;
    }
    const std::string name = c["name"].get<std::string>();
    const f64 omr = c["one_minus_rho"].get<f64>();
    ASSERT_GE(omr, ev::kDegenerateOneMinusRho) << name;
    const f64 rel = 1e-12 / omr;
    const Json &py = c["python"];
    const ev::PairedStats p = run_case(c);
    EXPECT_FALSE(p.degenerate) << name;
    EXPECT_NEAR(p.one_minus_rho / omr, 1.0, 1e-9) << name;
    expect_tied(p.sr, py["sr"], 1e-12, name + ".sr");
    expect_tied(p.sr_reference, py["sr_reference"], 1e-12, name + ".sr_reference");
    expect_tied(p.dsr, py["dsr"], rel, name + ".dsr");
    expect_tied(p.rho, py["rho"], 1e-12, name + ".rho");
    expect_tied(p.memmel_se, py["memmel_se"], rel, name + ".memmel_se");
    expect_opt_tied(p.t, py["t"], rel, name + ".t");
    expect_tied(p.cbb_lo, py["cbb_ci95"][0], rel, name + ".cbb_lo");
    expect_tied(p.cbb_hi, py["cbb_ci95"][1], rel, name + ".cbb_hi");
    EXPECT_EQ(p.cbb_valid_draws, py["cbb_valid_draws"].get<usize>()) << name;
    const Json &lw = py["lw"];
    expect_tied(p.lw.dsr_population, lw["dsr_population"], rel, name + ".lw.dsr_population");
    expect_tied(p.lw.se, lw["se"], rel, name + ".lw.se");
    expect_opt_tied(p.lw.ci_lo, lw["ci95"][0], rel, name + ".lw.ci_lo");
    expect_opt_tied(p.lw.ci_hi, lw["ci95"][1], rel, name + ".lw.ci_hi");
    EXPECT_EQ(p.lw.valid_draws, lw["valid_draws"].get<usize>()) << name;
    // the decision counts: p = (count + 1) / (valid + 1), equal bits iff equal counts
    ASSERT_TRUE(p.lw.p_value.has_value() && p.lw.p_one_sided.has_value()) << name;
    EXPECT_EQ(*p.lw.p_value, lw["p_value"].get<f64>()) << name;
    EXPECT_EQ(*p.lw.p_one_sided, lw["p_one_sided"].get<f64>()) << name;
    ++checked;
  }
  EXPECT_EQ(checked, 4U); // 5e-4, 5e-5, 5e-6 and 1.02e-6
}

// Ruling B2-R1 (c): below 1 - rho = 1e-6 (and a cell paired with itself, ruling B2-4) SE, t and the
// LW CI and p-values are refused; dSR, rho, the CBB interval and d0 are still reported.
TEST(EvalEdges, PairedRefusesADegeneratePair) {
  usize checked = 0;
  for (const Json &c : fixture()["paired"]["cases"]) {
    if (!c["degenerate"].get<bool>()) {
      continue;
    }
    const std::string name = c["name"].get<std::string>();
    const f64 omr = c["one_minus_rho"].get<f64>();
    ASSERT_LT(omr, ev::kDegenerateOneMinusRho) << name;
    const Json &py = c["python"];
    const ev::PairedStats p = run_case(c);
    EXPECT_TRUE(p.degenerate) << name;
    EXPECT_TRUE(std::isnan(p.memmel_se)) << name;
    EXPECT_FALSE(p.t.has_value()) << name;
    EXPECT_TRUE(std::isnan(p.lw.se)) << name;
    EXPECT_FALSE(p.lw.ci_lo.has_value() || p.lw.ci_hi.has_value()) << name;
    EXPECT_FALSE(p.lw.p_value.has_value() || p.lw.p_one_sided.has_value()) << name;
    EXPECT_EQ(p.lw.valid_draws, 0U) << name;
    const f64 rel = omr > 0.0 ? 1e-12 / omr : 0.0;
    expect_tied(p.sr, py["sr"], 1e-12, name + ".sr");
    expect_tied(p.sr_reference, py["sr_reference"], 1e-12, name + ".sr_reference");
    expect_tied(p.dsr, py["dsr"], rel, name + ".dsr");
    expect_tied(p.cbb_lo, py["cbb_ci95"][0], rel, name + ".cbb_lo");
    expect_tied(p.cbb_hi, py["cbb_ci95"][1], rel, name + ".cbb_hi");
    EXPECT_EQ(p.cbb_valid_draws, py["cbb_valid_draws"].get<usize>()) << name;
    ++checked;
  }
  EXPECT_EQ(checked, 4U); // 0.98e-6, 5e-9, 6e-13 and the self pair
}

// The verb's document and text for a degenerate pair: null SE / t / p, the flag and one line.
TEST(EvalEdges, VerbPrintsADegeneratePairAsRefused) {
  const fs::path dir = fs::temp_directory_path() / "atx_research_eval_edges";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const std::vector<f64> a = reference();
  std::string text = "session_ns,net_return\n";
  for (usize t = 0; t < a.size(); ++t) {
    std::ostringstream row;
    row.precision(17);
    row << (1'577'923'200'000'000'000LL + static_cast<i64>(t)) << "," << a[t] << "\n";
    text += row.str();
  }
  const fs::path csv = dir / "self.csv";
  {
    std::ofstream f(csv, std::ios::binary | std::ios::trunc);
    f << text;
  }
  ev::EvalRequest rq;
  rq.daily.push_back(ev::LabeledPath{"cell", csv.string()});
  rq.paired = csv.string();
  rq.paired_config.draws = 99;
  const auto doc = ev::evaluate(rq);
  ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
  const Json &p = (*doc)["cells"][0]["paired"];
  EXPECT_TRUE(p["degenerate"].get<bool>());
  EXPECT_EQ(p["one_minus_rho"].get<f64>(), 0.0);
  for (const char *key : {"memmel_se", "t", "se", "p_one", "p_two"}) {
    EXPECT_TRUE(p[key].is_null()) << key;
  }
  EXPECT_TRUE(p["lw"]["p_value"].is_null() && p["lw"]["se"].is_null());
  EXPECT_EQ(p["dsr"].get<f64>(), 0.0);
  EXPECT_TRUE((*doc)["cells"][0]["conditional"].is_null()); // no SE: not in the lineage
  const std::string out = ev::eval_text(*doc);
  EXPECT_NE(out.find("Memmel SE na | LW SE na p two-sided na one-sided na"), std::string::npos)
      << out;
  EXPECT_NE(out.find("paired: degenerate pair (1 - rho 0.000e+00 < 1e-06)"), std::string::npos)
      << out;
  fs::remove_all(dir, ec);
}

// The engine's CSCV report of a fixture grid (the generated candidates, stream seed + c at drift c,
// then the given constant columns), with every value tied to the section's Python: PBO, winner
// counts, the rank distribution and the logits exactly, the means and quantiles within 1e-12.
ev::CscvReport tied_cscv(const Json &cs, const std::vector<f64> &constants) {
  const Json &py = cs["python"];
  const usize t = cs["sessions"].get<usize>();
  const std::vector<f64> drifts = cs["drifts"].get<std::vector<f64>>();
  const usize n = drifts.size() + constants.size();
  std::vector<f64> m(t * n);
  for (usize c = 0; c < drifts.size(); ++c) {
    const std::vector<f64> u = uniforms(cs["seed"].get<u64>() + c, t);
    for (usize s = 0; s < t; ++s) {
      m[s * n + c] = scaled(u[s], 0.02, drifts[c]);
    }
  }
  for (usize k = 0; k < constants.size(); ++k) {
    for (usize s = 0; s < t; ++s) {
      m[s * n + drifts.size() + k] = constants[k];
    }
  }
  const auto r = ev::cscv_report(m, t, n, cs["blocks"].get<usize>());
  EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  if (!r.has_value()) {
    return {};
  }
  EXPECT_EQ(r->pbo, py["pbo"].get<f64>());
  EXPECT_EQ(r->winner_counts, py["winner_counts"].get<std::vector<usize>>());
  EXPECT_EQ(r->rank_splits, py["logit_distribution"]["splits"].get<std::vector<usize>>());
  EXPECT_EQ(r->logits, py["logits"].get<std::vector<f64>>());
  expect_tied(r->logit_mean, py["logit_mean"], 1e-12, "logit_mean");
  expect_tied(r->winner_is_sr_annual_mean, py["winner_is_sr_annual_mean"], 1e-12, "winner_is");
  expect_tied(r->winner_oos_sr_annual_mean, py["winner_oos_sr_annual_mean"], 1e-12, "winner_oos");
  EXPECT_EQ(r->prob_winner_oos_loss, py["prob_winner_oos_loss"].get<f64>());
  expect_opt_tied(r->degradation_slope, py["degradation_slope"], 1e-12, "degradation_slope");
  const char *q[] = {"p5", "p25", "p50", "p75", "p95"};
  for (usize k = 0; k < r->logit_quantiles.size(); ++k) {
    expect_tied(r->logit_quantiles[k], py["logit_quantiles"][q[k]], 1e-12, q[k]);
  }
  return *r;
}

// Ruling B2-R2: a constant nonzero candidate has Sharpe 0 in every split, so the PBO, the logit
// distribution and every winner statistic equal the Python's (the engine alone gave PBO 0: the
// constant column's rounding-residue sd made it win every split).
TEST(EvalEdges, CscvConstantCandidateHasSharpeZero) {
  const Json &cs = fixture()["cscv"];
  const ev::CscvReport r = tied_cscv(cs, {cs["constant"].get<f64>()});
  EXPECT_EQ(r.constant_candidates, std::vector<usize>{cs["drifts"].size()});
}

// Re-review 1 S7: every generated candidate drifts negative, so the constant wins the splits where
// all of them lose in sample; the zero column after it has Sharpe 0 too and loses every tie by
// index (the Python's first maximum in sample and its OOS rank rule): the path B2-R2 rests on.
TEST(EvalEdges, CscvConstantWinsAndTiesTheZeroColumnByIndex) {
  const Json &cs = fixture()["cscv_wins"];
  const usize k = cs["drifts"].size();
  const ev::CscvReport r = tied_cscv(cs, {cs["constant"].get<f64>(), 0.0});
  EXPECT_EQ(r.constant_candidates, (std::vector<usize>{k, k + 1U}));
  ASSERT_EQ(r.winner_counts.size(), k + 2U);
  EXPECT_GT(r.winner_counts[k], 0U);                // the constant wins some splits
  EXPECT_LT(r.winner_counts[k], r.splits);          // not every split
  EXPECT_EQ(r.winner_counts[k + 1U], 0U);           // the zero column never: the tie keeps index k
}

// ---------------------------------------------------------------------------- ruling B2-9
const Json &house() { return fixture()["house_dsr"]; }

f64 rel_diff(f64 got, f64 want) {
  return std::fabs(got - want) / std::fmax(std::fabs(want), std::fabs(got));
}

// The tie rule over a Python dict: every key of `want` in `got`, floats within 1e-12 relative,
// integers, strings and nulls exact (extra engine keys allowed).
void tie_json(const Json &want, const Json &got, const std::string &path,
              std::vector<std::string> &bad) {
  if (want.is_object()) {
    for (const auto &[k, v] : want.items()) {
      if (!got.is_object() || !got.contains(k)) {
        bad.push_back(path + "." + k + ": missing");
        continue;
      }
      tie_json(v, got[k], path + "." + k, bad);
    }
    return;
  }
  if (want.is_number_float()) {
    const f64 w = want.get<f64>();
    const f64 g = got.is_number() ? got.get<f64>() : std::numeric_limits<f64>::quiet_NaN();
    if (!(std::fabs(g - w) <= std::fmax(1e-12 * std::fmax(std::fabs(w), std::fabs(g)), 1e-300))) {
      std::ostringstream s;
      s.precision(17);
      s << path << ": engine " << g << " Python " << w;
      bad.push_back(s.str());
    }
    return;
  }
  if (want != got) {
    bad.push_back(path + ": engine " + got.dump() + " Python " + want.dump());
  }
}

// The house quantile is the Python bisection: at 1 - 1/N and 1 - 1/(N e), N from 2 to 1e6, it
// ties the Python value within 1e-12 (33 of the 34 quantiles are the same double; ppf_lo at
// N = 1e4 is 6 ulps away, 6.7e-16 relative: one ulp of erfc at a midpoint flips one halving).
// The engine's Acklam + Halley quantile, which the house rows used before, is printed for the
// record only (a note, not an assertion on another function: review B2 re-review 1 S6).
TEST(EvalEdges, HouseNormPpfIsThePythonBisectionForAnyN) {
  const Json &h = house();
  ASSERT_EQ(h["n"].front().get<usize>(), 2U);
  ASSERT_EQ(h["n"].back().get<usize>(), 1'000'000U);
  usize same = 0;
  usize total = 0;
  std::string differs;
  for (const Json &q : h["norm_ppf"]) {
    const usize n = q["n"].get<usize>();
    const f64 nf = static_cast<f64>(n);
    const f64 p_hi = 1.0 - 1.0 / nf;
    const f64 p_lo = 1.0 - 1.0 / (nf * std::numbers::e);
    EXPECT_EQ(p_hi, q["p_hi"].get<f64>()) << n; // the same probabilities as the Python
    EXPECT_EQ(p_lo, q["p_lo"].get<f64>()) << n;
    for (const auto &[p, key] : {std::pair<f64, const char *>{p_hi, "ppf_hi"},
                                 std::pair<f64, const char *>{p_lo, "ppf_lo"}}) {
      const f64 want = q[key].get<f64>();
      const f64 got = atx::engine::eval::house_norm_ppf(p);
      expect_tied(got, q[key], 1e-12, std::string(key) + " N " + std::to_string(n));
      ++total;
      if (got == want) {
        ++same;
      } else {
        std::ostringstream s;
        s.precision(17);
        s << " " << key << "@N=" << n << " engine " << got << " Python " << want;
        differs += s.str();
      }
    }
  }
  // The engine's SR* (Acklam + Halley quantile) on the same grid: what the house rows took before.
  f64 acklam_worst = 0.0;
  usize acklam_n = 0;
  for (const Json &m : h["direct"]) {
    for (const Json &row : m["rows"]) {
      const usize n = row["n"].get<usize>();
      const f64 d = rel_diff(
          atx::engine::eval::expected_max_sharpe(n, m["variance"].get<f64>()),
          row["sr0_daily"].get<f64>());
      if (d > acklam_worst) {
        acklam_worst = d;
        acklam_n = n;
      }
    }
  }
  std::printf("[ house_norm_ppf ] %zu of %zu quantiles bit-equal to the Python;%s\n"
              "[ house_norm_ppf ] the Acklam SR* (before B2-9) worst rel %.3e at N %zu\n",
              same, total, differs.empty() ? " all" : differs.c_str(), acklam_worst, acklam_n);
  EXPECT_TRUE(std::isnan(atx::engine::eval::house_norm_ppf(1.0)));
  EXPECT_TRUE(std::isnan(atx::engine::eval::house_norm_ppf(0.0)));
}

// SR* and the DSR of house_dsr (every house row's formula) at each moment set and V, N 2 to 1e6.
TEST(EvalEdges, HouseDsrTiesThePythonForAnyN) {
  usize checked = 0;
  for (const Json &m : house()["direct"]) {
    const std::string label = m["label"].get<std::string>();
    for (const Json &row : m["rows"]) {
      const usize n = row["n"].get<usize>();
      const atx::engine::eval::HouseDsr d = atx::engine::eval::house_dsr(
          m["sr_daily"].get<f64>(), m["sessions"].get<usize>(), m["skew"].get<f64>(),
          m["kurtosis"].get<f64>(), n, m["variance"].get<f64>());
      const std::string what = label + " N " + std::to_string(n);
      expect_tied(d.sr0_daily, row["sr0_daily"], 1e-12, what + " sr0");
      expect_opt_tied(d.dsr, row["dsr"], 1e-12, what + " dsr");
      ++checked;
    }
  }
  EXPECT_EQ(checked, 3U * house()["n"].size());
}

// End to end: the verb's cross and lo rows at --dsr-n N on two generated cells.
TEST(EvalEdges, VerbHouseRowsTieThePythonForAnyN) {
  const fs::path dir = fs::temp_directory_path() / "atx_research_eval_house";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const Json &h = house();
  std::vector<f64> b;
  for (const f64 u : uniforms(h["seed"].get<u64>(), reference().size())) {
    b.push_back(scaled(u, 0.02, h["drift"].get<f64>()));
  }
  std::vector<fs::path> csv;
  for (const std::vector<f64> &x : {reference(), b}) {
    std::string text = "session_ns,net_return\n";
    for (usize t = 0; t < x.size(); ++t) {
      std::ostringstream row;
      row.precision(17);
      row << (1'577'923'200'000'000'000LL + static_cast<i64>(t)) << "," << x[t] << "\n";
      text += row.str();
    }
    csv.push_back(dir / ("cell" + std::to_string(csv.size()) + ".csv"));
    std::ofstream f(csv.back(), std::ios::binary | std::ios::trunc);
    f << text;
  }
  std::vector<std::string> bad;
  for (const Json &v : h["verb"]) {
    ev::EvalRequest rq;
    rq.daily.push_back(ev::LabeledPath{"a", csv[0].string()});
    rq.daily.push_back(ev::LabeledPath{"b", csv[1].string()});
    rq.dsr_n = v["n"].get<usize>();
    const auto doc = ev::evaluate(rq);
    ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
    const std::string at = "N " + std::to_string(rq.dsr_n);
    for (usize k = 0; k < 2U; ++k) {
      tie_json(h["verb_moments"][k], (*doc)["cells"][k]["net_moments"], at + " moments", bad);
      tie_json(v["cross"][k], (*doc)["cells"][k]["dsr"]["cross"], at + " cross", bad);
    }
    tie_json(v["lo"], (*doc)["cells"][0]["dsr"]["lo"], at + " lo", bad);
  }
  EXPECT_TRUE(bad.empty()) << bad.size() << " difference(s), first: " << bad.front();
  fs::remove_all(dir, ec);
}

} // namespace
