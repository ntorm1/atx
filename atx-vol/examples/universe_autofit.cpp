// universe_autofit.cpp — fit an entire options universe from one OPRA snapshot,
// letting the library AUTO-SELECT the curve family per board (PricerConfig::curve
// left unset => unified fit policy + CurveSelector), then report exactly what the
// pipeline did per symbol: routing decision, chosen family, fit outcome, quality
// diagnostics, valuation NaN rates, and wall-clock timings.
//
// This is the "vola.dynamic claim" stress harness: point it at a {symbol}/{date}
// parquet hive holding one snapshot minute for N thousand underliers and it
// answers (a) does the pipeline survive the whole US universe, (b) where does it
// fail, (c) where does the CPU go.
//
//   universe_autofit --opra-root DIR --date YYYY-MM-DD --symbols-file FILE
//       [--snapshot-suffix T14:00:00Z] [--r 0.043] [--preset robust]
//       [--fit-workers N] [--limit N] [--out results.csv] [--no-value]
//       [--oos-max-expiries N] [--selector-budget-ms N] [--sparse-floor N]
//       [--min-direct-confidence X] [--path-template "{symbol}/{date}.parquet"]
//       [--no-fit]
//
// Output CSV: one row per symbol with load/fit/value status + diagnostics.
// Summary to stdout: status counts, curve-family histogram, profile histogram,
// error-code breakdown, timing percentiles, slowest boards.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/chain.hpp"         // OptionChain
#include "atx/vol/corpus.hpp"        // CorpusBoard
#include "atx/vol/fit_policy.hpp"    // FitDecision
#include "atx/vol/opra_batch.hpp"    // OpraBatchSpec, load_opra_daterange, corpus_board_from_opra
#include "atx/vol/detail/parallel_for.hpp"  // parallel_for, atx_auto_worker_count
#include "atx/vol/pricer_fitter.hpp" // PricerFitter, PricerConfig, OutputField
#include "atx/vol/profile.hpp"       // ProfileKind
#include "atx/vol/session.hpp"       // FitPreset, SessionDiagnostics
#include "atx/vol/surface_policy.hpp" // SurfaceHealth, ValidationDigest, SurfaceState
#include "atx/vol/types.hpp"         // Result
#include "atx/vol/vol_curve.hpp"     // VolCurveKind, to_string

using namespace atx::vol;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

const char *profile_name(ProfileKind k) {
  switch (k) {
  case ProfileKind::IndexEtfUltraLiquid: return "IndexEtfUltraLiquid";
  case ProfileKind::MegaCapEvent: return "MegaCapEvent";
  case ProfileKind::LiquidSingleName: return "LiquidSingleName";
  case ProfileKind::OrdinarySingleName: return "OrdinarySingleName";
  case ProfileKind::IlliquidSmallCap: return "IlliquidSmallCap";
  case ProfileKind::HtbDividendName: return "HtbDividendName";
  case ProfileKind::VolProduct: return "VolProduct";
  }
  return "?";
}

const char *source_name(FitDecisionSource s) {
  switch (s) {
  case FitDecisionSource::ProfileOverride: return "ProfileOverride";
  case FitDecisionSource::TickerPrior: return "TickerPrior";
  case FitDecisionSource::BoardFeatures: return "BoardFeatures";
  case FitDecisionSource::SparseGuard: return "SparseGuard";
  case FitDecisionSource::CrossValidation: return "CrossValidation";
  }
  return "?";
}

const char *preset_name(FitPreset p) {
  switch (p) {
  case FitPreset::Fast: return "fast";
  case FitPreset::Accurate: return "accurate";
  case FitPreset::Robust: return "robust";
  case FitPreset::Hft: return "hft";
  case FitPreset::Populate: return "populate";
  case FitPreset::Bulk: return "bulk";
  }
  return "?";
}

FitPreset parse_preset(std::string_view name) {
  if (name == "accurate") return FitPreset::Accurate;
  if (name == "robust") return FitPreset::Robust;
  if (name == "hft") return FitPreset::Hft;
  if (name == "populate") return FitPreset::Populate;
  if (name == "bulk") return FitPreset::Bulk;
  return FitPreset::Fast;
}

// One symbol's full outcome. Plain data; workers write disjoint slots.
struct Row {
  std::string symbol;
  std::string status{"skipped"}; // load_missing|load_error|chain_error|fit_error|fit_exception|ok
  std::string error;             // error to_string (or exception what)
  // board shape
  std::size_t n_rows{0};      // parquet quote rows
  std::size_t n_options{0};   // chain option ids (post-build)
  double spot{0.0};
  // routing decision
  std::string profile;
  double profile_conf{0.0};
  std::string decision_source;
  std::string effective_preset;
  std::string chosen_kind;
  std::string primary_kind;
  bool used_fallback{false};
  bool selector_fallback{false}; // selector refused; profile's direct route served
  std::string selector_error;    // the refusal text, when the selector produced one
  bool selector_ran{false};
  double selector_oos_vw{0.0};
  // Raw classifier features behind the routing decision. Emitted so a
  // reproducibility study can attribute a cross-session routing flip to the
  // board observable that moved, instead of inferring it from the verdict.
  std::uint32_t f_live_quotes{0};
  std::uint32_t f_live_expiries{0};
  std::uint32_t f_quoted_expiries{0};
  std::uint32_t f_atm_quotes{0};
  std::uint32_t f_ident_expiries{0};
  std::uint32_t f_max_nm_strikes{0};
  double f_median_spread{0.0};
  std::uint32_t f_front_expiries{0};
  bool f_weeklies{false};
  // fit diagnostics
  double worst_in_band{0.0};
  double mean_in_band{0.0};
  double mean_chi2{0.0};
  double mean_rmse_vol{0.0};
  bool calendar_arb_free{false};
  // The boolean conflates "the check ran and found violations" with "the check
  // itself failed" — a failed check is stamped with the sentinel count 1 so the
  // `calendar_arb_free == (n_calendar_viol_pre == 0)` invariant holds. Exporting
  // the raw count is what separates the two, and a count above the sentinel is
  // unambiguously real arbitrage.
  std::size_t n_calendar_viol{0};
  std::size_t n_price_bound_viol{0};
  std::size_t n_slices{0};
  std::size_t n_quotes_used{0};
  // ── Independent risk oracle (SurfaceHealth / ValidationDigest) ──────────────
  // Deliberately `oracle_`-prefixed and kept apart from the SessionDiagnostics
  // fields above: the two measure different things. The legacy booleans are
  // written per-lane over lane-specific bands (eSSVI |k| <= 3.0, polymorphic
  // |k| <= 0.6); the oracle certifies one band, |k| <= 0.5 at 1e-8, for every
  // board that reaches the risk stage. Confusing the two is what produced the
  // sprint plan's two withdrawn conclusions.
  //
  // `oracle_ran` is NOT redundant with the counters. A default-constructed
  // SurfaceHealth is {state=Rejected, reasons=InsufficientData, all counters 0},
  // so a board whose risk stage never executed is byte-identical to a board the
  // oracle inspected and found clean. Only the generation stamp separates them.
  bool oracle_ran{false};
  std::string oracle_state;       // to_string(SurfaceState)
  std::uint32_t oracle_reasons{0}; // ValidationFailure bitmask, as an integer
  std::uint64_t oracle_candidate_generation{0};
  std::uint64_t oracle_served_generation{0};
  std::uint32_t o_n_slices{0};
  std::uint32_t o_n_strike_samples{0};
  std::uint32_t o_n_calendar_samples{0};
  std::uint32_t o_n_non_finite{0};
  std::uint32_t o_n_price_bound_violations{0};
  std::uint32_t o_n_strike_monotonicity_violations{0};
  std::uint32_t o_n_butterfly_violations{0};
  std::uint32_t o_n_calendar_violations{0};
  std::uint32_t o_n_wing_violations{0};
  double o_max_calendar_slack{0.0};
  double o_max_butterfly_slack{0.0};
  double o_max_price_bound_slack{0.0};
  double o_max_wing_slope_excess{0.0};
  double o_first_calendar_k{0.0};
  double o_first_butterfly_k{0.0};
  // The market-mark surface's state, exported alongside the risk state so a
  // reader can tell WHICH surface `PricerFitter::surface()` handed back rather
  // than inferring it from the config.
  std::string mm_state;
  // valuation
  std::size_t n_valued{0};
  std::size_t n_price_nan{0};
  std::size_t n_bidiv_nan{0};
  std::size_t n_askiv_nan{0};
  // timings
  double load_ms{0.0}; // per-board share, measured around corpus_board_from_opra
  double chain_ms{0.0};
  double fit_ms{0.0};
  double value_ms{0.0};
};

void record_decision(Row &row, const FitDecision &d) {
  row.profile = profile_name(d.profile.kind);
  row.profile_conf = d.profile.confidence;
  row.decision_source = source_name(d.source);
  row.effective_preset = preset_name(d.preset);
  row.chosen_kind = to_string(d.curve.kind);
  row.primary_kind = to_string(d.primary_curve.kind);
  row.used_fallback = d.used_fallback;
  row.selector_fallback = d.selector_fallback;
  row.f_live_quotes = d.features.n_live_quotes;
  row.f_live_expiries = d.features.n_live_expiries;
  row.f_quoted_expiries = d.features.n_quoted_expiries;
  row.f_atm_quotes = d.features.n_atm_quotes;
  row.f_ident_expiries = d.features.n_identifiable_expiries;
  row.f_max_nm_strikes = d.features.max_near_money_strikes;
  row.f_median_spread = d.features.median_spread_pct;
  row.f_front_expiries = d.features.n_front_expiries;
  row.f_weeklies = d.features.has_weeklies;
}

// Copy the independent risk oracle's verdict out of the publication snapshot.
//
// `oracle_ran` is derived from `candidate_generation`, not from the counters,
// because the counters cannot distinguish "inspected, clean" from "never
// inspected" — see the Row comment. `PricerFitter` pre-increments its monotone
// generation counter on entry to `fit` and stamps it into every admission
// decision it takes, so a non-zero `candidate_generation` on `risk_health` is
// exactly the condition "a risk admission decision was reached for this board".
// A board whose policy omitted Risk from `outputs`, or whose build refused
// before the risk stage, leaves the default-constructed value 0.
void record_oracle(Row &row, const SurfaceBundle &bundle) {
  const SurfaceHealth &health = bundle.risk_health;
  const ValidationDigest &v = health.validation;
  row.oracle_ran = health.candidate_generation != 0;
  row.oracle_state = std::string(to_string(health.state));
  row.oracle_reasons = static_cast<std::uint32_t>(health.reasons);
  row.oracle_candidate_generation = health.candidate_generation;
  row.oracle_served_generation = health.served_generation;
  row.o_n_slices = v.n_slices;
  row.o_n_strike_samples = v.n_strike_samples;
  row.o_n_calendar_samples = v.n_calendar_samples;
  row.o_n_non_finite = v.n_non_finite;
  row.o_n_price_bound_violations = v.n_price_bound_violations;
  row.o_n_strike_monotonicity_violations = v.n_strike_monotonicity_violations;
  row.o_n_butterfly_violations = v.n_butterfly_violations;
  row.o_n_calendar_violations = v.n_calendar_violations;
  row.o_n_wing_violations = v.n_wing_violations;
  row.o_max_calendar_slack = v.max_calendar_slack;
  row.o_max_butterfly_slack = v.max_butterfly_slack;
  row.o_max_price_bound_slack = v.max_price_bound_slack;
  row.o_max_wing_slope_excess = v.max_wing_slope_excess;
  row.o_first_calendar_k = v.first_calendar_k;
  row.o_first_butterfly_k = v.first_butterfly_k;
  row.mm_state = std::string(to_string(bundle.market_mark_health.state));
}

std::vector<std::string> read_symbols_file(const std::string &path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

std::string csv_escape(const std::string &s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string q = "\"";
  for (char c : s) { if (c == '"') q += "\"\""; else q += c; }
  q += "\"";
  return q;
}

// The selector's refusal, when the fit fell back to the profile's own family.
// It survives in the build report as a Selection-stage attempt on BOTH the
// published and the last-attempt report, so an operator can see why a board was
// not cross-validated without re-running anything.
std::string selection_refusal(const PricerFitter &fitter) {
  for (const auto *report : {&fitter.published_report(), &fitter.last_attempt_report()}) {
    if (!report->has_value()) {
      continue;
    }
    for (const SurfaceBuildAttemptReport &attempt : (*report)->attempts) {
      if (attempt.stage == SurfaceBuildStage::Selection && attempt.failure.has_value()) {
        return attempt.failure->to_string();
      }
    }
  }
  return {};
}

} // namespace

int main(int argc, char **argv) {
  std::string opra_root, date, symbols_file, out_csv = "universe_autofit_results.csv";
  std::string snapshot_suffix = "T14:00:00Z";
  // Default matches the per-symbol OPRA v1 hive; a single-file-per-date hive is
  // reachable with --path-template "date={date}/data.parquet".
  std::string path_template = "{symbol}/{date}.parquet";
  std::string preset_name_arg = "robust";
  std::string pin_kind; // empty => auto-select; else pin this family for every board
  double r = 0.043;
  unsigned fit_workers = atx_auto_worker_count();
  std::size_t limit = 0;
  bool do_value = true;
  bool do_fit = true;
  std::optional<unsigned> oos_max_expiries;
  std::optional<double> selector_budget_ms;
  std::optional<std::uint32_t> sparse_floor;
  std::optional<double> min_direct_confidence;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--opra-root") opra_root = nv();
    else if (a == "--date") date = nv();
    else if (a == "--symbols-file") symbols_file = nv();
    else if (a == "--snapshot-suffix") snapshot_suffix = nv();
    else if (a == "--path-template") path_template = nv();
    else if (a == "--r") r = std::strtod(nv(), nullptr);
    else if (a == "--preset") preset_name_arg = nv();
    else if (a == "--fit-workers") fit_workers = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--limit") limit = static_cast<std::size_t>(std::strtoull(nv(), nullptr, 10));
    else if (a == "--out") out_csv = nv();
    else if (a == "--no-value") do_value = false;
    else if (a == "--no-fit") do_fit = do_value = false;
    else if (a == "--pin") pin_kind = nv();
    else if (a == "--oos-max-expiries")
      oos_max_expiries = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--selector-budget-ms") selector_budget_ms = std::strtod(nv(), nullptr);
    else if (a == "--sparse-floor")
      sparse_floor = static_cast<std::uint32_t>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--min-direct-confidence")
      min_direct_confidence = std::strtod(nv(), nullptr);
    else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }
  if (opra_root.empty() || date.empty() || symbols_file.empty()) {
    std::fprintf(stderr,
                 "usage: universe_autofit --opra-root DIR --date YYYY-MM-DD --symbols-file FILE "
                 "[--snapshot-suffix T14:00:00Z] [--r 0.043] [--preset robust] [--fit-workers N] "
                 "[--limit N] [--out FILE] [--no-value] [--no-fit] "
                 "[--oos-max-expiries N] [--selector-budget-ms N] [--sparse-floor N] "
                 "[--min-direct-confidence X] [--path-template T]\n");
    return 2;
  }

  std::vector<std::string> symbols = read_symbols_file(symbols_file);
  if (symbols.empty()) {
    std::fprintf(stderr, "no symbols in %s\n", symbols_file.c_str());
    return 2;
  }
  if (limit > 0 && symbols.size() > limit) symbols.resize(limit);
  const FitPreset preset = parse_preset(preset_name_arg);

  // Progress must be visible under output redirection (Windows stdio is fully
  // buffered when stdout is not a console).
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("[universe_autofit] symbols=%zu date=%s snapshot=%s preset=%s fit-workers=%u\n",
              symbols.size(), date.c_str(), snapshot_suffix.c_str(), preset_name_arg.c_str(),
              fit_workers);
#if defined(NDEBUG)
  std::printf("[build] Release (NDEBUG)\n");
#else
  std::printf("[build] *** DEBUG BUILD — timings not representative ***\n");
#endif

  // ── Load the snapshot hive (single date) ──────────────────────────────────
  const auto t_load0 = Clock::now();
  OpraBatchSpec spec;
  spec.symbols = symbols;
  spec.date_lo = date;
  spec.date_hi = date;
  spec.root_dir = opra_root;
  spec.snapshot_suffix = snapshot_suffix;
  spec.path_template = path_template;
  spec.r = r;
  const Result<OpraBatchResult> batch = load_opra_daterange(spec);
  if (!batch) {
    std::fprintf(stderr, "load_opra_daterange: %s\n", batch.error().to_string().c_str());
    return 1;
  }
  const double load_total_ms = ms_since(t_load0);
  std::printf("[load] loaded=%zu missing=%zu error=%zu of %zu in %.1fs\n", batch->n_loaded,
              batch->n_missing, batch->n_error, batch->n_total, load_total_ms / 1e3);

  // ── Boards + rows (parallel array; entry i <-> rows[i]) ───────────────────
  std::vector<Row> rows(batch->entries.size());
  std::vector<const OpraBatchEntry *> entries(batch->entries.size());
  for (std::size_t i = 0; i < batch->entries.size(); ++i) entries[i] = &batch->entries[i];

  const auto t_fit0 = Clock::now();
  std::atomic<std::size_t> n_done{0};
  parallel_for(entries.size(), fit_workers, [&](std::size_t i) {
    const OpraBatchEntry &e = *entries[i];
    Row &row = rows[i];
    row.symbol = e.symbol;
    // Progress line on every exit path (stderr is unbuffered, safe under redirect).
    struct Progress {
      const Row &r;
      std::atomic<std::size_t> &done;
      std::size_t total;
      Clock::time_point t0;
      ~Progress() {
        const std::size_t k = ++done;
        const double el = std::chrono::duration<double>(Clock::now() - t0).count();
        std::fprintf(stderr, "[%zu/%zu] %-8s %-14s fit=%.0fms eta=%.0fs\n", k, total,
                     r.symbol.c_str(), r.status.c_str(), r.fit_ms,
                     k ? el / static_cast<double>(k) * static_cast<double>(total - k) : 0.0);
      }
    } progress{row, n_done, entries.size(), t_fit0};
    if (!e.panel) {
      const bool missing = e.panel.error().code() == ErrorCode::NotFound;
      row.status = missing ? "load_missing" : "load_error";
      row.error = e.panel.error().to_string();
      return;
    }

    try {
      const auto t0 = Clock::now();
      CorpusBoard board = corpus_board_from_opra(e.date, e.symbol, *e.panel);
      row.load_ms = ms_since(t0);
      row.n_rows = board.frame.rows.size();
      row.spot = board.frame.spot;

      const auto t1 = Clock::now();
      Result<OptionChain> chain = OptionChain::from_frame(board.frame, board.env);
      row.chain_ms = ms_since(t1);
      if (!chain) {
        row.status = "chain_error";
        row.error = chain.error().to_string();
        return;
      }
      row.n_options = chain->ids().size();

      PricerConfig cfg;
      cfg.preset = preset;
      cfg.context = board.fit_context;
      cfg.n_threads = 1; // board-level parallelism only; keep each fit serial
      cfg.fit_workers = 1; // prevent board fan-out from nesting expiry fan-out
      if (oos_max_expiries.has_value()) cfg.selector.oos_max_expiries = *oos_max_expiries;
      if (selector_budget_ms.has_value()) cfg.selector.time_budget_ms = *selector_budget_ms;
      if (sparse_floor.has_value()) cfg.policy.sparse_validation_floor = *sparse_floor;
      if (min_direct_confidence.has_value())
        cfg.policy.min_direct_confidence = *min_direct_confidence;
      if (!pin_kind.empty()) {
        CurveConfig cc;
        if (pin_kind == "linear-variance") cc.kind = VolCurveKind::LinearVariance;
        else if (pin_kind == "essvi") cc.kind = VolCurveKind::Essvi;
        else if (pin_kind == "svi") cc.kind = VolCurveKind::Svi;
        else if (pin_kind == "c8") cc.kind = VolCurveKind::C8;
        else cc.kind = VolCurveKind::ConvexDense;
        cfg.curve = cc;
      }

      // Routing-only mode: resolve the policy exactly as PricerFitter would and
      // stop. Used to study classifier reproducibility over a whole universe
      // without paying for the fit, which dominates the wall clock.
      if (!do_fit) {
        record_decision(row, select_fit_policy(chain->underlying(), chain->underlying().ticker,
                                               cfg.context, cfg.policy));
        row.status = "ok";
        return;
      }
      PricerFitter fitter{cfg};

      const auto t2 = Clock::now();
      const Status st = fitter.fit(chain.value());
      row.fit_ms = ms_since(t2);
      if (!st) {
        row.status = "fit_error";
        row.error = st.error().to_string();
        // decision may still explain what the policy attempted
        if (fitter.decision()) {
          record_decision(row, *fitter.decision());
        }
        row.selector_error = selection_refusal(fitter);
        return;
      }

      if (fitter.decision()) {
        record_decision(row, *fitter.decision());
      }
      row.selector_error = selection_refusal(fitter);
      if (fitter.selection()) {
        row.selector_ran = true;
        const SelectorResult &sel = *fitter.selection();
        if (sel.chosen_index < sel.scores.size()) {
          row.selector_oos_vw = sel.scores[sel.chosen_index].oos_vw;
        }
      }
      const SessionDiagnostics &dg = fitter.surface()->diagnostics();
      row.worst_in_band = dg.worst_frac_within_bidask;
      row.mean_in_band = dg.mean_frac_within_bidask;
      row.mean_chi2 = dg.mean_chi2_reduced;
      row.mean_rmse_vol = dg.mean_rmse_vol;
      row.calendar_arb_free = dg.calendar_arb_free;
      row.n_calendar_viol = dg.n_calendar_viol_pre;
      row.n_price_bound_viol = dg.n_price_bound_violations;
      row.n_slices = dg.n_slices;
      row.n_quotes_used = dg.n_quotes;
      record_oracle(row, fitter.bundle());

      if (do_value) {
        const auto t3 = Clock::now();
        const Result<ChainValuation> val =
            fitter.value_chain(chain.value(), OutputField::Prices | OutputField::Bands, 1);
        row.value_ms = ms_since(t3);
        if (val) {
          row.n_valued = val->size();
          for (std::size_t k = 0; k < val->size(); ++k) {
            if (!(val->model_price[k] == val->model_price[k])) ++row.n_price_nan;
            if (!(val->bid_iv[k] == val->bid_iv[k])) ++row.n_bidiv_nan;
            if (!(val->ask_iv[k] == val->ask_iv[k])) ++row.n_askiv_nan;
          }
        }
      }
      row.status = "ok";
    } catch (const std::exception &ex) {
      row.status = "fit_exception";
      row.error = ex.what();
    } catch (...) {
      row.status = "fit_exception";
      row.error = "unknown exception";
    }
  });
  const double fit_total_ms = ms_since(t_fit0);

  // ── CSV ────────────────────────────────────────────────────────────────────
  {
    std::ofstream out(out_csv, std::ios::trunc);
    out << "symbol,status,error,n_rows,n_options,spot,profile,profile_conf,decision_source,"
           "effective_preset,chosen_kind,primary_kind,used_fallback,selector_ran,selector_oos_vw,"
           "worst_in_band,mean_in_band,mean_chi2,mean_rmse_vol,calendar_arb_free,"
           "n_calendar_viol,n_price_bound_viol,n_slices,"
           "n_quotes_used,n_valued,n_price_nan,n_bidiv_nan,n_askiv_nan,load_ms,chain_ms,fit_ms,"
           "value_ms,selector_fallback,f_live_quotes,f_live_expiries,f_quoted_expiries,"
           "f_atm_quotes,f_ident_expiries,f_max_nm_strikes,f_median_spread,f_front_expiries,"
           "f_weeklies,selector_error,"
           // Appended block: independent risk oracle. Existing columns and their
           // order are frozen so previously-written analysis scripts keep working.
           "oracle_ran,oracle_state,oracle_reasons,oracle_candidate_generation,"
           "oracle_served_generation,oracle_n_slices,oracle_n_strike_samples,"
           "oracle_n_calendar_samples,oracle_n_non_finite,oracle_n_price_bound_violations,"
           "oracle_n_strike_monotonicity_violations,oracle_n_butterfly_violations,"
           "oracle_n_calendar_violations,oracle_n_wing_violations,oracle_max_calendar_slack,"
           "oracle_max_butterfly_slack,oracle_max_price_bound_slack,"
           "oracle_max_wing_slope_excess,oracle_first_calendar_k,oracle_first_butterfly_k,"
           "mm_state\n";
    for (const Row &w : rows) {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    ",%zu,%zu,%.6f,%s,%.6f,%s,%s,%s,%s,%d,%d,%.4f,%.4f,%.4f,%.4f,%.6f,%d,%zu,%zu,"
                    "%zu,%zu,"
                    "%zu,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%d,",
                    w.n_rows, w.n_options, w.spot, w.profile.c_str(), w.profile_conf,
                    w.decision_source.c_str(), w.effective_preset.c_str(), w.chosen_kind.c_str(),
                    w.primary_kind.c_str(), w.used_fallback ? 1 : 0, w.selector_ran ? 1 : 0,
                    w.selector_oos_vw, w.worst_in_band, w.mean_in_band, w.mean_chi2,
                    w.mean_rmse_vol, w.calendar_arb_free ? 1 : 0, w.n_calendar_viol,
                    w.n_price_bound_viol, w.n_slices, w.n_quotes_used,
                    w.n_valued, w.n_price_nan, w.n_bidiv_nan, w.n_askiv_nan, w.load_ms, w.chain_ms,
                    w.fit_ms, w.value_ms, w.selector_fallback ? 1 : 0);
      char fbuf[256];
      std::snprintf(fbuf, sizeof(fbuf), "%u,%u,%u,%u,%u,%u,%.6g,%u,%d,", w.f_live_quotes,
                    w.f_live_expiries, w.f_quoted_expiries, w.f_atm_quotes, w.f_ident_expiries,
                    w.f_max_nm_strikes, w.f_median_spread, w.f_front_expiries,
                    w.f_weeklies ? 1 : 0);
      // ValidationDigest counters are std::uint32_t, not std::size_t: `%u`, never
      // `%zu`. A mismatched conversion specifier is undefined behaviour, and the
      // two types differ in width on this target. The 64-bit generation stamps
      // are cast to `unsigned long long` so `%llu` is exact by construction
      // rather than by assuming what std::uint64_t maps to.
      char obuf[512];
      std::snprintf(obuf, sizeof(obuf),
                    ",%d,%s,%u,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                    "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%s",
                    w.oracle_ran ? 1 : 0, w.oracle_state.c_str(), w.oracle_reasons,
                    static_cast<unsigned long long>(w.oracle_candidate_generation),
                    static_cast<unsigned long long>(w.oracle_served_generation), w.o_n_slices,
                    w.o_n_strike_samples, w.o_n_calendar_samples, w.o_n_non_finite,
                    w.o_n_price_bound_violations, w.o_n_strike_monotonicity_violations,
                    w.o_n_butterfly_violations, w.o_n_calendar_violations, w.o_n_wing_violations,
                    w.o_max_calendar_slack, w.o_max_butterfly_slack, w.o_max_price_bound_slack,
                    w.o_max_wing_slope_excess, w.o_first_calendar_k, w.o_first_butterfly_k,
                    w.mm_state.c_str());
      out << csv_escape(w.symbol) << ',' << w.status << ',' << csv_escape(w.error) << buf << fbuf
          << csv_escape(w.selector_error) << obuf << '\n';
    }
  }

  // ── Summary ────────────────────────────────────────────────────────────────
  std::map<std::string, std::size_t> by_status, by_kind, by_profile, by_error;
  std::vector<double> fit_times;
  double fit_ms_sum = 0.0;
  for (const Row &w : rows) {
    ++by_status[w.status];
    if (w.status == "ok") {
      ++by_kind[w.chosen_kind.empty() ? "(none)" : w.chosen_kind];
      ++by_profile[w.profile.empty() ? "(none)" : w.profile];
      fit_times.push_back(w.fit_ms);
      fit_ms_sum += w.fit_ms;
    } else if (!w.error.empty()) {
      ++by_error[w.error.substr(0, 90)];
    }
  }

  std::printf("\n=== universe_autofit summary ===\n");
  std::printf("wall: load=%.1fs fit+value=%.1fs (workers=%u) | serial fit cpu=%.1fs\n",
              load_total_ms / 1e3, fit_total_ms / 1e3, fit_workers, fit_ms_sum / 1e3);
  std::printf("-- status --\n");
  for (const auto &[k, n] : by_status) std::printf("  %-16s %6zu\n", k.c_str(), n);
  std::printf("-- chosen curve family (ok boards) --\n");
  for (const auto &[k, n] : by_kind) std::printf("  %-16s %6zu\n", k.c_str(), n);
  std::printf("-- profile (ok boards) --\n");
  for (const auto &[k, n] : by_profile) std::printf("  %-22s %6zu\n", k.c_str(), n);
  std::printf("-- top errors --\n");
  {
    std::vector<std::pair<std::size_t, std::string>> errs;
    for (const auto &[msg, n] : by_error) errs.emplace_back(n, msg);
    std::sort(errs.rbegin(), errs.rend());
    const std::size_t show = errs.size() < 15 ? errs.size() : 15;
    for (std::size_t i = 0; i < show; ++i)
      std::printf("  %5zu  %s\n", errs[i].first, errs[i].second.c_str());
  }
  if (!fit_times.empty()) {
    std::sort(fit_times.begin(), fit_times.end());
    const auto pct = [&](double p) {
      return fit_times[static_cast<std::size_t>(p * (fit_times.size() - 1))];
    };
    std::printf("-- fit_ms percentiles (ok boards, n=%zu) --\n", fit_times.size());
    std::printf("  p50=%.1f p90=%.1f p99=%.1f max=%.1f mean=%.1f\n", pct(0.50), pct(0.90),
                pct(0.99), fit_times.back(), fit_ms_sum / static_cast<double>(fit_times.size()));
  }
  {
    std::vector<const Row *> slow;
    for (const Row &w : rows)
      if (w.status == "ok") slow.push_back(&w);
    std::sort(slow.begin(), slow.end(),
              [](const Row *a, const Row *b) { return a->fit_ms > b->fit_ms; });
    const std::size_t show = slow.size() < 15 ? slow.size() : 15;
    std::printf("-- slowest fits --\n");
    for (std::size_t i = 0; i < show; ++i)
      std::printf("  %-8s fit=%8.1fms rows=%6zu kind=%-14s profile=%s\n", slow[i]->symbol.c_str(),
                  slow[i]->fit_ms, slow[i]->n_rows, slow[i]->chosen_kind.c_str(),
                  slow[i]->profile.c_str());
  }
  std::printf("\nresults: %s\n", out_csv.c_str());
  return 0;
}
