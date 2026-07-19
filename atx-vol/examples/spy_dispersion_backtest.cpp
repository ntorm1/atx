// Thin CLI over the listed SPY-dispersion library seam (atx/vol/dispersion_run.hpp).
//
// Each subcommand is a process boundary; no fitter/session object crosses it.
// All library workflow — corpus build, schedule build, listed + surface-only
// backtests, projected VaR, and the verify/reference-reconcile — lives in the
// library so it is unit-testable off the filesystem. This translation unit only
// parses argv and dispatches. The pinned admission thresholds that make the
// dispersion golden (final_nav = 247.4065016443293) reproduce byte-for-byte are
// DispersionCorpusPolicy library constants, not literals here.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/vol/dispersion_run.hpp"

namespace {

namespace fs = std::filesystem;
using atx::core::ErrorCode;
using atx::vol::Status;

void usage() {
  std::fprintf(stderr, "usage:\n"
                       "  atxvol_spy_dispersion_backtest build-corpus --spec FILE --out DIR\n"
                       "  atxvol_spy_dispersion_backtest build-schedule --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-surface-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-projected-var --run DIR\n"
                       "  atxvol_spy_dispersion_backtest verify --run DIR\n");
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  fs::path spec;
  fs::path run;
  for (int i = 2; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (i + 1 >= argc) {
      usage();
      return 2;
    }
    if (argument == "--spec") {
      spec = argv[++i];
    } else if (argument == "--out" || argument == "--run") {
      run = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  Status status = atx::core::Err(ErrorCode::InvalidArgument, "unknown command");
  if (command == "build-corpus" && !spec.empty() && !run.empty()) {
    status = atx::vol::dispersion_build_corpus(spec, run);
  } else if (command == "build-schedule" && !run.empty()) {
    status = atx::vol::dispersion_build_schedule(run);
  } else if (command == "run-backtest" && !run.empty()) {
    status = atx::vol::dispersion_run_backtest(run);
  } else if (command == "run-surface-backtest" && !run.empty()) {
    status = atx::vol::dispersion_run_surface_backtest(run);
  } else if (command == "run-projected-var" && !run.empty()) {
    status = atx::vol::dispersion_run_projected_var(run);
  } else if (command == "verify" && !run.empty()) {
    auto report = atx::vol::dispersion_verify(run);
    status = report ? atx::core::Ok() : atx::core::Err(report.error());
  } else {
    usage();
    return 2;
  }
  if (!status) {
    std::fprintf(stderr, "%s\n", status.error().to_string().c_str());
    return 1;
  }
  return 0;
}
