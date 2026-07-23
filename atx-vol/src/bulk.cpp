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

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"

namespace atx::vol {

namespace {

// A2 (review-pricing-iv C3): floor a SERVED cached American mark at
// max(intrinsic, euro, 0), matching the cold clamp chain (american.cpp
// floor_cached_price). The raw euro + F*correction carries no floor of its own,
// so out-of-box (clamped-correction) deep-ITM marks can print below intrinsic —
// arbitrageable. Intrinsic is spot-settled (American exercise value).
[[nodiscard]] double floor_american(double price, double euro, double intrinsic) noexcept {
  double p = price;
  if (intrinsic > p) {
    p = intrinsic;
  }
  if (euro > p) {
    p = euro;
  }
  return p < 0.0 ? 0.0 : p;
}

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

    // F4 (P5) NOT wired here — shape differs from the equal-T ladder batch:
    //   1. This engine walks a flat contract-id list resolving ctx PER LANE, not a
    //      grouped equal-T ladder; the correction cache/T only recur across a chain
    //      run, so a T-collapse would need run-detection the scalar engine lacks.
    //   2. PriceOnly here is raw euro + F·corr with NO intrinsic/euro floor, unlike
    //      american_price_cached_ladder — so that batch is not a drop-in.
    //   3. The B76Greeks / AmericanFirstOrder routes below consume eval_grad's dT
    //      (theta), and a T-collapsed plane has no T axis to differentiate.
    // bulk_price is also perf-review F12 (LOW, "only if on measured hot path"); the
    // F4 win lands on the fitter's board pricing via session::evaluate_ladder.
    // Spot-settled American intrinsic used to floor every cached mark below
    // (floor_american clamps a negative/OTM value to 0, so the raw signed
    // intrinsic is fine here).
    const double intrinsic = (side == Side::Put) ? (K - ctx.under->spot) : (ctx.under->spot - K);
    if (req.risk_mode == BulkRiskMode::PriceOnly) {
      const double euro = black76_price_from_lnfk(ctx.F, K, ctx.T, sigma, ctx.df, -k_log,
                                                  ctx.sqrt_t, side);
      price = euro;
      if (use_corr) {
        // A2: floor the cached mark at max(intrinsic, euro, 0) — the raw
        // euro + F*corr can print below intrinsic when the correction clamps
        // out-of-box (deep-ITM / short-T), exactly the defect fixed elsewhere.
        price = floor_american(euro + ctx.F * corr->eval(k_log, ctx.T, sigma), euro, intrinsic);
        route = static_cast<std::uint8_t>(PricingRoute::B76AlCache);
      }
    } else if (req.risk_mode == BulkRiskMode::B76Greeks) {
      const Black76Greeks bg =
          black76_greeks(ctx.F, K, ctx.T, sigma, ctx.r, ctx.df, side);
      price = bg.price;
      if (use_corr) {
        // A2: floor the cached B76+correction mark (the greeks stay European B76
        // by this mode's contract; only the served price gains the American floor).
        price = floor_american(bg.price + ctx.F * corr->eval(k_log, ctx.T, sigma), bg.price,
                               intrinsic);
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
      // A2: route through the correct, unit-consistent analytic first-order jet
      // (american.cpp american_greeks_first_order) rather than the hand-rolled
      // block whose corr branch left gamma/vanna/volga in FORWARD space while the
      // no-corr branch converted gamma to SPOT (an ~m^2 unit split), and whose
      // price carried no intrinsic floor. A null correction degrades to the
      // spot-converted Black-76 leg. F = spot*e^{(r-q)T} == ctx.F by construction
      // of ctx.q, so the euro leg is preserved.
      const auto ag = american_greeks(ctx.under->spot, K, ctx.T, sigma, ctx.r, ctx.q, side,
                                      use_corr ? corr : nullptr);
      route = static_cast<std::uint8_t>(use_corr ? PricingRoute::B76AlCache
                                                 : PricingRoute::B76AlCold);
      if (ag.has_value()) {
        const AmericanGreeks& g = ag.value();
        price = g.price; // already floored at intrinsic by american_greeks_first_order
        out.delta[i] = g.delta;
        out.gamma[i] = g.gamma;
        out.vega[i] = g.vega;
        out.theta[i] = g.theta;
        out.rho[i] = g.rho;
        out.vanna[i] = g.vanna;
        out.volga[i] = g.volga;
        out.charm[i] = g.charm;
      } else {
        price = kPortNaN; // -> NumericError below
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
