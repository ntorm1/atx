// CStar vs eSSVI evidence panel (Sub-Sprint S / W5.1-W5.2 ladder decision;
// extended for the Sprint-I real-OPRA validation).
//
// Fits BOTH the production eSSVI per-slice calibrator (`essvi_fit_slice`) and the
// CStar C16M modal calibrator (`cstar_calibrate_slice`, seeded from that same
// eSSVI fit) to an IDENTICAL observation set per board, and reports the metrics
// the ladder decision needs: fit wall (best-of-3), vol-RMSE, in-band fraction
// (model price within the quote half-spread), price χ², and butterfly-arb-flag
// count — CStar against the same board's eSSVI result.
//
// Two modes:
//   * default (--synthetic) — the S5 controlled A/B over representative synthetic
//     regimes (a dense low-vol index slice, a sparse steep high-vol small-cap
//     slice, a locally-bumpy smile that exceeds an eSSVI 3-parameter backbone,
//     and a fat-tailed wing). Built from a known target IV smile; no data files
//     required, so the bench target runs on any checkout.
//   * --real — the Sprint-I validation over REAL Databento OPRA snapshots: SPY
//     (dense index) + the 25-name recovery cohort (sparse small-caps). Real
//     boards resolve to per-expiry `Chain`s via the SUPPORTED public pipeline
//     (`load_opra_cbbo_parquet` -> `data_install` -> `Universe` chains; the term
//     forward/carry per expiry from a read-only `VolaSession`). This is the
//     per-expiry extraction seam the S5 doc flagged as blocked; it is solved here
//     entirely with read-only reuse of the shipped loader/session APIs — no edits
//     to any pipeline TU.
//
// Both modes score both engines on the identical `build_observations` set per
// board (raw-mid inversion; the SAME observation set `cstar_calibrate_slice`
// consumes internally), so the comparison isolates parametrization shape
// fidelity, not the de-Americanization pipeline (handoff: CStar consumes already-
// prepared American observations + the existing eSSVI seed).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_butterfly_slice
#include "atx/vol/black76.hpp"      // black76_price
#include "atx/vol/calib.hpp"        // CalibOpts, FitObs, ObsSet, build_observations
#include "atx/vol/cstar.hpp"        // CStarParams, cstar_slice_iv, cstar_min_roper_g
#include "atx/vol/cstar_calib.hpp"  // cstar_calibrate_slice
#include "atx/vol/data.hpp"         // data_install
#include "atx/vol/essvi_calib.hpp"  // essvi_fit_slice
#include "atx/vol/opra_panel.hpp"   // OpraLoadSpec, load_opra_cbbo_parquet
#include "atx/vol/session.hpp"      // VolaSession, make_session_inputs, FitPreset
#include "atx/vol/universe.hpp"     // Chain, Universe, Underlying, chain_index
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_total_w

namespace {

using namespace atx::vol;

// ── Shared scoring ──────────────────────────────────────────────────────────

struct SliceScore {
  double vol_rmse{};       // sqrt(mean (iv_model - iv_mkt)²)
  double in_band_frac{};   // fraction with |price_model - mid| <= half_spread
  double chi2{};           // sum ((price_model - mid)/half_spread)² / n
  int n_scored{};
  int arb_flags{};         // butterfly-arb grid violations
  double fit_ms{};         // best-of-3 fit wall
};

// Score a model given an IV evaluator iv(k) and a total-variance evaluator w(k).
template <class IvFn, class WFn>
[[nodiscard]] SliceScore score_model(std::span<const FitObs> obs, double F,
                                     double T, double half_spread,
                                     IvFn&& iv_at, WFn&& w_at) {
  SliceScore sc;
  double sse_vol = 0.0;
  double chi2 = 0.0;
  int in_band = 0;
  int n = 0;
  double k_lo = std::numeric_limits<double>::infinity();
  double k_hi = -std::numeric_limits<double>::infinity();
  for (const FitObs& o : obs) {
    const double iv = iv_at(o.k);
    if (!std::isfinite(iv) || iv <= 0.0) {
      continue;
    }
    const double dv = iv - o.sigma_mkt;
    sse_vol += dv * dv;
    const double price = black76_price(F, o.K, T, iv, o.df, o.side);
    const double hs = (o.spread > 1.0e-12) ? 0.5 * o.spread : half_spread;
    const double resid = (price - o.mid) / hs;
    chi2 += resid * resid;
    if (std::fabs(price - o.mid) <= hs) {
      ++in_band;
    }
    k_lo = std::min(k_lo, o.k);
    k_hi = std::max(k_hi, o.k);
    ++n;
  }
  sc.n_scored = n;
  if (n > 0) {
    sc.vol_rmse = std::sqrt(sse_vol / static_cast<double>(n));
    sc.chi2 = chi2 / static_cast<double>(n);
    sc.in_band_frac = static_cast<double>(in_band) / static_cast<double>(n);
    const auto bf = arb_check_butterfly_slice(w_at, T, k_lo - 0.5, k_hi + 0.5, 128u);
    sc.arb_flags = bf.has_value() ? static_cast<int>(bf->size()) : -1;
  }
  return sc;
}

[[nodiscard]] double best_of_3(auto&& fn) {
  double best = std::numeric_limits<double>::infinity();
  for (int r = 0; r < 3; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    best = std::min(best,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  return best;
}

// One expiry's paired result: eSSVI + (optionally) CStar on the identical obs.
struct SliceResult {
  bool essvi_ok{false};
  bool cstar_ok{false};
  SliceScore essvi{};
  SliceScore cstar{};
  double cstar_min_g{std::numeric_limits<double>::quiet_NaN()};
  std::string cstar_err{};
};

// Fit eSSVI + CStar to one already-built observation set. `chain` is the
// per-expiry Chain (for CStar's internal obs rebuild); `obs` is the scoring set;
// F/T/df are the slice's forward / maturity / discount factor.
[[nodiscard]] SliceResult fit_and_score_slice(const Chain& chain,
                                              std::span<const FitObs> obs, double F,
                                              double T, double df, double half_spread,
                                              const CalibOpts& opts, bool time_it) {
  SliceResult res;

  EssviParams essvi{};
  auto essvi_res = essvi_fit_slice(obs, T, F, opts);
  if (!essvi_res.has_value()) {
    return res;  // essvi_ok stays false; slice is a coverage gap for both
  }
  essvi = essvi_res.value();
  res.essvi_ok = true;

  double essvi_ms = 0.0;
  if (time_it) {
    essvi_ms = best_of_3([&] { (void)essvi_fit_slice(obs, T, F, opts); });
  }
  res.essvi = score_model(
      obs, F, T, half_spread,
      [&](double k) { return std::sqrt(essvi_total_w(essvi, k) / T); },
      [&](double k) { return essvi_total_w(essvi, k); });
  res.essvi.fit_ms = essvi_ms;

  auto cstar_res = cstar_calibrate_slice(essvi, chain, df, opts);
  if (!cstar_res.has_value()) {
    res.cstar_err = cstar_res.error().to_string();
    return res;  // CStar coverage gap (e.g. butterfly-inadmissible steep skew)
  }
  const CStarParams cstar = cstar_res.value();
  res.cstar_ok = true;

  double cstar_ms = 0.0;
  if (time_it) {
    cstar_ms = best_of_3([&] { (void)cstar_calibrate_slice(essvi, chain, df, opts); });
  }
  res.cstar = score_model(
      obs, F, T, half_spread,
      [&](double k) { return cstar_slice_iv(cstar, k); },
      [&](double k) { return cstar_slice_w(cstar, k); });
  res.cstar.fit_ms = cstar_ms;
  res.cstar_min_g = cstar_min_roper_g(cstar);
  return res;
}

// ── Synthetic panel (the original S5 A/B; kept verbatim in spirit) ───────────

// A board's target smile: an ARB-FREE eSSVI base (theta, phi, rho — bounded
// linear wings, so the padded arb-check does not explode) plus an optional
// localized Gaussian IV bump — a real microstructure feature an eSSVI 3-parameter
// backbone cannot represent but a CStar mode can. bump = 0 => a pure eSSVI smile
// (both engines should recover it; the test is that CStar does not regress).
struct Board {
  std::string name;
  std::string regime;
  double F{};
  double T{};
  double theta{};       // eSSVI ATM total variance w(0)
  double phi{};         // eSSVI curvature
  double rho{};         // eSSVI skew
  int n_strikes{};
  double k_lo{};        // log-moneyness range of the listed strikes
  double k_hi{};
  double half_spread{}; // price-domain half-spread planted on every quote
  double bump_amp{};    // localized IV bump amplitude (0 = pure eSSVI)
  double bump_k{};      // bump center in log-moneyness
  double bump_width{};  // bump width in log-moneyness
};

[[nodiscard]] EssviParams truth_essvi(const Board& b) {
  EssviParams p{};
  p.theta = b.theta;
  p.phi = b.phi;
  p.rho = b.rho;
  p.T = b.T;
  p.F = b.F;
  return p;
}

[[nodiscard]] double target_iv(const Board& b, const EssviParams& truth, double k) {
  const double w = essvi_total_w(truth, k);
  double iv = (w > 0.0) ? std::sqrt(w / b.T) : 1.0e-3;
  if (b.bump_amp != 0.0) {
    const double u = (k - b.bump_k) / b.bump_width;
    iv += b.bump_amp * std::exp(-0.5 * u * u);
  }
  return iv > 1.0e-3 ? iv : 1.0e-3;
}

// Build a single-expiry Chain: for each strike, plant a symmetric bid/ask around
// the Black-76 mid at the target IV, both sides.
[[nodiscard]] Chain make_chain(const Board& b, double df) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = b.T;
  const EssviParams truth = truth_essvi(b);

  c.strikes.reserve(static_cast<std::size_t>(b.n_strikes));
  for (int i = 0; i < b.n_strikes; ++i) {
    const double k = b.k_lo + (b.k_hi - b.k_lo) * static_cast<double>(i) /
                                  static_cast<double>(b.n_strikes - 1);
    c.strikes.push_back(b.F * std::exp(k));
  }

  const std::size_t n2 = c.strikes.size() * 2u;
  c.bids.assign(n2, 0.0);
  c.asks.assign(n2, 0.0);
  c.mids.assign(n2, 0.0);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.bid_sizes.assign(n2, 10);
  c.ask_sizes.assign(n2, 10);
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, 0u);

  for (std::size_t si = 0; si < c.strikes.size(); ++si) {
    const double K = c.strikes[si];
    const double k = std::log(K / b.F);
    const double iv = target_iv(b, truth, k);
    for (int side_i = 0; side_i < 2; ++side_i) {
      const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(si), side);
      const double mid = black76_price(b.F, K, b.T, iv, df, side);
      c.mids[idx] = mid;
      c.bids[idx] = mid - b.half_spread;
      c.asks[idx] = mid + b.half_spread;
    }
  }
  return c;
}

int run_synthetic() {
  // theta = atm_vol²·T. eSSVI arb-free: theta·phi·(1+|rho|) << 4 (all satisfied).
  const std::array<Board, 4> boards = {{
      // name, regime, F, T, theta, phi, rho, n, k_lo, k_hi, hs, bump{amp,k,w}
      {"SPY-like", "dense-index", 580.0, 0.05, 0.00098, 6.0, -0.55, 41, -0.18,
       0.18, 0.02, 0.0, 0.0, 1.0},
      {"cohort-steep", "sparse-smallcap", 15.0, 0.08, 0.03075, 3.0, -0.45, 21,
       -0.30, 0.25, 0.015, 0.0, 0.0, 1.0},
      {"bumpy-smile", "modal-feature", 100.0, 0.10, 0.00484, 5.0, -0.40, 33,
       -0.22, 0.22, 0.02, 0.010, -0.08, 0.03},
      {"wing-heavy", "fat-tails", 250.0, 0.15, 0.01350, 4.0, -0.35, 29, -0.30,
       0.30, 0.025, 0.0, 0.0, 1.0},
  }};

  std::printf(
      "%-14s %-16s | %-28s | %-28s\n", "board", "regime",
      "eSSVI  wall/rmse/band/chi2/arb", "CStar  wall/rmse/band/chi2/arb");
  std::printf("%s\n", std::string(96, '-').c_str());

  const CalibOpts opts{};
  double sum_essvi_rmse = 0.0;
  double sum_cstar_rmse = 0.0;
  int n_boards = 0;

  for (const Board& b : boards) {
    const double df = std::exp(-0.03 * b.T);
    const Chain chain = make_chain(b, df);
    auto obs_res = build_observations(chain, b.F, b.T, df, opts);
    if (!obs_res.has_value()) {
      std::printf("%-14s %-16s | build_observations failed: %s\n", b.name.c_str(),
                  b.regime.c_str(), obs_res.error().to_string().c_str());
      continue;
    }
    const std::span<const FitObs> obs{obs_res.value().obs};

    const SliceResult sr =
        fit_and_score_slice(chain, obs, b.F, b.T, df, b.half_spread, opts, true);
    if (!sr.essvi_ok) {
      std::printf("%-14s %-16s | eSSVI fit failed\n", b.name.c_str(),
                  b.regime.c_str());
      continue;
    }
    if (!sr.cstar_ok) {
      std::printf("%-14s %-16s | %6.3fms %.4f %5.1f%% %6.2f %d | CStar fit failed: %s\n",
                  b.name.c_str(), b.regime.c_str(), sr.essvi.fit_ms, sr.essvi.vol_rmse,
                  100.0 * sr.essvi.in_band_frac, sr.essvi.chi2, sr.essvi.arb_flags,
                  sr.cstar_err.c_str());
      continue;
    }

    std::printf(
        "%-14s %-16s | %6.3fms %.4f %5.1f%% %6.2f %d | %6.3fms %.4f %5.1f%% "
        "%6.2f %d (ming=%+.3f)\n",
        b.name.c_str(), b.regime.c_str(), sr.essvi.fit_ms, sr.essvi.vol_rmse,
        100.0 * sr.essvi.in_band_frac, sr.essvi.chi2, sr.essvi.arb_flags,
        sr.cstar.fit_ms, sr.cstar.vol_rmse, 100.0 * sr.cstar.in_band_frac,
        sr.cstar.chi2, sr.cstar.arb_flags, sr.cstar_min_g);

    sum_essvi_rmse += sr.essvi.vol_rmse;
    sum_cstar_rmse += sr.cstar.vol_rmse;
    ++n_boards;
  }

  if (n_boards > 0) {
    std::printf("%s\n", std::string(96, '-').c_str());
    std::printf("mean vol-RMSE  eSSVI=%.5f  CStar=%.5f  (CStar/eSSVI=%.2fx)\n",
                sum_essvi_rmse / n_boards, sum_cstar_rmse / n_boards,
                sum_essvi_rmse > 0.0 ? sum_cstar_rmse / sum_essvi_rmse : 0.0);
  }
  return 0;
}

// ── Real-OPRA panel (Sprint-I validation) ───────────────────────────────────

// The 25-name recovery cohort (sparse steep small-cap skews) — the ladder-
// decision stress set the S5 doc names.
inline constexpr std::array<const char*, 25> kCohort{{
    "CZR", "RPRX", "RXT", "ROIV", "HST", "FTV", "EQH", "IBN", "TSLQ", "JHX",
    "MNTS", "OKLL", "EQX", "SIDU", "HIMX", "GFI", "DGXX", "VNET", "ESI", "BFAM",
    "PCOR", "HTHT", "IBRX", "ALHC", "GGG"}};

// One SPY intraday fixture (spy_fit_slices), used to widen the dense-index
// evidence across liquidity/vol regimes.
struct SpySlice {
  const char* filename;
  const char* snapshot_iso;
  const char* regime;
};

inline constexpr std::array<SpySlice, 10> kSpySlices{{
    {"SPY_2026-02-12T1435Z.parquet", "2026-02-12T14:35:00Z", "selloff/open"},
    {"SPY_2026-02-12T1700Z.parquet", "2026-02-12T17:00:00Z", "selloff/mid"},
    {"SPY_2026-02-12T1955Z.parquet", "2026-02-12T19:55:00Z", "selloff/pm"},
    {"SPY_2026-03-09T1335Z.parquet", "2026-03-09T13:35:00Z", "rally/open"},
    {"SPY_2026-03-09T1600Z.parquet", "2026-03-09T16:00:00Z", "rally/mid"},
    {"SPY_2026-03-09T1955Z.parquet", "2026-03-09T19:55:00Z", "rally/close"},
    {"SPY_2026-05-27T1335Z.parquet", "2026-05-27T13:35:00Z", "calm/open"},
    {"SPY_2026-05-27T1600Z.parquet", "2026-05-27T16:00:00Z", "calm/mid"},
    {"SPY_2026-05-27T1955Z.parquet", "2026-05-27T19:55:00Z", "calm/close"},
    {"SPY_2026-06-05T1955Z.parquet", "2026-06-05T19:55:00Z", "highvol/close"},
}};

struct RealBoardSpec {
  std::string label;
  std::string symbol;
  std::string path;
  std::string snapshot_iso;
  std::string regime;
};

// Obs-weighted aggregate of one board's paired slices.
struct BoardAgg {
  std::string label;
  std::string symbol;
  std::string regime;
  std::string status{"ok"};

  int n_expiries{0};       // expiries with a built obs set (T >= T_min)
  int n_essvi_ok{0};       // eSSVI fit succeeded
  int n_cstar_ok{0};       // CStar fit succeeded (subset of essvi_ok)

  // Paired obs-level accumulators over the CStar-covered slices (identical obs).
  long paired_n{0};
  double essvi_sse{0.0};   // Σ (iv-iv)² over paired obs
  double cstar_sse{0.0};
  double essvi_chi2_sum{0.0};
  double cstar_chi2_sum{0.0};
  long essvi_in_band{0};
  long cstar_in_band{0};
  int essvi_arb{0};
  int cstar_arb{0};
  int cstar_min_g_neg{0};  // slices where analytic min Roper g < 0

  // Fit-wall accumulators (paired slices, best-of-3 sums).
  double essvi_ms{0.0};
  double cstar_ms{0.0};

  // "Modal win" tally: paired slices where CStar vol-RMSE beats eSSVI by > 20%.
  int n_modal_win{0};
  int n_paired_slices{0};

  [[nodiscard]] double essvi_rmse() const {
    return paired_n > 0 ? std::sqrt(essvi_sse / static_cast<double>(paired_n)) : 0.0;
  }
  [[nodiscard]] double cstar_rmse() const {
    return paired_n > 0 ? std::sqrt(cstar_sse / static_cast<double>(paired_n)) : 0.0;
  }
  [[nodiscard]] double essvi_chi2() const {
    return paired_n > 0 ? essvi_chi2_sum / static_cast<double>(paired_n) : 0.0;
  }
  [[nodiscard]] double cstar_chi2() const {
    return paired_n > 0 ? cstar_chi2_sum / static_cast<double>(paired_n) : 0.0;
  }
  [[nodiscard]] double essvi_band() const {
    return paired_n > 0 ? static_cast<double>(essvi_in_band) / static_cast<double>(paired_n) : 0.0;
  }
  [[nodiscard]] double cstar_band() const {
    return paired_n > 0 ? static_cast<double>(cstar_in_band) / static_cast<double>(paired_n) : 0.0;
  }
};

[[nodiscard]] std::string find_existing(const std::vector<std::string>& candidates) {
  for (const std::string& c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c, ec)) {
      return c;
    }
  }
  return {};
}

// Split a comma-separated symbol list into trimmed, non-empty tokens (spaces
// dropped). Used by the --symbols override so the real cohort can be named at
// runtime (the on-disk OPRA set differs from the hardcoded 25-name cohort).
[[nodiscard]] std::vector<std::string> split_csv(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char ch : s) {
    if (ch == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
      }
      cur.clear();
    } else if (ch != ' ' && ch != '\t') {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

// Discover every real per-date OPRA cbbo-1m snapshot for one symbol under the
// dispersion layout `<root>/opra/<SYM>/<YYYY-MM-DD>.parquet` (the real Databento
// pull layout used across the atx-data OPRA collection). Returns one board per
// file, ascending by date; empty when the directory is absent so the caller can
// fall back to the fixed-name layouts. The snapshot stamp is derived from the
// filename date, but the term structure T is driven by each row's real `ts`
// (`frame.snapshot_ts_ns`), so the stamp is cosmetic only.
[[nodiscard]] std::vector<RealBoardSpec> discover_opra_boards(
    const std::string& root, const std::string& symbol, const std::string& regime) {
  std::vector<RealBoardSpec> out;
  const std::filesystem::path dir = std::filesystem::path(root) / "opra" / symbol;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return out;
  }
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".parquet") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  out.reserve(files.size());
  for (const std::filesystem::path& p : files) {
    const std::string date = p.stem().string();  // "YYYY-MM-DD"
    RealBoardSpec spec;
    spec.label = symbol + "/" + date;
    spec.symbol = symbol;
    spec.path = p.string();
    spec.snapshot_iso = date + "T14:00:00Z";
    spec.regime = regime;
    out.push_back(std::move(spec));
  }
  return out;
}

// Run one real board through the per-expiry seam and fold into `agg`.
void run_real_board(const RealBoardSpec& spec, double r, double t_min,
                    const CalibOpts& opts, bool time_it, bool verbose, BoardAgg& agg) {
  agg.label = spec.label;
  agg.symbol = spec.symbol;
  agg.regime = spec.regime;

  OpraLoadSpec load;
  load.path = spec.path;
  load.underlying = spec.symbol;
  load.snapshot_iso = spec.snapshot_iso;
  load.r = r;
  auto panel = load_opra_cbbo_parquet(load);
  if (!panel.has_value()) {
    agg.status = "load_fail:" + panel.error().to_string();
    return;
  }

  Universe u;
  auto uid = data_install(u, panel->frame);
  if (!uid.has_value()) {
    agg.status = "install_fail:" + uid.error().to_string();
    return;
  }
  auto under = u.get_underlying(*uid);
  if (!under.has_value()) {
    agg.status = "underlying_fail";
    return;
  }
  const Underlying* U = *under;

  // A read-only Fast session supplies the per-expiry term forward / carry the
  // fit + scoring price on (S*e^{(r-q_eff)T} == forward). We consume it purely
  // for `expiries()` (SliceContext.forward); the eSSVI seed for CStar comes from
  // our own `essvi_fit_slice`, not the session's fitted curve.
  const auto in = make_session_inputs(FitPreset::Fast, panel->implied_spot, r,
                                      panel->frame.snapshot_ts_ns);
  const auto sess = VolaSession::from_frame(panel->frame, in);
  if (!sess.has_value()) {
    agg.status = "session_fail:" + sess.error().to_string();
    return;
  }
  const auto ctx = sess->expiries();

  for (const auto& c : ctx) {
    const double T = c.T;
    if (!(T >= t_min)) {
      continue;
    }
    const double F = c.forward;
    if (!(F > 0.0)) {
      continue;
    }
    const double df = std::exp(-r * T);

    // Locate the per-expiry Chain by T (SliceContext and chains are both
    // ascending-T, but match on T to stay robust to any dropped slice).
    const Chain* chain = nullptr;
    for (const Chain& ch : U->chains) {
      if (std::fabs(ch.T - T) < 1.0e-9) {
        chain = &ch;
        break;
      }
    }
    if (chain == nullptr) {
      continue;
    }

    auto obs_res = build_observations(*chain, F, T, df, opts);
    if (!obs_res.has_value() || obs_res.value().obs.size() < 5) {
      continue;
    }
    const std::span<const FitObs> obs{obs_res.value().obs};
    ++agg.n_expiries;

    const SliceResult sr =
        fit_and_score_slice(*chain, obs, F, T, df, 0.0, opts, time_it);
    if (!sr.essvi_ok) {
      continue;
    }
    ++agg.n_essvi_ok;
    if (!sr.cstar_ok) {
      if (verbose) {
        std::printf("    %-8s T=%.4f n=%d  CStar-gap: %s\n", spec.label.c_str(), T,
                    sr.essvi.n_scored, sr.cstar_err.c_str());
      }
      continue;  // coverage gap; excluded from the paired aggregate
    }
    ++agg.n_cstar_ok;

    // Fold the paired (identical-obs) slice.
    const int n = sr.cstar.n_scored;  // == sr.essvi.n_scored (same obs)
    agg.paired_n += n;
    agg.essvi_sse += sr.essvi.vol_rmse * sr.essvi.vol_rmse * n;
    agg.cstar_sse += sr.cstar.vol_rmse * sr.cstar.vol_rmse * n;
    agg.essvi_chi2_sum += sr.essvi.chi2 * n;
    agg.cstar_chi2_sum += sr.cstar.chi2 * n;
    agg.essvi_in_band += static_cast<long>(sr.essvi.in_band_frac * n + 0.5);
    agg.cstar_in_band += static_cast<long>(sr.cstar.in_band_frac * n + 0.5);
    if (sr.essvi.arb_flags > 0) {
      agg.essvi_arb += sr.essvi.arb_flags;
    }
    if (sr.cstar.arb_flags > 0) {
      agg.cstar_arb += sr.cstar.arb_flags;
    }
    if (std::isfinite(sr.cstar_min_g) && sr.cstar_min_g < 0.0) {
      ++agg.cstar_min_g_neg;
    }
    agg.essvi_ms += sr.essvi.fit_ms;
    agg.cstar_ms += sr.cstar.fit_ms;
    ++agg.n_paired_slices;
    if (sr.cstar.vol_rmse < 0.80 * sr.essvi.vol_rmse) {
      ++agg.n_modal_win;
    }

    if (verbose) {
      std::printf(
          "    %-8s T=%.4f n=%d | eSSVI rmse=%.4f band=%4.1f%% chi2=%.2f | "
          "CStar rmse=%.4f band=%4.1f%% chi2=%.2f ming=%+.3f\n",
          spec.label.c_str(), T, n, sr.essvi.vol_rmse, 100.0 * sr.essvi.in_band_frac,
          sr.essvi.chi2, sr.cstar.vol_rmse, 100.0 * sr.cstar.in_band_frac,
          sr.cstar.chi2, sr.cstar_min_g);
    }
  }
}

void print_board_row(const BoardAgg& a) {
  if (a.status != "ok") {
    std::printf("%-14s %-14s | %s\n", a.label.c_str(), a.regime.c_str(),
                a.status.c_str());
    return;
  }
  std::printf(
      "%-14s %-14s | exp %2d essvi %2d cstar %2d | eSSVI rmse=%.4f band=%4.1f%% chi2=%5.2f "
      "arb=%d | CStar rmse=%.4f band=%4.1f%% chi2=%5.2f arb=%d gneg=%d | modal %d/%d\n",
      a.label.c_str(), a.regime.c_str(), a.n_expiries, a.n_essvi_ok, a.n_cstar_ok,
      a.essvi_rmse(), 100.0 * a.essvi_band(), a.essvi_chi2(), a.essvi_arb,
      a.cstar_rmse(), 100.0 * a.cstar_band(), a.cstar_chi2(), a.cstar_arb,
      a.cstar_min_g_neg, a.n_modal_win, a.n_paired_slices);
}

int run_real(const std::string& root, const std::vector<std::string>& cohort_override,
             int spy_limit, int universe_limit, bool time_it, bool verbose) {
  const double r = 0.043;
  const double t_min = 0.019;  // ~1 week: exclude the 0DTE/weekly regime
  const CalibOpts opts{};

  std::vector<RealBoardSpec> boards;

  // SPY dense-index boards. Prefer the real dispersion layout
  // (`<root>/opra/SPY/<date>.parquet`, one board per available snapshot date);
  // fall back to the fixed intraday `spy_fit_slices/` names when that directory
  // is absent so the tool still works against the originally-specced layout.
  std::vector<RealBoardSpec> spy_boards =
      discover_opra_boards(root, "SPY", "dense-index");
  if (spy_boards.empty()) {
    for (const SpySlice& s : kSpySlices) {
      const std::string path = find_existing({
          root + "/spy_fit_slices/" + s.filename,
          "data/spy_fit_slices/" + std::string(s.filename),
      });
      RealBoardSpec spec;
      spec.label = std::string("SPY/") + s.regime;
      spec.symbol = "SPY";
      spec.path = path;  // empty => reported data_missing in the run loop below
      spec.snapshot_iso = s.snapshot_iso;
      spec.regime = s.regime;
      spy_boards.push_back(std::move(spec));
    }
  }
  const int n_spy = (spy_limit <= 0)
                        ? static_cast<int>(spy_boards.size())
                        : std::min<int>(spy_limit, static_cast<int>(spy_boards.size()));
  for (int i = 0; i < n_spy; ++i) {
    boards.push_back(spy_boards[static_cast<std::size_t>(i)]);
  }

  // Single-name cohort. Symbols come from --symbols when supplied (the on-disk
  // OPRA set differs from the hardcoded 25-name recovery cohort), else the
  // default kCohort. For each symbol prefer the real dispersion layout
  // (`<root>/opra/<SYM>/<date>.parquet`, one board per date); fall back to the
  // fixed `opra_universe/<SYM>/2026-07-01.parquet` name.
  const std::vector<std::string> cohort =
      cohort_override.empty()
          ? std::vector<std::string>(kCohort.begin(), kCohort.end())
          : cohort_override;
  const int n_cohort =
      (universe_limit <= 0)
          ? static_cast<int>(cohort.size())
          : std::min<int>(universe_limit, static_cast<int>(cohort.size()));
  for (int i = 0; i < n_cohort; ++i) {
    const std::string& sym = cohort[static_cast<std::size_t>(i)];
    std::vector<RealBoardSpec> sym_boards =
        discover_opra_boards(root, sym, "single-name");
    if (sym_boards.empty()) {
      const std::string path = find_existing({
          root + "/opra_universe/" + sym + "/2026-07-01.parquet",
          "data/opra_universe/" + sym + "/2026-07-01.parquet",
      });
      RealBoardSpec spec;
      spec.label = sym;
      spec.symbol = sym;
      spec.path = path;
      spec.snapshot_iso = "2026-07-01T14:00:00Z";
      spec.regime = "cohort-smallcap";
      sym_boards.push_back(std::move(spec));
    }
    for (RealBoardSpec& b : sym_boards) {
      boards.push_back(std::move(b));
    }
  }

  std::printf("CStar vs eSSVI — REAL OPRA panel  (root=%s, r=%.3f, T>=%.3f)\n",
              root.c_str(), r, t_min);
  std::printf("%s\n", std::string(132, '-').c_str());

  std::vector<BoardAgg> aggs;
  aggs.reserve(boards.size());
  int n_missing = 0;
  for (const RealBoardSpec& spec : boards) {
    BoardAgg agg;
    if (spec.path.empty()) {
      agg.label = spec.label;
      agg.symbol = spec.symbol;
      agg.regime = spec.regime;
      agg.status = "data_missing";
      ++n_missing;
    } else {
      run_real_board(spec, r, t_min, opts, time_it, verbose, agg);
    }
    print_board_row(agg);
    aggs.push_back(std::move(agg));
  }

  // ── Grand aggregates, split SPY (dense index) vs cohort (small-cap) ─────────
  const auto summarize = [&](const char* tag, bool want_spy) {
    BoardAgg tot;
    int boards_ok = 0;
    for (const BoardAgg& a : aggs) {
      const bool is_spy = (a.symbol == "SPY");
      if (is_spy != want_spy || a.status != "ok") {
        continue;
      }
      ++boards_ok;
      tot.n_expiries += a.n_expiries;
      tot.n_essvi_ok += a.n_essvi_ok;
      tot.n_cstar_ok += a.n_cstar_ok;
      tot.paired_n += a.paired_n;
      tot.essvi_sse += a.essvi_sse;
      tot.cstar_sse += a.cstar_sse;
      tot.essvi_chi2_sum += a.essvi_chi2_sum;
      tot.cstar_chi2_sum += a.cstar_chi2_sum;
      tot.essvi_in_band += a.essvi_in_band;
      tot.cstar_in_band += a.cstar_in_band;
      tot.essvi_arb += a.essvi_arb;
      tot.cstar_arb += a.cstar_arb;
      tot.cstar_min_g_neg += a.cstar_min_g_neg;
      tot.essvi_ms += a.essvi_ms;
      tot.cstar_ms += a.cstar_ms;
      tot.n_modal_win += a.n_modal_win;
      tot.n_paired_slices += a.n_paired_slices;
    }
    if (boards_ok == 0) {
      return;
    }
    const int cover_gap = tot.n_essvi_ok - tot.n_cstar_ok;
    const double cover_pct =
        tot.n_essvi_ok > 0
            ? 100.0 * static_cast<double>(cover_gap) / static_cast<double>(tot.n_essvi_ok)
            : 0.0;
    std::printf("%s\n", std::string(132, '-').c_str());
    std::printf(
        "[%s] boards=%d expiries=%d essvi_ok=%d cstar_ok=%d (coverage gap %d = %.1f%%)\n",
        tag, boards_ok, tot.n_expiries, tot.n_essvi_ok, tot.n_cstar_ok, cover_gap,
        cover_pct);
    std::printf(
        "[%s] paired obs=%ld | eSSVI rmse=%.4f band=%.1f%% chi2=%.3f arb=%d | "
        "CStar rmse=%.4f band=%.1f%% chi2=%.3f arb=%d gneg=%d\n",
        tag, tot.paired_n, tot.essvi_rmse(), 100.0 * tot.essvi_band(), tot.essvi_chi2(),
        tot.essvi_arb, tot.cstar_rmse(), 100.0 * tot.cstar_band(), tot.cstar_chi2(),
        tot.cstar_arb, tot.cstar_min_g_neg);
    std::printf("[%s] modal-win slices %d/%d (CStar vol-RMSE < 0.8x eSSVI)", tag,
                tot.n_modal_win, tot.n_paired_slices);
    if (time_it && tot.n_paired_slices > 0) {
      std::printf(" | mean fit wall  eSSVI=%.3fms  CStar=%.3fms (%.0fx)",
                  tot.essvi_ms / tot.n_paired_slices, tot.cstar_ms / tot.n_paired_slices,
                  tot.essvi_ms > 0.0 ? tot.cstar_ms / tot.essvi_ms : 0.0);
    }
    std::printf("\n");
  };

  summarize("SPY", true);
  summarize("COHORT", false);

  if (n_missing > 0) {
    std::printf("%s\n", std::string(132, '-').c_str());
    std::printf("(%d board(s) had no parquet on disk — reported as data_missing)\n",
                n_missing);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool real = false;
  bool verbose = false;
  bool time_it = true;
  std::string root = "C:/atx/data";
  int spy_limit = 3;       // representative SPY regime spread by default
  int universe_limit = 0;  // 0 = all cohort names
  std::vector<std::string> cohort_override;  // --symbols; empty => default cohort

  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    if (a == "--real") {
      real = true;
    } else if (a == "--synthetic") {
      real = false;
    } else if (a == "--verbose") {
      verbose = true;
    } else if (a == "--no-timing") {
      time_it = false;
    } else if (a == "--data-root" && i + 1 < argc) {
      root = argv[++i];
    } else if (a == "--symbols" && i + 1 < argc) {
      cohort_override = split_csv(argv[++i]);
    } else if (a == "--spy-limit" && i + 1 < argc) {
      spy_limit = std::atoi(argv[++i]);
    } else if (a == "--universe-limit" && i + 1 < argc) {
      universe_limit = std::atoi(argv[++i]);
    } else if (a == "--help" || a == "-h") {
      std::printf(
          "usage: cstar_panel [--synthetic | --real] [--data-root DIR] "
          "[--symbols SYM1,SYM2,...] [--spy-limit N] [--universe-limit N] "
          "[--verbose] [--no-timing]\n");
      return 0;
    }
  }

  if (real) {
    return run_real(root, cohort_override, spy_limit, universe_limit, time_it, verbose);
  }
  return run_synthetic();
}
