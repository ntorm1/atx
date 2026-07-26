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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"            // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"            // Clock, MarketSnapshot
#include "atx/vol/corpus.hpp"              // CorpusManifest, CorpusEntry, write_manifest_file
#include "atx/vol/dispersion.hpp"          // DispersionConfig, build_dispersion_book
#include "atx/vol/dispersion_backtest.hpp" // DispersionBacktestConfig, dispersion_config_from
#include "atx/vol/dispersion_run.hpp"
#include "atx/vol/dispersion_workflow.hpp" // universe_at, utc_date_from_ns
#include "atx/vol/priced_surface.hpp"      // PricedSurface, PricingContext
#include "atx/vol/surface_archive.hpp"     // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"      // SliceContext
#include "atx/vol/vol_curve.hpp"           // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"         // EssviParams

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

// The four loose artifacts `reconcile_dispersion_reference` opens. A directory
// missing any one of them cannot drive the reconciler at all, so it is not
// evidence either way and is excluded rather than failed.
constexpr const char *kLooseReconcilerInputs[] = {"trade_schedule.tsv", "contract_marks.tsv",
                                                  "reconciliation.tsv", "backtest.tsv"};

// Directories under `root` that carry all four. `rejected` accumulates one line
// per excluded directory naming what it lacked — so a caller that finds NOTHING
// can say why, instead of skipping green with no signal (REV-TAIL I-1(b)).
// Factored out of the data-gated test so the selection rule itself is testable
// on synthetic input, off `tmp_path`, on every box.
std::vector<fs::path> scan_complete_reference_run_dirs(const fs::path &root, std::string &rejected) {
  std::vector<fs::path> run_dirs;
  std::error_code error;
  for (const fs::directory_entry &entry : fs::directory_iterator(root, error)) {
    if (!entry.is_directory(error)) {
      continue;
    }
    const fs::path dir = entry.path();
    std::string missing;
    for (const char *artifact : kLooseReconcilerInputs) {
      if (!fs::is_regular_file(dir / artifact, error)) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += artifact;
      }
    }
    if (missing.empty()) {
      run_dirs.push_back(dir);
    } else {
      rejected += "  - " + dir.filename().string() + ": missing " + missing + "\n";
    }
  }
  return run_dirs;
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

// ── (5) MINORS M10: a missing STRING column must stop the run ───────────────
//
// The reconciler's three column accessors did not agree on what a missing
// column means. `dec()` and `intcol()` (dispersion_run.cpp) return
// `recon_fail("invalid decimal/integer column <name>")`; `str()` returned a
// reference to a function-local static empty string, so an artifact that had
// lost a string column outright kept reconciling — with "" substituted for the
// value everywhere it was read.
//
// That is not a style point: both scenarios below reconcile CLEAN today.
// Scenario A publishes roll records whose `date` field is the empty string;
// scenario B scores every lot "no usable quote" because the column that says
// otherwise is gone, and agrees with a reconciliation file that recorded the
// same zero coverage for the same reason.

namespace {

// Deletes `name` from a header-indexed TSV — the header cell and the
// corresponding cell of every row — leaving an artifact that is still
// well-formed (every row aligns to the header, so `read_dict_tsv`'s ragged-row
// gate does not fire) and merely one column short. That is the shape a schema
// change or a truncated writer produces, and the shape the reconciler has to
// survive loudly.
std::string drop_column(const std::string &text, std::string_view name, bool has_magic) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      if (start < text.size()) {
        lines.push_back(text.substr(start));
      }
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  const std::size_t header_index = has_magic ? 1u : 0u;
  EXPECT_GT(lines.size(), header_index) << "fixture has no header line";
  auto split_tab = [](const std::string &line) {
    std::vector<std::string> out;
    std::size_t cursor = 0;
    while (true) {
      const std::size_t tab = line.find('\t', cursor);
      if (tab == std::string::npos) {
        out.push_back(line.substr(cursor));
        break;
      }
      out.push_back(line.substr(cursor, tab - cursor));
      cursor = tab + 1;
    }
    return out;
  };
  const std::vector<std::string> header = split_tab(lines[header_index]);
  std::size_t column = header.size();
  for (std::size_t i = 0; i < header.size(); ++i) {
    if (header[i] == name) {
      column = i;
    }
  }
  EXPECT_LT(column, header.size()) << "fixture has no column named " << name;
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i < header_index) {
      out += lines[i];
      out.push_back('\n');
      continue;
    }
    std::vector<std::string> cells = split_tab(lines[i]);
    if (column < cells.size()) {
      cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(column));
    }
    for (std::size_t c = 0; c < cells.size(); ++c) {
      if (c != 0) {
        out.push_back('\t');
      }
      out += cells[c];
    }
    out.push_back('\n');
  }
  return out;
}

// The reconciliation a run produces when `contract_marks.tsv` has no `status`
// column: every lot is scored "no usable quote", so quote-mid P&L, quote NAV
// and coverage are all zero and `n_quote_mid_lots` is zero on every date. The
// numbers are internally consistent — which is exactly why the missing column
// is invisible without an explicit presence check.
std::string reconciliation_text_zero_quote_coverage() {
  std::string text =
      tsv_row({"date", "valuation_ts_ns", "held_cohort", "model_option_pnl", "quote_mid_pnl",
               "model_minus_quote_pnl", "model_nav", "quote_mid_nav", "quote_mid_coverage",
               "n_held_lots", "n_quote_mid_lots"});
  text += tsv_row({"2026-07-10", "100", "1", "0", "0", "0", "0", "0", "0", "4", "0"});
  text += tsv_row({"2026-07-11", "200", "1", "200", "0", "200", "200", "0", "0", "4", "0"});
  return text;
}

// ── Projected-VaR route fixture (REVIEW C-1 / C-15) ─────────────────────────
//
// `dispersion_run_projected_var` had NO test at all, which is how a book anchored
// on the FIRST session while its VaR reference is the LAST session survived
// review. Driving the real entry point needs a real run directory: a run spec
// both readers accept, a multi-block point-in-time universe schedule, a manifest,
// and on-disk ATXVSA archives. Everything below is synthetic eSSVI (the
// `backtest_driver_test.cpp` pattern) — no fitting, no external data.

constexpr double kPvR = 0.043;
// 2026-10-01T20:00:00Z. This constant is load-bearing: the point-in-time resolver
// keys off the SNAPSHOT's own `ts_ns` through `utc_date_from_ns`, NOT the
// manifest's date string, so the archives must genuinely land on the dates the
// universe blocks name. `PvFixture.AnchorTimestampsLandOnTheNamedDates` pins it.
constexpr std::int64_t kPvBaseNs = 1'790'884'800'000'000'000LL;
constexpr std::int64_t kPvDayNs = 86'400LL * 1'000'000'000LL;
// Symbol -> uid, identical on every fixture date so `uid_of` is deterministic:
//   SPY = 1 (the index leg), AAA = 2, BBB = 3, CCC = 4.

[[nodiscard]] PricedSurface pv_surface(std::uint32_t uid, double spot, std::int64_t now_ts,
                                       double vol_bump) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = spot;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kPvR * T)));
    ctx.push_back(SliceContext{T, spot, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = spot;
  pc.r = kPvR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

struct PvUniverseBlock {
  std::string effective_date;
  std::vector<std::pair<std::string, double>> names; // symbol, raw weight
};

struct PvFixture {
  fs::path dir;
  std::vector<std::string> dates;
};

// `n_dates` consecutive UTC sessions from kPvBaseNs. Every archive carries EVERY
// symbol in `pv_symbols()`, so both the first-session basket and the last-session
// basket resolve against every date — which is what makes the anchor choice show
// up as a different BOOK rather than as an error.
[[nodiscard]] PvFixture make_pv_fixture(std::string_view leaf,
                                        const std::vector<PvUniverseBlock> &blocks,
                                        const std::string &extra_spec = {},
                                        std::size_t n_dates = 5) {
  PvFixture fixture;
  fixture.dir = make_run_dir(leaf);
  const fs::path archive_dir = fixture.dir / "surfaces";
  std::error_code error;
  fs::create_directories(archive_dir, error);

  CorpusManifest manifest;
  for (std::size_t d = 0; d < n_dates; ++d) {
    const std::int64_t now = kPvBaseNs + static_cast<std::int64_t>(d) * kPvDayNs;
    const std::string date = utc_date_from_ns(now);
    // Spot AND vol move materially per session, so a book sized on the first
    // session carries different quantities from one sized on the last. The path
    // is deliberately NON-MONOTONE: under a monotone path every historical loss
    // has the same sign and the tail quantile collapses to zero, which would
    // make the VaR itself a vacuous number to compare across the fix.
    const double drift = 1.0 + 0.05 * std::sin(1.7 * static_cast<double>(d));
    const double bump = 0.010 * std::sin(2.3 * static_cast<double>(d) + 0.7);
    const PricedSurface index = pv_surface(1, 500.0 * drift, now, 0.00 + bump);
    const PricedSurface aaa = pv_surface(2, 100.0 * drift, now, 0.02 + bump);
    const PricedSurface bbb = pv_surface(3, 120.0 * drift, now, 0.03 + bump);
    const PricedSurface ccc = pv_surface(4, 80.0 * drift, now, 0.05 + bump);
    const std::string path = (archive_dir / (date + ".atxvsa")).string();
    const std::vector<SurfaceArchiveItem> items = {SurfaceArchiveItem{"SPY", &index},
                                                   SurfaceArchiveItem{"AAA", &aaa},
                                                   SurfaceArchiveItem{"BBB", &bbb},
                                                   SurfaceArchiveItem{"CCC", &ccc}};
    const Status written = write_surface_archive_v2_file(path, items);
    EXPECT_TRUE(written.has_value())
        << (written.has_value() ? std::string{} : written.error().to_string());
    manifest.dates.push_back(date);
    CorpusEntry entry;
    entry.date = date;
    entry.symbol = "SPY";
    entry.status = CorpusFitStatus::Ok;
    entry.archive_path = path;
    manifest.entries.push_back(std::move(entry));
    fixture.dates.push_back(date);
  }
  manifest.n_boards = static_cast<std::uint32_t>(n_dates);
  manifest.n_ok = static_cast<std::uint32_t>(n_dates);
  const Status manifest_written =
      write_manifest_file((fixture.dir / "surface_manifest.tsv").string(), manifest);
  EXPECT_TRUE(manifest_written.has_value())
      << (manifest_written.has_value() ? std::string{} : manifest_written.error().to_string());

  std::string universe = "effective_date\tsymbol\traw_weight\tsource\tas_of\n";
  for (const PvUniverseBlock &block : blocks) {
    for (const auto &[symbol, weight] : block.names) {
      char cell[64];
      std::snprintf(cell, sizeof cell, "%.17g", weight);
      universe += tsv_row({block.effective_date, symbol, cell, "test", block.effective_date});
    }
  }
  write_file(fixture.dir / "universe_schedule.tsv", universe);

  // Keys both `read_run_spec` (loose) and `read_dispersion_run_config` (strict)
  // accept, so the SAME file drives the route before and after the C-15 cutover.
  std::string spec;
  spec += tsv_row({"date_lo", fixture.dates.front()});
  spec += tsv_row({"date_hi", fixture.dates.back()});
  spec += tsv_row({"opra_root", "."});
  spec += tsv_row({"universe_schedule", "universe_schedule.tsv"});
  spec += tsv_row({"min_names", "2"});
  spec += tsv_row({"target_dte_days", "30"});
  spec += tsv_row({"min_dte_days", "21"});
  spec += tsv_row({"max_dte_days", "60"});
  spec += tsv_row({"gross_index_vega", "10000"});
  spec += extra_spec;
  write_file(fixture.dir / "run_spec.tsv", spec);
  return fixture;
}

// Header + rows of a TSV, split on '\t'. Empty vector if the file is unreadable.
[[nodiscard]] std::vector<std::vector<std::string>> read_tsv_rows(const fs::path &path) {
  std::vector<std::vector<std::string>> rows;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return rows;
  }
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    std::vector<std::string> cells;
    std::size_t start = 0;
    while (true) {
      const std::size_t tab = line.find('\t', start);
      cells.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
      if (tab == std::string::npos) {
        break;
      }
      start = tab + 1;
    }
    rows.push_back(std::move(cells));
  }
  return rows;
}

// Index of `name` in a header row, or SIZE_MAX.
[[nodiscard]] std::size_t column_of(const std::vector<std::string> &header, std::string_view name) {
  for (std::size_t i = 0; i < header.size(); ++i) {
    if (header[i] == name) {
      return i;
    }
  }
  return static_cast<std::size_t>(-1);
}

// The two point-in-time blocks used by the C-1 gate: the second REMOVES BBB,
// ADDS CCC and reweights, so the first-session basket and the last-session
// basket share exactly one name.
[[nodiscard]] std::vector<PvUniverseBlock> pv_reconstituting_blocks() {
  return {PvUniverseBlock{"2026-10-01", {{"AAA", 0.6}, {"BBB", 0.4}}},
          PvUniverseBlock{"2026-10-03", {{"AAA", 0.5}, {"CCC", 0.5}}}};
}

} // namespace

TEST(DispersionReferenceReconcile, M10_ScheduleWithNoRollDateColumnIsRejected) {
  const fs::path run = make_run_dir("m10_roll_date");
  // Every numeric column the reconciler reads is intact, so `dec()`/`intcol()`
  // are all satisfied. Only `roll_date` — read exclusively through `str()` — is
  // gone, and it is the identity of the roll record that gets PUBLISHED into
  // reference_reconciliation.tsv.
  write_file(run / "trade_schedule.tsv",
             drop_column(schedule_text(), "roll_date", /*has_magic=*/true));

  auto records = reconcile_dispersion_reference(run, /*schedule_only=*/true);
  ASSERT_FALSE(records)
      << "a trade_schedule.tsv with no roll_date column reconciled clean and published "
      << records->size() << " record(s), the first carrying date=\""
      << (records->empty() ? std::string{} : (*records)[0].date) << "\"";
  EXPECT_NE(records.error().to_string().find("roll_date"), std::string::npos)
      << "the error must name the missing column, as dec()/intcol() do: "
      << records.error().to_string();

  std::error_code error;
  fs::remove_all(run, error);
}

// The same property on the real artifacts this box carries, rather than on
// synthetic ones. The M10 fix turns a missing string column into an error, so
// the question it raises is empirical: does any real reconciler input actually
// LACK one of the columns `str()` reads? This drives every published run
// directory under the reference corpus through the reconciler and requires both
// that it succeeds and that no published record carries an empty date — the
// observable signature of the degrade this fix removes.
//
// STRICTLY READ-ONLY. `reconcile_dispersion_reference` opens the four artifacts
// and writes nothing; the sidecar is published by the separate
// `write_reference_reconciliation_file`, which is deliberately NOT called here.
// The reference corpus is never written to.
TEST(DispersionReferenceReconcileRealData, PublishedRunDirectoriesCarryEveryStringColumn) {
  const fs::path root = "C:/atx-data/spy-dispersion/runs";
  std::error_code error;
  if (!fs::is_directory(root, error)) {
    GTEST_SKIP() << "reference run corpus absent: " << root.string()
                 << " -- this check is empirical and needs the published corpus. Its synthetic "
                    "twin (DispersionReferenceReconcile.M10_*) runs everywhere and is what gates "
                    "the fix itself.";
  }
  std::string rejected;
  const std::vector<fs::path> run_dirs = scan_complete_reference_run_dirs(root, rejected);
  // REV-TAIL I-1(b). This used to `GTEST_SKIP` green when nothing qualified,
  // which made it a test that could stop covering anything without saying so.
  // The RunArchive cutover positively asserts the shipped pipeline NO LONGER
  // writes these four loose artifacts (test_dispersion_runarchive_e2e.py:366-367,
  // `test_run_archive_is_published` — cite the TEST NAME, not only the line: this
  // citation has now gone stale twice by line-shift alone, REV-FIXTAIL Minor 1),
  // so every run directory published from now on is excluded BY CONSTRUCTION and
  // this test's coverage decays silently as the corpus ages. 9 of the 20
  // directories under the corpus root qualify today (measured 2026-07-25); the
  // floor below turns the day that reaches 0 into a red test rather than a green
  // skip nobody reads. A missing corpus root (CI) still skips, loudly, above.
  ASSERT_FALSE(run_dirs.empty())
      << "the reference run corpus at " << root.string()
      << " exists but no longer carries a single directory with all four loose artifacts, so this "
         "check has silently stopped covering anything. Either the corpus has aged past the "
         "RunArchive cutover -- in which case this test needs re-basing onto run.atxrun rather "
         "than deleting -- or the corpus moved. Rejected directories:\n"
      << rejected;
  for (const fs::path &dir : run_dirs) {
    auto records = reconcile_dispersion_reference(dir, /*schedule_only=*/false);
    ASSERT_TRUE(records) << dir.filename().string() << ": " << records.error().to_string();
    EXPECT_FALSE(records->empty()) << dir.filename().string() << " produced no records";
    for (const ReferenceReconRecord &record : *records) {
      EXPECT_FALSE(record.date.empty())
          << dir.filename().string() << ": a published " << record.record_type
          << " record carries an empty date — the signature of a string column read through a "
             "silent empty-string degrade";
    }
  }
}

// ── REV-TAIL I-1(b) — the floor under the data-gated M10 check ────────────────
//
// The selection rule the real-data test depends on, driven on synthetic input so
// it runs on every box including CI. Without this the floor added above would
// itself be untested: the only thing standing between "the corpus aged out" and
// "a green skip nobody reads" is that this scan returns empty AND names why.
TEST(DispersionReferenceRunDirScan, ExcludesIncompleteDirectoriesAndNamesWhatTheyLack) {
  const fs::path root = make_run_dir("scan_root");
  const fs::path complete = root / "complete-run";
  const fs::path partial = root / "partial-run";
  fs::create_directories(complete);
  fs::create_directories(partial);
  for (const char *artifact : kLooseReconcilerInputs) {
    write_file(complete / artifact, "x\n");
  }
  // Everything but `backtest.tsv` — the artifact the RunArchive cutover stopped
  // writing, which is precisely how a real directory falls out of the corpus.
  write_file(partial / "trade_schedule.tsv", "x\n");
  write_file(partial / "contract_marks.tsv", "x\n");
  write_file(partial / "reconciliation.tsv", "x\n");

  std::string rejected;
  const std::vector<fs::path> found = scan_complete_reference_run_dirs(root, rejected);

  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found.front().filename().string(), "complete-run");
  EXPECT_NE(rejected.find("partial-run"), std::string::npos)
      << "an excluded directory must be NAMED, or an empty result explains nothing: " << rejected;
  EXPECT_NE(rejected.find("backtest.tsv"), std::string::npos)
      << "the rejection must name the missing artifact: " << rejected;

  std::error_code error;
  fs::remove_all(root, error);
}

// And the empty case the floor exists to catch: a corpus root that exists but
// carries nothing usable must produce an empty result WITH a non-empty
// explanation, which is what makes the ASSERT_FALSE message above actionable.
TEST(DispersionReferenceRunDirScan, AnAgedOutCorpusYieldsNothingAndSaysWhy) {
  const fs::path root = make_run_dir("scan_aged");
  fs::create_directories(root / "post-cutover-run");
  write_file(root / "post-cutover-run" / "run.atxrun", "x\n");

  std::string rejected;
  const std::vector<fs::path> found = scan_complete_reference_run_dirs(root, rejected);

  EXPECT_TRUE(found.empty());
  EXPECT_FALSE(rejected.empty())
      << "an empty scan with an empty explanation is the silent-skip defect this floor removes";
  EXPECT_NE(rejected.find("post-cutover-run"), std::string::npos) << rejected;

  std::error_code error;
  fs::remove_all(root, error);
}

// ── REV-TAIL I-1 — the three LIBRARY-ONLY entry points ────────────────────────
//
// `dispersion_build_schedule`, `dispersion_run_backtest` and `dispersion_verify`
// have no production caller: the shipped subcommands of the same name keep their
// own bodies -- `build_schedule_command` and `run_backtest_command` publish
// run.atxrun sections (each via `RunDir::write_run_archive`,
// spy_dispersion_backtest.cpp:337 and :483) and `verify_command` READS one
// (`RunDir(run_dir).verify()`, :359) -- because the
// three library twins write the loose result files the RunArchive cutover
// replaced. That is a DELIBERATE split and is documented at dispersion_run.hpp's
// declaration block.
//
// What was NOT true is the sentence that block used to carry: that each is
// "covered directly off the filesystem by dispersion_run_test.cpp". This file
// called none of them, so ~500 lines of reserve were held in the tree on the
// strength of a coverage claim with nothing behind it. This is that coverage,
// written to be honest about what it is: it does not pretend to exercise the
// economics, it pins that each entry point is LINKED, REACHABLE, and FAILS
// CLOSED naming its missing input rather than succeeding vacuously on an empty
// directory. A reserve nobody calls at least cannot rot into a silent no-op.
TEST(DispersionLibraryOnlyEntryPoints, EachIsReachableAndFailsClosedNamingTheMissingSpec) {
  const fs::path run = make_run_dir("library_only_empty");

  const Status build_schedule = dispersion_build_schedule(run);
  ASSERT_FALSE(build_schedule) << "dispersion_build_schedule accepted an empty run directory";
  EXPECT_NE(build_schedule.error().to_string().find("run_spec"), std::string::npos)
      << "the error must name the input it could not read: " << build_schedule.error().to_string();

  const Status run_backtest = dispersion_run_backtest(run);
  ASSERT_FALSE(run_backtest) << "dispersion_run_backtest accepted an empty run directory";
  EXPECT_NE(run_backtest.error().to_string().find("run_spec"), std::string::npos)
      << "the error must name the input it could not read: " << run_backtest.error().to_string();

  const Result<DispersionVerifyReport> verify = dispersion_verify(run);
  ASSERT_FALSE(verify) << "dispersion_verify passed an empty run directory";
  EXPECT_NE(verify.error().to_string().find("run_spec"), std::string::npos)
      << "the error must name the input it could not read: " << verify.error().to_string();

  std::error_code error;
  fs::remove_all(run, error);
}

// ── REV-TAIL I-3 — the four keys that parsed, validated, and did nothing ──────
//
// `dispersion_run_surface_backtest` (dispersion_run.cpp:2407, whose strict read
// -- `read_dispersion_run_config` -- is at :2412) reads the STRICT typed config,
// so `unpriced`, `provenance`,
// `book_entry_fill_slippage` and `reconcile_nav` each bind by name (`provenance`
// at :1428-1431, the other three at :1434, :1442 and :1443) and survive
// `reject_unknown()`. It then hands off to `dispersion_backtest_config_from`,
// which hardcoded `run.unpriced = UnpricedLotPolicy::Error` and never set the
// other three at all. Four spec keys accepted by name, zero effect on the shipped
// `run-surface-backtest` — the sprint's signature defect class, on the route
// 347ad44's own evidence is built on.
//
// This asserts the one property that makes a knob a knob: a non-default value set
// on the typed spec is the value the engine actually runs under. `config.run` is
// what reaches `run_backtest` on the SHIPPED route — `run_dispersion_surface_
// backtest`, at dispersion_run.cpp:971 and :978 — so this is the engine's real
// input and not a bookkeeping copy. (REV-FIXTAIL Minor 2: this used to cite
// dispersion_backtest.cpp:112,120. Those lines are in `run_dispersion_backtest`,
// a route this fix is not about; the property held, the pointer did not.)
TEST(DispersionBacktestConfigFrom, EveryDeclaredEngineKnobReachesTheEngineRunConfig) {
  DispersionRunConfig config;
  config.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  config.provenance = SurfaceProvenancePolicy::RequireAdmittedRisk;
  config.book_entry_fill_slippage = true;
  config.reconcile_nav = true;

  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(config);

  EXPECT_EQ(backtest.run.unpriced, UnpricedLotPolicy::ExcludeAndReport)
      << "spec key `unpriced` was accepted and then overwritten with a hardcoded Error";
  EXPECT_EQ(backtest.run.surface_provenance_policy, SurfaceProvenancePolicy::RequireAdmittedRisk)
      << "spec key `provenance` was accepted and never assigned";
  EXPECT_TRUE(backtest.run.book_entry_fill_slippage)
      << "spec key `book_entry_fill_slippage` was accepted and never assigned";
  EXPECT_TRUE(backtest.run.reconcile_nav)
      << "spec key `reconcile_nav` was accepted and never assigned";
}

// The other half of I-3, and the reason wiring the four keys cannot move a golden:
// every one of them defaults to exactly the value the surface route hardcoded or
// inherited, so a spec that does not mention them produces a byte-identical engine
// config before and after the fix. If a default here ever diverges from
// `RunConfig`'s, this pins the day it happens.
TEST(DispersionBacktestConfigFrom, DefaultSpecKeepsTheShippedEngineDefaults) {
  const DispersionBacktestConfig backtest = dispersion_backtest_config_from(DispersionRunConfig{});

  EXPECT_EQ(backtest.run.unpriced, UnpricedLotPolicy::Error);
  EXPECT_EQ(backtest.run.surface_provenance_policy, SurfaceProvenancePolicy::Compatibility);
  EXPECT_FALSE(backtest.run.book_entry_fill_slippage);
  EXPECT_FALSE(backtest.run.reconcile_nav);
}

// The two builders must agree on every knob they both carry. The declaration
// comment on `dispersion_engine_run_config_from` (dispersion_run.hpp:324)
// calls it "the single place the typed spec becomes
// engine behaviour, so a knob that is set here is provably reachable and one that is
// not is provably dead". That claim is only true while the surface route's builder
// cannot drift from it — which is what this pins.
TEST(DispersionBacktestConfigFrom, AgreesWithTheEngineRunConfigBuilderOnEveryKnob) {
  DispersionRunConfig config;
  config.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  config.provenance = SurfaceProvenancePolicy::RequireAdmittedRisk;
  config.book_entry_fill_slippage = true;
  config.reconcile_nav = true;
  config.rate.flat_rate = 0.043;
  config.rate.apply_to_financing = true;

  const RunConfig engine = dispersion_engine_run_config_from(config);
  const RunConfig surface = dispersion_backtest_config_from(config).run;

  EXPECT_EQ(surface.unpriced, engine.unpriced);
  EXPECT_EQ(surface.surface_provenance_policy, engine.surface_provenance_policy);
  EXPECT_EQ(surface.book_entry_fill_slippage, engine.book_entry_fill_slippage);
  EXPECT_EQ(surface.reconcile_nav, engine.reconcile_nav);
  EXPECT_DOUBLE_EQ(surface.financing.borrow_rate, engine.financing.borrow_rate);
  EXPECT_EQ(surface.financing.finance_premium, engine.financing.finance_premium);
}

// ── REV-MTIDY I-1 — the guard the quote-knob repair shipped without ──────────
//
// REV-FIXTAIL I-A made the three `quote_*` spec keys reach the SHIPPED
// `build-schedule` selection with one assignment in the example's `main`.
// Measured before this test existed: deleting that assignment and running the
// full label gate gave 2262/2262 passed, 0 failed — byte-identical to the run
// with it, `atx-vol-python` (the only ctest entry that executes the example
// binary) included. I-A's own two gtests call `listed_selection_config_from`,
// one layer BELOW the assignment, and the e2e CLI chain drives only default
// values, which equal the pre-fix behaviour by construction. The construction
// now lives in `listed_schedule_spec_from` and these call it, so deleting the
// `quality` line turns THIS red instead of nothing.
//
// The composition through `listed_selection_config_from` is deliberate: what
// matters economically is not that a POD field was copied but that the declared
// policy is the one `select_listed_dispersion` runs under, and the composition
// below is exactly what both `build_schedule_command` and the library
// `dispersion_build_schedule` now perform.
TEST(DispersionScheduleSpecFrom, EveryDeclaredScheduleKnobReachesTheSelectionPolicy) {
  RunSpec spec;
  spec.target_dte_days = 41.0;
  spec.min_dte_days = 23.0;
  spec.max_dte_days = 71.0;
  spec.roll_dte_days = 9.0;
  spec.min_names = 17u;
  spec.min_weight_coverage = 0.63;
  spec.gross_index_vega = 25000.0;
  spec.core_mode = true;

  DispersionRunConfig config;
  config.quote_quality.min_bid = 0.07;
  config.quote_quality.max_quote_age_ns = 123'456'789;
  config.quote_quality.reject_locked = true;

  const ListedScheduleSpec sched = listed_schedule_spec_from(spec, config);

  EXPECT_DOUBLE_EQ(sched.target_dte_days, 41.0);
  EXPECT_DOUBLE_EQ(sched.min_dte_days, 23.0);
  EXPECT_DOUBLE_EQ(sched.max_dte_days, 71.0);
  EXPECT_DOUBLE_EQ(sched.roll_dte_days, 9.0);
  EXPECT_EQ(sched.min_names, 17u);
  EXPECT_DOUBLE_EQ(sched.min_weight_coverage, 0.63);
  EXPECT_DOUBLE_EQ(sched.gross_index_vega, 25000.0);
  EXPECT_TRUE(sched.core_mode);

  // The three that only the STRICT typed read can supply, checked where they
  // actually bite — on the selection config the builder's loop runs under.
  const ListedDispersionSelectionConfig selection = listed_selection_config_from(sched);
  EXPECT_DOUBLE_EQ(selection.quality.min_bid, 0.07)
      << "spec key `quote_min_bid` was bound, validated, published to run_config.tsv "
         "as EFFECTIVE, and reached no shipped selection";
  EXPECT_EQ(selection.quality.max_quote_age_ns, 123'456'789)
      << "spec key `quote_max_age_ns` was bound and never assigned";
  EXPECT_TRUE(selection.quality.reject_locked)
      << "spec key `quote_reject_locked` was bound and never assigned";

  // The four the loose RunSpec supplies must survive the same trip.
  EXPECT_DOUBLE_EQ(selection.target_dte_days, 41.0);
  EXPECT_DOUBLE_EQ(selection.min_dte_days, 23.0);
  EXPECT_DOUBLE_EQ(selection.max_dte_days, 71.0);
  EXPECT_EQ(selection.min_names, 17u);
}

// The other half, and the reason the I-A wiring could not move a golden: a spec
// naming none of the three `quote_*` keys must produce exactly what the
// selection loop default-constructed before I-A existed. If a default on either
// side ever diverges, this pins the day it happens.
TEST(DispersionScheduleSpecFrom, DefaultSpecKeepsTheShippedScheduleDefaults) {
  const ListedScheduleSpec sched = listed_schedule_spec_from(RunSpec{}, DispersionRunConfig{});
  const ListedDispersionSelectionConfig selection = listed_selection_config_from(sched);
  const ListedDispersionSelectionConfig pre_fix_default;

  EXPECT_DOUBLE_EQ(selection.quality.min_bid, pre_fix_default.quality.min_bid);
  EXPECT_EQ(selection.quality.max_quote_age_ns, pre_fix_default.quality.max_quote_age_ns);
  EXPECT_EQ(selection.quality.reject_locked, pre_fix_default.quality.reject_locked);
  EXPECT_DOUBLE_EQ(selection.target_dte_days, pre_fix_default.target_dte_days);
  EXPECT_DOUBLE_EQ(selection.min_dte_days, pre_fix_default.min_dte_days);
  EXPECT_DOUBLE_EQ(selection.max_dte_days, pre_fix_default.max_dte_days);
  EXPECT_EQ(selection.min_names, pre_fix_default.min_names);
  EXPECT_DOUBLE_EQ(selection.required_multiplier, pre_fix_default.required_multiplier);
}

TEST(DispersionReferenceReconcile, M10_ContractMarksWithNoStatusColumnIsRejected) {
  const fs::path run = make_run_dir("m10_status");
  write_file(run / "trade_schedule.tsv", schedule_text());
  write_file(run / "contract_marks.tsv", drop_column(marks_text(), "status", /*has_magic=*/false));
  write_file(run / "reconciliation.tsv", reconciliation_text_zero_quote_coverage());
  write_file(run / "backtest.tsv", backtest_text());

  auto records = reconcile_dispersion_reference(run, /*schedule_only=*/false);
  ASSERT_FALSE(records)
      << "contract marks with no status column reconciled clean: every lot was scored \"no "
         "usable quote\", giving quote NAV "
      << (records->size() < 3u ? 0.0 : (*records)[2].computed_quote_mid_nav)
      << " and coverage " << (records->size() < 3u ? 0.0 : (*records)[2].quote_mid_coverage)
      << " against a model NAV of "
      << (records->size() < 3u ? 0.0 : (*records)[2].computed_model_nav);
  EXPECT_NE(records.error().to_string().find("status"), std::string::npos)
      << "the error must name the missing column, as dec()/intcol() do: "
      << records.error().to_string();

  std::error_code error;
  fs::remove_all(run, error);
}

// ── REVIEW C-1: the projected-VaR book must be the AS-OF book ───────────────
//
// `dispersion_book_var` defines the VaR reference as the LAST frame's value
// (listed_dispersion_pipeline.cpp:508-516) and every loss as
// `reference_value - frame.value` (historical_projection.cpp:118-147). That is
// historical-simulation VaR, and it is only VaR if the book being re-valued is
// the book held AT the reference date. The route used to resolve membership and
// size quantities on `snapshots.front()` — the OLDEST session — so the published
// number was the risk of a book nobody holds: a reconstituted-away name still in
// it, a current name missing from it, and quantities sized on stale spot/vol.
//
// AS-OF SEMANTICS (recorded deliberately, per the brief): the as-of is the LAST
// QUALIFIED SNAPSHOT IN THE MANIFEST CLOCK. Chosen over a caller-supplied as-of
// because the reference value is not a free parameter — it is `frames.back()`,
// fixed by the seam — so any anchor other than the last session re-opens exactly
// the mismatch this closes. A caller who wants a different as-of moves `date_hi`,
// which already bounds the manifest. The artifact records which session was used
// so no reader has to infer it.

// The fixture's own contract. `make_pit_universe_resolver` resolves on
// `utc_date_from_ns(snapshot.ts_ns())`, so if this drifts the universe blocks
// below stop meaning what they say and both gates go quietly vacuous.
TEST(DispersionProjectedVarFixture, AnchorTimestampsLandOnTheNamedDates) {
  EXPECT_EQ(utc_date_from_ns(kPvBaseNs), "2026-10-01");
  EXPECT_EQ(utc_date_from_ns(kPvBaseNs + 2 * kPvDayNs), "2026-10-03");
  EXPECT_EQ(utc_date_from_ns(kPvBaseNs + 4 * kPvDayNs), "2026-10-05");
}

TEST(DispersionProjectedVar, C1_BookIsResolvedAndSizedAtTheAsOfSnapshotNotTheFirst) {
  const PvFixture fixture = make_pv_fixture("pv_c1_asof", pv_reconstituting_blocks());
  ASSERT_EQ(fixture.dates.size(), 5u);

  const Status ran = dispersion_run_projected_var(fixture.dir);
  ASSERT_TRUE(ran.has_value()) << (ran.has_value() ? std::string{} : ran.error().to_string());

  const std::vector<std::vector<std::string>> legs =
      read_tsv_rows(fixture.dir / "projected_risk_legs.tsv");
  ASSERT_GE(legs.size(), 2u);
  const std::size_t uid_col = column_of(legs.front(), "uid");
  ASSERT_NE(uid_col, static_cast<std::size_t>(-1));
  std::vector<std::string> uids;
  for (std::size_t row = 1; row < legs.size(); ++row) {
    ASSERT_GT(legs[row].size(), uid_col);
    if (std::find(uids.begin(), uids.end(), legs[row][uid_col]) == uids.end()) {
      uids.push_back(legs[row][uid_col]);
    }
  }
  std::sort(uids.begin(), uids.end());

  // Block 1 (2026-10-01) is {AAA=2, BBB=3}; block 2 (2026-10-03, and therefore
  // the 2026-10-05 as-of) is {AAA=2, CCC=4}. A book anchored on the FIRST
  // session prices BBB and never sees CCC.
  const std::vector<std::string> as_of_basket = {"1", "2", "4"};
  EXPECT_EQ(uids, as_of_basket)
      << "the projected book's uids are not the as-of basket: a first-anchored book still "
         "holds the reconstituted-away name (uid 3 = BBB) and is missing the current one "
         "(uid 4 = CCC)";

  const std::vector<std::vector<std::string>> summary =
      read_tsv_rows(fixture.dir / "projected_var.tsv");
  ASSERT_GE(summary.size(), 2u);
  const std::size_t as_of_date_col = column_of(summary.front(), "as_of_date");
  const std::size_t as_of_ts_col = column_of(summary.front(), "as_of_ts_ns");
  const std::size_t book_fp_col = column_of(summary.front(), "book_fingerprint");
  ASSERT_NE(as_of_date_col, static_cast<std::size_t>(-1))
      << "projected_var.tsv does not record WHICH session the book was built on";
  ASSERT_NE(as_of_ts_col, static_cast<std::size_t>(-1));
  ASSERT_NE(book_fp_col, static_cast<std::size_t>(-1))
      << "projected_var.tsv does not record a book fingerprint";
  for (std::size_t row = 1; row < summary.size(); ++row) {
    EXPECT_EQ(summary[row][as_of_date_col], fixture.dates.back());
    EXPECT_EQ(summary[row][as_of_ts_col], std::to_string(kPvBaseNs + 4 * kPvDayNs));
    EXPECT_NE(summary[row][book_fp_col], "0");
  }

  // The reference the losses are measured from is the as-of session's own value,
  // which is what makes the number VaR rather than a drift between two books.
  const std::vector<std::vector<std::string>> frames =
      read_tsv_rows(fixture.dir / "projected_risk_scenarios.tsv");
  ASSERT_EQ(frames.size(), fixture.dates.size() + 1u);
  const std::size_t frame_value_col = column_of(frames.front(), "value");
  const std::size_t ref_col = column_of(summary.front(), "reference_value");
  ASSERT_NE(frame_value_col, static_cast<std::size_t>(-1));
  ASSERT_NE(ref_col, static_cast<std::size_t>(-1));
  EXPECT_EQ(summary[1][ref_col], frames.back()[frame_value_col]);

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}
