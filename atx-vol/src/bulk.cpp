// Bulk chain pricer + selection front-end.
//
// Ports ats_vol_bulk_select.c (selection + aggregation) and
// ats_vol_bulk_pricer.c (the streaming price engine), scalar path only. The C
// AVX2 batch4 kernels and the arena/scratch plumbing are deferred; the engine
// here walks the dense contract-id list one lane at a time with a per-lane
// expiry-context resolve, which is numerically identical to the C's
// homogeneous-run batching.
//
// PORT NOTE: the cache route (a bound American CorrectionCache) is wired but
// its dedicated parity tests are deferred — a populated cache is a heavy
// Andersen-Lake build, and the correction math mirrors american_*_from_b76
// exactly. The European B76 / American-no-correction routes are fully tested.

#include "atx/vol/portfolio.hpp"

#include <cmath>

#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"

namespace atx::vol {

namespace {

// ── Selection (ports bulk_select_count + bulk_select_emit) ───────────────

[[nodiscard]] Result<std::vector<ContractId>> bulk_select(
    const BulkRequest& req, const MarketBinding& binding) {
  std::vector<ContractId> ids;

  if (req.select_kind == BulkSelectKind::ContractList) {
    ids.assign(req.contract_ids.begin(), req.contract_ids.end());
    return ids;
  }

  auto und = binding.universe->get_underlying(req.uid);
  if (!und || *und == nullptr) {
    return Err(ErrorCode::NotFound, "unknown uid");
  }
  const Underlying* under = *und;
  const UnderlyingMarket* market = binding.market_for(req.uid);
  const CurveSet* curves = (market != nullptr) ? market->curves : nullptr;

  for (const Chain& c : under->chains) {
    double lo = 0.0;
    double hi = 0.0;
    if (req.select_kind == BulkSelectKind::ChainStrikeRange) {
      lo = req.strike_lo;
      hi = req.strike_hi;
    } else {  // ChainMoneynessBand
      double f = kPortNaN;
      if (curves != nullptr) {
        f = curves->forward.forward_at(static_cast<std::size_t>(c.expiry_id));
      }
      if (!(std::isfinite(f) && f > 0.0) && under->spot > 0.0) {
        f = under->spot;
      }
      if (!(std::isfinite(f) && f > 0.0)) {
        continue;
      }
      lo = f * (1.0 - req.moneyness_pct);
      hi = f * (1.0 + req.moneyness_pct);
    }
    if (!(hi >= lo)) {
      continue;
    }
    // Calls then puts, ascending strike (matches the C emit order).
    for (std::size_t k = 0; k < c.strikes.size(); ++k) {
      if (c.strikes[k] >= lo && c.strikes[k] <= hi) {
        ids.push_back(make_contract_id(req.uid, c.expiry_id,
                                       static_cast<std::uint16_t>(k), Side::Call));
      }
    }
    for (std::size_t k = 0; k < c.strikes.size(); ++k) {
      if (c.strikes[k] >= lo && c.strikes[k] <= hi) {
        ids.push_back(make_contract_id(req.uid, c.expiry_id,
                                       static_cast<std::uint16_t>(k), Side::Put));
      }
    }
  }
  return ids;
}

// ── Lane output init ─────────────────────────────────────────────────────

void init_output(BulkOutput& out, std::size_t n) {
  out.contract_id.assign(n, ContractId{0});
  out.price.assign(n, kPortNaN);
  out.iv.assign(n, kPortNaN);
  out.delta.assign(n, kPortNaN);
  out.gamma.assign(n, kPortNaN);
  out.vega.assign(n, kPortNaN);
  out.theta.assign(n, kPortNaN);
  out.rho.assign(n, kPortNaN);
  out.vanna.assign(n, kPortNaN);
  out.volga.assign(n, kPortNaN);
  out.charm.assign(n, kPortNaN);
  out.route.assign(n, 0xFFu);
  out.status.assign(n, LaneStatus::InvalidContract);
}

// ── Price engine (ports ats_vol_bulk_price_engine, scalar path) ──────────

void bulk_price_engine(const std::vector<ContractId>& ids,
                       const BulkRequest& req, const MarketBinding& binding,
                       BulkOutput& out) {
  const std::size_t n = ids.size();
  init_output(out, n);

  for (std::size_t i = 0; i < n; ++i) {
    const ContractId cid = ids[i];
    out.contract_id[i] = cid;

    const Uid uid = cid_uid(cid);
    const ExpiryId eid = cid_expiry(cid);
    const std::uint16_t sx = cid_strike_idx(cid);
    const Side side = cid_side(cid);

    if (uid == kInvalidUid ||
        (req.uid != kInvalidUid && uid != req.uid) ||
        (side != Side::Call && side != Side::Put)) {
      continue;  // stays InvalidContract
    }

    auto ctxr = detail::resolve_expiry_context(binding, uid, eid);
    if (!ctxr) {
      out.status[i] = LaneStatus::ModelUnavailable;
      continue;
    }
    const detail::ExpiryContext& ctx = *ctxr;
    if (ctx.surface == nullptr || ctx.surface->n_slices() == 0) {
      out.status[i] = LaneStatus::ModelUnavailable;
      continue;
    }
    if (static_cast<std::size_t>(sx) >= ctx.chain->strikes.size()) {
      continue;  // stays InvalidContract
    }
    const double K = ctx.chain->strikes[static_cast<std::size_t>(sx)];
    if (!(K > 0.0) || !(ctx.F > 0.0)) {
      continue;
    }

    const double k_log = std::log(K / ctx.F);
    const double sigma = (ctx.slice_idx != kNoSliceMatch)
                             ? ctx.surface->iv_on_slice(ctx.slice_idx, k_log)
                             : ctx.surface->iv(k_log, ctx.T);
    if (!(std::isfinite(sigma) && sigma > 0.0)) {
      out.status[i] = LaneStatus::ModelUnavailable;
      continue;
    }
    out.iv[i] = sigma;

    const CorrectionCache* corr =
        (side == Side::Call) ? ctx.correction_call : ctx.correction_put;
    const bool use_corr = (corr != nullptr) && corr->populated();

    double price = kPortNaN;
    std::uint8_t route = static_cast<std::uint8_t>(PricingRoute::B76Only);

    if (req.risk_mode == BulkRiskMode::PriceOnly) {
      price = black76_price_from_lnfk(ctx.F, K, ctx.T, sigma, ctx.df, -k_log,
                                      ctx.sqrt_t, side);
      if (use_corr) {
        price += ctx.F * corr->eval(k_log, ctx.T, sigma);
        route = static_cast<std::uint8_t>(PricingRoute::B76AlCache);
      }
    } else {
      const Black76Greeks bg =
          black76_greeks(ctx.F, K, ctx.T, sigma, ctx.r, ctx.df, side);

      if (req.risk_mode == BulkRiskMode::B76Greeks) {
        price = bg.price;
        if (use_corr) {
          price += ctx.F * corr->eval(k_log, ctx.T, sigma);
          route = static_cast<std::uint8_t>(PricingRoute::B76AlCache);
        }
        out.delta[i] = bg.greeks.delta;
        out.gamma[i] = bg.greeks.gamma;
        out.vega[i] = bg.greeks.vega;
        out.theta[i] = bg.greeks.theta;
        out.rho[i] = bg.greeks.rho;
        out.vanna[i] = bg.greeks.vanna;
        out.volga[i] = bg.greeks.volga;
        out.charm[i] = bg.greeks.charm;
      } else {  // AmericanFirstOrder
        const double s_spot = ctx.under->spot;
        const double m = std::exp((ctx.r - ctx.q) * ctx.T);
        double d_fwd = bg.greeks.delta;
        double vega = bg.greeks.vega;
        double theta = bg.greeks.theta;
        double rho = bg.greeks.rho;
        double gamma = bg.greeks.gamma;

        if (use_corr) {
          double dk = 0.0;
          double d_t = 0.0;
          double dsig = 0.0;
          const double c_val =
              corr->eval_grad(k_log, ctx.T, sigma, &dk, &d_t, &dsig);
          d_fwd = bg.greeks.delta + c_val - dk;
          vega = bg.greeks.vega + ctx.F * dsig;
          rho = bg.greeks.rho + ctx.T * ctx.F * d_fwd;
          theta = bg.greeks.theta - (ctx.r - ctx.q) * ctx.F * d_fwd - ctx.F * d_t;
          price = bg.price + ctx.F * c_val;
          route = static_cast<std::uint8_t>(PricingRoute::B76AlCache);
        } else {
          rho = bg.greeks.rho + ctx.T * ctx.F * d_fwd;
          theta = bg.greeks.theta - (ctx.r - ctx.q) * ctx.F * d_fwd;
          if (s_spot > 0.0) {
            gamma = bg.greeks.gamma * (m / s_spot) * ctx.F;
          }
          price = bg.price;
          route = static_cast<std::uint8_t>(PricingRoute::B76AlCold);
        }

        out.delta[i] = m * d_fwd;  // spot delta
        out.gamma[i] = gamma;
        out.vega[i] = vega;
        out.theta[i] = theta;
        out.rho[i] = rho;
        out.vanna[i] = bg.greeks.vanna;
        out.volga[i] = bg.greeks.volga;
        out.charm[i] = bg.greeks.charm;
      }
    }

    out.route[i] = route;
    if (!(std::isfinite(price) && price >= 0.0)) {
      out.status[i] = LaneStatus::NumericError;
      continue;
    }
    out.price[i] = price;
    out.status[i] = LaneStatus::Ok;
  }
}

// ── Aggregation (ports bulk_aggregate_outputs) ───────────────────────────

[[nodiscard]] double finite_or_zero(const std::vector<double>& col,
                                    std::size_t i) noexcept {
  return (i < col.size() && std::isfinite(col[i])) ? col[i] : 0.0;
}

[[nodiscard]] std::vector<GreeksAggregate> bulk_aggregate(
    const BulkRequest& req, const BulkOutput& out, AggMode mode) {
  std::vector<GreeksAggregate> aggs;
  aggs.reserve(out.size());

  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out.status[i] != LaneStatus::Ok) {
      continue;
    }
    double q = 1.0;
    if (req.select_kind == BulkSelectKind::ContractList &&
        i < req.qty.size()) {
      q = static_cast<double>(req.qty[i]);
    }
    if (q == 0.0) {
      continue;
    }

    const ContractId cid = out.contract_id[i];
    const Uid uid = cid_uid(cid);
    const ExpiryId eid = cid_expiry(cid);

    std::uint64_t key = 0ull;
    if (mode == AggMode::ByUid) {
      key = static_cast<std::uint64_t>(uid);
    } else if (mode == AggMode::ByUidExpiry) {
      key = (static_cast<std::uint64_t>(uid) << 16) |
            static_cast<std::uint64_t>(eid);
    }

    GreeksAggregate* slot = nullptr;
    for (GreeksAggregate& a : aggs) {
      if (a.group_key == key) {
        slot = &a;
        break;
      }
    }
    if (slot == nullptr) {
      GreeksAggregate a;
      a.group_key = key;
      a.uid = (mode == AggMode::Total) ? kInvalidUid : uid;
      a.expiry_id = (mode == AggMode::ByUidExpiry) ? eid : kInvalidExpiry;
      aggs.push_back(a);
      slot = &aggs.back();
    }

    slot->greeks.delta += q * finite_or_zero(out.delta, i);
    slot->greeks.gamma += q * finite_or_zero(out.gamma, i);
    slot->greeks.vega += q * finite_or_zero(out.vega, i);
    slot->greeks.theta += q * finite_or_zero(out.theta, i);
    slot->greeks.rho += q * finite_or_zero(out.rho, i);
    slot->greeks.vanna += q * finite_or_zero(out.vanna, i);
    slot->greeks.volga += q * finite_or_zero(out.volga, i);
    slot->greeks.charm += q * finite_or_zero(out.charm, i);
    slot->net_qty += q;
  }
  return aggs;
}

}  // namespace

// ── Public entry (ports ats_vol_bulk_price) ──────────────────────────────

Result<BulkResult> bulk_price(const BulkRequest& req,
                              const MarketBinding& binding, AggMode agg_mode) {
  if (binding.universe == nullptr) {
    return Err(ErrorCode::InvalidArgument, "null universe");
  }
  if (agg_mode == AggMode::ByGroupId) {
    return Err(ErrorCode::NotImplemented,
               "bulk path has no per-leg group_id; use price_portfolio");
  }

  switch (req.select_kind) {
    case BulkSelectKind::ContractList:
      break;
    case BulkSelectKind::ChainMoneynessBand:
      if (!std::isfinite(req.moneyness_pct) || req.moneyness_pct < 0.0) {
        return Err(ErrorCode::InvalidArgument, "bad moneyness_pct");
      }
      if (req.uid == kInvalidUid) {
        return Err(ErrorCode::InvalidArgument, "chain selector needs a uid");
      }
      break;
    case BulkSelectKind::ChainStrikeRange:
      if (!std::isfinite(req.strike_lo) || !std::isfinite(req.strike_hi)) {
        return Err(ErrorCode::InvalidArgument, "bad strike range");
      }
      if (!(req.strike_hi >= req.strike_lo)) {
        return Err(ErrorCode::InvalidArgument, "inverted strike range");
      }
      if (req.uid == kInvalidUid) {
        return Err(ErrorCode::InvalidArgument, "chain selector needs a uid");
      }
      break;
  }

  ATX_TRY(auto ids, bulk_select(req, binding));

  BulkResult res;
  bulk_price_engine(ids, req, binding, res.out);
  res.aggregates = bulk_aggregate(req, res.out, agg_mode);
  return res;
}

}  // namespace atx::vol
