// Tick-path chain refresh. See api/pricing/chain_tick.hpp for the contract.

#include "atx/vol/api/pricing/chain_tick.hpp"

#include <cmath>
#include <cstddef>
#include <limits>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool shapes_ok(double spot, const ChainContractsView &chain,
                             const ChainTickOutputs &out) noexcept {
  return std::isfinite(spot) && spot > 0.0 && chain.consistent() && out.consistent() &&
         out.size() == chain.size();
}

void write_nan(const ChainTickOutputs &out, std::size_t i, TickStatus st) noexcept {
  out.price[i] = kNaN;
  out.delta[i] = kNaN;
  out.gamma[i] = kNaN;
  out.vega[i] = kNaN;
  out.theta[i] = kNaN;
  out.status[i] = st;
}

} // namespace

Status refresh_chain_cached(double spot, const ChainContractsView &chain,
                            const AmericanCorrectionCaches &caches, const ChainTickOutputs &out) {
  if (!shapes_ok(spot, chain, out)) {
    return Err(ErrorCode::InvalidArgument,
               "refresh_chain_cached: column lengths disagree or spot is not finite > 0");
  }
  const std::size_t n = chain.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Side side = chain.side[i];
    const CorrectionCache *cache = caches.for_side(side);
    const bool usable = cache != nullptr && cache->populated() && cache->side() == side;
    const Result<AmericanGreeks> g = american_greeks(spot, chain.strike[i], chain.expiry[i],
                                                     chain.sigma[i], chain.rate[i],
                                                     chain.carry[i], side, cache);
    if (!g) {
      write_nan(out, i, TickStatus::Unpriceable);
      continue;
    }
    out.price[i] = g->price;
    out.delta[i] = g->delta;
    out.gamma[i] = g->gamma;
    out.vega[i] = g->vega;
    out.theta[i] = g->theta;
    out.status[i] = usable ? TickStatus::Ok : TickStatus::Fallback;
  }
  return Ok();
}

} // namespace atx::vol
