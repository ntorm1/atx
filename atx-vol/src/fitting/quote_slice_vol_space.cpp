#include "fitting/quote_slice_vol_space.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace atx::vol {
namespace {

bool positive(double value) noexcept { return value > 0.0 && std::isfinite(value); }
bool nonnegative(double value) noexcept { return value >= 0.0 && std::isfinite(value); }

// A contract that can carry a channel: valid strike and side, 0 <= bid < ask.
bool channel_quote(const QuoteSliceQuote &quote) noexcept {
  return positive(quote.strike) && (quote.side == Side::Call || quote.side == Side::Put) &&
         nonnegative(quote.bid) && positive(quote.ask) && quote.ask > quote.bid;
}

} // namespace

std::vector<ChannelObservation>
vol_space_channel_observations(QuoteSliceFit &out, std::span<const QuoteSliceQuote> quotes,
                               DeAmIvView &cache, const QuoteSliceChannelConfig &channel) {
  const double forward = out.diagnostics.forward;
  std::vector<ChannelObservation> observations;
  observations.reserve(quotes.size());
  std::vector<double> mid_strikes;
  mid_strikes.reserve(quotes.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const QuoteSliceQuote &quote = quotes[i];
    if (!channel_quote(quote)) {
      continue;
    }
    const DeAmQuoteIv &band = cache.band(i);
    if (!band.well_posed) {
      continue;
    }
    ChannelObservation obs;
    obs.k = std::log(quote.strike / forward);
    obs.sigma_lo = band.bid_iv;
    obs.sigma_hi = band.ask_iv;
    const QuoteSliceRow &row = out.rows[i];
    if (row.admitted && positive(row.market_iv)) {
      obs.sigma_mid = row.market_iv;
    } else if (quote.bid > 0.0) {
      const double mid = 0.5 * quote.bid + 0.5 * quote.ask;
      // The native fit route (K4) may decide the band's edges by bound without pricing them
      // (floor_price / cap_price NaN): its decision is the band's mid_inside.
      const bool inside = channel.deam_native_route
                              ? band.mid_inside
                              : mid > band.floor_price && mid < band.cap_price;
      if (inside) {
        const auto iv = cache.mid_iv(i);
        if (iv) obs.sigma_mid = *iv;
      }
    }
    const double local_k = std::log(quote.strike / band.local_forward);
    const bool in_the_money = (local_k < 0.0 && quote.side == Side::Call) ||
                              (local_k > 0.0 && quote.side == Side::Put);
    obs.mid_scale = in_the_money ? channel.itm_mid_weight : 1.0;
    if (std::isfinite(obs.sigma_mid)) mid_strikes.push_back(quote.strike);
    observations.push_back(obs);
  }
  std::sort(mid_strikes.begin(), mid_strikes.end());
  QuoteSliceChannelDiagnostics &diag = out.diagnostics.channel;
  diag.used = true;
  diag.channel_quotes = observations.size();
  diag.mid_quotes = mid_strikes.size();
  diag.mid_strikes = static_cast<std::size_t>(
      std::unique(mid_strikes.begin(), mid_strikes.end()) - mid_strikes.begin());
  return observations;
}

} // namespace atx::vol
