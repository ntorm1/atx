#include "atx/vol/earnings_repro_config.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "atx/vol/earnings_forecast_loader.hpp" // load_earnings_events (Task 5)
#include "atx/vol/event_vol.hpp"                // EventSchedule
#include "atx/vol/opra_panel.hpp"                // OpraLoadSpec, load_opra_cbbo_parquet
#include "atx/vol/session.hpp"                   // VolaSession, make_session_inputs

// Task 9 cohort-validation harness gate: the checked-in truth CSV
// (tests/support/tickerhistory_2026-02-10_cohort.csv, sliced from the SR
// tbltickerhistory3 warehouse) + one cohort name with an on-disk OPRA board
// (NVDA/2026-02-10) driven through validate_cohort_name under the DEFAULT
// EarningsReproConfig. Asserts the per-tenor residual vector has size 12, the
// per-name RMSE is finite, and -- the schedule-alignment gate -- the model's
// nEarnCnt_Nd computed from the earnings schedule EXACTLY matches the truth
// nEarnCnt_Nd column for every one of the 12 SR tenors.
//
// Labeled atx_vol_slow (real parquet load + full-surface fit); skips cleanly
// (GTEST_SKIP) if the NVDA fixture parquet is absent, matching the Task 7
// smoke test's std::nullopt-on-missing convention, so a source-only checkout
// without the C:/atx-data cache still builds and passes.

namespace {

namespace fs = std::filesystem;

using atx::vol::CohortTruthRow;
using atx::vol::EarningsReproConfig;
using atx::vol::EventSchedule;
using atx::vol::FitPreset;
using atx::vol::load_earnings_events;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::make_session_inputs;
using atx::vol::OpraLoadSpec;
using atx::vol::parse_cohort_truth_csv;
using atx::vol::validate_cohort_name;
using atx::vol::VolaSession;

constexpr const char *kNvdaParquet = "C:/atx-data/spy-dispersion/opra/NVDA/2026-02-10.parquet";

// Same relative-candidate probe the Task 5/7 tests use (ninja invokes clang-cl
// with a build-dir-relative path, so __FILE__ is not reliably absolute).
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

TEST(EarningsValidation, NvdaCohortRow_TwelveResiduals_FiniteRmse_ExactNEarnMatch) {
  // Truth CSV must parse and carry the NVDA cohort row.
  const auto truth =
      parse_cohort_truth_csv(fixture("tickerhistory_2026-02-10_cohort.csv").string());
  ASSERT_TRUE(truth.has_value()) << truth.error().to_string();

  const CohortTruthRow *nvda = nullptr;
  for (const auto &row : *truth) {
    if (row.ticker == "NVDA") {
      nvda = &row;
      break;
    }
  }
  ASSERT_NE(nvda, nullptr) << "NVDA row missing from cohort truth CSV";
  EXPECT_NEAR(nvda->iemove, 0.0665, 0.001); // cross-check against the brief gate

  std::error_code ec;
  if (!fs::exists(kNvdaParquet, ec)) {
    GTEST_SKIP() << "NVDA fixture parquet not found at " << kNvdaParquet;
  }

  const EarningsReproConfig cfg; // default convention set

  OpraLoadSpec load;
  load.path = kNvdaParquet;
  load.underlying = "NVDA";
  // This optional external fixture may be refreshed in place. Leave the
  // override empty so the loader uses the file's authoritative stamped instant.
  load.snapshot_iso.clear();
  load.r = 0.043;
  load.time = cfg.time;
  const auto panel = load_opra_cbbo_parquet(load);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  auto in = make_session_inputs(FitPreset::Fast, panel->implied_spot, load.r,
                                 panel->frame.snapshot_ts_ns);
  in.time = cfg.time;
  auto sess = VolaSession::from_frame(panel->frame, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto events =
      load_earnings_events(fixture("earnings_forecast_sample.tsv").string(), "NVDA");
  ASSERT_TRUE(events.has_value()) << events.error().to_string();
  const EventSchedule sched(*events);

  const std::int64_t now_ns = panel->frame.snapshot_ts_ns;

  const auto res = validate_cohort_name(*sess, sched, now_ns, *nvda, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();

  // Per-tenor residual vector has size 12.
  EXPECT_EQ(res->residual.size(), std::size_t{12});
  EXPECT_EQ(res->model_atm_cen_i.size(), std::size_t{12});

  // Per-name cohort RMSE is finite (Task 10 tightens it; here only finiteness).
  EXPECT_TRUE(std::isfinite(res->rmse_vol)) << "rmse_vol not finite";
  EXPECT_GE(res->rmse_vol, 0.0);

  // Model iEMove is finite / non-negative.
  EXPECT_TRUE(std::isfinite(res->model_emove));
  EXPECT_GE(res->model_emove, 0.0);

  // Every per-tenor residual is finite.
  for (std::size_t i = 0; i < res->residual.size(); ++i) {
    EXPECT_TRUE(std::isfinite(res->residual[i])) << "residual[" << i << "] not finite";
  }

  // Schedule-alignment gate: model nEarnCnt_Nd EXACTLY matches truth for all 12
  // SR tenors (NVDA truth = 0,0,1,1,1,2,2,2,3,4,6,8).
  EXPECT_TRUE(res->n_earn_match) << "model nEarnCnt_Nd does not match truth exactly";
  for (std::size_t i = 0; i < res->model_n_earn.size(); ++i) {
    EXPECT_EQ(res->model_n_earn[i], nvda->n_earn[i])
        << "nEarnCnt mismatch at tenor index " << i;
  }
}

} // namespace
