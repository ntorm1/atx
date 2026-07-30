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
//       [--min-direct-confidence X]
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
  }
  return "?";
}

FitPreset parse_preset(std::string_view name) {
  if (name == "accurate") return FitPreset::Accurate;
  if (name == "robust") return FitPreset::Robust;
  if (name == "hft") return FitPreset::Hft;
  if (name == "populate") return FitPreset::Populate;
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
  bool selector_ran{false};
  double selector_oos_vw{0.0};
  // fit diagnostics
  double worst_in_band{0.0};
  double mean_in_band{0.0};
  double mean_chi2{0.0};
  double mean_rmse_vol{0.0};
  bool calendar_arb_free{false};
  std::size_t n_slices{0};
  std::size_t n_quotes_used{0};
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

} // namespace

int main(int argc, char **argv) {
  std::string opra_root, date, symbols_file, out_csv = "universe_autofit_results.csv";
  std::string snapshot_suffix = "T14:00:00Z";
  std::string preset_name_arg = "robust";
  std::string pin_kind; // empty => auto-select; else pin this family for every board
  double r = 0.043;
  unsigned fit_workers = atx_auto_worker_count();
  std::size_t limit = 0;
  bool do_value = true;
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
    else if (a == "--r") r = std::strtod(nv(), nullptr);
    else if (a == "--preset") preset_name_arg = nv();
    else if (a == "--fit-workers") fit_workers = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--limit") limit = static_cast<std::size_t>(std::strtoull(nv(), nullptr, 10));
    else if (a == "--out") out_csv = nv();
    else if (a == "--no-value") do_value = false;
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
                 "[--limit N] [--out FILE] [--no-value] [--oos-max-expiries N] "
                 "[--selector-budget-ms N] [--sparse-floor N] "
                 "[--min-direct-confidence X]\n");
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
      PricerFitter fitter{cfg};

      const auto t2 = Clock::now();
      const Status st = fitter.fit(chain.value());
      row.fit_ms = ms_since(t2);
      if (!st) {
        row.status = "fit_error";
        row.error = st.error().to_string();
        // decision may still explain what the policy attempted
        if (fitter.decision()) {
          const FitDecision &d = *fitter.decision();
          row.profile = profile_name(d.profile.kind);
          row.profile_conf = d.profile.confidence;
          row.decision_source = source_name(d.source);
          row.effective_preset = preset_name(d.preset);
          row.chosen_kind = to_string(d.curve.kind);
          row.primary_kind = to_string(d.primary_curve.kind);
          row.used_fallback = d.used_fallback;
        }
        return;
      }

      if (fitter.decision()) {
        const FitDecision &d = *fitter.decision();
        row.profile = profile_name(d.profile.kind);
        row.profile_conf = d.profile.confidence;
        row.decision_source = source_name(d.source);
        row.effective_preset = preset_name(d.preset);
        row.chosen_kind = to_string(d.curve.kind);
        row.primary_kind = to_string(d.primary_curve.kind);
        row.used_fallback = d.used_fallback;
      }
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
      row.n_slices = dg.n_slices;
      row.n_quotes_used = dg.n_quotes;

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
           "worst_in_band,mean_in_band,mean_chi2,mean_rmse_vol,calendar_arb_free,n_slices,"
           "n_quotes_used,n_valued,n_price_nan,n_bidiv_nan,n_askiv_nan,load_ms,chain_ms,fit_ms,"
           "value_ms\n";
    for (const Row &w : rows) {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    ",%zu,%zu,%.6f,%s,%.3f,%s,%s,%s,%s,%d,%d,%.4f,%.4f,%.4f,%.4f,%.6f,%d,%zu,%zu,"
                    "%zu,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f\n",
                    w.n_rows, w.n_options, w.spot, w.profile.c_str(), w.profile_conf,
                    w.decision_source.c_str(), w.effective_preset.c_str(), w.chosen_kind.c_str(),
                    w.primary_kind.c_str(), w.used_fallback ? 1 : 0, w.selector_ran ? 1 : 0,
                    w.selector_oos_vw, w.worst_in_band, w.mean_in_band, w.mean_chi2,
                    w.mean_rmse_vol, w.calendar_arb_free ? 1 : 0, w.n_slices, w.n_quotes_used,
                    w.n_valued, w.n_price_nan, w.n_bidiv_nan, w.n_askiv_nan, w.load_ms, w.chain_ms,
                    w.fit_ms, w.value_ms);
      out << csv_escape(w.symbol) << ',' << w.status << ',' << csv_escape(w.error) << buf;
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
