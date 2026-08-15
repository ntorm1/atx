#include "atx/vol/api/analytics/deriv_pnl.hpp"

#include <cmath>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// One component: `sensitivity * move`, or NaN plus a flag when either half was
// never measured. Written once rather than five times because the failure mode
// this guards against -- a NaN sensitivity quietly multiplying to a zero, or a
// zero substituted for an unmeasured move -- is the same mistake each time.
[[nodiscard]] double term(double sensitivity, double move, DerivPnlFlags bit,
                          DerivPnlFlags& flags) noexcept {
  if (!std::isfinite(sensitivity) || !std::isfinite(move)) {
    flags |= bit;
    return kQuietNaN;
  }
  return sensitivity * move;
}

}  // namespace

double var_swap_fixing_weight(const DerivContract& contract, double df) noexcept {
  // Only the uncapped variance leg is linear in a fixing. A vol swap's payoff
  // is the square root of the average, and both capped kinds truncate it, so
  // neither has a constant per-fixing weight -- see this function's own doc.
  if (contract.kind != DerivKind::VarSwap) {
    return kQuietNaN;
  }
  if (contract.rv_spec.n_obs_total == 0u) {
    return kQuietNaN;
  }
  if (!std::isfinite(df) || !std::isfinite(contract.notional)) {
    return kQuietNaN;
  }
  return df * contract.notional / static_cast<double>(contract.rv_spec.n_obs_total);
}

Result<DerivPnlExplain> deriv_pnl_explain(const DerivPnlInputs& in) {
  if (!std::isfinite(in.dt_years) || in.dt_years < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "deriv_pnl_explain: dt_years must be finite and >= 0");
  }

  DerivPnlExplain out{};
  DerivPnlFlags flags = DerivPnlFlags::None;

  if (!std::isfinite(in.from.pv) || !std::isfinite(in.to.pv)) {
    flags |= DerivPnlFlags::MarkUnavailable;
    out.d_pv = kQuietNaN;
  } else {
    out.d_pv = in.to.pv - in.from.pv;
  }

  out.carry = term(in.greeks.theta_zero_fixing, in.dt_years, DerivPnlFlags::CarryUnavailable,
                   flags);
  out.realized = term(in.fixing_weight, in.realized_var_dec,
                      DerivPnlFlags::RealizedUnavailable, flags);
  out.vol_level = term(in.greeks.vega, in.to.sigma_atm - in.from.sigma_atm,
                       DerivPnlFlags::VolLevelUnavailable, flags);
  out.skew = term(in.greeks.skew_vega, in.to.skew_slope - in.from.skew_slope,
                  DerivPnlFlags::SkewUnavailable, flags);
  out.convexity = term(in.greeks.convexity_vega,
                       in.to.smile_curvature - in.from.smile_curvature,
                       DerivPnlFlags::ConvexityUnavailable, flags);
  out.discount = term(in.greeks.rho, in.to.zero_rate - in.from.zero_rate,
                      DerivPnlFlags::DiscountUnavailable, flags);

  // Left to right, in the order the header states the identity. The order is
  // part of the contract: a caller reproducing `residual` from the published
  // components must get the same bits back, and floating-point addition does
  // not commute across a re-association.
  const double explained =
      out.carry + out.realized + out.vol_level + out.skew + out.convexity + out.discount;
  out.residual = out.d_pv - explained;

  out.flags = flags;
  return Ok(out);
}

Result<TheoEdgeLedger> theo_edge_ledger(const TheoEdgeLedgerInputs& in) {
  if (!std::isfinite(in.dt_years) || in.dt_years < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "theo_edge_ledger: dt_years must be finite and >= 0");
  }

  TheoEdgeLedger out{};
  TheoEdgeLedgerFlags flags = TheoEdgeLedgerFlags::None;

  // Line 1: realized-leg accrual at dollar gamma. Every factor must be a real
  // measurement -- a NaN gamma multiplying a zero surprise must stay NaN.
  if (!std::isfinite(in.gamma) || !std::isfinite(in.spot) || !std::isfinite(in.r_step) ||
      !std::isfinite(in.sigma_imp)) {
    flags |= TheoEdgeLedgerFlags::CollectedUnavailable;
    out.collected = kQuietNaN;
  } else {
    out.collected = 0.5 * in.gamma * in.spot * in.spot *
                    (in.r_step * in.r_step - in.sigma_imp * in.sigma_imp * in.dt_years);
  }

  // Line 2: the unaccrued fraction remarked at today's implied vol.
  if (!std::isfinite(in.vega) || !std::isfinite(in.d_iv)) {
    flags |= TheoEdgeLedgerFlags::RepricedUnavailable;
    out.repriced = kQuietNaN;
  } else {
    out.repriced = in.vega * in.d_iv;
  }

  out.flags = flags;
  return Ok(out);
}

}  // namespace atx::vol
