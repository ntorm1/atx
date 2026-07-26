// Portfolio pricing hot path + shared market-context resolution.
//
// Ports ats_vol_portfolio_price.c (the serial `ats_vol_portfolio_price`
// entry). The C's plan/run SoA grouping is an ordering optimization only; this
// scalar port walks the book in input order with a small (uid, expiry) context
// cache, which produces identical per-leg output and identical (order-modulo)
// aggregates.
//
// PORT NOTE: the parallel entry (`_price_parallel`), the theoretical-leg
// superset (`_price_ex`), and the PnL-explain projection are part of the
// separate portfolio-RISK track and are not ported here.

#include "atx/vol/portfolio.hpp"

#include <cmath>
#include <utility>

#include "atx/vol/black76.hpp"

namespace atx::vol {

// ── MarketBinding ────────────────────────────────────────────────────────

void MarketBinding::set_market(Uid uid, const UnderlyingMarket& market) {
  for (auto& kv : markets_) {
    if (kv.first == uid) {
      kv.second = market;
      return;
    }
  }
  markets_.emplace_back(uid, market);
}

const UnderlyingMarket* MarketBinding::market_for(Uid uid) const noexcept {
  for (const auto& kv : markets_) {
    if (kv.first == uid) {
      return &kv.second;
    }
  }
  return nullptr;
}

// ── Group key (ports agg_key_for_leg) ────────────────────────────────────

std::uint64_t group_key_for_leg(const PortfolioLeg& leg, AggMode mode) noexcept {
  switch (mode) {
    case AggMode::Total:
      return 0ull;
    case AggMode::ByUid:
      return static_cast<std::uint64_t>(leg.uid);
    case AggMode::ByUidExpiry: {
      const std::uint64_t uid_part = static_cast<std::uint64_t>(leg.uid) << 16;
      std::uint64_t expiry_part = 0xFFFFull;
      if (leg.kind == LegKind::Option) {
        expiry_part = static_cast<std::uint64_t>(cid_expiry(leg.contract_id));
      }
      return uid_part | expiry_part;
    }
    case AggMode::ByGroupId:
      return static_cast<std::uint64_t>(leg.group_id);
  }
  return 0ull;
}

// ── Expiry-context resolution (ports port_load_expiry_ctx) ───────────────

namespace detail {

Result<ExpiryContext> resolve_expiry_context(const MarketBinding& binding,
                                             Uid uid, ExpiryId expiry_id) {
  if (binding.universe == nullptr) {
    return Err(ErrorCode::InvalidArgument, "null universe");
  }
  auto und = binding.universe->get_underlying(uid);
  if (!und || *und == nullptr) {
    return Err(ErrorCode::NotFound, "unknown uid");
  }
  const Underlying* under = *und;

  // Direct index first, linear fallback (matches the C's defensive path).
  const Chain* chain = nullptr;
  if (static_cast<std::size_t>(expiry_id) < under->chains.size() &&
      under->chains[static_cast<std::size_t>(expiry_id)].expiry_id == expiry_id) {
    chain = &under->chains[static_cast<std::size_t>(expiry_id)];
  } else {
    for (const Chain& c : under->chains) {
      if (c.expiry_id == expiry_id) {
        chain = &c;
        break;
      }
    }
  }
  if (chain == nullptr) {
    return Err(ErrorCode::NotFound, "unknown expiry");
  }
  const double T = chain->T;
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "non-positive T");
  }

  const UnderlyingMarket* market = binding.market_for(uid);
  const VolSurface* surface = (market != nullptr) ? market->surface : nullptr;
  const CurveSet* curves = (market != nullptr) ? market->curves : nullptr;

  const double S = under->spot;
  double r = 0.0;
  if (curves != nullptr && curves->yield.size() > 0) {
    r = curves->yield.zero(T);
  }
  if (!std::isfinite(r)) {
    r = 0.0;
  }

  double F = kPortNaN;
  if (curves != nullptr) {
    F = curves->forward.forward_at(static_cast<std::size_t>(expiry_id));
  }
  if (!(std::isfinite(F) && F > 0.0) && S > 0.0 && std::isfinite(S)) {
    F = S * std::exp(r * T);
  }
  if (!(std::isfinite(F) && F > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "degenerate forward");
  }

  double q = 0.0;
  if (S > 0.0 && std::isfinite(S)) {
    q = r - std::log(F / S) / T;
    if (!std::isfinite(q)) {
      q = 0.0;
    }
  }

  ExpiryContext ctx;
  ctx.under = under;
  ctx.chain = chain;
  ctx.surface = surface;
  ctx.correction_call = (market != nullptr) ? market->correction_call : nullptr;
  ctx.correction_put = (market != nullptr) ? market->correction_put : nullptr;
  ctx.T = T;
  ctx.F = F;
  ctx.r = r;
  ctx.q = q;
  ctx.df = std::exp(-r * T);
  ctx.sqrt_t = std::sqrt(T);
  ctx.log_f = std::log(F);
  if (surface != nullptr) {
    ctx.slice_idx = surface->find_exact_T(T);
  } else {
    ctx.slice_idx = kNoSliceMatch;
  }
  return ctx;
}

}  // namespace detail

// ── Option-leg pricing kernel (ports the two run kernels, per lane) ──────

namespace {

struct OptionPx {
  double sigma{kPortNaN};
  double price{kPortNaN};
  double delta{kPortNaN};  // spot delta (first-order); 0 (price-only)
  double vega{kPortNaN};
  double theta{kPortNaN};
  double rho{kPortNaN};
  std::uint8_t route{static_cast<std::uint8_t>(PricingRoute::B76Only)};
  LaneStatus status{LaneStatus::ModelUnavailable};
};

[[nodiscard]] OptionPx price_option(const detail::ExpiryContext& ctx, double K,
                                    Side side, PortfolioRiskMode mode) noexcept {
  OptionPx r{};

  const double k_log =
      (std::isfinite(K) && K > 0.0 && ctx.F > 0.0) ? std::log(K / ctx.F) : 0.0;

  double sigma = kPortNaN;
  if (ctx.surface != nullptr) {
    sigma = (ctx.slice_idx != kNoSliceMatch)
                ? ctx.surface->iv_on_slice(ctx.slice_idx, k_log)
                : ctx.surface->iv(k_log, ctx.T);
  }
  r.sigma = sigma;

  const CorrectionCache* corr =
      (side == Side::Call) ? ctx.correction_call : ctx.correction_put;
  const bool use_corr = (corr != nullptr) && corr->populated();

  if (mode == PortfolioRiskMode::PriceOnly) {
    double price = black76_price_from_lnfk(ctx.F, K, ctx.T, sigma, ctx.df,
                                           -k_log, ctx.sqrt_t, side);
    std::uint8_t route = static_cast<std::uint8_t>(PricingRoute::B76Only);
    if (use_corr) {
      price += ctx.F * corr->eval(k_log, ctx.T, sigma);
      route = static_cast<std::uint8_t>(PricingRoute::B76AlCache);
    }
    r.price = price;
    r.route = route;
    r.delta = 0.0;  // not meaningful in price-only (matches the C)
  } else {
    const Black76Greeks bg =
        black76_greeks(ctx.F, K, ctx.T, sigma, ctx.r, ctx.df, side);
    double d_fwd = bg.greeks.delta;
    double vega = bg.greeks.vega;
    double theta = bg.greeks.theta;
    double rho = bg.greeks.rho;
    double final_price = bg.price;
    std::uint8_t route = static_cast<std::uint8_t>(PricingRoute::B76Only);

    if (use_corr) {
      double dk = 0.0;
      double d_t = 0.0;
      double dsig = 0.0;
      const double c_val = corr->eval_grad(k_log, ctx.T, sigma, &dk, &d_t, &dsig);
      final_price += ctx.F * c_val;
      d_fwd = bg.greeks.delta + c_val - dk;
      vega = bg.greeks.vega + ctx.F * dsig;
      rho = bg.greeks.rho + ctx.T * ctx.F * d_fwd;
      theta = bg.greeks.theta - (ctx.r - ctx.q) * ctx.F * d_fwd - ctx.F * d_t;
      route = static_cast<std::uint8_t>(PricingRoute::B76AlCache);
    } else {
      // American no-correction spot scaling (matches the C: portfolio option
      // legs are American, only the early-exercise premium is gated on a cache).
      rho = bg.greeks.rho + ctx.T * ctx.F * d_fwd;
      theta = bg.greeks.theta - (ctx.r - ctx.q) * ctx.F * d_fwd;
    }

    const double m = std::exp((ctx.r - ctx.q) * ctx.T);
    r.price = final_price;
    r.delta = m * d_fwd;  // convert forward delta to spot delta
    r.vega = vega;
    r.theta = theta;
    r.rho = rho;
    r.route = route;
  }

  if (!(std::isfinite(sigma) && sigma > 0.0)) {
    r.status = LaneStatus::ModelUnavailable;
  } else if (!(std::isfinite(r.price) && r.price >= 0.0)) {
    r.status = LaneStatus::NumericError;
  } else {
    r.status = LaneStatus::Ok;
  }
  return r;
}

// Small cached (uid, expiry) -> context table; reserved so element addresses
// stay stable for the pointer returned per lookup.
struct CachedCtx {
  std::uint64_t key{0};
  bool ok{false};
  detail::ExpiryContext ctx{};
};

[[nodiscard]] std::uint64_t ctx_key(Uid uid, ExpiryId expiry_id) noexcept {
  return (static_cast<std::uint64_t>(uid) << 16) |
         static_cast<std::uint64_t>(expiry_id);
}

}  // namespace

// ── Public entry ─────────────────────────────────────────────────────────

Result<PortfolioValuation> price_portfolio(std::span<const PortfolioLeg> book,
                                           const MarketBinding& binding,
                                           PortfolioRiskMode risk_mode,
                                           AggMode agg_mode) {
  if (binding.universe == nullptr) {
    return Err(ErrorCode::InvalidArgument, "null universe");
  }

  PortfolioValuation val;
  val.legs.resize(book.size());

  std::vector<PortfolioAggregate> aggs;
  auto bucket = [&aggs](std::uint64_t key) -> PortfolioAggregate& {
    for (auto& a : aggs) {
      if (a.group_key == key) {
        return a;
      }
    }
    aggs.push_back(PortfolioAggregate{});
    aggs.back().group_key = key;
    return aggs.back();
  };

  std::vector<CachedCtx> ctx_cache;
  ctx_cache.reserve(book.size());
  auto get_ctx = [&](Uid uid, ExpiryId eid) -> const detail::ExpiryContext* {
    const std::uint64_t key = ctx_key(uid, eid);
    for (const CachedCtx& c : ctx_cache) {
      if (c.key == key) {
        return c.ok ? &c.ctx : nullptr;
      }
    }
    auto r = detail::resolve_expiry_context(binding, uid, eid);
    CachedCtx entry;
    entry.key = key;
    entry.ok = r.has_value();
    if (r) {
      entry.ctx = *r;
    }
    ctx_cache.push_back(entry);
    return ctx_cache.back().ok ? &ctx_cache.back().ctx : nullptr;
  };

  for (std::size_t i = 0; i < book.size(); ++i) {
    const PortfolioLeg& leg = book[i];
    LegValue& lv = val.legs[i];
    const double mult =
        (std::isfinite(leg.multiplier) && leg.multiplier > 0.0) ? leg.multiplier
                                                                : 1.0;

    if (leg.kind == LegKind::Cash) {
      const double per_unit = std::isfinite(leg.cash_value) ? leg.cash_value : 1.0;
      lv.price = per_unit * mult;
      lv.iv = kPortNaN;
      lv.delta = 0.0;
      lv.gamma = 0.0;
      lv.vega = 0.0;
      lv.theta = 0.0;
      lv.rho = 0.0;
      lv.route = 0xFFu;
      lv.status = LaneStatus::Ok;
    } else if (leg.kind == LegKind::Stock) {
      auto und = binding.universe->get_underlying(leg.uid);
      if (!und || *und == nullptr) {
        lv.status = LaneStatus::InvalidContract;
      } else {
        const double s = (*und)->spot;
        lv.price = s * mult;
        lv.iv = kPortNaN;
        lv.delta = 1.0 * mult;  // dollar delta per share
        lv.gamma = 0.0;
        lv.vega = 0.0;
        lv.theta = 0.0;
        lv.rho = 0.0;
        lv.route = 0xFFu;
        lv.status = LaneStatus::Ok;
      }
    } else {  // Option
      const Uid uid = cid_uid(leg.contract_id);
      const ExpiryId eid = cid_expiry(leg.contract_id);
      const std::uint16_t sx = cid_strike_idx(leg.contract_id);
      const Side side = cid_side(leg.contract_id);
      const detail::ExpiryContext* ctx = get_ctx(uid, eid);
      if (ctx == nullptr || ctx->surface == nullptr ||
          ctx->surface->n_slices() == 0) {
        lv.status = LaneStatus::ModelUnavailable;
      } else {
        const double K =
            (static_cast<std::size_t>(sx) < ctx->chain->strikes.size())
                ? ctx->chain->strikes[static_cast<std::size_t>(sx)]
                : kPortNaN;
        const OptionPx px = price_option(*ctx, K, side, risk_mode);
        lv.iv = px.sigma;
        lv.price = px.price;
        lv.delta = px.delta;
        lv.vega = px.vega;
        lv.theta = px.theta;
        lv.rho = px.rho;
        lv.route = px.route;
        lv.status = px.status;
      }
    }

    // GR-P2-5: status-gate the aggregation (the canonical PortfolioPricer path
    // does). `isfinite(lv.price)` used to admit a NumericError/ModelUnavailable
    // lane that still carried a finite price (e.g. a finite-but-negative overlay
    // price, or a sigma<=0 intrinsic marked ModelUnavailable), polluting the
    // buckets. Stock/cash Ok lanes are unaffected (they set status Ok above).
    if (lv.status == LaneStatus::Ok) {
      PortfolioAggregate& a = bucket(group_key_for_leg(leg, agg_mode));
      if (leg.kind == LegKind::Option) {
        const double qxm = leg.qty * mult;
        a.value += qxm * lv.price;
        if (risk_mode == PortfolioRiskMode::FirstOrder) {
          a.delta += qxm * lv.delta;
          a.vega += qxm * lv.vega;
          a.theta += qxm * lv.theta;
          a.rho += qxm * lv.rho;
        }
      } else {
        // Stock/cash: price already carries the multiplier.
        a.value += leg.qty * lv.price;
        a.delta += (leg.kind == LegKind::Stock) ? leg.qty * mult : 0.0;
      }
      a.n_legs += 1u;
    }
  }

  val.aggregates = std::move(aggs);
  return val;
}

}  // namespace atx::vol
