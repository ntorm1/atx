// Portfolio Greeks aggregation.
//
// Ports ats_greeks_portfolio.c (the `ats_greeks_portfolio` entry). Computes the
// eight analytic European Black-76 Greeks per option leg against the leg's
// resolved surface/forward state, then reduces them qty-weighted into buckets
// keyed by AggMode. Raw-qty weighting and European B76 Greeks match the C's
// `AtsVolAggregate` convention (distinct from price_portfolio's qty*multiplier
// dollar convention).
//
// PORT NOTE: the C companion `ats_greeks_scenario` (spot/vol/rate/time shock
// revaluation) is a scenario-engine concern folded into the portfolio-RISK
// track and is not ported here.

#include "atx/vol/portfolio.hpp"

#include <cmath>

#include "atx/vol/greeks.hpp"

namespace atx::vol {

Result<std::vector<GreeksAggregate>> aggregate_greeks(
    std::span<const PortfolioLeg> book, const MarketBinding& binding,
    AggMode agg_mode) {
  if (binding.universe == nullptr) {
    return Err(ErrorCode::InvalidArgument, "null universe");
  }

  std::vector<GreeksAggregate> out;
  out.reserve(book.size());

  auto bucket = [&out, agg_mode](const PortfolioLeg& leg) -> GreeksAggregate& {
    const std::uint64_t key = group_key_for_leg(leg, agg_mode);
    for (GreeksAggregate& a : out) {
      if (a.group_key == key) {
        return a;
      }
    }
    GreeksAggregate a;
    a.group_key = key;
    a.uid = (agg_mode == AggMode::Total) ? kInvalidUid : leg.uid;
    a.expiry_id = (agg_mode == AggMode::ByUidExpiry)
                      ? cid_expiry(leg.contract_id)
                      : kInvalidExpiry;
    out.push_back(a);
    return out.back();
  };

  for (const PortfolioLeg& leg : book) {
    if (leg.kind != LegKind::Option) {
      continue;  // ats_greeks_portfolio aggregates option positions only
    }
    const Uid uid = cid_uid(leg.contract_id);
    const ExpiryId eid = cid_expiry(leg.contract_id);
    auto ctxr = detail::resolve_expiry_context(binding, uid, eid);
    if (!ctxr) {
      continue;  // skip unresolvable positions (matches resolve_position != 0)
    }
    const detail::ExpiryContext& ctx = *ctxr;

    const std::uint16_t sx = cid_strike_idx(leg.contract_id);
    if (static_cast<std::size_t>(sx) >= ctx.chain->strikes.size()) {
      continue;
    }
    const double K = ctx.chain->strikes[static_cast<std::size_t>(sx)];
    if (!(K > 0.0)) {
      continue;
    }
    const Side side = cid_side(leg.contract_id);
    const double k_log = std::log(K / ctx.F);

    double sigma = 0.20;  // C's flat fallback when no surface is fitted
    if (ctx.surface != nullptr && ctx.surface->n_slices() > 0) {
      sigma = (ctx.slice_idx != kNoSliceMatch)
                  ? ctx.surface->iv_on_slice(ctx.slice_idx, k_log)
                  : ctx.surface->iv(k_log, ctx.T);
      if (!(std::isfinite(sigma) && sigma > 0.0)) {
        continue;
      }
    }

    const Greeks g =
        black76_greeks(ctx.F, K, ctx.T, sigma, ctx.r, ctx.df, side).greeks;

    GreeksAggregate& a = bucket(leg);
    const double qty = leg.qty;
    a.greeks.delta += qty * g.delta;
    a.greeks.gamma += qty * g.gamma;
    a.greeks.vega += qty * g.vega;
    a.greeks.theta += qty * g.theta;
    a.greeks.rho += qty * g.rho;
    a.greeks.vanna += qty * g.vanna;
    a.greeks.volga += qty * g.volga;
    a.greeks.charm += qty * g.charm;
    a.net_qty += qty;
  }

  return out;
}

}  // namespace atx::vol
