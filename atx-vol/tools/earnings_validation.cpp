// earnings_validation.cpp — Task 9 batch validation driver.
//
// Runs the shared cohort-validation harness (atx/vol/earnings_repro_config.hpp)
// over every name in the checked-in SpiderRock truth CSV: per name it loads the
// OPRA board, fits a VolaSession, builds the earnings schedule, runs
// `run_earnings_repro` under a chosen `EarningsReproConfig`, and diffs the
// reproduced 12-tenor atmCenI / iEMove / nEarnCnt_Nd against truth. Emits a
// per-name table (RMSE, iEMove residual, the nEarnCnt schedule-alignment gate,
// fit code), a per-tenor residual-attribution row, and the pooled cohort RMSE.
//
// A name whose parquet is absent is reported as SKIPPED (not a crash), so a
// cohort with a data gap (e.g. ADBE, no on-disk board) still runs the rest.
//
// usage: earnings-validation --truth <cohort.csv> --earnings <tsv>
//                            --opra-root <dir> [--date YYYY-MM-DD]
//                            [--r RATE] [--convention calendar|voltime]
//                            [--interp variance|vol] [--censor-space on|off]
//                            [--clock-days N]

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "analytics/earnings_forecast_loader.hpp" // load_earnings_events
#include "analytics/earnings_repro_config.hpp"     // EarningsReproConfig + harness
#include "atx/vol/api/analytics/event_vol.hpp"                 // EventSchedule
#include "atx/vol/api/marketdata/opra_panel.hpp"                // OpraLoadSpec, load_opra_cbbo_parquet
#include "atx/vol/api/fitting/session.hpp"                   // VolaSession, make_session_inputs
#include "atx/vol/api/fitting/sr_tenor_grid.hpp"             // SrTenorGrid::kTradingDays

using namespace atx::vol;

namespace {

namespace fs = std::filesystem;

void print_usage() {
  std::fprintf(stderr,
               "usage: earnings-validation --truth <cohort.csv> --earnings <tsv> "
               "--opra-root <dir> [--date YYYY-MM-DD] [--r RATE] "
               "[--convention calendar|voltime] [--interp variance|vol] "
               "[--censor-space on|off] [--clock-days N]\n");
}

} // namespace

int main(int argc, char **argv) {
  std::string truth_path;
  std::string earnings_path;
  std::string opra_root;
  std::string date = "2026-02-10";
  double r = 0.043;
  EarningsReproConfig cfg; // default convention set

  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    auto next = [&](const char *&dst) {
      if (i + 1 < argc) {
        dst = argv[++i];
      }
    };
    const char *val = nullptr;
    if (a == "--truth") {
      next(val);
      if (val) truth_path = val;
    } else if (a == "--earnings") {
      next(val);
      if (val) earnings_path = val;
    } else if (a == "--opra-root") {
      next(val);
      if (val) opra_root = val;
    } else if (a == "--date") {
      next(val);
      if (val) date = val;
    } else if (a == "--r") {
      next(val);
      if (val) r = std::atof(val);
    } else if (a == "--convention") {
      next(val);
      if (val && std::string_view{val} == "voltime") {
        cfg.time.convention = TimeConvention::VolTime;
      }
    } else if (a == "--interp") {
      next(val);
      if (val && std::string_view{val} == "vol") {
        cfg.interp = InterpSpace::Vol;
      }
    } else if (a == "--censor-space") {
      next(val);
      if (val) cfg.censor_space = (std::string_view{val} != "off");
    } else if (a == "--clock-days") {
      next(val);
      if (val) cfg.clock_days_per_year = std::atof(val);
    } else if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    }
  }

  if (truth_path.empty() || earnings_path.empty() || opra_root.empty()) {
    print_usage();
    return 1;
  }

  const auto truth = parse_cohort_truth_csv(truth_path);
  if (!truth.has_value()) {
    std::fprintf(stderr, "truth CSV load failed: %s\n", truth.error().to_string().c_str());
    return 1;
  }

  std::printf("# earnings-validation  date=%s  convention=%s  interp=%s  censor_space=%s  "
              "clock_days_per_year=%.4f\n",
              date.c_str(),
              cfg.time.convention == TimeConvention::VolTime ? "voltime" : "calendar",
              cfg.interp == InterpSpace::Vol ? "vol" : "variance",
              cfg.censor_space ? "on" : "off", cfg.clock_days_per_year);
  std::printf("%-6s %10s %10s %10s %10s %8s %8s  %s\n", "ticker", "iEMove", "truth", "dEMove",
              "rmse", "nEcMatch", "fitCode", "status");

  std::vector<CohortResult> results;
  results.reserve(truth->size());

  for (const CohortTruthRow &row : *truth) {
    const fs::path parquet = fs::path(opra_root) / row.ticker / (date + ".parquet");
    std::error_code ec;
    if (!fs::exists(parquet, ec)) {
      std::printf("%-6s %10s %10.4f %10s %10s %8s %8s  SKIPPED(no parquet)\n", row.ticker.c_str(),
                  "-", row.iemove, "-", "-", "-", "-");
      continue;
    }

    OpraLoadSpec load;
    load.path = parquet.string();
    load.underlying = row.ticker;
    load.snapshot_iso = date + "T14:00:00Z";
    load.r = r;
    load.time = cfg.time;
    const auto panel = load_opra_cbbo_parquet(load);
    if (!panel.has_value()) {
      std::printf("%-6s %10s %10.4f %10s %10s %8s %8s  ERR(opra: %s)\n", row.ticker.c_str(), "-",
                  row.iemove, "-", "-", "-", "-", panel.error().to_string().c_str());
      continue;
    }

    SessionInputs in =
        make_session_inputs(FitPreset::Fast, panel->implied_spot, r, panel->frame.snapshot_ts_ns);
    in.time = cfg.time;
    auto sess = VolaSession::from_frame(panel->frame, in);
    if (!sess.has_value()) {
      std::printf("%-6s %10s %10.4f %10s %10s %8s %8s  ERR(fit: %s)\n", row.ticker.c_str(), "-",
                  row.iemove, "-", "-", "-", "-", sess.error().to_string().c_str());
      continue;
    }

    const auto events = load_earnings_events(earnings_path, row.ticker);
    if (!events.has_value()) {
      std::printf("%-6s %10s %10.4f %10s %10s %8s %8s  ERR(earn: %s)\n", row.ticker.c_str(), "-",
                  row.iemove, "-", "-", "-", "-", events.error().to_string().c_str());
      continue;
    }
    const EventSchedule sched(*events);

    const std::int64_t now_ns = panel->frame.snapshot_ts_ns;
    const auto res = validate_cohort_name(*sess, sched, now_ns, row, cfg);
    if (!res.has_value()) {
      std::printf("%-6s %10s %10.4f %10s %10s %8s %8s  ERR(repro: %s)\n", row.ticker.c_str(), "-",
                  row.iemove, "-", "-", "-", "-", res.error().to_string().c_str());
      continue;
    }

    std::printf("%-6s %10.4f %10.4f %10.4f %10.5f %8s %8d  OK\n", res->ticker.c_str(),
                res->model_emove, res->truth_emove, res->emove_residual, res->rmse_vol,
                res->n_earn_match ? "yes" : "NO", static_cast<int>(res->fit_code));
    results.push_back(*res);
  }

  if (results.empty()) {
    std::printf("\n# no names validated (all skipped or errored)\n");
    return 0;
  }

  // Per-tenor residual attribution: mean(model - truth) across validated names.
  std::printf("\n# per-tenor residual attribution (mean model-truth across %zu names)\n",
              results.size());
  std::printf("%-6s %10s %10s\n", "Nd", "meanResid", "rmsResid");
  for (std::size_t i = 0; i < kSrTenorCount; ++i) {
    double sum = 0.0;
    double sse = 0.0;
    std::size_t n = 0;
    for (const CohortResult &c : results) {
      const double d = c.residual[i];
      if (std::isfinite(d)) {
        sum += d;
        sse += d * d;
        ++n;
      }
    }
    const double mean = (n > 0) ? sum / static_cast<double>(n) : 0.0;
    const double rms = (n > 0) ? std::sqrt(sse / static_cast<double>(n)) : 0.0;
    std::printf("%-6d %10.5f %10.5f\n", SrTenorGrid::kTradingDays[i], mean, rms);
  }

  const double pooled = cohort_rmse_vol(results);
  std::printf("\n# cohort atmCenI RMSE (pooled over %zu names x 12 tenors) = %.6f\n",
              results.size(), pooled);
  return 0;
}
