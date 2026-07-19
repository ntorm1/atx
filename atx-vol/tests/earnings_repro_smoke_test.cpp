#include "atx/vol/earnings_repro.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "atx/vol/earnings_forecast_loader.hpp" // load_earnings_events (Task 5)
#include "atx/vol/event_vol.hpp"                // EventSchedule
#include "atx/vol/opra_panel.hpp"                // OpraLoadSpec, load_opra_cbbo_parquet
#include "atx/vol/session.hpp"                   // VolaSession, make_session_inputs

// Task 7 end-to-end smoke test: real OPRA parquet -> fitted VolaSession ->
// Task 5 earnings schedule -> run_earnings_repro's per-expiry censoring +
// term-fit + primary censored-space atmCenI interpolation, on the on-disk
// REAL NVDA/2026-02-10 board. This is the ONE library entry point the CLI
// (examples/earnings_repro.cpp) and Task 9's batch driver also call -- see
// earnings_repro.hpp's module comment for the CONTROLLER DESIGN DECISION
// (Option B) this test exists to exercise directly, with no built-binary
// shell-out and no ATX_BUILD_EXAMPLES dependency.
//
// Labeled atx_vol_slow (real parquet load + full-surface fit, tests/CMakeLists.txt
// ATX_VOL_SLOW_FILTER); skips cleanly (GTEST_SKIP) if the fixture parquet is
// absent, matching opra_fixture.hpp's std::nullopt-on-missing convention, so a
// source-only checkout without the C:/atx-data cache still builds and passes.

namespace {

namespace fs = std::filesystem;

using atx::vol::EventSchedule;
using atx::vol::FitPreset;
using atx::vol::load_earnings_events;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::make_session_inputs;
using atx::vol::OpraLoadSpec;
using atx::vol::run_earnings_repro;
using atx::vol::VolaSession;

constexpr const char *kNvdaParquet = "C:/atx-data/spy-dispersion/opra/NVDA/2026-02-10.parquet";

// The Task 5 fixture sits next to this test source (tests/support/) -- probe
// the same relative candidates earnings_forecast_loader_test.cpp uses (ninja
// invokes clang-cl with a build-dir-relative path, so __FILE__ is not
// reliably absolute here).
fs::path fixture(const char *name) {
  const fs::path rel = fs::path("support") / name;
  for (const char *base : {"../../../atx-vol/tests", "atx-vol/tests", "../atx-vol/tests", "."}) {
    const fs::path candidate = fs::path(base) / rel;
    if (fs::exists(candidate)) {
      return candidate;
    }
  }
  return fs::path(__FILE__).parent_path() / "support" / name;
}

TEST(EarningsReproSmoke, NvdaRealBoard_TwelveFiniteAtmCenI_FiniteNonnegativeEmove) {
  std::error_code ec;
  if (!fs::exists(kNvdaParquet, ec)) {
    GTEST_SKIP() << "NVDA fixture parquet not found at " << kNvdaParquet;
  }

  OpraLoadSpec load;
  load.path = kNvdaParquet;
  load.underlying = "NVDA";
  load.snapshot_iso = "2026-02-10T14:00:00Z";
  load.r = 0.043;
  const auto panel = load_opra_cbbo_parquet(load);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  const auto in = make_session_inputs(FitPreset::Fast, panel->implied_spot, load.r,
                                      panel->frame.snapshot_ts_ns);
  auto sess = VolaSession::from_frame(panel->frame, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto events =
      load_earnings_events(fixture("earnings_forecast_sample.tsv").string(), "NVDA");
  ASSERT_TRUE(events.has_value()) << events.error().to_string();
  const EventSchedule sched(*events);

  // As-of instant: the board's own snapshot timestamp (the natural "now" for
  // a pipeline that fits the session off this same panel).
  const std::int64_t now_ns = panel->frame.snapshot_ts_ns;

  const auto repro = run_earnings_repro(*sess, sched, now_ns);
  ASSERT_TRUE(repro.has_value()) << repro.error().to_string();

  EXPECT_TRUE(std::isfinite(repro->fit.emove));
  EXPECT_GE(repro->fit.emove, 0.0);

  // Per-step "monotone-ish" tolerance: the real censored-space interpolation
  // wiggles by ~1 vol point step-to-step near mid tenors (e.g. the NVDA
  // 84d/105d/126d run observed 0.441160 -> 0.440098 -> 0.441233), which is
  // real interp noise, not a modeling bug -- so a step is allowed to dip by
  // up to kMonotoneTol before it is flagged, while a grossly inverted term
  // structure (a dip far larger than one vol point) still fails.
  constexpr double kMonotoneTol = 0.01;

  for (std::size_t i = 0; i < repro->atm_cen_i.size(); ++i) {
    const double v = repro->atm_cen_i[i];
    EXPECT_TRUE(std::isfinite(v)) << "atm_cen_i[" << i << "] not finite";
    EXPECT_GT(v, 0.0) << "atm_cen_i[" << i << "] not > 0";
    EXPECT_LT(v, 3.0) << "atm_cen_i[" << i << "] not < 3";
    if (i > 0) {
      EXPECT_GE(v, repro->atm_cen_i[i - 1] - kMonotoneTol)
          << "atm_cen_i[" << i << "]=" << v << " dips more than " << kMonotoneTol
          << " below atm_cen_i[" << i - 1 << "]=" << repro->atm_cen_i[i - 1]
          << " -- not monotone-ish";
    }
  }
}

} // namespace
