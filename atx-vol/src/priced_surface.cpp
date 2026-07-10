#include "atx/vol/priced_surface.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool valid_query(double K, double T) noexcept {
  return std::isfinite(K) && (K > 0.0) && std::isfinite(T) && (T > 0.0);
}

}  // namespace

PricedSurface::PricedSurface(CurveSurface &&surface, std::vector<SliceContext> &&ctx,
                             const PricingContext &pricing) noexcept
    : surface_{std::move(surface)}, ctx_{std::move(ctx)}, pricing_{pricing} {
  for (const std::unique_ptr<IVolCurve> &slice : surface_.slices()) {
    if (slice->df() != std::exp(-pricing_.r * slice->T())) {
      term_rates_ = true;
      break;
    }
  }
}

Result<PricedSurface> PricedSurface::create(CurveSurface&& surface,
                                            std::vector<SliceContext> context,
                                            const PricingContext& pricing) {
  if (surface.empty()) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::create: empty surface");
  }
  if (context.size() != surface.n_slices()) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::create: context length != slice count");
  }
  if (!(pricing.S > 0.0) || !std::isfinite(pricing.r)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::create: non-positive spot or non-finite rate");
  }
  for (std::size_t i = 1; i < context.size(); ++i) {
    if (!(context[i].T > context[i - 1].T)) {
      return Err(ErrorCode::InvalidArgument,
                 "PricedSurface::create: slice T's not strictly ascending");
    }
  }
  return PricedSurface{std::move(surface), std::move(context), pricing};
}

PricedSurface::ForwardCarry PricedSurface::interp_forward(double T) const noexcept {
  // Precondition: ctx_ non-empty and ascending in T (create guarantees it). This
  // is byte-identical to VolaSession::interp_forward so the served theo matches.
  const SliceContext& first = ctx_.front();
  const SliceContext& last = ctx_.back();
  const auto slice_rate = [this](std::size_t index) noexcept {
    if (!term_rates_) {
      return pricing_.r;
    }
    const IVolCurve &slice = *surface_.slices()[index];
    return slice.T() > 0.0 && slice.df() > 0.0 && std::isfinite(slice.df())
               ? -std::log(slice.df()) / slice.T()
               : pricing_.r;
  };
  if (T <= first.T) {
    return ForwardCarry{first.forward, first.q_eff, slice_rate(0u)};
  }
  if (T >= last.T) {
    return ForwardCarry{last.forward, last.q_eff, slice_rate(ctx_.size() - 1u)};
  }
  std::size_t hi = 0;
  while (hi < ctx_.size() && ctx_[hi].T <= T) {
    ++hi;
  }
  const std::size_t lo = hi - 1;
  const SliceContext& a = ctx_[lo];
  const SliceContext& b = ctx_[hi];
  const double span = b.T - a.T;
  const double alpha = (span > 0.0) ? (T - a.T) / span : 0.0;
  const double rate_lo = slice_rate(lo);
  const double rate_hi = slice_rate(hi);
  return ForwardCarry{a.forward + alpha * (b.forward - a.forward),
                      a.q_eff + alpha * (b.q_eff - a.q_eff), rate_lo + alpha * (rate_hi - rate_lo)};
}

double PricedSurface::forward_at(double T) const noexcept {
  if (!(T > 0.0) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).forward;
}

double PricedSurface::q_eff_at(double T) const noexcept {
  if (!(T > 0.0) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).q_eff;
}

double PricedSurface::rate_at(double T) const noexcept {
  if (!(T > 0.0) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).rate;
}

double PricedSurface::iv(double K, double T) const noexcept {
  if (!valid_query(K, T)) {
    return kNaN;
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  return surface_.iv(k, T);
}

double PricedSurface::total_variance(double K, double T) const noexcept {
  if (!valid_query(K, T)) {
    return kNaN;
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  return surface_.w(k, T);
}

Result<double> PricedSurface::fair_value(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::fair_value: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = surface_.iv(k, T);
  // Cold Andersen-Lake — the override-path re-pricing the session uses. Passing the
  // resolved preset as an engaged optional reproduces the session's own call
  // `american_price(..., in_.deam.al_opts)` exactly (in_ carries the resolved AL
  // opts post-build).
  return american_price(pricing_.S, K, T, sigma, fc.rate, fc.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

Result<AmericanGreeks> PricedSurface::greeks(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::greeks: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = surface_.iv(k, T);
  // American Greeks via finite differences on the SAME cold american_price (method
  // + resolved AL preset) fair_value() prices with, so greeks().price == fair_value()
  // bit-identical and the coefficients are American (not the European Black-76 leg).
  // Cold (warm_start=false) keeps greeks bit-reproducible across a surface archive
  // round-trip (the LifecycleIntegration contract); the warm hot path lives in the
  // backtest engine (cross-step reuse), not this bit-stable reprice primitive.
  return american_greeks_fd(pricing_.S, K, T, sigma, fc.rate, fc.q_eff, side, pricing_.method,
                            std::optional<AlOpts>{pricing_.al_opts});
}

Result<AmericanGreeks> PricedSurface::greeks_analytic(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::greeks_analytic: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = surface_.iv(k, T);
  // Analytic Andersen-Lake greeks (5 solves; theta/charm via the continuation PDE).
  // Same base boundary as fair_value(), so greeks_analytic().price == fair_value().
  // BAW / degenerate corners fall back to the cold FD path inside american_greeks_al.
  if (pricing_.method == AmericanMethod::AndersenLake) {
    return american_greeks_al(pricing_.S, K, T, sigma, fc.rate, fc.q_eff, side,
                              std::optional<AlOpts>{pricing_.al_opts});
  }
  return american_greeks_fd(pricing_.S, K, T, sigma, fc.rate, fc.q_eff, side, pricing_.method,
                            std::optional<AlOpts>{pricing_.al_opts});
}

Result<double> PricedSurface::delta(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::delta: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = surface_.iv(k, T);
  // Delta-only fast path — same (S, sigma, r, q_eff, method, al_opts) plumbing as
  // greeks(), so this returns greeks().delta bit-identically at ~1-2 boundary solves
  // instead of seventeen (see american_delta).
  return american_delta(pricing_.S, K, T, sigma, fc.rate, fc.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

VolCurveKind PricedSurface::kind_at(std::size_t i) const noexcept {
  return surface_.slices()[i]->kind();
}

}  // namespace atx::vol
