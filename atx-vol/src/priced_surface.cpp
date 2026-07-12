#include "atx/vol/priced_surface.hpp"

#include "atx/vol/american_batch.hpp" // exact resolved price-only batch
#include "atx/vol/counters.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void poison(AmericanGreeks &greeks) noexcept {
  greeks.delta = greeks.gamma = greeks.vega = greeks.theta = greeks.rho = greeks.vanna =
      greeks.volga = greeks.charm = greeks.price = kNaN;
}

[[nodiscard]] bool valid_query(double K, double T) noexcept {
  return std::isfinite(K) && (K > 0.0) && std::isfinite(T) && (T > 0.0);
}

template <class Input, class Output>
[[nodiscard]] bool spans_overlap(std::span<Input> input, std::span<Output> output) noexcept {
  if (input.empty() || output.empty()) {
    return false;
  }
  // SAFETY: converting valid object pointers to uintptr_t is implementation-
  // defined but supported by both repository toolchains. Integer comparison
  // avoids undefined relational comparison between unrelated object pointers.
  const std::uintptr_t input_begin = reinterpret_cast<std::uintptr_t>(input.data());
  const std::uintptr_t output_begin = reinterpret_cast<std::uintptr_t>(output.data());
  if (input_begin <= output_begin) {
    return (output_begin - input_begin) < input.size_bytes();
  }
  return (input_begin - output_begin) < output.size_bytes();
}

} // namespace

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

Result<PricedSurface> PricedSurface::create(CurveSurface &&surface,
                                            std::vector<SliceContext> context,
                                            const PricingContext &pricing) {
  if (surface.empty()) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::create: empty surface");
  }
  if (context.size() != surface.n_slices()) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::create: context length != slice count");
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
  const SliceContext &first = ctx_.front();
  const SliceContext &last = ctx_.back();
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
  // Interior (first.T < T < last.T): `hi` is the first index with ctx_[hi].T > T
  // — exactly where the old linear scan `while (ctx_[hi].T <= T) ++hi` stops. On
  // an exact node hit (T == ctx_[j].T) the `<=` scan steps PAST node j, and
  // upper_bound (first element strictly greater than T) lands on j+1 identically,
  // so lo=j, alpha=0 — the same off-by-one. ctx_ is strictly ascending, so this
  // is bit-for-bit the same (lo, hi) bracket the scan selected.
  const auto it =
      std::upper_bound(ctx_.begin(), ctx_.end(), T,
                       [](double t, const SliceContext &s) noexcept { return t < s.T; });
  const std::size_t hi = static_cast<std::size_t>(it - ctx_.begin());
  const std::size_t lo = hi - 1;
  const SliceContext &a = ctx_[lo];
  const SliceContext &b = ctx_[hi];
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

PricedSurface::ResolvedSurfacePoint PricedSurface::resolve(double K, double T) const noexcept {
  ResolvedSurfacePoint p;
  p.K = K;
  p.T = T;
  if (!valid_query(K, T)) {
    return p; // valid == false; all numeric fields left 0
  }
  return resolve_with_carry(K, T, interp_forward(T));
}

PricedSurface::ResolvedSurfacePoint
PricedSurface::resolve_with_carry(double K, double T, ForwardCarry fc) const noexcept {
  // Precondition: T is a valid query T (finite, > 0) so `fc` == interp_forward(T);
  // only K's validity is re-checked here so a strike ladder can reuse one carry.
  ResolvedSurfacePoint p;
  p.K = K;
  p.T = T;
  if (!(std::isfinite(K) && (K > 0.0))) {
    return p; // valid == false
  }
  p.forward = fc.forward;
  p.q_eff = fc.q_eff;
  p.rate = fc.rate;
  p.k_log = std::log(K / fc.forward);
  p.sigma = surface_.iv(p.k_log, T);
  p.valid = true;
  return p;
}

double PricedSurface::iv(double K, double T) const noexcept {
  const ResolvedSurfacePoint p = resolve(K, T);
  return p.valid ? p.sigma : kNaN;
}

double PricedSurface::total_variance(double K, double T) const noexcept {
  const ResolvedSurfacePoint p = resolve(K, T);
  // Same k_log as iv(); total variance is the curve's w(k, T) (not sigma).
  return p.valid ? surface_.w(p.k_log, T) : kNaN;
}

Result<double> PricedSurface::fair_value(double K, double T, Side side) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::fair_value: non-finite or non-positive K/T");
  }
  // Cold Andersen-Lake — the override-path re-pricing the session uses. Passing the
  // resolved preset as an engaged optional reproduces the session's own call
  // `american_price(..., in_.deam.al_opts)` exactly (in_ carries the resolved AL
  // opts post-build).
  return american_price(pricing_.S, K, T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

Result<AmericanGreeks> PricedSurface::greeks(double K, double T, Side side) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::greeks: non-finite or non-positive K/T");
  }
  // American Greeks via finite differences on the SAME cold american_price (method
  // + resolved AL preset) fair_value() prices with, so greeks().price == fair_value()
  // bit-identical and the coefficients are American (not the European Black-76 leg).
  // Cold (warm_start=false) keeps greeks bit-reproducible across a surface archive
  // round-trip (the LifecycleIntegration contract); the warm hot path lives in the
  // backtest engine (cross-step reuse), not this bit-stable reprice primitive.
  return american_greeks_fd(pricing_.S, K, T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                            std::optional<AlOpts>{pricing_.al_opts});
}

Result<AmericanGreeks> PricedSurface::greeks_analytic(double K, double T, Side side) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::greeks_analytic: non-finite or non-positive K/T");
  }
  // Analytic Andersen-Lake greeks (5 solves; theta/charm via the continuation PDE).
  // Same base boundary as fair_value(), so greeks_analytic().price == fair_value().
  // BAW / degenerate corners fall back to the cold FD path inside american_greeks_al.
  if (pricing_.method == AmericanMethod::AndersenLake) {
    return american_greeks_al(pricing_.S, K, T, p.sigma, p.rate, p.q_eff, side,
                              std::optional<AlOpts>{pricing_.al_opts});
  }
  return american_greeks_fd(pricing_.S, K, T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                            std::optional<AlOpts>{pricing_.al_opts});
}

Result<double> PricedSurface::delta(double K, double T, Side side) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::delta: non-finite or non-positive K/T");
  }
  // Delta-only fast path — same (S, sigma, r, q_eff, method, al_opts) plumbing as
  // greeks(), so this returns greeks().delta bit-identically at ~1-2 boundary solves
  // instead of seventeen (see american_delta).
  return american_delta(pricing_.S, K, T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

Result<double> PricedSurface::vega(double K, double T, Side side) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::vega: non-finite or non-positive K/T");
  }
  // Vega-only fast path. Same routing greeks_analytic() uses: the AndersenLake
  // method takes the native analytic route (american_vega_al, bit-identical to
  // greeks_analytic(K,T,side).vega at ~0-2 boundary solves instead of 5); any
  // other method falls back to the SAME american_greeks_fd call
  // greeks_analytic() itself forwards on that branch, so the extracted .vega is
  // bit-identical there too.
  if (pricing_.method == AmericanMethod::AndersenLake) {
    return american_vega_al(pricing_.S, K, T, p.sigma, p.rate, p.q_eff, side,
                            std::optional<AlOpts>{pricing_.al_opts});
  }
  const Result<AmericanGreeks> g =
      american_greeks_fd(pricing_.S, K, T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                         std::optional<AlOpts>{pricing_.al_opts});
  if (!g) {
    return Err(g.error());
  }
  return Ok(g->vega);
}

PricedSurface::FusedResult PricedSurface::evaluate_resolved(const ResolvedSurfacePoint &p,
                                                            Side side, EvalField fields,
                                                            bool analytic) const {
  FusedResult r;
  if (!p.valid) {
    r.iv = kNaN;
    r.price = kNaN;
    poison(r.greeks);
    r.status =
        Err(ErrorCode::InvalidArgument, "PricedSurface::evaluate: non-finite or non-positive K/T");
    return r;
  }
  // IV is free from the single resolution — always populated when valid.
  r.iv = p.sigma;
  // Poison selective outputs up front so an earlier requested-route failure
  // cannot leave a requested axis looking like a valid zero.
  if (has_field(fields, EvalField::Delta)) {
    r.greeks.delta = kNaN;
  }
  if (has_field(fields, EvalField::Vega)) {
    r.greeks.vega = kNaN;
  }

  const bool want_greeks =
      has_field(fields, EvalField::FirstOrder) || has_field(fields, EvalField::SecondOrder);
  if (want_greeks) {
    // Route exactly as greeks() / greeks_analytic() do; american_greeks_*().price
    // IS the fair value (bit-identical), so Greeks yield the mark for free.
    ATX_VOL_COUNT(SurfaceFullGreekRoutes);
    Result<AmericanGreeks> g =
        (analytic && pricing_.method == AmericanMethod::AndersenLake)
            ? american_greeks_al(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                                 std::optional<AlOpts>{pricing_.al_opts})
            : american_greeks_fd(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                                 pricing_.method, std::optional<AlOpts>{pricing_.al_opts});
    if (!g.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(g.error());
      return r;
    }
    r.greeks = *g;
    r.price = g->price;
    return r;
  }
  if (has_field(fields, EvalField::Price)) {
    ATX_VOL_COUNT(SurfaceScalarPriceRoutes);
    Result<double> fv = american_price(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                                       pricing_.method, std::optional<AlOpts>{pricing_.al_opts});
    if (!fv.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(fv.error());
      return r;
    }
    r.price = *fv;
  }
  if (has_field(fields, EvalField::Delta)) {
    ATX_VOL_COUNT(SurfaceDeltaRoutes);
    const Result<double> delta =
        american_delta(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                       std::optional<AlOpts>{pricing_.al_opts});
    if (!delta.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(delta.error());
      return r;
    }
    r.greeks.delta = *delta;
  }
  if (has_field(fields, EvalField::Vega)) {
    ATX_VOL_COUNT(SurfaceVegaRoutes);
    Result<double> vega_result;
    if (pricing_.method == AmericanMethod::AndersenLake) {
      vega_result = american_vega_al(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                                     std::optional<AlOpts>{pricing_.al_opts});
    } else {
      const Result<AmericanGreeks> greeks =
          american_greeks_fd(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                             std::optional<AlOpts>{pricing_.al_opts});
      if (!greeks.has_value()) {
        vega_result = Err(greeks.error());
      } else {
        vega_result = Ok(greeks->vega);
      }
    }
    if (!vega_result.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(vega_result.error());
      return r;
    }
    r.greeks.vega = *vega_result;
  }
  // Iv-only (or None): one resolution, no pricer solve.
  return r;
}

PricedSurface::FusedResult PricedSurface::evaluate(double K, double T, Side side, EvalField fields,
                                                   bool analytic) const {
  return evaluate_resolved(resolve(K, T), side, fields, analytic);
}

Status PricedSurface::evaluate_batch(std::span<const double> K, std::span<const double> T,
                                     std::span<const Side> side, EvalField fields, bool analytic,
                                     EvaluationSoA out,
                                     simd::SimdIsa resolved_price_isa) const {
  const std::size_t n = K.size();
  if (T.size() != n || side.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: K/T/side length mismatch");
  }
  const bool want_greeks =
      has_field(fields, EvalField::FirstOrder) || has_field(fields, EvalField::SecondOrder);
  const bool want_price = has_field(fields, EvalField::Price);
  const bool want_iv = has_field(fields, EvalField::Iv);
  const bool want_delta = has_field(fields, EvalField::Delta);
  const bool want_vega = has_field(fields, EvalField::Vega);
  const bool selective_only = !want_greeks && (want_delta || want_vega);
  const auto optional_size_ok = [n](const auto output) noexcept {
    return output.empty() || output.size() == n;
  };
  if (!selective_only && (out.iv.size() != n || out.price.size() != n || out.status.size() != n)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: iv/price/status out-span size != query count");
  }
  if (!optional_size_ok(out.delta) || !optional_size_ok(out.vega) ||
      (want_delta && out.delta.size() != n) || (want_vega && out.vega.size() != n)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: dedicated axis out-span size invalid");
  }
  if (selective_only &&
      (out.status.size() != n || !optional_size_ok(out.iv) || !optional_size_ok(out.price) ||
       !optional_size_ok(out.greeks) || (want_iv && out.iv.size() != n) ||
       (want_price && out.price.size() != n))) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: selective out-span size invalid");
  }
  if (want_greeks && out.greeks.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: greeks out-span size != query count");
  }
  const auto overlaps_input = [&](const auto output) noexcept {
    return spans_overlap(K, output) || spans_overlap(T, output) || spans_overlap(side, output);
  };
  const bool outputs_overlap =
      spans_overlap(out.iv, out.price) || spans_overlap(out.iv, out.greeks) ||
      spans_overlap(out.iv, out.delta) || spans_overlap(out.iv, out.vega) ||
      spans_overlap(out.iv, out.status) || spans_overlap(out.price, out.greeks) ||
      spans_overlap(out.price, out.delta) || spans_overlap(out.price, out.vega) ||
      spans_overlap(out.price, out.status) || spans_overlap(out.greeks, out.delta) ||
      spans_overlap(out.greeks, out.vega) || spans_overlap(out.greeks, out.status) ||
      spans_overlap(out.delta, out.vega) || spans_overlap(out.delta, out.status) ||
      spans_overlap(out.vega, out.status);
  if (overlaps_input(out.iv) || overlaps_input(out.price) || overlaps_input(out.greeks) ||
      overlaps_input(out.status) || overlaps_input(out.delta) || overlaps_input(out.vega) ||
      outputs_overlap) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: query/output spans overlap");
  }

  std::size_t i = 0;
  while (i < n) {
    const double t = T[i];
    // A run [i, j) of BIT-identical T (raw double compare, no tolerance) shares
    // one T-bracket + carry — the ladder-reuse win.
    std::size_t j = i + 1;
    while (j < n && std::bit_cast<std::uint64_t>(T[j]) == std::bit_cast<std::uint64_t>(t)) {
      ++j;
    }
    const bool t_valid = std::isfinite(t) && (t > 0.0);
    const ForwardCarry fc = t_valid ? interp_forward(t) : ForwardCarry{};
    if (want_price && !want_greeks && !selective_only) {
      if (!t_valid) {
        for (std::size_t e = i; e < j; ++e) {
          ResolvedSurfacePoint p;
          p.K = K[e];
          p.T = t;
          const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic);
          out.iv[e] = fr.iv;
          out.price[e] = fr.price;
          out.status[e] = fr.status;
        }
        i = j;
        continue;
      }

      const auto dispatch_valid = [&](std::size_t begin, std::size_t end) -> Status {
        if (begin == end) {
          return Ok();
        }
        const std::size_t run_size = end - begin;
        const ResolvedAmericanPriceBatchRequest request{
            .S = pricing_.S,
            .T = t,
            .r = fc.rate,
            .q = fc.q_eff,
            .K = K.subspan(begin, run_size),
            .sigma = out.iv.subspan(begin, run_size),
            .side = side.subspan(begin, run_size),
            .method = pricing_.method,
            .al_opts = std::optional<AlOpts>{pricing_.al_opts},
            .isa = resolved_price_isa,
            .price = out.price.subspan(begin, run_size),
            .status = out.status.subspan(begin, run_size),
            .pack_dispatch = {},
        };
        return american_price_batch_resolved(request);
      };

      std::size_t valid_begin = i;
      for (std::size_t e = i; e < j; ++e) {
        const ResolvedSurfacePoint p = resolve_with_carry(K[e], t, fc);
        if (p.valid) {
          out.iv[e] = p.sigma;
          continue;
        }
        const Status batch_status = dispatch_valid(valid_begin, e);
        if (!batch_status.has_value()) {
          return batch_status;
        }
        const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic);
        out.iv[e] = fr.iv;
        out.price[e] = fr.price;
        out.status[e] = fr.status;
        valid_begin = e + 1;
      }
      const Status batch_status = dispatch_valid(valid_begin, j);
      if (!batch_status.has_value()) {
        return batch_status;
      }
      i = j;
      continue;
    }
    for (std::size_t e = i; e < j; ++e) {
      // Bit-identical to evaluate(K[e], t, ...): resolve_with_carry(K,t,interp_forward(t))
      // == resolve(K,t), and evaluate_resolved is the shared routing.
      ResolvedSurfacePoint p;
      if (t_valid) {
        p = resolve_with_carry(K[e], t, fc);
      } else {
        p.K = K[e];
        p.T = t; // valid == false
      }
      const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic);
      if (!selective_only || want_iv) {
        out.iv[e] = fr.iv;
      }
      if (!selective_only || want_price) {
        out.price[e] = fr.price;
      }
      if (want_greeks) {
        out.greeks[e] = fr.greeks;
      }
      if (want_delta) {
        out.delta[e] = fr.greeks.delta;
      }
      if (want_vega) {
        out.vega[e] = fr.greeks.vega;
      }
      out.status[e] = fr.status;
    }
    i = j;
  }
  return Ok();
}

VolCurveKind PricedSurface::kind_at(std::size_t i) const noexcept {
  return surface_.slices()[i]->kind();
}

} // namespace atx::vol
