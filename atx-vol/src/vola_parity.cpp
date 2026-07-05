#include "atx/vol/vola_parity.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"     // AmericanMethod, AlOpts
#include "atx/vol/black76.hpp"       // black76_value_and_vega
#include "atx/vol/calib.hpp"        // FitObs, FitDiag, CalibOpts
#include "atx/vol/deamer.hpp"       // de_americanize_chain, european_equiv_iv, otm_side
#include "atx/vol/essvi_calib.hpp"  // essvi_fit_slice
#include "atx/vol/fit_metrics.hpp"  // slice_fit_metrics
#include "atx/vol/parity.hpp"       // chain_parity, ParityInputs
#include "atx/vol/s3.hpp"           // s3_seed_from_ivs, s3_iv
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"     // Chain, chain_index
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_total_w

// PORT / PARITY NOTES
// -------------------
// * Model-IV evaluation of the fitted eSSVI slice. The fitted slice is scored to
//   a model IV with the FREE total-variance evaluator `essvi_total_w(slice, k)`
//   from vol_surface.hpp:  iv_model = sqrt(max(essvi_total_w(slice, k), 1e-12) / T).
//   This avoids constructing a whole VolSurface just to read a single slice back;
//   `essvi_total_w` is the same backbone(+residual) evaluator VolSurface calls
//   internally, so the number is identical to `VolSurface::iv_on_slice`.
//
// * Strike-index alignment. `de_americanize_chain` returns a COMPACTED strip
//   (dropped strikes removed) and does NOT report the source strike index, so we
//   re-derive the whole aligned observation set here with a single self-contained
//   loop over the chain's strikes, applying the SAME OTM-leg / drop rules the
//   de-Am driver uses (K > 0; leg bid > 0, ask > 0, ask >= bid, mid finite > 0;
//   invertible). Because we invert with the same `european_equiv_iv` at the same
//   q_eff (derived from the de-Am forward), the recovered market IVs are
//   bit-identical to the de-Am strip, while strike / bid / ask / mid / k / iv all
//   stay in lock-step — exactly what chain_parity's parallel spans require.
//
// * The q_eff bridge. We take the term forward F and borrow from
//   de_americanize_chain and set q_eff = r - ln(F / S) / T, so S*e^{(r-q_eff)T}
//   == F exactly. chain_parity re-derives the identical forward internally
//   (fwd = S*e^{(r-q_eff)T}), so de-Am inversion and re-Am scoring price on one
//   coherent forward.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Minimum strikes that must survive to attempt a fit. Mirrors the C
// build_observations "< 5 rows => NotFound" floor and keeps the 3-parameter
// SSVI backbone comfortably over-determined (N > dof = 3 for the reduced
// chi-square denominators downstream).
constexpr std::size_t kMinUsableObs = 5;

// Floor on the bid/ask spread in the w-space weight so a locked (bid == ask)
// but otherwise valid quote cannot divide by zero (matches deamer's floor).
constexpr double kMinSpread = 1.0e-8;

// Total-variance positivity net before taking sqrt, matching the eSSVI hot path.
constexpr double kMinTotalVar = 1.0e-12;

// True iff the chosen leg's quote is invertible: strictly positive, non-crossed
// bid/ask and a finite positive mid. `idx` is chain_index(strike_idx, side).
// Identical predicate to de_americanize_chain's leg_quote_valid.
[[nodiscard]] bool leg_quote_valid(const Chain& chain, std::size_t idx) noexcept {
  const double bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  const double mid = chain.mids[idx];
  return (bid > 0.0) && (ask > 0.0) && (ask >= bid) && std::isfinite(mid) &&
         (mid > 0.0);
}

// The aligned, self-contained observation set rebuilt from the chain on the
// de-Am forward. Every vector is the same length (n_used); `obs` feeds the
// curve fitter, the rest feed chain_parity / slice_fit_metrics.
struct AlignedObs {
  std::vector<FitObs> obs;
  std::vector<double> strike;
  std::vector<double> bid;
  std::vector<double> ask;
  std::vector<double> mid;
  std::vector<Side> side;
  std::vector<double> k_log;
  std::vector<double> market_iv;
  std::vector<double> vega;
  std::size_t n_dropped{0};
};

// Rebuild the aligned observation set on forward `F` / carry `q_eff`. Any strike
// whose OTM leg is unquotable or fails to invert is counted in `n_dropped`.
[[nodiscard]] AlignedObs build_aligned_obs(const Chain& chain, double S, double r,
                                           double F, double q_eff,
                                           const DeAmOptions& deam) {
  const double T = chain.T;
  const double df = std::exp(-r * T);
  const std::size_t n = chain.n_strikes();

  AlignedObs a;
  a.obs.reserve(n);
  a.strike.reserve(n);
  a.bid.reserve(n);
  a.ask.reserve(n);
  a.mid.reserve(n);
  a.side.reserve(n);
  a.k_log.reserve(n);
  a.market_iv.reserve(n);
  a.vega.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    const double K = chain.strikes[i];
    if (!(K > 0.0)) {
      ++a.n_dropped;
      continue;
    }
    const double k = std::log(K / F);
    const Side side = otm_side(k);
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
    if (!leg_quote_valid(chain, idx)) {
      ++a.n_dropped;
      continue;
    }

    const Result<double> iv_res = european_equiv_iv(
        chain.mids[idx], S, K, T, r, q_eff, side, deam.method, deam.al_opts);
    if (!iv_res) {
      ++a.n_dropped;
      continue;
    }
    const double iv = *iv_res;

    const double bid = chain.bids[idx];
    const double ask = chain.asks[idx];
    const double mid = chain.mids[idx];
    const double spread = ask - bid;
    const double vega = black76_value_and_vega(F, K, T, iv, df, side).vega;

    // w-space weight = vega^2 / spread^2 / (2*sigma*T)^2 (calib.hpp FitObs
    // convention): a price half-spread maps to a vol bar through vega, and
    // dw/dsigma = 2*sigma*T maps that to a total-variance bar. Guarded against a
    // vanishing vega / spread; a degenerate result falls back to unit weight so
    // the row still constrains the fit rather than dropping silently.
    const double sp = std::fmax(spread, kMinSpread);
    const double two_sig_t = 2.0 * iv * T;
    double weight_w = 0.0;
    if (vega > 0.0 && std::isfinite(vega) && two_sig_t > 0.0) {
      weight_w = (vega * vega) / (sp * sp * two_sig_t * two_sig_t);
    }
    if (!std::isfinite(weight_w) || !(weight_w > 0.0)) {
      weight_w = 1.0;
    }

    FitObs fo{};
    fo.k = k;
    fo.sigma_mkt = iv;
    fo.w_mkt = iv * iv * T;
    fo.weight_w = weight_w;
    fo.active_weight_w = weight_w;
    fo.K = K;
    fo.F = F;
    fo.df = df;
    fo.mid = mid;
    fo.spread = spread;
    fo.vega = vega;
    fo.noise_sigma = (vega > 0.0) ? (spread / vega) : 0.0;
    fo.side = side;
    a.obs.push_back(fo);

    a.strike.push_back(K);
    a.bid.push_back(bid);
    a.ask.push_back(ask);
    a.mid.push_back(mid);
    a.side.push_back(side);
    a.k_log.push_back(k);
    a.market_iv.push_back(iv);
    a.vega.push_back(vega);
  }
  return a;
}

// Fit the chosen curve and evaluate it to a model IV at each surviving k.
[[nodiscard]] Result<std::vector<double>> fit_model_ivs(const AlignedObs& a,
                                                        double T, double F,
                                                        const ExpiryParityInputs& in) {
  std::vector<double> model_iv;
  model_iv.reserve(a.k_log.size());

  if (in.curve == ParityCurve::Essvi) {
    FitDiag diag{};
    ATX_TRY(const EssviParams slice,
            essvi_fit_slice(a.obs, T, F, in.calib, &diag));
    for (const double k : a.k_log) {
      const double w = essvi_total_w(slice, k);
      model_iv.push_back(std::sqrt(std::fmax(w, kMinTotalVar) / T));
    }
  } else {
    ATX_TRY(const S3Params params, s3_seed_from_ivs(a.k_log, a.market_iv, T));
    for (const double k : a.k_log) {
      model_iv.push_back(s3_iv(k, T, params));
    }
  }
  return Ok(std::move(model_iv));
}

}  // namespace

Result<ExpiryParityReport> run_expiry_parity(const Chain& chain,
                                             const ExpiryParityInputs& in) {
  const double T = chain.T;
  if (!(in.S > 0.0) || !(T > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument,
               "run_expiry_parity: non-finite/non-positive S, r, or chain.T");
  }

  // 1. De-Americanize: implied (or fixed) borrow + the term forward.
  ATX_TRY(const DeAmResult d,
          de_americanize_chain(chain, in.S, in.r, in.cash_divs, in.now_ts_ns,
                               in.deam));
  const double F = d.forward;
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::Internal,
               "run_expiry_parity: non-finite/non-positive forward from de-Am");
  }

  // q_eff bridge: S*e^{(r-q_eff)T} == F exactly (see PORT NOTES).
  const double q_eff = in.r - std::log(F / in.S) / T;

  // 2. Aligned, self-contained observation rebuild on (F, q_eff).
  const AlignedObs a = build_aligned_obs(chain, in.S, in.r, F, q_eff, in.deam);
  const std::size_t n_used = a.obs.size();
  if (n_used < kMinUsableObs) {
    return Err(ErrorCode::NotFound,
               "run_expiry_parity: fewer than the minimum usable strikes to fit");
  }

  // 3. Fit the chosen curve -> model IVs at each surviving k.
  ATX_TRY(std::vector<double> model_iv, fit_model_ivs(a, T, F, in));

  // 4. Re-Americanize the model vols and score parity. n_curve_params = 3 for
  //    both the eSSVI backbone and S3.
  ParityInputs pin{};
  pin.S = in.S;
  pin.r = in.r;
  pin.q_eff = q_eff;
  pin.T = T;
  pin.method = in.deam.method;
  pin.al_opts = in.deam.al_opts;
  pin.band_k = in.band_k;
  pin.n_curve_params = 3;
  ATX_TRY(const ParityReport parity,
          chain_parity(a.strike, a.bid, a.ask, a.mid, a.side, model_iv,
                       a.market_iv, pin));

  // 5. Fit-quality metrics of the model vs the de-Am market IVs, with error bars
  //    derived from each quote's bid/ask spread and Black-76 vega (dof = 3).
  ATX_TRY(const SliceFitMetrics fm,
          slice_fit_metrics(model_iv, a.market_iv, a.bid, a.ask, a.vega,
                            /*dof=*/3));

  ExpiryParityReport out{};
  out.parity = parity;
  out.implied_borrow = d.borrow;
  out.forward = F;
  out.fit_rmse_vol = fm.rmse_vol;
  out.fit_chi2_reduced = fm.chi2_reduced;
  out.n_used = n_used;
  out.n_dropped = a.n_dropped;
  return Ok(std::move(out));
}

}  // namespace atx::vol
