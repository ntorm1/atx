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
#include <map>
#include <memory>
#include <span>
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
#include "atx/vol/listed_opra.hpp"         // ListedDefinitionTable, write_listed_definitions_file
#include "atx/vol/occ_ess.hpp"             // read_occ_ess_report_file
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

// `second_total` is the second session's P&L (and therefore its NAV): the
// default reproduces the reconciliation the marks fixture above implies, and
// "0" gives the flat run a no-move marks fixture implies.
std::string backtest_text(std::string_view second_total = "200") {
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
  return header + row("2026-07-10", "100", "0") + row("2026-07-11", "200", second_total);
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
// `include_spy = false` leaves SPY out of every archive (it stays in the
// manifest label only). That is what makes an index-symbol routing defect
// OBSERVABLE: a route that takes a hardcoded "SPY" index leg then cannot resolve
// a uid for it at all, instead of quietly resolving the wrong surface.
[[nodiscard]] PvFixture make_pv_fixture(std::string_view leaf,
                                        const std::vector<PvUniverseBlock> &blocks,
                                        const std::string &extra_spec = {}, std::size_t n_dates = 5,
                                        std::size_t n_unused_surfaces = 0,
                                        bool symbol_derived_uids = false, bool include_spy = true) {
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
    const auto fixture_uid = [symbol_derived_uids](std::string_view symbol,
                                                   std::uint32_t legacy) {
      return symbol_derived_uids ? uid_for_symbol(symbol) : legacy;
    };
    const PricedSurface index =
        pv_surface(fixture_uid("SPY", 1u), 500.0 * drift, now, 0.00 + bump);
    const PricedSurface aaa =
        pv_surface(fixture_uid("AAA", 2u), 100.0 * drift, now, 0.02 + bump);
    const PricedSurface bbb =
        pv_surface(fixture_uid("BBB", 3u), 120.0 * drift, now, 0.03 + bump);
    const PricedSurface ccc =
        pv_surface(fixture_uid("CCC", 4u), 80.0 * drift, now, 0.05 + bump);
    const std::string path = (archive_dir / (date + ".atxvsa")).string();
    std::vector<std::string> unused_symbols;
    std::vector<PricedSurface> unused_surfaces;
    unused_symbols.reserve(n_unused_surfaces);
    unused_surfaces.reserve(n_unused_surfaces);
    for (std::size_t i = 0u; i < n_unused_surfaces; ++i) {
      unused_symbols.push_back("UNUSED" + std::to_string(i));
      unused_surfaces.push_back(
          pv_surface(static_cast<std::uint32_t>(100u + i),
                     (40.0 + static_cast<double>(i)) * drift, now,
                     0.01 + 0.0001 * static_cast<double>(i) + bump));
    }
    std::vector<SurfaceArchiveItem> items;
    items.reserve(4u + n_unused_surfaces);
    if (include_spy) {
      items.push_back(SurfaceArchiveItem{"SPY", &index});
    }
    items.push_back(SurfaceArchiveItem{"AAA", &aaa});
    items.push_back(SurfaceArchiveItem{"BBB", &bbb});
    items.push_back(SurfaceArchiveItem{"CCC", &ccc});
    for (std::size_t i = 0u; i < n_unused_surfaces; ++i) {
      items.push_back(SurfaceArchiveItem{unused_symbols[i], &unused_surfaces[i]});
    }
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

// The same reconstitution, but with UNEQUAL as-of weights. On a 50/50 basket
// vega-neutral and equal-vega sizing coincide exactly, which would make any gate
// on the `weighting` knob vacuous — it would pass whether or not the knob was
// read. Used by the C-15 gates, which are about knobs reaching the book.
[[nodiscard]] std::vector<PvUniverseBlock> pv_unequal_weight_blocks() {
  return {PvUniverseBlock{"2026-10-01", {{"AAA", 0.6}, {"BBB", 0.4}}},
          PvUniverseBlock{"2026-10-03", {{"AAA", 0.75}, {"CCC", 0.25}}}};
}

} // namespace

TEST(DispersionDividends, ExactCorpusInputsPersistAndReachSurfaceAndListedRunConfigs) {
  const std::string financing_keys =
      tsv_row({"financing_shares_carry", "1"});
  const PvFixture without =
      make_pv_fixture("f1_without_dividends", pv_reconstituting_blocks(),
                      financing_keys, 5u, 0u, /*symbol_derived_uids=*/true);
  const PvFixture with =
      make_pv_fixture("f1_with_dividends", pv_reconstituting_blocks(),
                      financing_keys + tsv_row({"dividend_ledger", "share_dividends.tsv"}),
                      5u, 0u, /*symbol_derived_uids=*/true);

  const std::int64_t ex_ts_ns = kPvBaseNs + kPvDayNs;
  std::vector<ShareDividendObservation> observations;
  for (const std::string &symbol : {"SPY", "AAA", "BBB", "CCC"}) {
    observations.push_back(ShareDividendObservation{
        with.dates.front(), symbol, uid_for_symbol(symbol), ex_ts_ns, 10.0,
        "dividend-fixture", with.dates.front() + "T12:00:00Z", 101u, 202u});
  }
  ASSERT_TRUE(write_share_dividend_artifact(
      with.dir / "share_dividends.tsv", observations));

  auto parsed = read_dispersion_run_config(with.dir / "run_spec.tsv");
  ASSERT_TRUE(parsed.has_value())
      << (parsed.has_value() ? std::string{} : parsed.error().to_string());
  ASSERT_EQ(parsed->financing.share_dividends.size(), observations.size());
  const RunConfig listed_engine = dispersion_engine_run_config_from(*parsed);
  ASSERT_EQ(listed_engine.financing.share_dividends.size(),
            parsed->financing.share_dividends.size())
      << "the listed replay builder dropped the corpus dividend ledger";
  const DispersionBacktestConfig surface_engine =
      dispersion_backtest_config_from(*parsed);
  ASSERT_EQ(surface_engine.run.financing.share_dividends.size(),
            parsed->financing.share_dividends.size())
      << "the surface route dropped the corpus dividend ledger";
  for (std::size_t i = 0u; i < parsed->financing.share_dividends.size(); ++i) {
    const ShareDividend &expected = parsed->financing.share_dividends[i];
    const ShareDividend &listed = listed_engine.financing.share_dividends[i];
    const ShareDividend &surface =
        surface_engine.run.financing.share_dividends[i];
    EXPECT_EQ(listed.uid, expected.uid);
    EXPECT_EQ(listed.ex_ts_ns, expected.ex_ts_ns);
    EXPECT_DOUBLE_EQ(listed.amount, expected.amount);
    EXPECT_EQ(surface.uid, expected.uid);
    EXPECT_EQ(surface.ex_ts_ns, expected.ex_ts_ns);
    EXPECT_DOUBLE_EQ(surface.amount, expected.amount);
  }

  const Status without_status = dispersion_run_surface_backtest(without.dir);
  ASSERT_TRUE(without_status.has_value())
      << (without_status.has_value() ? std::string{}
                                     : without_status.error().to_string());
  const Status with_status = dispersion_run_surface_backtest(with.dir);
  ASSERT_TRUE(with_status.has_value())
      << (with_status.has_value() ? std::string{} : with_status.error().to_string());

  const auto without_rows = read_tsv_rows(without.dir / "surface_backtest.tsv");
  const auto with_rows = read_tsv_rows(with.dir / "surface_backtest.tsv");
  ASSERT_EQ(without_rows.size(), with_rows.size());
  ASSERT_GE(with_rows.size(), 3u);
  const std::size_t financing_col = column_of(with_rows.front(), "financing");
  ASSERT_NE(financing_col, static_cast<std::size_t>(-1));
  EXPECT_NE(std::stod(with_rows[2][financing_col]),
            std::stod(without_rows[2][financing_col]))
      << "the exact ex-date dividend reached config but did not reach the hedge ledger";

  std::error_code error;
  fs::remove_all(without.dir, error);
  fs::remove_all(with.dir, error);
}

TEST(DispersionDividends, CorpusDividendInputCarriesExactScheduleAndProvenance) {
  const fs::path dir = make_run_dir("f1_dividend_inputs");
  const fs::path path = dir / "dividend_inputs.tsv";
  write_file(path,
             "ATX_CORPUS_DIVIDENDS\t1\n"
             "date\tsymbol\tex_ts_ns\tamount\tsource\tas_of\n" +
                 tsv_row({"2026-10-01", "AAA", "1790971200000000000", "1.25",
                          "vendor-dividends", "2026-09-30T20:00:00Z"}));
  auto table = read_corpus_dividend_inputs(path);
  ASSERT_TRUE(table.has_value())
      << (table.has_value() ? std::string{} : table.error().to_string());
  const CorpusMarketInputCell *cell = table->find("2026-10-01", "AAA");
  ASSERT_NE(cell, nullptr);
  ASSERT_EQ(cell->cash_divs.size(), 1u);
  EXPECT_EQ(cell->cash_divs[0].ex_date_ns, 1790971200000000000LL);
  EXPECT_DOUBLE_EQ(cell->cash_divs[0].amount, 1.25);
  EXPECT_EQ(cell->provenance.dividends.source, "vendor-dividends");
  EXPECT_EQ(cell->provenance.dividends.as_of, "2026-09-30T20:00:00Z");
  EXPECT_NE(cell->provenance.fingerprint, 0u);

  std::error_code error;
  fs::remove_all(dir, error);
}

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

// ── REVIEW C-15: the route must build the book the SPEC describes ───────────
//
// `dispersion_run_projected_var` read the loose `RunSpec` and then hardcoded
// `side = ShortIndexLongNames` and `multiplier = 100.0`, and never saw
// `weighting` or `strike` at all — while the strict typed configuration exposes
// all four. One production spec therefore built one book in the surface/listed
// backtest and a DIFFERENT book in projected VaR, with no error and no
// diagnostic. The gate below is deliberately NOT a builder-to-builder identity
// (that would be tautological once both call the same function): it compares the
// legs the ROUTE actually published against a book the TEST builds through the
// surface route's own builder.

TEST(DispersionProjectedVar, P1_LoadsAnchorOnceAndSubsetsHistoricalArchives) {
  constexpr std::size_t n_dates = 7u;
  constexpr std::size_t n_unused = 24u;
  const PvFixture fixture =
      make_pv_fixture("pv_p1_bounded", pv_reconstituting_blocks(), {}, n_dates, n_unused);

  MarketSnapshot::reset_open_count();
  MarketSnapshot::reset_deserialized_bytes();
  const Status ran = dispersion_run_projected_var(fixture.dir);
  ASSERT_TRUE(ran.has_value()) << (ran.has_value() ? std::string{} : ran.error().to_string());
  const std::uint64_t route_opens = MarketSnapshot::open_count();
  const std::uint64_t route_bytes = MarketSnapshot::deserialized_bytes();
  EXPECT_EQ(route_opens, n_dates)
      << "the anchor must be reused for its scenario, not loaded a second time";
  ASSERT_TRUE(verify_projected_var_artifacts(fixture.dir, n_dates));

  MarketSnapshot::reset_open_count();
  MarketSnapshot::reset_deserialized_bytes();
  for (const std::string &date : fixture.dates) {
    auto snapshot = MarketSnapshot::load(
        (fixture.dir / "surfaces" / (date + ".atxvsa")).string(),
        QueryPricingTier::LegacyCompatible, {}, ArchiveBacking::Sealed);
    ASSERT_TRUE(snapshot.has_value())
        << (snapshot.has_value() ? std::string{} : snapshot.error().to_string());
  }
  const std::uint64_t whole_board_bytes = MarketSnapshot::deserialized_bytes();
  EXPECT_EQ(MarketSnapshot::open_count(), n_dates);
  EXPECT_LT(route_bytes, whole_board_bytes)
      << "projected VaR still materialized every unused surface record";

  MarketSnapshot::reset_open_count();
  MarketSnapshot::reset_deserialized_bytes();
  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

TEST(DispersionProjectedVar, C14_FailedRerunInvalidatesThePreviousGeneration) {
  const PvFixture fixture =
      make_pv_fixture("pv_c14_failed_rerun", pv_reconstituting_blocks());
  ASSERT_TRUE(dispersion_run_projected_var(fixture.dir));
  ASSERT_TRUE(verify_projected_var_artifacts(fixture.dir, fixture.dates.size()));

  write_file(fixture.dir / "run_spec.tsv", "key\tvalue\nunknown_projected_key\t1\n");
  EXPECT_FALSE(dispersion_run_projected_var(fixture.dir));
  EXPECT_FALSE(fs::exists(fixture.dir / "projected_var.tsv"));
  EXPECT_FALSE(verify_projected_var_artifacts(fixture.dir, fixture.dates.size()))
      << "stale companions from the prior success verified after a failed rerun";

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// Guard against a vacuous parity gate: if `weighting` and `strike` did not move
// the book, the comparison below would pass with BOTH routes ignoring them.
TEST(DispersionProjectedVar, C15_WeightingAndStrikePolicyDoMoveTheBook) {
  const PvFixture fixture = make_pv_fixture("pv_c15_knobs_bite", pv_unequal_weight_blocks());
  auto config = read_dispersion_run_config(fixture.dir / "run_spec.tsv");
  ASSERT_TRUE(config.has_value())
      << (config.has_value() ? std::string{} : config.error().to_string());
  const std::string archive =
      (fixture.dir / "surfaces" / (fixture.dates.back() + ".atxvsa")).string();
  auto snapshot = MarketSnapshot::load(archive);
  ASSERT_TRUE(snapshot.has_value())
      << (snapshot.has_value() ? std::string{} : snapshot.error().to_string());
  auto rows = read_universe(fixture.dir / "universe_schedule.tsv");
  ASSERT_TRUE(rows.has_value());
  auto authored = universe_at(*rows, fixture.dates.back(), config->universe.index_symbol);
  ASSERT_TRUE(authored.has_value());
  auto resolved = resolve_universe_uids(
      *authored, [&](std::string_view symbol) { return snapshot->uid_of(symbol); },
      MissingNameSpec{MissingNamePolicy::DropRenormalize, config->universe.min_names});
  ASSERT_TRUE(resolved.has_value());

  const auto quantities = [&](WeightingScheme weighting, StrikePolicy strike) {
    DispersionBacktestConfig backtest = dispersion_backtest_config_from(*config);
    backtest.weighting = weighting;
    backtest.strike = strike;
    DispersionConfig dispersion = dispersion_config_from(backtest);
    dispersion.projected_maturity = ProjectedMaturitySpec::days(30);
    auto book = build_dispersion_book(resolved->universe, snapshot->set(), dispersion);
    EXPECT_TRUE(book.has_value()) << (book.has_value() ? std::string{} : book.error().to_string());
    std::vector<double> out;
    if (book.has_value()) {
      for (const Position &position : book->positions) {
        out.push_back(position.qty);
      }
    }
    return out;
  };

  StrikePolicy atm;
  StrikePolicy moneyness;
  moneyness.rule = StrikeRule::FixedMoneyness;
  moneyness.log_moneyness = -0.05;

  const std::vector<double> vega_neutral = quantities(WeightingScheme::VegaNeutral, atm);
  ASSERT_FALSE(vega_neutral.empty());
  EXPECT_NE(vega_neutral, quantities(WeightingScheme::EqualVega, atm))
      << "`weighting` does not change the book, so a parity gate on it proves nothing";
  EXPECT_NE(vega_neutral, quantities(WeightingScheme::VegaNeutral, moneyness))
      << "`strike` does not change the book, so a parity gate on it proves nothing";

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

TEST(DispersionProjectedVar, C15_RouteHonorsTheTypedSideMultiplierWeightingAndStrike) {
  // Every construction knob NON-DEFAULT: the point is that the two routes agree
  // on values neither would fall back to anyway.
  std::string knobs;
  knobs += tsv_row({"side", "long_index_short_names"});
  knobs += tsv_row({"multiplier", "50"});
  knobs += tsv_row({"weighting", "equal_vega"});
  knobs += tsv_row({"strike", "fixed_moneyness"});
  knobs += tsv_row({"strike_log_moneyness", "-0.05"});
  const PvFixture fixture = make_pv_fixture("pv_c15_parity", pv_unequal_weight_blocks(), knobs);

  const Status ran = dispersion_run_projected_var(fixture.dir);
  ASSERT_TRUE(ran.has_value()) << (ran.has_value() ? std::string{} : ran.error().to_string());

  // The book the SURFACE route's builder produces from the same spec at the same
  // as-of session. Nothing here is transcribed out of dispersion_run.cpp.
  auto config = read_dispersion_run_config(fixture.dir / "run_spec.tsv");
  ASSERT_TRUE(config.has_value())
      << (config.has_value() ? std::string{} : config.error().to_string());
  const std::string archive =
      (fixture.dir / "surfaces" / (fixture.dates.back() + ".atxvsa")).string();
  auto snapshot = MarketSnapshot::load(archive);
  ASSERT_TRUE(snapshot.has_value());
  auto rows = read_universe(fixture.dir / "universe_schedule.tsv");
  ASSERT_TRUE(rows.has_value());
  auto authored = universe_at(*rows, fixture.dates.back(), config->universe.index_symbol);
  ASSERT_TRUE(authored.has_value());
  auto resolved = resolve_universe_uids(
      *authored, [&](std::string_view symbol) { return snapshot->uid_of(symbol); },
      MissingNameSpec{MissingNamePolicy::DropRenormalize, config->universe.min_names});
  ASSERT_TRUE(resolved.has_value());
  DispersionConfig dispersion = dispersion_config_from(dispersion_backtest_config_from(*config));
  dispersion.projected_maturity = ProjectedMaturitySpec::days(30);
  auto expected = build_dispersion_book(resolved->universe, snapshot->set(), dispersion);
  ASSERT_TRUE(expected.has_value())
      << (expected.has_value() ? std::string{} : expected.error().to_string());
  ASSERT_FALSE(expected->positions.empty());

  const std::vector<std::vector<std::string>> legs =
      read_tsv_rows(fixture.dir / "projected_risk_legs.tsv");
  ASSERT_GE(legs.size(), 2u);
  const std::size_t date_col = column_of(legs.front(), "date");
  const std::size_t uid_col = column_of(legs.front(), "uid");
  const std::size_t qty_col = column_of(legs.front(), "quantity");
  const std::size_t mult_col = column_of(legs.front(), "multiplier");
  ASSERT_NE(date_col, static_cast<std::size_t>(-1));
  ASSERT_NE(uid_col, static_cast<std::size_t>(-1));
  ASSERT_NE(qty_col, static_cast<std::size_t>(-1));
  ASSERT_NE(mult_col, static_cast<std::size_t>(-1));

  std::vector<std::vector<std::string>> first_date_legs;
  for (std::size_t row = 1; row < legs.size(); ++row) {
    if (legs[row][date_col] == fixture.dates.front()) {
      first_date_legs.push_back(legs[row]);
    }
  }
  ASSERT_EQ(first_date_legs.size(), expected->positions.size())
      << "the route published a different number of legs than the shared builder sizes";
  for (std::size_t leg = 0; leg < first_date_legs.size(); ++leg) {
    const Position &position = expected->positions[leg];
    EXPECT_EQ(first_date_legs[leg][uid_col], std::to_string(position.contract.uid)) << "leg " << leg;
    EXPECT_DOUBLE_EQ(std::stod(first_date_legs[leg][qty_col]), position.qty)
        << "leg " << leg
        << ": the route sized this leg differently from the surface route's builder — `side`, "
           "`weighting` or `strike` did not reach the projected book";
    EXPECT_DOUBLE_EQ(std::stod(first_date_legs[leg][mult_col]), position.multiplier)
        << "leg " << leg << ": spec `multiplier` did not reach the projected book";
  }

  // `side` is directly readable off the artifact: under LongIndexShortNames the
  // index leg (uid 1) is LONG. Asserted separately from the parity loop so a
  // regression names the knob instead of a leg index.
  bool saw_index_leg = false;
  for (const std::vector<std::string> &row : first_date_legs) {
    if (row[uid_col] == "1") {
      saw_index_leg = true;
      EXPECT_GT(std::stod(row[qty_col]), 0.0)
          << "spec `side = long_index_short_names` did not reach the projected book: the index "
             "leg is still SHORT";
    }
  }
  EXPECT_TRUE(saw_index_leg);

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// The spec's `index_symbol` was hardcoded to the "SPY" default at
// `make_pit_universe_resolver`, so a run whose index leg is anything else could
// not resolve its own basket at all.
TEST(DispersionProjectedVar, C15_RouteHonorsTheTypedIndexSymbol) {
  // AAA is the index leg here; SPY is a member of nothing.
  const std::vector<PvUniverseBlock> blocks = {
      PvUniverseBlock{"2026-10-01", {{"BBB", 0.5}, {"CCC", 0.5}}}};
  const PvFixture fixture =
      make_pv_fixture("pv_c15_index_symbol", blocks, tsv_row({"index_symbol", "AAA"}));

  const Status ran = dispersion_run_projected_var(fixture.dir);
  ASSERT_TRUE(ran.has_value())
      << "spec `index_symbol` did not reach the projected-VaR universe resolution: "
      << (ran.has_value() ? std::string{} : ran.error().to_string());

  const std::vector<std::vector<std::string>> legs =
      read_tsv_rows(fixture.dir / "projected_risk_legs.tsv");
  ASSERT_GE(legs.size(), 2u);
  const std::size_t uid_col = column_of(legs.front(), "uid");
  ASSERT_NE(uid_col, static_cast<std::size_t>(-1));
  std::vector<std::string> uids;
  for (std::size_t row = 1; row < legs.size(); ++row) {
    if (std::find(uids.begin(), uids.end(), legs[row][uid_col]) == uids.end()) {
      uids.push_back(legs[row][uid_col]);
    }
  }
  std::sort(uids.begin(), uids.end());
  EXPECT_EQ(uids, (std::vector<std::string>{"2", "3", "4"}))
      << "the projected book is not {AAA index, BBB, CCC}";

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// ── REVIEW C-6: benchmark rows are parsed WITH dates and then joined BY date ──
//
// `read_dispersion_benchmark_series` used to parse `date<TAB>pnl` and throw the
// date away; `benchmark_stats` then aligned by vector POSITION over
// `min(strategy.size(), benchmark.size())`. A shifted, reversed, duplicated,
// missing-date or short benchmark therefore produced entirely plausible
// alpha/beta/IR/tracking-error numbers for the WRONG observations, and the short
// case silently dropped the strategy tail.
//
// The shifted-equal-length case below is the one that matters: every other shape
// error is at least visible as a wrong count, while this one is confidently wrong
// and looks right.

namespace {

// A `date<TAB>pnl` benchmark file over `dates`, with a deterministic non-constant
// P&L so beta/IR are well defined.
[[nodiscard]] std::string pv_benchmark_body(std::span<const std::string> dates) {
  std::string body = "date\tpnl\n";
  for (std::size_t i = 0; i < dates.size(); ++i) {
    char cell[64];
    std::snprintf(cell, sizeof cell, "%.17g", 10.0 * std::sin(1.3 * static_cast<double>(i)) + 1.0);
    body += tsv_row({dates[i], cell});
  }
  return body;
}

// key -> value over a `metric<TAB>value` / `key<TAB>value` TSV.
[[nodiscard]] std::map<std::string, std::string> pv_read_kv(const fs::path &path) {
  std::map<std::string, std::string> out;
  const std::vector<std::vector<std::string>> rows = read_tsv_rows(path);
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (rows[i].size() >= 2) {
      out.emplace(rows[i][0], rows[i][1]);
    }
  }
  return out;
}

} // namespace

TEST(DispersionBenchmarkJoin, C6_AMatchingBenchmarkJoinsOnDateAndReportsEveryObservation) {
  // The strategy's return observations are steps 1..N-1 — `date[1..]` — so a
  // benchmark that covers exactly those sessions must be accepted whole.
  PvFixture fixture = make_pv_fixture("c6_exact", pv_reconstituting_blocks(),
                                      tsv_row({"benchmark_series", "benchmark.tsv"}));
  ASSERT_EQ(fixture.dates.size(), 5u);
  const std::vector<std::string> return_dates(fixture.dates.begin() + 1, fixture.dates.end());
  write_file(fixture.dir / "benchmark.tsv", pv_benchmark_body(return_dates));

  const Status ran = dispersion_run_surface_backtest(fixture.dir);
  ASSERT_TRUE(ran.has_value()) << (ran.has_value() ? std::string{} : ran.error().to_string());

  const std::map<std::string, std::string> sheet =
      pv_read_kv(fixture.dir / "surface_tearsheet.tsv");
  ASSERT_NE(sheet.find("benchmark_n_obs"), sheet.end()) << "no benchmark block was published";
  EXPECT_EQ(sheet.at("benchmark_n_obs"), "4")
      << "every strategy observation must be paired, not truncated";
  ASSERT_NE(sheet.find("benchmark_join"), sheet.end())
      << "the report must name the join that produced the benchmark block";
  EXPECT_EQ(sheet.at("benchmark_join"), "exact_dates");

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

TEST(DispersionBenchmarkJoin, C6_AShiftedEqualLengthBenchmarkIsRejectedNotSilentlyMisaligned) {
  // Same LENGTH as the strategy return series, same values, dates shifted back by
  // one session. Positional alignment accepts this happily and reports
  // alpha/beta/IR for observations that are off by a day.
  PvFixture fixture = make_pv_fixture("c6_shifted", pv_reconstituting_blocks(),
                                      tsv_row({"benchmark_series", "benchmark.tsv"}));
  ASSERT_EQ(fixture.dates.size(), 5u);
  const std::vector<std::string> shifted(fixture.dates.begin(), fixture.dates.end() - 1);
  write_file(fixture.dir / "benchmark.tsv", pv_benchmark_body(shifted));

  const Status ran = dispersion_run_surface_backtest(fixture.dir);
  ASSERT_FALSE(ran.has_value())
      << "a benchmark whose dates are shifted off the strategy's was accepted and reported";
  const std::string message = ran.error().to_string();
  // The error must name BOTH sides of the first disagreement, or an operator
  // cannot tell which file to fix.
  EXPECT_NE(message.find(fixture.dates[1]), std::string::npos) << message;
  EXPECT_NE(message.find(fixture.dates[0]), std::string::npos) << message;

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}

// ── The spec's `index_symbol` reaches the file-oriented entry points ──────────
//
// `all_symbols`/`universe_at` carried an `index_symbol = "SPY"` DEFAULT, and the
// corpus build, the schedule build and the replay's reconciliation all took it
// while the configured symbol sat in scope. A run whose index leg is not SPY
// therefore fetched, resolved and reconciled against SPY without a word — the
// same defect class as REVIEW C-15, which had already been fixed once on the
// projected-VaR route only. The default is now GONE from both declarations, so
// the compiler is what finds a site that forgets to thread the symbol; these
// tests pin the two entry points whose routing is observable off the filesystem.

namespace {

// A source spec + universe schedule for a run whose index leg is `index_symbol`.
// The OPRA root EXISTS but is EMPTY, so every (date, symbol) load misses: the
// corpus produces nothing and the only thing under test is WHICH symbols the
// build asked for, which `input_inventory.tsv` records verbatim.
struct IndexRoutingCorpusFixture {
  fs::path root;
  fs::path spec_path;
  fs::path run_dir;
};

[[nodiscard]] IndexRoutingCorpusFixture
make_index_routing_corpus_fixture(std::string_view leaf, std::string_view index_symbol,
                                  std::string_view date) {
  IndexRoutingCorpusFixture fixture;
  fixture.root = make_run_dir(leaf);
  fixture.spec_path = fixture.root / "source" / "run_spec.tsv";
  fixture.run_dir = fixture.root / "run";
  std::error_code error;
  fs::create_directories(fixture.root / "source", error);
  fs::create_directories(fixture.root / "opra", error);
  fs::create_directories(fixture.run_dir, error);

  write_file(fixture.root / "source" / "universe_schedule.tsv",
             "effective_date\tsymbol\traw_weight\tsource\tas_of\n" +
                 tsv_row({date, "AAA", "0.5", "test", date}) +
                 tsv_row({date, "BBB", "0.5", "test", date}));

  std::string spec;
  spec += tsv_row({"date_lo", date});
  spec += tsv_row({"date_hi", date});
  spec += tsv_row({"opra_root", (fixture.root / "opra").string()});
  spec += tsv_row({"universe_schedule", "universe_schedule.tsv"});
  spec += tsv_row({"min_names", "2"});
  spec += tsv_row({"index_symbol", index_symbol});
  write_file(fixture.spec_path, spec);
  return fixture;
}

// `dispersion_build_schedule` gates on OCC ESS authority for every admitted
// date before it reaches the universe, so a fixture that wants to exercise the
// universe has to carry that evidence. One minimal well-formed report per date,
// plus the inventory that cross-checks it — the fingerprint is read back through
// the production parser rather than recomputed here, so this fixture cannot
// drift from what `verify_occ_ess_evidence` accepts.
void write_occ_ess_evidence(const fs::path &run_dir, std::span<const std::string> dates) {
  const fs::path evidence_dir = run_dir / "occ_ess";
  std::error_code error;
  fs::create_directories(evidence_dir, error);
  std::string inventory = "date\tpath\tn_special_symbols\tsource_fingerprint\n";
  for (const std::string &date : dates) {
    ASSERT_EQ(date.size(), 10u) << date;
    const std::string mm_dd_yy =
        date.substr(5, 2) + "/" + date.substr(8, 2) + "/" + date.substr(2, 2);
    const std::string yyyymmdd = date.substr(0, 4) + date.substr(5, 2) + date.substr(8, 2);
    const fs::path report_path = evidence_dir / (date + ".txt");
    write_file(report_path,
               "1THE OPTIONS CLEARING CORPORATION\r\n"
               " NON-STANDARD SETTLEMENTS MRD REPORT ACTIVITY DATE " +
                   mm_dd_yy +
                   " PROGRAM-ID DLVC1910AS\r\n"
                   " REC PROD PKND SRTK ONN CMPN CMPN SECU UNIT SETL STRK FIXED PROCESS SETTLE\r\n"
                   "0706  ADVM    OSTK  USD   EU    01     01   ADVM 100  MON 100  3.560000  " +
                   yyyymmdd + "  20251209\r\n");
    const auto report = read_occ_ess_report_file(report_path.string());
    ASSERT_TRUE(report.has_value())
        << (report.has_value() ? std::string{} : report.error().to_string());
    inventory += tsv_row({date, report_path.lexically_normal().string(),
                          std::to_string(report->special_symbols().size()),
                          std::to_string(report->source_fingerprint())});
  }
  write_file(run_dir / "occ_ess_inventory.tsv", inventory);
}

// The distinct symbols `input_inventory.tsv` records the build as having asked
// the OPRA loader for, sorted.
[[nodiscard]] std::vector<std::string> requested_symbols(const fs::path &inventory_path) {
  const std::vector<std::vector<std::string>> rows = read_tsv_rows(inventory_path);
  std::vector<std::string> symbols;
  if (rows.empty()) {
    return symbols;
  }
  const std::size_t symbol_col = column_of(rows.front(), "symbol");
  if (symbol_col == static_cast<std::size_t>(-1)) {
    return symbols;
  }
  for (std::size_t r = 1; r < rows.size(); ++r) {
    if (symbol_col < rows[r].size() &&
        std::find(symbols.begin(), symbols.end(), rows[r][symbol_col]) == symbols.end()) {
      symbols.push_back(rows[r][symbol_col]);
    }
  }
  std::sort(symbols.begin(), symbols.end());
  return symbols;
}

} // namespace

TEST(DispersionIndexRouting, CorpusBuildFetchesTheConfiguredIndexLegNotSpy) {
  const IndexRoutingCorpusFixture fixture =
      make_index_routing_corpus_fixture("index_routing_corpus_qqq", "QQQ", "2026-07-10");

  // The build itself cannot admit anything from an empty OPRA root; its Status
  // is deliberately not asserted. What must hold either way is the symbol set it
  // requested, which it records before it can know whether a source exists.
  const Status built =
      dispersion_build_corpus(fixture.spec_path, fixture.run_dir, DispersionCorpusPolicy{});
  static_cast<void>(built);

  EXPECT_EQ(requested_symbols(fixture.run_dir / "input_inventory.tsv"),
            (std::vector<std::string>{"AAA", "BBB", "QQQ"}))
      << "the corpus build did not fetch the configured index leg QQQ — it took the "
         "`index_symbol = \"SPY\"` default of `all_symbols` while `index_symbol` was in scope, so "
         "the whole corpus is built against the wrong index";

  std::error_code error;
  fs::remove_all(fixture.root, error);
}

// The SPY-index control: the same route, the same fixture shape, index left at
// SPY. This is what pins that threading the symbol changed NOTHING for a SPY
// run — without it the test above would also pass on a build that simply always
// echoed `index_symbol` and had stopped seeding the fetch set at all.
TEST(DispersionIndexRouting, CorpusBuildWithASpyIndexIsUnchanged) {
  const IndexRoutingCorpusFixture fixture =
      make_index_routing_corpus_fixture("index_routing_corpus_spy", "SPY", "2026-07-10");

  const Status built =
      dispersion_build_corpus(fixture.spec_path, fixture.run_dir, DispersionCorpusPolicy{});
  static_cast<void>(built);

  EXPECT_EQ(requested_symbols(fixture.run_dir / "input_inventory.tsv"),
            (std::vector<std::string>{"AAA", "BBB", "SPY"}));

  std::error_code error;
  fs::remove_all(fixture.root, error);
}

// ── The reconciliation tolerance gates reject a non-finite side ──────────────
//
// `close_to` was `if (std::abs(actual - expected) > tolerance) fail;`. Every
// comparison against a NaN is false, so a gate whose recomputed side had gone
// non-finite PASSED — the reconciler agreed with the artifact it exists to
// check, and published the NaN. `dec()` already refuses a literal "nan"/"inf"
// cell, so the way a non-finite number actually reaches a gate is arithmetic:
// below, one lot's `quantity * multiplier` overflows to +Inf and its mark does
// not move, and `Inf * 0.0` is NaN.

namespace {

// Four legs, entry and held marks IDENTICAL on every one, so every P&L is
// exactly zero — except SPY2, whose quantity x multiplier is 1e307 * 100 and
// therefore +Inf. Every cell is individually well-formed and finite.
[[nodiscard]] std::string overflowing_marks_text() {
  std::string text =
      tsv_row({"date", "valuation_ts_ns", "role", "cohort", "symbol", "uid", "instrument_id",
               "raw_symbol", "expiry_ts_ns", "strike", "side", "quantity", "multiplier", "status",
               "raw_bid", "raw_ask", "raw_mid", "model_mark", "model_in_spread"});
  struct Leg {
    std::string_view raw, symbol, uid, iid, side, quantity, mark;
  };
  const Leg legs[] = {
      {"AAPL3", "AAPL", "2", "3", "C", "1", "5"},
      {"AAPL4", "AAPL", "2", "4", "P", "1", "5"},
      {"SPY1", "SPY", "1", "1", "C", "-1", "10"},
      {"SPY2", "SPY", "1", "2", "P", "1e307", "9"},
  };
  for (const std::string_view role : {"Entry", "Held"}) {
    const std::string_view date = role == "Entry" ? "2026-07-10" : "2026-07-11";
    const std::string_view ts = role == "Entry" ? "100" : "200";
    for (const Leg &leg : legs) {
      text += tsv_row({date, ts, role, "1", leg.symbol, leg.uid, leg.iid, leg.raw, "100000", "100",
                       leg.side, leg.quantity, "100", "Ok", leg.mark, leg.mark, leg.mark, leg.mark,
                       "1"});
    }
  }
  return text;
}

// What a run of `overflowing_marks_text()` reconciles to if the arithmetic is
// taken at face value: nothing moved, so every P&L and NAV is zero on both
// dates and all four lots are quoted.
[[nodiscard]] std::string flat_reconciliation_text() {
  std::string text =
      tsv_row({"date", "valuation_ts_ns", "held_cohort", "model_option_pnl", "quote_mid_pnl",
               "model_minus_quote_pnl", "model_nav", "quote_mid_nav", "quote_mid_coverage",
               "n_held_lots", "n_quote_mid_lots"});
  text += tsv_row({"2026-07-10", "100", "1", "0", "0", "0", "0", "0", "1", "4", "4"});
  text += tsv_row({"2026-07-11", "200", "1", "0", "0", "0", "0", "0", "1", "4", "4"});
  return text;
}

// `schedule_text()` plus a THIRD straddle pair, ZZZ, carrying zero vega per unit
// vol on both legs and a zero straddle target. Everything else stays internally
// consistent — the recomputed per-contract vega, achieved leg vega, weight sum,
// basket/index target, net and gross vega all still agree with their persisted
// columns — so the roll passes every other check and the only thing wrong with
// it is that `pair_target / pair_vega` is 0/0.
[[nodiscard]] std::string zero_vega_pair_schedule_text() {
  std::string text = "ATX_LISTED_DISPERSION_SCHEDULE\t1\n";
  text += tsv_row({"roll_date", "valuation_ts_ns", "cohort", "expiry_ts_ns",
                   "gross_index_vega_target", "net_vega", "gross_vega", "n_names", "is_index",
                   "symbol", "uid", "instrument_id", "raw_symbol", "strike", "side", "quantity",
                   "multiplier", "raw_bid", "raw_ask", "raw_mid", "model_mark", "delta_per_share",
                   "vega_per_unit_vol", "vega_per_contract_per_vol_point", "normalized_weight",
                   "target_straddle_vega", "achieved_leg_vega", "source_fingerprint",
                   "surface_fingerprint"});
  const auto leg = [&](std::string_view is_index, std::string_view symbol, std::string_view uid,
                       std::string_view iid, std::string_view raw, std::string_view side,
                       std::string_view quantity, std::string_view unit_vega,
                       std::string_view contract_vega, std::string_view weight,
                       std::string_view target, std::string_view achieved) {
    text += tsv_row({"2026-07-10", "100", "1", "100000", "100", "0", "200", "2", is_index, symbol,
                     uid, iid, raw, "100", side, quantity, "100", "9", "11", "10", "10", "0",
                     unit_vega, contract_vega, weight, target, achieved, iid, "99"});
  };
  leg("1", "SPY", "1", "1", "SPY1", "C", "-1", "50", "50", "0", "-100", "-50");
  leg("1", "SPY", "1", "2", "SPY2", "P", "-1", "50", "50", "0", "-100", "-50");
  leg("0", "AAPL", "2", "3", "AAPL3", "C", "1", "50", "50", "0.5", "100", "50");
  leg("0", "AAPL", "2", "4", "AAPL4", "P", "1", "50", "50", "0.5", "100", "50");
  leg("0", "ZZZ", "5", "5", "ZZZ5", "C", "1", "0", "0", "0.5", "0", "0");
  leg("0", "ZZZ", "5", "6", "ZZZ6", "P", "1", "0", "0", "0.5", "0", "0");
  return text;
}

} // namespace

TEST(DispersionReferenceReconcile, AZeroVegaStraddlePairIsRejectedRatherThanDividedBy) {
  const fs::path run = make_run_dir("recon_zero_pair_vega");
  write_file(run / "trade_schedule.tsv", zero_vega_pair_schedule_text());

  const auto records = reconcile_dispersion_reference(run, /*schedule_only=*/true);

  ASSERT_FALSE(records) << "a straddle pair with zero vega was accepted: `expected_quantity = "
                           "pair_target / pair_vega` is 0/0 there, and the gate it feeds cannot "
                           "reject the NaN it produces";
  EXPECT_NE(records.error().to_string().find("pair vega"), std::string::npos)
      << "the reconciler must name the unusable divisor rather than report the NaN it computed "
         "from it as a downstream tolerance failure: "
      << records.error().to_string();

  std::error_code error;
  fs::remove_all(run, error);
}

TEST(DispersionReferenceReconcile, ANonFiniteRecomputedPnlFailsTheToleranceGate) {
  const fs::path run = make_run_dir("recon_nan_gate");
  write_file(run / "trade_schedule.tsv", schedule_text());
  write_file(run / "contract_marks.tsv", overflowing_marks_text());
  write_file(run / "reconciliation.tsv", flat_reconciliation_text());
  write_file(run / "backtest.tsv", backtest_text(/*second_total=*/"0"));

  const auto records = reconcile_dispersion_reference(run, /*schedule_only=*/false);

  ASSERT_FALSE(records)
      << "a reconciliation whose recomputed model P&L is NaN was accepted: every tolerance gate "
         "compares with `>`, which is false against a NaN, so the reconciler agreed with the "
         "artifact it exists to check and published "
      << records->size() << " record(s)";
  EXPECT_NE(records.error().to_string().find("model option P&L"), std::string::npos)
      << "the failure must name the gate that caught it: " << records.error().to_string();

  std::error_code error;
  fs::remove_all(run, error);
}

TEST(DispersionIndexRouting, ScheduleBuildResolvesTheConfiguredIndexLeg) {
  // AAA is the index leg and SPY is in NO archive, so the wrong index leg is not
  // a silently different surface — it is a uid that cannot be resolved at all.
  const std::vector<PvUniverseBlock> blocks = {
      PvUniverseBlock{"2026-10-01", {{"BBB", 0.5}, {"CCC", 0.5}}}};
  const PvFixture fixture =
      make_pv_fixture("index_routing_schedule", blocks, tsv_row({"index_symbol", "AAA"}),
                      /*n_dates=*/2, /*n_unused_surfaces=*/0, /*symbol_derived_uids=*/false,
                      /*include_spy=*/false);
  ASSERT_TRUE(write_listed_definitions_file((fixture.dir / "definitions.tsv").string(),
                                            ListedDefinitionTable{})
                  .has_value());
  write_occ_ess_evidence(fixture.dir, fixture.dates);

  // The fixture carries no OPRA quotes, so selection admits no roll and the
  // build fails EITHER WAY. What the index symbol decides is WHERE it fails:
  // the per-date admission tally is written after the loop that resolves the
  // universe, so its presence is the evidence that the loop ran to completion.
  const Status built = dispersion_build_schedule(fixture.dir);
  ASSERT_FALSE(built.has_value()) << "a fixture with no quotes must not produce a schedule";
  const std::string message = built.error().to_string();
  EXPECT_EQ(message.find("SPY"), std::string::npos)
      << "the schedule build resolved its universe against SPY — it took the `index_symbol = "
         "\"SPY\"` default of `universe_at` while the configured index leg AAA was in scope, so a "
         "non-SPY run cannot build a schedule at all: "
      << message;
  EXPECT_TRUE(fs::exists(fixture.dir / "quote_rejects.tsv"))
      << "the build stopped before the end of the per-date loop, i.e. before selection ever ran: "
      << message;

  std::error_code error;
  fs::remove_all(fixture.dir, error);
}
