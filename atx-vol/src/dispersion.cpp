// DispersionBook implementation — implied-correlation signal and vega-neutral
// straddle sizing. See dispersion.hpp for the model.

#include "atx/vol/dispersion.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"        // AmericanGreeks
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Result, Side
#include "atx/vol/vol_curve.hpp"       // CurveSurface

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// The ATM-forward straddle of one member at tenor T: strike K = forward_at(T),
// ATM vol sigma = iv(K, T), and per-share straddle vega = call vega + put vega.
// Resolves the member's surface by uid and validates every quantity, naming the
// symbol on any failure (a member with no fittable ATM straddle sinks the book —
// the signal needs every leg).
[[nodiscard]] Result<DispersionLeg> resolve_leg(const SurfaceSet& surfaces,
                                                const DispersionMember& m, double T) {
  const PricedSurface* surf = surfaces.find(m.uid);
  if (surf == nullptr) {
    return Err(ErrorCode::NotFound,
               "dispersion: no surface registered for symbol '" + m.symbol + "'");
  }
  const double K = surf->forward_at(T);
  if (!(K > 0.0)) {
    return Err(ErrorCode::Unavailable,
               "dispersion: no ATM forward for symbol '" + m.symbol + "' at the tenor");
  }
  const double sigma = surf->iv(K, T);
  if (!std::isfinite(sigma) || sigma <= 0.0) {
    return Err(ErrorCode::Unavailable,
               "dispersion: ATM vol unavailable for symbol '" + m.symbol +
                   "' (tenor outside surface domain)");
  }

  const Result<AmericanGreeks> call = surf->greeks(K, T, Side::Call);
  if (!call) {
    return Err(call.error());
  }
  const Result<AmericanGreeks> put = surf->greeks(K, T, Side::Put);
  if (!put) {
    return Err(put.error());
  }
  const double straddle_vega = call->vega + put->vega;
  if (!std::isfinite(straddle_vega) || straddle_vega <= 0.0) {
    return Err(ErrorCode::Unavailable,
               "dispersion: degenerate ATM straddle vega for symbol '" + m.symbol + "'");
  }

  DispersionLeg leg;
  leg.symbol = m.symbol;
  leg.uid = m.uid;
  leg.K = K;
  leg.T = T;
  leg.sigma = sigma;
  leg.straddle_vega = straddle_vega;
  leg.straddle_qty = 0.0;  // sized by the caller
  return Ok(std::move(leg));
}

// Emit the two positions (Call then Put, same K/T/qty) of one sized straddle,
// appending to `out` with monotonically increasing ids from `next_id`.
void emit_straddle(const DispersionLeg& leg, double multiplier, std::uint64_t& next_id,
                   std::vector<Position>& out) {
  for (const Side side : {Side::Call, Side::Put}) {
    Position p;
    p.id = next_id++;
    p.contract = OptionContract{leg.uid, leg.K, leg.T, side};
    p.qty = leg.straddle_qty;
    p.multiplier = multiplier;
    out.push_back(p);
  }
}

}  // namespace

Result<DispersionSignal> dispersion_signal(const DispersionUniverse& universe,
                                           const SurfaceSet& surfaces, double T) {
  if (universe.names.size() < 2) {
    return Err(ErrorCode::InvalidArgument, "dispersion: need at least two basket names");
  }
  if (!std::isfinite(T) || T <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: tenor T must be finite and positive");
  }

  double sum_w = 0.0;
  for (const DispersionMember& n : universe.names) {
    if (!std::isfinite(n.weight)) {
      return Err(ErrorCode::InvalidArgument,
                 "dispersion: non-finite weight for symbol '" + n.symbol + "'");
    }
    sum_w += n.weight;
  }
  if (!(sum_w > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "dispersion: basket weights must sum to a positive value");
  }

  const Result<DispersionLeg> idx = resolve_leg(surfaces, universe.index, T);
  if (!idx) {
    return Err(idx.error());
  }

  DispersionSignal sig;
  sig.T_used = T;
  sig.sigma_index = idx->sigma;
  sig.sigma_names.reserve(universe.names.size());

  // Weights are normalized internally (w_hat_i = w_i / Σw) so rho_imp is
  // scale-invariant and correct for ANY positive-weight vector (float drift, a
  // partial basket): the index implied-correlation identity assumes a portfolio
  // with Σw = 1.
  double sum_w_sigma = 0.0;
  double sum_w2_sigma2 = 0.0;
  for (const DispersionMember& n : universe.names) {
    const Result<DispersionLeg> leg = resolve_leg(surfaces, n, T);
    if (!leg) {
      return Err(leg.error());
    }
    const double sigma = leg->sigma;
    const double w_hat = n.weight / sum_w;
    sig.sigma_names.push_back(sigma);
    sum_w_sigma += w_hat * sigma;
    sum_w2_sigma2 += w_hat * w_hat * sigma * sigma;
  }

  // Denominator == Σ_{i≠j} w_i w_j sigma_i sigma_j; positive whenever >= 2 names
  // carry positive weight+vol. Guarded per the brief.
  const double denom = sum_w_sigma * sum_w_sigma - sum_w2_sigma2;
  if (!(denom > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "dispersion: degenerate correlation denominator");
  }

  sig.sum_w_sigma = sum_w_sigma;
  sig.sum_w2_sigma2 = sum_w2_sigma2;
  sig.implied_corr = (sig.sigma_index * sig.sigma_index - sum_w2_sigma2) / denom;
  return Ok(std::move(sig));
}

Result<DispersionBook> build_dispersion_book(const DispersionUniverse& universe,
                                             const SurfaceSet& surfaces,
                                             const DispersionConfig& cfg) {
  if (!std::isfinite(cfg.target_T) || cfg.target_T <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: target_T must be finite and positive");
  }
  if (!std::isfinite(cfg.target_vega) || cfg.target_vega <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: target_vega must be finite and positive");
  }
  if (!std::isfinite(cfg.multiplier) || cfg.multiplier <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "dispersion: multiplier must be finite and positive");
  }

  Result<DispersionSignal> sig = dispersion_signal(universe, surfaces, cfg.target_T);
  if (!sig) {
    return Err(sig.error());
  }

  // Normalize the basket weights (dispersion_signal already validated Σw > 0), so
  // the weighted basket vega matches the index leg EXACTLY (Σ ŵ_i = 1) for any
  // positive-weight input — the book stays vega-neutral under weight scaling.
  double sum_w = 0.0;
  for (const DispersionMember& n : universe.names) {
    sum_w += n.weight;
  }

  // ShortIndexLongNames: index straddle short (qty < 0), names long (qty > 0).
  const double idx_sign = (cfg.side == DispersionSide::ShortIndexLongNames) ? -1.0 : 1.0;
  const double name_sign = -idx_sign;
  const double mult = cfg.multiplier;

  Result<DispersionLeg> index_leg = resolve_leg(surfaces, universe.index, cfg.target_T);
  if (!index_leg) {
    return Err(index_leg.error());
  }
  index_leg->straddle_qty = idx_sign * cfg.target_vega / (index_leg->straddle_vega * mult);

  DispersionBook book;
  book.signal = std::move(*sig);
  book.index_leg = std::move(*index_leg);
  book.name_legs.reserve(universe.names.size());
  book.positions.reserve(2 * (1 + universe.names.size()));

  std::uint64_t next_id = 0;
  emit_straddle(book.index_leg, mult, next_id, book.positions);

  for (const DispersionMember& n : universe.names) {
    Result<DispersionLeg> leg = resolve_leg(surfaces, n, cfg.target_T);
    if (!leg) {
      return Err(leg.error());
    }
    // Normalized-weighted basket vega matches the index leg:
    // n_i · vega_i · mult = (w_i / Σw) · target_vega.
    const double w_hat = n.weight / sum_w;
    leg->straddle_qty = name_sign * w_hat * cfg.target_vega / (leg->straddle_vega * mult);
    emit_straddle(*leg, mult, next_id, book.positions);
    book.name_legs.push_back(std::move(*leg));
  }

  return Ok(std::move(book));
}

Result<PricedSurface> with_uid(const PricedSurface& src, std::uint32_t uid) {
  CurveSurface curves = src.surface().clone();
  std::vector<SliceContext> ctx(src.context().begin(), src.context().end());
  PricingContext pc = src.pricing();
  pc.uid = uid;
  return PricedSurface::create(std::move(curves), std::move(ctx), pc);
}

}  // namespace atx::vol
