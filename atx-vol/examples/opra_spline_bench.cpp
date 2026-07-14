// opra_spline_bench.cpp — SpiderRock "SRCubic" cubic-spline surface parity
// benchmark over a full OPRA universe snapshot.
//
// Pins VolCurveKind::SplineVol (atx-vol's port of SpiderRock LiveVolSurfaces'
// SRCubic curve: a natural cubic spline over the vol MULTIPLE sigma(K)/sigma_ATM
// on the fixed 29-point standardized-moneyness grid `kSrMoneynessGrid`) and fits
// every underlier in one OPRA snapshot minute, board-parallel with each fit
// serial. It answers three questions SpiderRock's docs pose but leave
// unquantified:
//
//   (a) SPEED   — wall-clock to fit the whole equity universe, throughput in
//                 surfaces/sec, slices/sec, quotes/sec, and the fit-latency
//                 distribution. SpiderRock: "~45 seconds to process all equity
//                 and futures expirations" on their distributed fitter.
//   (b) ACCURACY— the SpiderRock-native fit-quality metrics rolled up from
//                 ParityReport::band per surface: max premium bid-ask violation
//                 (fitMaxPrcErr), call/put bid/ask miss counts, fraction of
//                 quotes inside the bid-ask channel, and RMSE(model - mkt vol)
//                 (fitAvgErr analogue). SpiderRock: "fitMaxPrcErr is zero for
//                 most (typically 90%) of all SpiderRock surface fits."
//   (c) HOT PATH— per-stage CPU decomposition (parquet load / chain build / fit
//                 / valuation) aggregated across the universe, the slowest
//                 boards, and an optional single-board repeat-fit micro-profile
//                 to isolate steady-state per-slice fit cost.
//
//   opra_spline_bench --opra-root DIR --date YYYY-MM-DD
//       [--symbols-file FILE]      (default: enumerate {opra-root}/*/{date}.parquet)
//       [--snapshot-suffix T14:00:00Z] [--r 0.043] [--preset accurate]
//       [--fit-workers N] [--limit N] [--out results.csv] [--no-value]
//       [--spline-lambda X] [--spline-mult-floor X] [--spline-min-obs N]
//       [--profile-symbol SYM] [--profile-iters N]
//
// Output CSV: one row per symbol. Summary to stdout: status, throughput, fit
// percentiles, per-stage hot-path split, SpiderRock accuracy roll-up + verdict,
// slowest boards, top errors.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/chain.hpp"         // OptionChain
#include "atx/vol/corpus.hpp"        // CorpusBoard, corpus_board_from_opra
#include "atx/vol/opra_batch.hpp"    // OpraBatchSpec, load_opra_daterange
#include "atx/vol/parallel_for.hpp"  // parallel_for, atx_auto_worker_count
#include "atx/vol/prepared_policy.hpp" // PreparedObservationPolicy
#include "atx/vol/pricer_fitter.hpp" // PricerFitter, PricerConfig, OutputField
#include "atx/vol/session.hpp"       // FitPreset, SessionDiagnostics, SessionInputs
#include "atx/vol/surface_parity.hpp" // CalendarRepair
#include "atx/vol/american.hpp"      // al_fast_opts, al_default_opts
#include "atx/vol/types.hpp"         // Result
#include "atx/vol/vol_curve.hpp"     // VolCurveKind, CurveConfig, to_string

using namespace atx::vol;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

FitPreset parse_preset(std::string_view name) {
  if (name == "accurate") return FitPreset::Accurate;
  if (name == "robust") return FitPreset::Robust;
  if (name == "hft") return FitPreset::Hft;
  return FitPreset::Fast;
}

// One symbol's full outcome. Plain data; workers write disjoint slots.
struct Row {
  std::string symbol;
  std::string status{"skipped"}; // load_missing|load_error|chain_error|fit_error|fit_exception|ok
  std::string error;
  std::size_t n_rows{0};      // parquet quote rows
  std::size_t n_options{0};   // chain option ids (post-build)
  double spot{0.0};
  std::string chosen_kind;    // must read "spline-vol" when the pin took
  // SpiderRock-native fit-quality (rolled up from ParityReport::band)
  double worst_in_band{0.0};  // min over expiries of frac inside bid-ask
  double mean_in_band{0.0};   // mean over expiries
  double mean_rmse_vol{0.0};  // mean RMSE(model - mkt vol)  ~ SpiderRock fitAvgErr
  double mean_chi2{0.0};
  double max_prc_err{0.0};    // max premium bid-ask violation ~ SpiderRock fitMaxPrcErr
  std::size_t n_bid_miss{0};
  std::size_t n_ask_miss{0};
  bool calendar_arb_free{false};
  std::size_t n_slices{0};
  std::size_t n_quotes_used{0};
  // valuation
  std::size_t n_valued{0};
  std::size_t n_price_nan{0};
  std::size_t n_bidiv_nan{0};
  std::size_t n_askiv_nan{0};
  // timings (per board, ms)
  double load_ms{0.0};
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

// Enumerate every {opra_root}/{SYMBOL}/{date}.parquet: the symbol is the
// subdirectory name that actually holds the snapshot file for `date`.
std::vector<std::string> enumerate_symbols(const std::string &opra_root, const std::string &date) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;
  std::error_code ec;
  const std::string leaf = date + ".parquet";
  for (fs::directory_iterator it(opra_root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    const fs::path file = it->path() / leaf;
    if (fs::exists(file, ec)) out.push_back(it->path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string csv_escape(const std::string &s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string q = "\"";
  for (char c : s) { if (c == '"') q += "\"\""; else q += c; }
  q += "\"";
  return q;
}

double pctile(std::vector<double> &v, double p) {
  if (v.empty()) return 0.0;
  return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

// Build a SplineVol-pinned config from CLI knobs.
PricerConfig make_spline_config(FitPreset preset, const FitContext &ctx, double lambda,
                                double mult_floor, std::size_t min_obs,
                                const std::string &fit_prep_arg, bool audit_fit_inv,
                                bool warm_carry) {
  PricerConfig cfg;
  cfg.preset = preset;
  cfg.context = ctx;
  cfg.n_threads = 1; // board-level parallelism only; each fit stays serial
  CurveConfig cc;
  cc.kind = VolCurveKind::SplineVol;
  cc.spline.lambda = lambda;
  cc.spline.mult_floor = mult_floor;
  cc.spline.min_obs = min_obs;
  cfg.curve = cc;
  // Observation-preparation policy for the polymorphic (SplineVol) fit path.
  // "configured" (default) reproduces the historical strict-floor baseline;
  // "legacy" keeps thin single-name expiries via the permissive eSSVI predicate.
  cfg.fit_prep_policy = (fit_prep_arg == "legacy")
                            ? PreparedObservationPolicy::LegacyEssviCompatibility
                            : PreparedObservationPolicy::Configured;
  cfg.audit_fit_inversions = audit_fit_inv;
  // Opt into the cross-pair warm start for the de-Am carry solve (off = the
  // bit-identical cold reference path).
  cfg.warm_start_carry = warm_carry;
  return cfg;
}

} // namespace

int main(int argc, char **argv) {
  std::string opra_root, date, symbols_file, out_csv = "opra_spline_bench_results.csv";
  std::string snapshot_suffix = "T14:00:00Z";
  std::string preset_name_arg = "accurate";
  // Observation-preparation policy knobs. Defaults reproduce the current report
  // baseline: strict "configured" preparation with fit-inversion audit ON.
  std::string fit_prep_arg = "configured";
  bool audit_fit_inv = true;
  bool warm_carry = false;
  double r = 0.043;
  double spline_lambda = 1.0e-3;
  double spline_mult_floor = 0.05;
  std::size_t spline_min_obs = 6;
  unsigned fit_workers = atx_auto_worker_count();
  std::size_t limit = 0;
  bool do_value = true;
  std::string profile_symbol;
  std::size_t profile_iters = 200;
  // De-Am / calendar sweep overrides applied via session_overlay AFTER the
  // preset + pinned curve + PricerConfig overrides. Each nullopt keeps the
  // preset default, so a plain run is byte-identical to the historical path.
  std::optional<std::uint32_t> ov_max_borrow_pairs, ov_n_atm;
  std::optional<bool> ov_al_fast, ov_score_parity, ov_cal_floor;
  std::optional<double> ov_iv_tol;
  std::optional<int> ov_cal_repair; // 0=None, 1=MonotoneFit
  std::optional<std::uint32_t> ov_max_obs; // per-slice de-Am inversion cap (0 = none)
  std::optional<bool> ov_corr_cache;       // use_correction_cache override
  std::optional<bool> ov_slice_fallback;   // per-slice LinearVariance fallback override
  std::optional<std::uint32_t> ov_max_deam_strikes; // legacy-prep de-Am strike cap (0 = none)

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
    else if (a == "--spline-lambda") spline_lambda = std::strtod(nv(), nullptr);
    else if (a == "--spline-mult-floor") spline_mult_floor = std::strtod(nv(), nullptr);
    else if (a == "--spline-min-obs") spline_min_obs = static_cast<std::size_t>(std::strtoull(nv(), nullptr, 10));
    else if (a == "--profile-symbol") profile_symbol = nv();
    else if (a == "--profile-iters") profile_iters = static_cast<std::size_t>(std::strtoull(nv(), nullptr, 10));
    else if (a == "--fit-prep") fit_prep_arg = nv();
    else if (a == "--no-audit-fit-inv") audit_fit_inv = false;
    else if (a == "--warm-carry") warm_carry = true;
    else if (a == "--max-borrow-pairs") ov_max_borrow_pairs = static_cast<std::uint32_t>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--n-atm") ov_n_atm = static_cast<std::uint32_t>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--al") { const std::string v = nv(); ov_al_fast = (v == "fast"); }
    else if (a == "--iv-tol") ov_iv_tol = std::strtod(nv(), nullptr);
    else if (a == "--calendar-repair") { const std::string v = nv(); ov_cal_repair = (v == "monotone") ? 1 : 0; }
    else if (a == "--no-score-parity") ov_score_parity = false;
    else if (a == "--no-cal-floor") ov_cal_floor = false;
    else if (a == "--max-obs") ov_max_obs = static_cast<std::uint32_t>(std::strtoul(nv(), nullptr, 10));
    else if (a == "--no-correction-cache") ov_corr_cache = false;
    else if (a == "--slice-fallback") { const std::string v = nv(); ov_slice_fallback = (v == "on"); }
    else if (a == "--max-deam-strikes") ov_max_deam_strikes = static_cast<std::uint32_t>(std::strtoul(nv(), nullptr, 10));
    else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }
  if (opra_root.empty() || date.empty()) {
    std::fprintf(stderr,
                 "usage: opra_spline_bench --opra-root DIR --date YYYY-MM-DD "
                 "[--symbols-file FILE] [--snapshot-suffix T14:00:00Z] [--r 0.043] "
                 "[--preset accurate] [--fit-workers N] [--limit N] [--out FILE] [--no-value] "
                 "[--spline-lambda X] [--spline-mult-floor X] [--spline-min-obs N] "
                 "[--fit-prep configured|legacy] [--no-audit-fit-inv] [--warm-carry] "
                 "[--profile-symbol SYM] [--profile-iters N]\n");
    return 2;
  }

  std::vector<std::string> symbols =
      symbols_file.empty() ? enumerate_symbols(opra_root, date) : read_symbols_file(symbols_file);
  if (symbols.empty()) {
    std::fprintf(stderr, "no symbols (root=%s date=%s file=%s)\n", opra_root.c_str(), date.c_str(),
                 symbols_file.c_str());
    return 2;
  }
  if (limit > 0 && symbols.size() > limit) symbols.resize(limit);
  const FitPreset preset = parse_preset(preset_name_arg);

  // De-Am / calendar sweep overlay: the final word on SessionInputs, applied
  // just before VolaSession::build. Leaves the pinned curve + prep policy alone.
  const std::function<void(SessionInputs &)> overlay =
      [=](SessionInputs &in) {
        if (ov_max_borrow_pairs) in.deam.max_borrow_pairs = *ov_max_borrow_pairs;
        if (ov_n_atm) in.deam.n_atm = *ov_n_atm;
        if (ov_al_fast) in.deam.al_opts = *ov_al_fast ? al_fast_opts() : al_default_opts();
        if (ov_iv_tol) in.deam.iv_tol = *ov_iv_tol;
        if (ov_cal_repair)
          in.calendar_repair =
              (*ov_cal_repair == 1) ? CalendarRepair::MonotoneFit : CalendarRepair::None;
        if (ov_score_parity) in.score_parity = *ov_score_parity;
        if (ov_cal_floor) in.enforce_calendar_floor = *ov_cal_floor;
        if (ov_max_obs) in.calib.max_obs_per_slice = *ov_max_obs;
        if (ov_corr_cache) in.use_correction_cache = *ov_corr_cache;
        if (ov_slice_fallback) in.calib.per_slice_linear_fallback = *ov_slice_fallback;
        if (ov_max_deam_strikes) in.calib.max_deam_strikes_per_expiry = *ov_max_deam_strikes;
      };

  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("[opra_spline_bench] curve=SplineVol(SRCubic) symbols=%zu date=%s snapshot=%s "
              "preset=%s fit-workers=%u\n",
              symbols.size(), date.c_str(), snapshot_suffix.c_str(), preset_name_arg.c_str(),
              fit_workers);
  std::printf("[spline] lambda=%.3g mult_floor=%.3g min_obs=%zu grid=29pt(kSrMoneynessGrid)\n",
              spline_lambda, spline_mult_floor, spline_min_obs);
  std::printf("[prep] fit-prep=%s audit-fit-inv=%s\n", fit_prep_arg.c_str(),
              audit_fit_inv ? "on" : "off");
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
  std::printf("[load] loaded=%zu missing=%zu error=%zu of %zu in %.2fs\n", batch->n_loaded,
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
    struct Progress {
      const Row &r;
      std::atomic<std::size_t> &done;
      std::size_t total;
      Clock::time_point t0;
      ~Progress() {
        const std::size_t k = ++done;
        if (k % 100 == 0 || k == total) {
          const double el = std::chrono::duration<double>(Clock::now() - t0).count();
          std::fprintf(stderr, "[%zu/%zu] %-8s %-12s fit=%.0fms eta=%.0fs\n", k, total,
                       r.symbol.c_str(), r.status.c_str(), r.fit_ms,
                       k ? el / static_cast<double>(k) * static_cast<double>(total - k) : 0.0);
        }
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

      const PricerConfig cfg = make_spline_config(preset, board.fit_context, spline_lambda,
                                                  spline_mult_floor, spline_min_obs,
                                                  fit_prep_arg, audit_fit_inv, warm_carry);
      PricerFitter fitter{cfg};

      const auto t2 = Clock::now();
      const Status st = fitter.fit(chain.value(), overlay);
      row.fit_ms = ms_since(t2);
      if (!st) {
        row.status = "fit_error";
        row.error = st.error().to_string();
        return;
      }

      if (const FittedSurface *surf = fitter.surface()) {
        const SessionDiagnostics &dg = surf->diagnostics();
        row.worst_in_band = dg.worst_frac_within_bidask;
        row.mean_in_band = dg.mean_frac_within_bidask;
        row.mean_chi2 = dg.mean_chi2_reduced;
        row.mean_rmse_vol = dg.mean_rmse_vol;
        row.max_prc_err = dg.max_prc_err;
        row.n_bid_miss = dg.n_bid_miss;
        row.n_ask_miss = dg.n_ask_miss;
        row.calendar_arb_free = dg.calendar_arb_free;
        row.n_slices = dg.n_slices;
        row.n_quotes_used = dg.n_quotes;
      }
      row.chosen_kind = to_string(VolCurveKind::SplineVol);

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
    out << "symbol,status,error,n_rows,n_options,spot,chosen_kind,worst_in_band,mean_in_band,"
           "mean_rmse_vol,mean_chi2,max_prc_err,n_bid_miss,n_ask_miss,calendar_arb_free,n_slices,"
           "n_quotes_used,n_valued,n_price_nan,n_bidiv_nan,n_askiv_nan,load_ms,chain_ms,fit_ms,"
           "value_ms\n";
    for (const Row &w : rows) {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    ",%zu,%zu,%.6f,%s,%.6f,%.6f,%.6f,%.4f,%.6f,%zu,%zu,%d,%zu,%zu,%zu,%zu,%zu,%zu,"
                    "%.3f,%.3f,%.3f,%.3f\n",
                    w.n_rows, w.n_options, w.spot, w.chosen_kind.c_str(), w.worst_in_band,
                    w.mean_in_band, w.mean_rmse_vol, w.mean_chi2, w.max_prc_err, w.n_bid_miss,
                    w.n_ask_miss, w.calendar_arb_free ? 1 : 0, w.n_slices, w.n_quotes_used,
                    w.n_valued, w.n_price_nan, w.n_bidiv_nan, w.n_askiv_nan, w.load_ms, w.chain_ms,
                    w.fit_ms, w.value_ms);
      out << csv_escape(w.symbol) << ',' << w.status << ',' << csv_escape(w.error) << buf;
    }
  }

  // ── Aggregates ──────────────────────────────────────────────────────────────
  std::map<std::string, std::size_t> by_status, by_error;
  std::vector<double> fit_times;
  double sum_load = 0, sum_chain = 0, sum_fit = 0, sum_value = 0; // per-stage CPU (ms) across ALL rows
  // accuracy accumulators over OK boards
  std::size_t n_ok = 0, n_slices_tot = 0, n_quotes_tot = 0, n_options_tot = 0;
  std::size_t n_zero_prc_viol = 0, n_cal_arb_free = 0, bid_miss_tot = 0, ask_miss_tot = 0;
  std::size_t price_nan_tot = 0, valued_tot = 0;
  double mean_in_band_sum = 0, worst_in_band_min = 1.0, rmse_vol_sum = 0;
  std::vector<double> prc_errs;
  for (const Row &w : rows) {
    ++by_status[w.status];
    sum_load += w.load_ms;
    sum_chain += w.chain_ms;
    sum_fit += w.fit_ms;
    sum_value += w.value_ms;
    if (w.status == "ok") {
      ++n_ok;
      fit_times.push_back(w.fit_ms);
      n_slices_tot += w.n_slices;
      n_quotes_tot += w.n_quotes_used;
      n_options_tot += w.n_options;
      mean_in_band_sum += w.mean_in_band;
      worst_in_band_min = std::min(worst_in_band_min, w.worst_in_band);
      rmse_vol_sum += w.mean_rmse_vol;
      prc_errs.push_back(w.max_prc_err);
      if (w.max_prc_err <= 1e-9) ++n_zero_prc_viol;
      if (w.calendar_arb_free) ++n_cal_arb_free;
      bid_miss_tot += w.n_bid_miss;
      ask_miss_tot += w.n_ask_miss;
      valued_tot += w.n_valued;
      price_nan_tot += w.n_price_nan;
    } else if (!w.error.empty()) {
      ++by_error[w.error.substr(0, 90)];
    }
  }
  const double fit_wall_s = fit_total_ms / 1e3;
  const double pipeline_wall_s = (load_total_ms + fit_total_ms) / 1e3;
  const double denom_ok = n_ok ? static_cast<double>(n_ok) : 1.0;

  // ── Summary ────────────────────────────────────────────────────────────────
  std::printf("\n=== opra_spline_bench summary (SpiderRock SRCubic parity) ===\n");
  std::printf("wall: load=%.2fs  fit+value=%.2fs (workers=%u)  pipeline=%.2fs\n",
              load_total_ms / 1e3, fit_wall_s, fit_workers, pipeline_wall_s);

  std::printf("-- status --\n");
  for (const auto &[k, n] : by_status) std::printf("  %-16s %6zu\n", k.c_str(), n);

  // ---- Throughput ----
  std::printf("-- throughput (ok boards) --\n");
  std::printf("  surfaces/s = %.1f   slices/s = %.0f   quotes/s = %.0f   options-valued/s = %.0f\n",
              static_cast<double>(n_ok) / std::max(fit_wall_s, 1e-9),
              static_cast<double>(n_slices_tot) / std::max(fit_wall_s, 1e-9),
              static_cast<double>(n_quotes_tot) / std::max(fit_wall_s, 1e-9),
              static_cast<double>(valued_tot) / std::max(fit_wall_s, 1e-9));

  // ---- Fit-latency distribution ----
  if (!fit_times.empty()) {
    std::sort(fit_times.begin(), fit_times.end());
    double fit_ms_sum = 0;
    for (double t : fit_times) fit_ms_sum += t;
    std::printf("-- fit_ms per board (ok, n=%zu) --\n", fit_times.size());
    std::printf("  p50=%.2f p90=%.2f p99=%.2f max=%.2f mean=%.2f\n", pctile(fit_times, 0.50),
                pctile(fit_times, 0.90), pctile(fit_times, 0.99), fit_times.back(),
                fit_ms_sum / static_cast<double>(fit_times.size()));
  }

  // ---- HOT PATH: per-stage CPU decomposition ----
  const double cpu_tot = sum_load + sum_chain + sum_fit + sum_value;
  const double cpu_den = cpu_tot > 0 ? cpu_tot : 1.0;
  std::printf("-- hot path: aggregate CPU by stage (sum over all boards) --\n");
  std::printf("  parquet-load  %9.1fs  %5.1f%%\n", sum_load / 1e3, 100.0 * sum_load / cpu_den);
  std::printf("  chain-build   %9.1fs  %5.1f%%\n", sum_chain / 1e3, 100.0 * sum_chain / cpu_den);
  std::printf("  fit(deAm+spl) %9.1fs  %5.1f%%\n", sum_fit / 1e3, 100.0 * sum_fit / cpu_den);
  std::printf("  valuation     %9.1fs  %5.1f%%\n", sum_value / 1e3, 100.0 * sum_value / cpu_den);
  std::printf("  total-cpu     %9.1fs   (parallel speedup vs fit-wall = %.1fx)\n", cpu_tot / 1e3,
              cpu_tot / std::max(fit_total_ms, 1e-9));

  // ---- ACCURACY: SpiderRock-native roll-up ----
  std::printf("-- accuracy vs SpiderRock LiveVolSurfaces metrics (ok boards) --\n");
  std::printf("  mean frac-in-bidask       = %.4f   (SR: fit within bid-ask channel)\n",
              mean_in_band_sum / denom_ok);
  std::printf("  worst frac-in-bidask      = %.4f   (min over all surfaces)\n", worst_in_band_min);
  std::printf("  mean RMSE(model-mkt vol)  = %.6f   (~ SR fitAvgErr, vol pts)\n",
              rmse_vol_sum / denom_ok);
  if (!prc_errs.empty()) {
    std::sort(prc_errs.begin(), prc_errs.end());
    double prc_sum = 0;
    for (double v : prc_errs) prc_sum += v;
    std::printf("  fitMaxPrcErr (premium $)   p50=%.4f p90=%.4f p99=%.4f max=%.4f mean=%.4f\n",
                pctile(prc_errs, 0.50), pctile(prc_errs, 0.90), pctile(prc_errs, 0.99),
                prc_errs.back(), prc_sum / static_cast<double>(prc_errs.size()));
  }
  std::printf("  zero bid-ask violation     = %zu / %zu  = %.1f%%   (SR target ~90%%)\n",
              n_zero_prc_viol, n_ok, 100.0 * static_cast<double>(n_zero_prc_viol) / denom_ok);
  std::printf("  calendar-arb-free          = %zu / %zu  = %.1f%%\n", n_cal_arb_free, n_ok,
              100.0 * static_cast<double>(n_cal_arb_free) / denom_ok);
  std::printf("  bid/ask misses (total)     = %zu / %zu   (quotes crossing surface)\n",
              bid_miss_tot + ask_miss_tot, n_quotes_tot);
  if (do_value)
    std::printf("  valuation price-NaN        = %zu / %zu  = %.3f%%\n", price_nan_tot, valued_tot,
                100.0 * static_cast<double>(price_nan_tot) / std::max<std::size_t>(valued_tot, 1));

  // ---- SpiderRock verdict ----
  std::printf("-- SpiderRock parity verdict --\n");
  std::printf("  universe fit wall = %.2fs vs SR ~45s full-universe target  ->  %.1fx %s\n",
              pipeline_wall_s, 45.0 / std::max(pipeline_wall_s, 1e-9),
              pipeline_wall_s <= 45.0 ? "FASTER" : "slower");
  std::printf("  clean-fit rate    = %.1f%% zero-premium-violation vs SR ~90%%  ->  %s\n",
              100.0 * static_cast<double>(n_zero_prc_viol) / denom_ok,
              (100.0 * static_cast<double>(n_zero_prc_viol) / denom_ok) >= 90.0 ? "MEETS" : "below");

  // ---- Top errors ----
  if (!by_error.empty()) {
    std::vector<std::pair<std::size_t, std::string>> errs;
    for (const auto &[msg, n] : by_error) errs.emplace_back(n, msg);
    std::sort(errs.rbegin(), errs.rend());
    std::printf("-- top errors --\n");
    const std::size_t show = std::min<std::size_t>(errs.size(), 12);
    for (std::size_t i = 0; i < show; ++i)
      std::printf("  %5zu  %s\n", errs[i].first, errs[i].second.c_str());
  }

  // ---- Slowest boards ----
  {
    std::vector<const Row *> slow;
    for (const Row &w : rows)
      if (w.status == "ok") slow.push_back(&w);
    std::sort(slow.begin(), slow.end(),
              [](const Row *a, const Row *b) { return a->fit_ms > b->fit_ms; });
    std::printf("-- slowest fits --\n");
    const std::size_t show = std::min<std::size_t>(slow.size(), 15);
    for (std::size_t i = 0; i < show; ++i)
      std::printf("  %-8s fit=%8.2fms rows=%6zu slices=%3zu quotes=%5zu maxPrcErr=%.4f\n",
                  slow[i]->symbol.c_str(), slow[i]->fit_ms, slow[i]->n_rows, slow[i]->n_slices,
                  slow[i]->n_quotes_used, slow[i]->max_prc_err);
  }
  (void)n_options_tot;
  (void)ask_miss_tot;

  // ── Optional single-board repeat-fit micro-profile ──────────────────────────
  if (!profile_symbol.empty()) {
    std::printf("\n-- micro-profile: %s repeat-fit x%zu (steady-state per-fit cost) --\n",
                profile_symbol.c_str(), profile_iters);
    const OpraBatchEntry *pe = nullptr;
    for (const auto *e : entries)
      if (e->symbol == profile_symbol && e->panel) { pe = e; break; }
    if (!pe) {
      std::printf("  symbol %s not loaded; skipping micro-profile\n", profile_symbol.c_str());
    } else {
      CorpusBoard board = corpus_board_from_opra(pe->date, pe->symbol, *pe->panel);
      Result<OptionChain> chain = OptionChain::from_frame(board.frame, board.env);
      if (!chain) {
        std::printf("  chain build failed: %s\n", chain.error().to_string().c_str());
      } else {
        const PricerConfig cfg = make_spline_config(preset, board.fit_context, spline_lambda,
                                                    spline_mult_floor, spline_min_obs,
                                                    fit_prep_arg, audit_fit_inv, warm_carry);
        std::vector<double> t_fit;
        t_fit.reserve(profile_iters);
        std::size_t n_slices = 0;
        for (std::size_t it = 0; it < profile_iters; ++it) {
          PricerFitter fitter{cfg};
          const auto t0 = Clock::now();
          const Status st = fitter.fit(chain.value(), overlay);
          t_fit.push_back(ms_since(t0));
          if (st && fitter.surface()) n_slices = fitter.surface()->diagnostics().n_slices;
        }
        std::sort(t_fit.begin(), t_fit.end());
        double sum = 0;
        for (double t : t_fit) sum += t;
        const double mean_ms = sum / static_cast<double>(t_fit.size());
        std::printf("  n_options=%zu slices=%zu\n", chain->ids().size(), n_slices);
        std::printf("  per-fit  p50=%.3fms p99=%.3fms min=%.3fms mean=%.3fms\n",
                    pctile(t_fit, 0.50), pctile(t_fit, 0.99), t_fit.front(), mean_ms);
        if (n_slices)
          std::printf("  per-slice mean = %.1f us   (fit-cost / slice)\n",
                      1000.0 * mean_ms / static_cast<double>(n_slices));
      }
    }
  }

  std::printf("\nresults: %s\n", out_csv.c_str());
  return 0;
}
