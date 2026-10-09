// DIAGNOSTIC (lane/oracle-exercise-style-20260819) — NOT production code.
//
// Answers ONE question, per underlier root, on the sanctioned smoke+tune
// population: does SpiderRock's srPrc reproduce the AMERICAN premium or the
// EUROPEAN one, on our exact convention inputs?
//
// It exists because the exercise-style axis added to `ConventionMap` must be
// chosen on evidence rather than on a plausible-sounding ticker list. The sweep
// measures which RULE wins; this measures the per-ROOT fact each rule is
// claimed to encode, so a root that is in the rule and a root that is not can
// both be checked against the same number.
//
// Both legs are priced for EVERY root, deliberately ignoring the map's routing:
// a root the rule does not route is a negative control, and a diagnostic that
// only priced the leg the rule already chose could not produce one.
//
// Usage:
//   oracle_exercise_style_diag --store <store-root> --cohort <cohort.json> [...]
//
// stdout: CSV `underlier,rows,american_mae_ticks,european_mae_ticks,
//              european_closer_frac,european_exact_frac,american_exact_frac`
// where *_exact_frac is the fraction of rows that leg reproduces to within one
// tenth of a cent per share.
//
// NEVER point this at holdout. It reads whatever cohort JSON it is given, and
// reading holdout to pick a convention would destroy the only unbiased estimate
// the loop will ever have of this change.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "oracle_cohort_reader.hpp"
#include "oracle_conventions.hpp"

namespace {

using atx::vol::AmericanGreeks;
using atx::vol::Result;
using atx::vol::oracle::Cohort;
using atx::vol::oracle::ConventionMap;
using atx::vol::oracle::EnginePricingInputs;
using atx::vol::oracle::InputModel;
using atx::vol::oracle::OracleRow;

// Per-root accumulation. `exact` counts reproductions inside a tenth of a cent
// per share — two orders below the 1-tick price tolerance, so it separates
// "this is the leg" from "this leg happens to be close here".
constexpr double kExactTolerance = 0.001;

struct RootStats {
  long long rows = 0;
  double american_abs = 0.0;
  double european_abs = 0.0;
  long long european_closer = 0;
  long long european_exact = 0;
  long long american_exact = 0;
};

} // namespace

int main(int argc, char **argv) {
  std::string store;
  std::vector<std::string> cohort_paths;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--store") == 0 && i + 1 < argc) {
      store = argv[++i];
    } else if (std::strcmp(argv[i], "--cohort") == 0 && i + 1 < argc) {
      cohort_paths.emplace_back(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (store.empty() || cohort_paths.empty()) {
    std::fprintf(stderr, "usage: --store <root> --cohort <cohort.json> [--cohort ...]\n");
    return 2;
  }

  // The pinned input model, so the two legs differ ONLY in exercise style and
  // the number reported is attributable to that axis alone.
  ConventionMap map = atx::vol::oracle::baseline_convention();
  map.input_model = InputModel::DiscreteDividendPvSdivYield;

  std::map<std::string, RootStats> by_root;
  long long refused = 0;
  for (const std::string &path : cohort_paths) {
    const Result<Cohort> cohort = atx::vol::oracle::load_cohort_json(path);
    if (!cohort.has_value()) {
      std::fprintf(stderr, "cohort %s: %s\n", path.c_str(), cohort.error().to_string().c_str());
      return 1;
    }
    const auto scan = atx::vol::oracle::read_cohort_rows(*cohort, store);
    if (!scan.has_value()) {
      std::fprintf(stderr, "scan %s: %s\n", path.c_str(), scan.error().to_string().c_str());
      return 1;
    }
    for (const OracleRow &row : scan->rows) {
      const EnginePricingInputs in = atx::vol::oracle::mode_a_inputs(row, map);
      const Result<double> american = atx::vol::andersen_lake(
          in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side,
          atx::vol::al_fast_opts());
      const Result<AmericanGreeks> european = atx::vol::oracle::european_greeks(
          in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side);
      if (!american.has_value() || !european.has_value()) {
        ++refused; // the row Mode A would count as rows_engine_error and drop
        continue;
      }
      const double am_err = std::abs(*american - row.sr_prc);
      const double eu_err = std::abs(european->price - row.sr_prc);
      if (!std::isfinite(am_err) || !std::isfinite(eu_err)) {
        ++refused;
        continue;
      }
      RootStats &stats = by_root[row.underlier];
      ++stats.rows;
      stats.american_abs += am_err;
      stats.european_abs += eu_err;
      stats.european_closer += (eu_err < am_err) ? 1 : 0;
      stats.european_exact += (eu_err <= kExactTolerance) ? 1 : 0;
      stats.american_exact += (am_err <= kExactTolerance) ? 1 : 0;
    }
  }

  std::fprintf(stderr, "rows refused by a pricer: %lld\n", refused);
  std::printf("underlier,rows,american_mae_ticks,european_mae_ticks,european_closer_frac,"
              "european_exact_frac,american_exact_frac\n");
  for (const auto &[root, stats] : by_root) {
    const double n = static_cast<double>(stats.rows);
    std::printf("%s,%lld,%.6f,%.6f,%.6f,%.6f,%.6f\n", root.c_str(), stats.rows,
                100.0 * stats.american_abs / n, 100.0 * stats.european_abs / n,
                static_cast<double>(stats.european_closer) / n,
                static_cast<double>(stats.european_exact) / n,
                static_cast<double>(stats.american_exact) / n);
  }
  return 0;
}
