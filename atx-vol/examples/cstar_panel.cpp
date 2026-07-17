// CStar vs eSSVI evidence panel (Sub-Sprint S / W5.1-W5.2 ladder decision).
//
// Fits BOTH the production eSSVI per-slice calibrator (`essvi_fit_slice`) and the
// CStar C16M modal calibrator (`cstar_calibrate_slice`, seeded from that same
// eSSVI fit) to an IDENTICAL observation set per board, and reports the metrics
// the Sprint-I ladder decision needs: fit wall (best-of-3), vol-RMSE, in-band
// fraction, price χ², and butterfly-arb-flag count — CStar against the same
// board's eSSVI result.
//
// DATA CAVEAT (recorded in the ledger + review doc): the 25-name recovery cohort
// and the SPY snapshot Parquet payloads live outside the repo and require the
// Sprint-R per-expiry slice-extraction plumbing (calib.cpp / prepared_fitting,
// owned by another engineer) to turn a real board into the per-expiry `Chain`
// this tool fits. This panel therefore drives the REAL calibrators over
// representative SYNTHETIC regimes (a dense low-vol index slice, a sparse steep
// high-vol small-cap slice like the recovery cohort, and a locally-bumpy smile
// that exceeds an eSSVI 3-parameter backbone) built from a known target IV
// smile. It is a controlled A/B of the two engines, not a real-OPRA panel; the
// real-data run is a Sprint-I task once the slice-extraction seam is available.

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_butterfly_slice
#include "atx/vol/black76.hpp"      // black76_price
#include "atx/vol/calib.hpp"        // CalibOpts, FitObs, ObsSet, build_observations
#include "atx/vol/cstar.hpp"        // CStarParams, cstar_slice_iv, cstar_min_roper_g
#include "atx/vol/cstar_calib.hpp"  // cstar_calibrate_slice
#include "atx/vol/essvi_calib.hpp"  // essvi_fit_slice
#include "atx/vol/universe.hpp"     // Chain, chain_index
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_total_w

namespace {

using namespace atx::vol;

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

}  // namespace

int main() {
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

    // ── eSSVI baseline (also the CStar seed) ──────────────────────────────
    EssviParams essvi{};
    const double essvi_ms = best_of_3([&] {
      auto r = essvi_fit_slice(obs, b.T, b.F, opts);
      if (r.has_value()) {
        essvi = r.value();
      }
    });
    auto essvi_res = essvi_fit_slice(obs, b.T, b.F, opts);
    if (!essvi_res.has_value()) {
      std::printf("%-14s %-16s | eSSVI fit failed: %s\n", b.name.c_str(),
                  b.regime.c_str(), essvi_res.error().to_string().c_str());
      continue;
    }
    essvi = essvi_res.value();

    const SliceScore es = score_model(
        obs, b.F, b.T, b.half_spread,
        [&](double k) { return std::sqrt(essvi_total_w(essvi, k) / b.T); },
        [&](double k) { return essvi_total_w(essvi, k); });

    // ── CStar (seeded from the eSSVI fit) ─────────────────────────────────
    CStarParams cstar{};
    const double cstar_ms = best_of_3([&] {
      auto r = cstar_calibrate_slice(essvi, chain, df, opts);
      if (r.has_value()) {
        cstar = r.value();
      }
    });
    auto cstar_res = cstar_calibrate_slice(essvi, chain, df, opts);
    if (!cstar_res.has_value()) {
      std::printf("%-14s %-16s | CStar fit failed: %s\n", b.name.c_str(),
                  b.regime.c_str(), cstar_res.error().to_string().c_str());
      continue;
    }
    cstar = cstar_res.value();

    SliceScore cs = score_model(
        obs, b.F, b.T, b.half_spread,
        [&](double k) { return cstar_slice_iv(cstar, k); },
        [&](double k) { return cstar_slice_w(cstar, k); });
    // Analytic butterfly signal (S1) as an independent arb check.
    const double cstar_min_g = cstar_min_roper_g(cstar);

    const SliceScore es_t = [&] { SliceScore t = es; t.fit_ms = essvi_ms; return t; }();
    cs.fit_ms = cstar_ms;

    std::printf(
        "%-14s %-16s | %6.3fms %.4f %5.1f%% %6.2f %d | %6.3fms %.4f %5.1f%% "
        "%6.2f %d (ming=%+.3f)\n",
        b.name.c_str(), b.regime.c_str(), es_t.fit_ms, es_t.vol_rmse,
        100.0 * es_t.in_band_frac, es_t.chi2, es_t.arb_flags, cs.fit_ms,
        cs.vol_rmse, 100.0 * cs.in_band_frac, cs.chi2, cs.arb_flags, cstar_min_g);

    sum_essvi_rmse += es_t.vol_rmse;
    sum_cstar_rmse += cs.vol_rmse;
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
