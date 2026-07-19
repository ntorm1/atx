// earnings_repro.cpp — Task 7 end-to-end CLI: OPRA parquet -> fitted
// VolaSession -> Task 5 earnings TSV -> `EventSchedule` -> the shared library
// pipeline `run_earnings_repro` (atx/vol/earnings_repro.hpp) -> prints
// iEMove, {st,lt,decay}, and the 12 SR-tenor `atmCenI_{Nd}` values.
//
// Thin CLI ONLY: argument parsing, IO (parquet + TSV load), and printing.
// All computation lives in `run_earnings_repro` -- the same library entry
// point the slow smoke test (tests/earnings_repro_smoke_test.cpp) and Task
// 9's batch validation driver call directly, per the CONTROLLER DESIGN
// DECISION (Option B) recorded in earnings_repro.hpp's module doc.
//
// usage: earnings-repro --opra <parquet> --earnings <tsv> --ticker <SYM>
//                        --now <iso> [--convention calendar|voltime] [--r RATE]

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "atx/vol/earnings_forecast_loader.hpp" // load_earnings_events
#include "atx/vol/earnings_repro.hpp"           // run_earnings_repro
#include "atx/vol/event_vol.hpp"                // EventSchedule
#include "atx/vol/opra_panel.hpp"                // OpraLoadSpec, load_opra_cbbo_parquet
#include "atx/vol/session.hpp"                   // VolaSession, make_session_inputs
#include "atx/vol/sr_tenor_grid.hpp"             // SrTenorGrid::kTradingDays
#include "atx/vol/vol_time.hpp"                  // TimeSpec, TimeConvention

using namespace atx::vol;

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage: earnings-repro --opra <parquet> --earnings <tsv> --ticker <SYM> "
               "--now <iso> [--convention calendar|voltime] [--r RATE]\n");
}

} // namespace

int main(int argc, char **argv) {
  std::string opra_path;
  std::string earnings_path;
  std::string ticker;
  std::string now_iso;
  std::string convention = "calendar";
  double r = 0.043;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    if (a == "--opra" && i + 1 < argc) {
      opra_path = argv[++i];
    } else if (a == "--earnings" && i + 1 < argc) {
      earnings_path = argv[++i];
    } else if (a == "--ticker" && i + 1 < argc) {
      ticker = argv[++i];
    } else if (a == "--now" && i + 1 < argc) {
      now_iso = argv[++i];
    } else if (a == "--convention" && i + 1 < argc) {
      convention = argv[++i];
    } else if (a == "--r" && i + 1 < argc) {
      r = std::atof(argv[++i]);
    } else if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    }
  }

  if (opra_path.empty() || earnings_path.empty() || ticker.empty() || now_iso.empty()) {
    print_usage();
    return 1;
  }

  TimeSpec time_spec{};
  if (convention == "voltime") {
    time_spec.convention = TimeConvention::VolTime;
  } else if (convention != "calendar") {
    std::fprintf(stderr, "unknown --convention '%s' (expected calendar|voltime)\n",
                convention.c_str());
    return 1;
  }

  // ── Load + build (mirrors cstar_panel.cpp's --real path) ─────────────────
  OpraLoadSpec load;
  load.path = opra_path;
  load.underlying = ticker;
  load.snapshot_iso = now_iso;
  load.r = r;
  load.time = time_spec;
  const auto panel = load_opra_cbbo_parquet(load);
  if (!panel.has_value()) {
    std::fprintf(stderr, "opra load failed: %s\n", panel.error().to_string().c_str());
    return 1;
  }

  SessionInputs in =
      make_session_inputs(FitPreset::Fast, panel->implied_spot, r, panel->frame.snapshot_ts_ns);
  in.time = time_spec;
  auto sess = VolaSession::from_frame(panel->frame, in);
  if (!sess.has_value()) {
    std::fprintf(stderr, "session build failed: %s\n", sess.error().to_string().c_str());
    return 1;
  }

  const auto events = load_earnings_events(earnings_path, ticker);
  if (!events.has_value()) {
    std::fprintf(stderr, "earnings load failed: %s\n", events.error().to_string().c_str());
    return 1;
  }
  const EventSchedule sched(*events);

  // ── Pipeline (the one shared library entry point) ────────────────────────
  const std::int64_t now_ns = panel->frame.snapshot_ts_ns;
  const auto repro = run_earnings_repro(*sess, sched, now_ns);
  if (!repro.has_value()) {
    std::fprintf(stderr, "earnings repro failed: %s\n", repro.error().to_string().c_str());
    return 1;
  }

  // ── Print ──────────────────────────────────────────────────────────────
  std::printf("ticker=%s now=%s r=%.4f convention=%s n_listed=%zu\n", ticker.c_str(),
             now_iso.c_str(), r, convention.c_str(), repro->listed_obs.size());
  std::printf("iEMove=%.6f  st=%.6f  lt=%.6f  decay=%.6f  fit_error=%.6f  fit_code=%d\n",
             repro->fit.emove, repro->fit.st, repro->fit.lt, repro->fit.decay,
             repro->fit.fit_error, static_cast<int>(repro->fit.fit_code));
  std::printf("%-6s %10s %12s %8s\n", "Nd", "T", "atmCenI", "nEarn");
  for (std::size_t i = 0; i < SrTenorGrid::kTradingDays.size(); ++i) {
    std::printf("%-6d %10.5f %12.6f %8zu\n", SrTenorGrid::kTradingDays[i], repro->tenor_T[i],
               repro->atm_cen_i[i], repro->n_earn[i]);
  }
  return 0;
}
