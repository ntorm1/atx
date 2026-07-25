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
    GTEST_SKIP() << "reference run corpus absent: " << root.string();
  }
  std::vector<fs::path> run_dirs;
  for (const fs::directory_entry &entry : fs::directory_iterator(root, error)) {
    if (!entry.is_directory(error)) {
      continue;
    }
    const fs::path dir = entry.path();
    bool complete = true;
    for (const char *artifact :
         {"trade_schedule.tsv", "contract_marks.tsv", "reconciliation.tsv", "backtest.tsv"}) {
      if (!fs::is_regular_file(dir / artifact, error)) {
        complete = false;
      }
    }
    if (complete) {
      run_dirs.push_back(dir);
    }
  }
  if (run_dirs.empty()) {
    GTEST_SKIP() << "no complete run directory under " << root.string();
  }
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

// ── REV-TAIL I-3 — the four keys that parsed, validated, and did nothing ──────
//
// `dispersion_run_surface_backtest` (dispersion_run.cpp:2377) reads the STRICT
// typed config, so `unpriced`, `provenance`, `book_entry_fill_slippage` and
// `reconcile_nav` each bind by name (binder at :1434-1443) and survive
// `reject_unknown()`. It then hands off to `dispersion_backtest_config_from`,
// which hardcoded `run.unpriced = UnpricedLotPolicy::Error` and never set the
// other three at all. Four spec keys accepted by name, zero effect on the shipped
// `run-surface-backtest` — the sprint's signature defect class, on the route
// 347ad44's own evidence is built on.
//
// This asserts the one property that makes a knob a knob: a non-default value set
// on the typed spec is the value the engine actually runs under. `config.run` is
// what reaches `run_backtest` (dispersion_backtest.cpp:112,120), so this is the
// engine's real input and not a bookkeeping copy.
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

// The two builders must agree on every knob they both carry. `dispersion_run.hpp:291`
// calls `dispersion_engine_run_config_from` "the single place the typed spec becomes
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
