// Unit + end-to-end tests for the listed SPY-dispersion library seam
// (atx/vol/dispersion_run.hpp) extracted out of examples/spy_dispersion_backtest.cpp.
//
// Coverage:
//   1. DispersionCorpusPolicy defaults assemble the byte-identical pinned
//      QualifiedCorpusConfig (admission thresholds + fingerprints) — the
//      reproduction anchor for the dispersion golden.
//   2. dispersion_input_fingerprint / dispersion_hash_text pin the corpus identity.
//   3. dispersion_backtest_config_from_run_spec maps the run spec onto the engine.
//   4. reconcile_dispersion_reference (M1) recomputes + numerically compares a full
//      synthetic run END-TO-END via the library API (not the CLI), matching the
//      independent reference tool tools/reference_spy_dispersion.py, and a corrupt
//      schedule quantity is rejected.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/dispersion_run.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

std::string tsv_row(std::initializer_list<std::string_view> fields) {
  std::string line;
  bool first = true;
  for (const std::string_view field : fields) {
    if (!first) {
      line.push_back('\t');
    }
    line.append(field);
    first = false;
  }
  line.push_back('\n');
  return line;
}

void write_file(const fs::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(static_cast<bool>(out));
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  ASSERT_TRUE(static_cast<bool>(out));
}

fs::path make_run_dir(std::string_view leaf) {
  const fs::path dir = fs::temp_directory_path() / ("atx_disp_run_" + std::string(leaf));
  std::error_code error;
  fs::remove_all(dir, error);
  fs::create_directories(dir, error);
  return dir;
}

// Mirrors reference_spy_dispersion_test.py::schedule_rows.
std::string schedule_text(std::string_view spy_call_quantity = "-1") {
  std::string text = "ATX_LISTED_DISPERSION_SCHEDULE\t1\n";
  text += tsv_row({"roll_date", "valuation_ts_ns", "cohort", "expiry_ts_ns",
                   "gross_index_vega_target", "net_vega", "gross_vega", "n_names", "is_index",
                   "symbol", "uid", "instrument_id", "raw_symbol", "strike", "side", "quantity",
                   "multiplier", "raw_bid", "raw_ask", "raw_mid", "model_mark", "delta_per_share",
                   "vega_per_unit_vol", "vega_per_contract_per_vol_point", "normalized_weight",
                   "target_straddle_vega", "achieved_leg_vega", "source_fingerprint",
                   "surface_fingerprint"});
  // roll_date, ts, cohort, expiry, gross_target, net, gross, n_names, is_index, symbol, uid,
  // iid, raw_symbol, strike, side, quantity, mult, bid, ask, mid, mark, dps, vpu, vpc, weight,
  // target_straddle, achieved, source_fp, surface_fp
  text += tsv_row({"2026-07-10", "100", "1", "100000", "100", "0", "200", "1", "1", "SPY", "1",
                   "1", "SPY1", "100", "C", spy_call_quantity, "100", "9", "11", "10", "10", "0",
                   "50", "50", "0", "-100", "-50", "1", "99"});
  text += tsv_row({"2026-07-10", "100", "1", "100000", "100", "0", "200", "1", "1", "SPY", "1",
                   "2", "SPY2", "100", "P", "-1", "100", "9", "11", "10", "10", "0", "50", "50",
                   "0", "-100", "-50", "2", "99"});
  text += tsv_row({"2026-07-10", "100", "1", "100000", "100", "0", "200", "1", "0", "AAPL", "2",
                   "3", "AAPL3", "100", "C", "1", "100", "9", "11", "10", "10", "0", "50", "50",
                   "1", "100", "50", "3", "99"});
  text += tsv_row({"2026-07-10", "100", "1", "100000", "100", "0", "200", "1", "0", "AAPL", "2",
                   "4", "AAPL4", "100", "P", "1", "100", "9", "11", "10", "10", "0", "50", "50",
                   "1", "100", "50", "4", "99"});
  return text;
}

// Mirrors reference_spy_dispersion_test.py::mark_rows (sorted by date, raw_symbol).
std::string marks_text() {
  std::string text = tsv_row({"date", "valuation_ts_ns", "role", "cohort", "symbol", "uid",
                              "instrument_id", "raw_symbol", "expiry_ts_ns", "strike", "side",
                              "quantity", "multiplier", "status", "raw_bid", "raw_ask", "raw_mid",
                              "model_mark", "model_in_spread"});
  struct Leg {
    std::string_view raw, symbol, uid, iid, side, quantity, entry, held;
  };
  const Leg legs[] = {
      {"AAPL3", "AAPL", "2", "3", "C", "1", "5", "6"},
      {"AAPL4", "AAPL", "2", "4", "P", "1", "5", "6"},
      {"SPY1", "SPY", "1", "1", "C", "-1", "10", "11"},
      {"SPY2", "SPY", "1", "2", "P", "-1", "9", "8"},
  };
  // Entry marks (2026-07-10) then held marks (2026-07-11), each block sorted by raw_symbol.
  for (const Leg &leg : legs) {
    text += tsv_row({"2026-07-10", "100", "Entry", "1", leg.symbol, leg.uid, leg.iid, leg.raw,
                     "100000", "100", leg.side, leg.quantity, "100", "Ok", leg.entry, leg.entry,
                     leg.entry, leg.entry, "1"});
  }
  for (const Leg &leg : legs) {
    text += tsv_row({"2026-07-11", "200", "Held", "1", leg.symbol, leg.uid, leg.iid, leg.raw,
                     "100000", "100", leg.side, leg.quantity, "100", "Ok", leg.held, leg.held,
                     leg.held, leg.held, "1"});
  }
  return text;
}

std::string reconciliation_text() {
  std::string text =
      tsv_row({"date", "valuation_ts_ns", "held_cohort", "model_option_pnl", "quote_mid_pnl",
               "model_minus_quote_pnl", "model_nav", "quote_mid_nav", "quote_mid_coverage",
               "n_held_lots", "n_quote_mid_lots"});
  text += tsv_row({"2026-07-10", "100", "1", "0", "0", "0", "0", "0", "1", "4", "4"});
  text += tsv_row({"2026-07-11", "200", "1", "200", "200", "0", "200", "200", "1", "4", "4"});
  return text;
}

std::string backtest_text() {
  const std::vector<std::string_view> fields = {
      "date",         "ts_ns",           "pnl_total",     "pnl_delta",      "pnl_gamma",
      "pnl_vega",     "pnl_vanna",       "pnl_volga",     "pnl_theta",      "pnl_rho",
      "pnl_charm",    "pnl_unexplained", "pnl_settlement","pnl_shares",     "financing",
      "cost",         "nav",             "cash",          "gross_delta",    "gross_gamma",
      "gross_vega",   "gross_theta",     "turnover_notional", "turnover_vega", "n_open_lots",
      "n_unpriced_lots", "n_unpriced_greeks"};
  std::string header;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      header.push_back('\t');
    }
    header.append(fields[i]);
  }
  header.push_back('\n');
  auto row = [&](std::string_view date, std::string_view ts, std::string_view total) {
    std::string line;
    for (std::size_t i = 0; i < fields.size(); ++i) {
      std::string_view value = "0";
      if (fields[i] == "date") {
        value = date;
      } else if (fields[i] == "ts_ns") {
        value = ts;
      } else if (fields[i] == "pnl_total" || fields[i] == "pnl_unexplained" || fields[i] == "nav") {
        value = total;
      } else if (fields[i] == "n_open_lots") {
        value = "4";
      }
      if (i != 0) {
        line.push_back('\t');
      }
      line.append(value);
    }
    line.push_back('\n');
    return line;
  };
  return header + row("2026-07-10", "100", "0") + row("2026-07-11", "200", "200");
}

} // namespace

// ── (1) pinned admission policy is byte-identical library constants ─────────

TEST(DispersionRunPolicy, DefaultsAssembleThePinnedCorpusConfig) {
  const DispersionCorpusPolicy policy;
  const std::uint64_t input_fp = dispersion_input_fingerprint("2026-01-02", "2026-04-30", 51);
  const QualifiedCorpusConfig config = dispersion_corpus_config(policy, 0, input_fp);

  EXPECT_EQ(config.build.fit_template.preset, FitPreset::Hft);
  ASSERT_TRUE(config.build.fit_template.curve.has_value());
  EXPECT_EQ(config.build.fit_template.curve->kind, VolCurveKind::LinearVariance);
  EXPECT_TRUE(config.build.fit_template.enforce_calendar_floor);
  EXPECT_TRUE(config.admission.enabled);

  for (const CorpusAdmissionRule &rule : config.admission.by_profile) {
    EXPECT_EQ(rule.min_quotes, 20u);
    EXPECT_EQ(rule.min_slices, 2u);
    EXPECT_TRUE(rule.require_calendar_arb_free);
    EXPECT_DOUBLE_EQ(rule.calendar_abs_k, 0.7);
    EXPECT_TRUE(rule.require_source_provenance);
  }

  // The pinned fingerprint material is load-bearing for the golden reproduction.
  EXPECT_EQ(config.policy_fingerprint,
            dispersion_hash_text(
                "spy-listed-dispersion-admission-v4-pinned-linear-calendar-floor-k0.7"));
  EXPECT_EQ(config.input_fingerprint, input_fp);
  EXPECT_EQ(config.input_fingerprint, dispersion_hash_text("2026-01-02|2026-04-30|51"));
}

TEST(DispersionRunPolicy, InputFingerprintIsNonzeroAndPathIndependent) {
  EXPECT_NE(dispersion_hash_text("x"), 0u);
  EXPECT_EQ(dispersion_input_fingerprint("2026-01-02", "2026-04-30", 51),
            dispersion_hash_text("2026-01-02|2026-04-30|51"));
  EXPECT_NE(dispersion_input_fingerprint("2026-01-02", "2026-04-30", 51),
            dispersion_input_fingerprint("2026-01-02", "2026-04-30", 52));
}

// ── (2) run-spec → engine config mapping ────────────────────────────────────

TEST(DispersionRunConfig, BacktestConfigFromRunSpecMapsFields) {
  RunSpec spec;
  spec.target_dte_days = 33.0;
  spec.roll_dte_days = 9.0;
  spec.gross_index_vega = 12345.0;
  spec.delta_band = 0.25;
  spec.min_names = 7;

  const DispersionBacktestConfig config = dispersion_backtest_config_from_run_spec(spec);
  EXPECT_DOUBLE_EQ(config.target_dte_days, 33.0);
  EXPECT_DOUBLE_EQ(config.roll_dte_days, 9.0);
  EXPECT_DOUBLE_EQ(config.gross_index_vega, 12345.0);
  EXPECT_DOUBLE_EQ(config.delta_band, 0.25);
  EXPECT_EQ(config.min_names, 7u);
  EXPECT_EQ(config.run.unpriced, UnpricedLotPolicy::Error);
}

// ── (3) native reference reconciliation end-to-end (M1) ─────────────────────

TEST(DispersionReferenceReconcile, RecomputesFullSyntheticRun) {
  const fs::path run = make_run_dir("full");
  write_file(run / "trade_schedule.tsv", schedule_text());
  write_file(run / "contract_marks.tsv", marks_text());
  write_file(run / "reconciliation.tsv", reconciliation_text());
  write_file(run / "backtest.tsv", backtest_text());

  auto records = reconcile_dispersion_reference(run, /*schedule_only=*/false);
  ASSERT_TRUE(records) << records.error().to_string();
  // One roll record + two date records.
  ASSERT_EQ(records->size(), 3u);
  EXPECT_EQ((*records)[0].record_type, "roll");
  EXPECT_EQ((*records)[0].date, "2026-07-10");
  EXPECT_DOUBLE_EQ((*records)[0].computed_net_vega, 0.0);
  EXPECT_DOUBLE_EQ((*records)[0].computed_gross_vega, 200.0);
  EXPECT_EQ((*records)[2].record_type, "date");
  EXPECT_DOUBLE_EQ((*records)[2].computed_model_nav, 200.0);
  EXPECT_DOUBLE_EQ((*records)[2].computed_quote_mid_nav, 200.0);

  const fs::path sidecar = run / "reference_reconciliation.tsv";
  ASSERT_TRUE(write_reference_reconciliation_file(sidecar, *records));
  std::ifstream in(sidecar, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("roll\t2026-07-10"), std::string::npos);
  EXPECT_NE(text.find("date\t2026-07-11"), std::string::npos);

  std::error_code error;
  fs::remove_all(run, error);
}

TEST(DispersionReferenceReconcile, ScheduleOnlyPassesWithoutOtherArtifacts) {
  const fs::path run = make_run_dir("sched_only");
  write_file(run / "trade_schedule.tsv", schedule_text());
  auto records = reconcile_dispersion_reference(run, /*schedule_only=*/true);
  ASSERT_TRUE(records) << records.error().to_string();
  EXPECT_EQ(records->size(), 1u);
  std::error_code error;
  fs::remove_all(run, error);
}

TEST(DispersionReferenceReconcile, CorruptScheduleQuantityIsRejected) {
  const fs::path run = make_run_dir("corrupt");
  // A -2 index-call quantity breaks the persisted vega-flat identity.
  write_file(run / "trade_schedule.tsv", schedule_text("-2"));
  auto records = reconcile_dispersion_reference(run, /*schedule_only=*/true);
  EXPECT_FALSE(records);
  std::error_code error;
  fs::remove_all(run, error);
}
