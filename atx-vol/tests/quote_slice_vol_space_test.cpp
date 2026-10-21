// Opt-in per-quote de-Americanised IV cache (lane L2a, QuoteSliceFitSpace::VolChannel).
//
// Pins: (1) on the accurate kernel the cache serves exactly the PriceChannel fit,
// cached or not, on a memo miss and on a memo hit, and a hit performs no American
// work; (2) the PriceChannel path ignores the deam_ keys; (3) a Fast-kernel cache
// entry never answers an Accurate request, and a Fast inversion without a usable
// vega is redone on the accurate kernel; (4) the memo key covers every input that
// changes an inversion and none of the rung-level fit settings; (5) the memo is
// bounded and an evicted cache stays alive for a fit that still holds it.

#include "fitting/deam_iv_cache.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// An SPY-shaped chain: spot 670, strikes +-25% in 2.5% steps, a skewed smile
// priced with the Andersen-Lake engine the fitter inverts, bidless far
// out-of-the-money wings, and a deep in-the-money put whose bid sits below its
// minimum-volatility price (a band with no lower information).
Chain spy_chain(double years, bool strike_carry, double dividend_pv) {
  constexpr double kSpot = 670.0;
  constexpr double kRate = 0.04;
  constexpr double kYield = 0.012;
  Chain out;
  out.market = QuoteSliceMarket{kSpot, years, years, kRate, kYield, dividend_pv};
  for (int step = -10; step <= 10; ++step) {
    const double strike = std::round(kSpot * (1.0 + 0.025 * step));
    const double k = std::log(strike / kSpot);
    const double sigma = std::max(0.08, 0.16 - 0.30 * k + 0.80 * k * k);
    const double q = strike_carry ? kYield + 0.004 * k : kYield;
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(kSpot, strike, years, sigma, kRate, q, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double half = std::max(0.02, 0.03 * *price);
      const bool otm = (side == Side::Call) == (k > 0.0);
      const double bid = otm && std::abs(k) > 0.2 ? 0.0 : std::max(*price - half, 0.0);
      QuoteSliceQuote quote{strike, side, bid, *price + half};
      if (strike_carry) quote.continuous_yield = q;
      out.quotes.push_back(quote);
    }
  }
  QuoteSliceQuote &deep_put = out.quotes.back();
  deep_put.bid = 0.98 * (deep_put.strike - kSpot);
  return out;
}

bool same_bits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Every served or reported value of two fits, bit for bit (timing and the
// cache's own work counters excluded).
void expect_identical(const QuoteSliceFit &a, const QuoteSliceFit &b, const std::string &label) {
  SCOPED_TRACE(label);
  EXPECT_EQ(a.fit_status, b.fit_status);
  EXPECT_EQ(a.fit_error, b.fit_error);
  const QuoteSliceDiagnostics &da = a.diagnostics;
  const QuoteSliceDiagnostics &db = b.diagnostics;
  EXPECT_EQ(da.iv_attempted, db.iv_attempted);
  EXPECT_EQ(da.quotes_admitted, db.quotes_admitted);
  EXPECT_EQ(da.distinct_strikes, db.distinct_strikes);
  EXPECT_EQ(da.prices_produced, db.prices_produced);
  EXPECT_EQ(da.weights_capped, db.weights_capped);
  EXPECT_EQ(da.channel.used, db.channel.used);
  EXPECT_EQ(da.channel.channel_quotes, db.channel.channel_quotes);
  EXPECT_EQ(da.channel.mid_quotes, db.channel.mid_quotes);
  EXPECT_EQ(da.channel.mid_strikes, db.channel.mid_strikes);
  EXPECT_EQ(da.channel.violations, db.channel.violations);
  EXPECT_EQ(da.channel.iterations, db.channel.iterations);
  EXPECT_EQ(da.channel.converged, db.channel.converged);
  EXPECT_TRUE(same_bits(da.channel.max_violation_half_widths, db.channel.max_violation_half_widths));
  EXPECT_EQ(da.channel.repaired_nodes, db.channel.repaired_nodes);
  ASSERT_EQ(a.rows.size(), b.rows.size());
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    const QuoteSliceRow &ra = a.rows[i];
    const QuoteSliceRow &rb = b.rows[i];
    EXPECT_EQ(ra.quote_status, rb.quote_status) << i;
    EXPECT_EQ(ra.evaluation_status, rb.evaluation_status) << i;
    EXPECT_EQ(ra.admitted, rb.admitted) << i;
    EXPECT_TRUE(same_bits(ra.market_iv, rb.market_iv)) << i;
    EXPECT_TRUE(same_bits(ra.fitted_iv, rb.fitted_iv)) << i;
    EXPECT_TRUE(same_bits(ra.fitted_price, rb.fitted_price)) << i;
    EXPECT_TRUE(same_bits(ra.spread_vol, rb.spread_vol)) << i;
    EXPECT_TRUE(same_bits(ra.weight_w, rb.weight_w)) << i;
    EXPECT_EQ(ra.weight_capped, rb.weight_capped) << i;
  }
  EXPECT_EQ(a.moved_nodes, b.moved_nodes);
  EXPECT_EQ(a.price_only_nodes, b.price_only_nodes);
  ASSERT_EQ(a.prices.has_value(), b.prices.has_value());
  if (a.prices) {
    const auto na = a.prices->nodes();
    const auto nb = b.prices->nodes();
    ASSERT_EQ(na.size(), nb.size());
    for (std::size_t i = 0; i < na.size(); ++i) {
      EXPECT_TRUE(same_bits(na[i].strike, nb[i].strike)) << i;
      EXPECT_TRUE(same_bits(na[i].call, nb[i].call)) << i;
      EXPECT_TRUE(same_bits(na[i].put, nb[i].put)) << i;
    }
  }
  ASSERT_EQ(a.curve != nullptr, b.curve != nullptr);
  if (a.curve) {
    for (double k = -0.4; k <= 0.4; k += 0.05) {
      EXPECT_TRUE(same_bits(a.curve->iv(k), b.curve->iv(k))) << k;
    }
  }
}

QuoteSliceConfig price_config(QuoteSliceDividendModel model) {
  QuoteSliceConfig config;
  config.dividend_model = model;
  config.channel.repair_half_spreads = 0.5;
  return config;
}

QuoteSliceConfig vol_config(QuoteSliceConfig config, bool cache,
                            QuoteSliceDeamProfile profile = QuoteSliceDeamProfile::Accurate) {
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  config.channel.deam_profile = profile;
  config.channel.deam_cache = cache;
  return config;
}

struct GridCase {
  double years;
  bool strike_carry;
  QuoteSliceDividendModel model;
  std::optional<VolCurveKind> family;
};

TEST(QuoteSliceVolSpace, AccurateCacheServesThePriceChannelFitBitForBitOnMissAndHit) {
  const std::vector<GridCase> grid = {
      {0.03, false, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
      {0.03, true, QuoteSliceDividendModel::AggregateFundingPresentValue, std::nullopt},
      {0.15, false, QuoteSliceDividendModel::AggregateFundingPresentValue, std::nullopt},
      {0.15, true, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
      {0.15, false, QuoteSliceDividendModel::ContinuousYield, VolCurveKind::Essvi},
      {0.75, true, QuoteSliceDividendModel::AggregateFundingPresentValue, std::nullopt},
      {0.75, false, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
  };
  std::size_t served = 0;
  for (const GridCase &g : grid) {
    const std::string label = "T=" + std::to_string(g.years) +
                              (g.strike_carry ? " strike-carry" : " reference-carry") +
                              " model=" + to_string(g.model) + (g.family ? " family" : "");
    SCOPED_TRACE(label);
    const double pv = g.model == QuoteSliceDividendModel::AggregateFundingPresentValue ? 4.0 : 0.0;
    const Chain c = spy_chain(g.years, g.strike_carry, pv);
    QuoteSliceConfig off = price_config(g.model);
    off.channel.family = g.family;
    deam_iv_memo_clear();

    const auto off_point = fit_quote_slice(c.market, c.quotes, off);
    const auto off_channel = fit_quote_slice_channel(c.market, c.quotes, off, nullptr);
    ASSERT_TRUE(off_point) << off_point.error().to_string();
    ASSERT_TRUE(off_channel) << off_channel.error().to_string();
    EXPECT_FALSE(off_channel->diagnostics.deam.used);

    // Uncached: the fit owns its cache; the memo is untouched.
    const auto own = fit_quote_slice_channel(c.market, c.quotes, vol_config(off, false), nullptr);
    ASSERT_TRUE(own) << own.error().to_string();
    EXPECT_TRUE(own->diagnostics.deam.used);
    EXPECT_FALSE(own->diagnostics.deam.reused);
    EXPECT_GT(own->diagnostics.deam.inversions, 0u);
    EXPECT_EQ(deam_iv_memo_size(), 0u);
    expect_identical(*off_channel, *own, "uncached channel");

    // Memo, in the graded surface's order: the point fit fills the mids (miss),
    // the channel fit reuses them and adds the bands, the next rung reuses all.
    const QuoteSliceConfig on = vol_config(off, true);
    const auto point = fit_quote_slice(c.market, c.quotes, on);
    const auto first = fit_quote_slice_channel(c.market, c.quotes, on, nullptr);
    const auto again = fit_quote_slice_channel(c.market, c.quotes, on, nullptr);
    ASSERT_TRUE(point && first && again);
    EXPECT_TRUE(point->diagnostics.deam.used);
    EXPECT_FALSE(point->diagnostics.deam.reused);
    EXPECT_TRUE(first->diagnostics.deam.reused);
    EXPECT_GT(first->diagnostics.deam.model_prices, 0u);
    EXPECT_TRUE(again->diagnostics.deam.reused);
    EXPECT_EQ(again->diagnostics.deam.inversions, 0u);
    EXPECT_EQ(again->diagnostics.deam.model_prices, 0u);
    EXPECT_EQ(deam_iv_memo_size(), 1u);
    expect_identical(*off_point, *point, "memo point (miss)");
    expect_identical(*off_channel, *first, "memo channel (hit, bands added)");
    expect_identical(*off_channel, *again, "memo channel (full hit)");
    if (off_channel->fit_status == QuoteSliceFitStatus::Success) ++served;
    if (off_point->fit_status == QuoteSliceFitStatus::Success) ++served;
  }
  // The grid must exercise certified serving, not only refusals.
  EXPECT_GE(served, grid.size());
  deam_iv_memo_clear();
}

TEST(QuoteSliceVolSpace, PriceChannelIgnoresTheCacheKeys) {
  const Chain c = spy_chain(0.15, true, 0.0);
  const QuoteSliceConfig off = price_config(QuoteSliceDividendModel::ContinuousYield);
  QuoteSliceConfig keyed = off; // fit_space stays PriceChannel
  keyed.channel.deam_cache = true;
  keyed.channel.deam_profile = QuoteSliceDeamProfile::Fast;
  deam_iv_memo_clear();
  const auto plain_channel = fit_quote_slice_channel(c.market, c.quotes, off, nullptr);
  const auto keyed_channel = fit_quote_slice_channel(c.market, c.quotes, keyed, nullptr);
  const auto plain_point = fit_quote_slice(c.market, c.quotes, off);
  const auto keyed_point = fit_quote_slice(c.market, c.quotes, keyed);
  ASSERT_TRUE(plain_channel && keyed_channel && plain_point && keyed_point);
  for (const QuoteSliceFit *fit : {&*keyed_channel, &*keyed_point}) {
    const QuoteSliceDeamDiagnostics &deam = fit->diagnostics.deam;
    EXPECT_FALSE(deam.used);
    EXPECT_FALSE(deam.reused);
    EXPECT_EQ(deam.inversions, 0u);
    EXPECT_EQ(deam.model_prices, 0u);
    EXPECT_EQ(deam.accurate_fallbacks, 0u);
  }
  EXPECT_EQ(deam_iv_memo_size(), 0u);
  expect_identical(*plain_channel, *keyed_channel, "channel");
  expect_identical(*plain_point, *keyed_point, "point");
}

TEST(QuoteSliceVolSpace, FastKernelCacheNeverAnswersAnAccurateRequest) {
  const Chain c = spy_chain(0.15, false, 0.0);
  const QuoteSliceConfig off = price_config(QuoteSliceDividendModel::ContinuousYield);
  deam_iv_memo_clear();
  const auto reference = fit_quote_slice_channel(c.market, c.quotes, off, nullptr);
  const auto fast = fit_quote_slice_channel(
      c.market, c.quotes, vol_config(off, true, QuoteSliceDeamProfile::Fast), nullptr);
  ASSERT_TRUE(reference && fast);
  ASSERT_TRUE(fast->diagnostics.deam.used);
  ASSERT_GT(fast->diagnostics.deam.inversions, 0u);
  // The Fast kernel does move some quote volatilities (otherwise this test
  // would not distinguish the kernels).
  std::size_t moved = 0;
  for (std::size_t i = 0; i < c.quotes.size(); ++i) {
    if (!same_bits(fast->rows[i].market_iv, reference->rows[i].market_iv)) ++moved;
  }
  ASSERT_GT(moved, 0u);
  // Same chain, same everything, Accurate kernel: a separate memo entry.
  const auto accurate = fit_quote_slice_channel(c.market, c.quotes, vol_config(off, true), nullptr);
  ASSERT_TRUE(accurate);
  EXPECT_FALSE(accurate->diagnostics.deam.reused);
  EXPECT_GT(accurate->diagnostics.deam.inversions, 0u);
  EXPECT_EQ(deam_iv_memo_size(), 2u);
  expect_identical(*reference, *accurate, "accurate after fast");
  deam_iv_memo_clear();
}

// Opt-in Premium inversion of the cache (lane L2e, QuoteSliceDeamInversion::Premium):
// on the SPY-shaped grid it serves the same rows (statuses, admission, fit status) as
// the Search cache with quote volatilities within the inversion tolerance band; its
// memo entry never answers a Search fit (key word), so a Search fit after it is the
// PriceChannel fit bit for bit; its values do not depend on the order the fits ask for
// them (mids first, edges from the mid's points); Premium on the Fast kernel is refused.
TEST(QuoteSliceVolSpace, PremiumCacheServesTheSearchRowsAndNeverAnswersASearchFit) {
  const std::vector<GridCase> grid = {
      {0.03, false, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
      {0.15, true, QuoteSliceDividendModel::AggregateFundingPresentValue, std::nullopt},
      {0.75, false, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
      {2.0, true, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
  };
  const double tol = QuoteSliceConfig{}.iv_tolerance;
  std::size_t premium_inversions = 0, premium_fallbacks = 0, compared = 0;
  double max_dsigma = 0.0;
  for (const GridCase &g : grid) {
    SCOPED_TRACE("T=" + std::to_string(g.years));
    const double pv = g.model == QuoteSliceDividendModel::AggregateFundingPresentValue ? 4.0 : 0.0;
    const Chain c = spy_chain(g.years, g.strike_carry, pv);
    const QuoteSliceConfig off = price_config(g.model);
    const QuoteSliceConfig search = vol_config(off, true);
    QuoteSliceConfig premium = search;
    premium.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
    deam_iv_memo_clear();
    const auto reference = fit_quote_slice_channel(c.market, c.quotes, off, nullptr);
    // Premium in the graded order (point fit first: mids), then channel-only on a
    // cleared memo: the same bits either way.
    const auto prem_point = fit_quote_slice(c.market, c.quotes, premium);
    const auto prem_channel = fit_quote_slice_channel(c.market, c.quotes, premium, nullptr);
    ASSERT_TRUE(reference && prem_point && prem_channel);
    EXPECT_TRUE(prem_channel->diagnostics.deam.reused);
    // The Search cache after the Premium one: a separate entry, the PriceChannel fit.
    const auto search_after = fit_quote_slice_channel(c.market, c.quotes, search, nullptr);
    ASSERT_TRUE(search_after);
    EXPECT_FALSE(search_after->diagnostics.deam.reused);
    EXPECT_EQ(search_after->diagnostics.deam.premium_inversions, 0u);
    EXPECT_EQ(deam_iv_memo_size(), 2u);
    expect_identical(*reference, *search_after, "search after premium");
    deam_iv_memo_clear();
    const auto prem_alone = fit_quote_slice_channel(c.market, c.quotes, premium, nullptr);
    ASSERT_TRUE(prem_alone);
    expect_identical(*prem_channel, *prem_alone, "premium, channel only");
    premium_inversions += prem_point->diagnostics.deam.premium_inversions +
                          prem_channel->diagnostics.deam.premium_inversions;
    premium_fallbacks += prem_point->diagnostics.deam.premium_fallbacks +
                         prem_channel->diagnostics.deam.premium_fallbacks;
    // Same rows served as the Search cache (= the PriceChannel fit), vols within band.
    EXPECT_EQ(prem_channel->fit_status, reference->fit_status);
    ASSERT_EQ(prem_channel->rows.size(), reference->rows.size());
    for (std::size_t i = 0; i < reference->rows.size(); ++i) {
      const QuoteSliceRow &p = prem_channel->rows[i];
      const QuoteSliceRow &r = reference->rows[i];
      EXPECT_EQ(p.quote_status, r.quote_status) << i;
      EXPECT_EQ(p.evaluation_status, r.evaluation_status) << i;
      EXPECT_EQ(p.admitted, r.admitted) << i;
      if (std::isfinite(p.market_iv) && std::isfinite(r.market_iv)) {
        ++compared;
        max_dsigma = std::max(max_dsigma, std::abs(p.market_iv - r.market_iv));
        EXPECT_LE(std::abs(p.market_iv - r.market_iv), 4.0 * tol) << i;
      }
    }
  }
  std::printf("[premium cache] %zu inversions, %zu fallbacks, %zu mid IVs compared, max "
              "|dsigma| vs Search %.3g\n",
              premium_inversions, premium_fallbacks, compared, max_dsigma);
  EXPECT_GT(premium_inversions, 0u);
  EXPECT_LT(premium_fallbacks * 10, premium_inversions);
  EXPECT_GT(compared, 0u);
  deam_iv_memo_clear();
  // Premium is defined on the accurate kernel only.
  const Chain c = spy_chain(0.15, false, 0.0);
  QuoteSliceConfig fast =
      vol_config(price_config(QuoteSliceDividendModel::ContinuousYield), true,
                 QuoteSliceDeamProfile::Fast);
  fast.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
  const auto refused = fit_quote_slice_channel(c.market, c.quotes, fast, nullptr);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  deam_iv_memo_clear();
}

// Screen-seeded Premium cache (SPY1s Phase 1, QuoteSliceDeamProfile::Screen): on the
// SPY-shaped grid it serves the same rows (statuses, admission, fit status) as the
// PriceChannel fit with every quote volatility CONFIRMED on the accurate kernel (within
// the inversion tolerance band of the Search values; confirms counted, none skipped under
// deam_confirm Always); under deam_confirm Band the gate skips confirms and the served
// rows stay the same; a Search fit on Screen and confirm keys without Screen are refused.
TEST(QuoteSliceVolSpace, ScreenSeededPremiumCacheServesConfirmedVolatilities) {
  const std::vector<GridCase> grid = {
      {0.03, false, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
      {0.15, true, QuoteSliceDividendModel::AggregateFundingPresentValue, std::nullopt},
      {0.75, false, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
      {2.0, true, QuoteSliceDividendModel::ContinuousYield, std::nullopt},
  };
  const double tol = QuoteSliceConfig{}.iv_tolerance;
  std::size_t confirms = 0, ambiguous = 0, routed = 0, compared = 0, band_skipped = 0;
  double max_dsigma = 0.0, max_band_dsigma = 0.0;
  for (const GridCase &g : grid) {
    SCOPED_TRACE("T=" + std::to_string(g.years));
    const double pv = g.model == QuoteSliceDividendModel::AggregateFundingPresentValue ? 4.0 : 0.0;
    const Chain c = spy_chain(g.years, g.strike_carry, pv);
    const QuoteSliceConfig off = price_config(g.model);
    QuoteSliceConfig screen = vol_config(off, true, QuoteSliceDeamProfile::Screen);
    screen.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
    deam_iv_memo_clear();
    const auto reference = fit_quote_slice_channel(c.market, c.quotes, off, nullptr);
    const auto seeded = fit_quote_slice_channel(c.market, c.quotes, screen, nullptr);
    ASSERT_TRUE(reference && seeded);
    const QuoteSliceDeamDiagnostics &work = seeded->diagnostics.deam;
    EXPECT_TRUE(work.used);
    EXPECT_GT(work.premium_inversions, 0u);
    EXPECT_EQ(work.confirms_skipped, 0u);
    confirms += work.confirms;
    ambiguous += work.ambiguous_edges;
    routed += work.seed_routed;
    EXPECT_EQ(seeded->fit_status, reference->fit_status);
    ASSERT_EQ(seeded->rows.size(), reference->rows.size());
    for (std::size_t i = 0; i < reference->rows.size(); ++i) {
      const QuoteSliceRow &p = seeded->rows[i];
      const QuoteSliceRow &r = reference->rows[i];
      EXPECT_EQ(p.quote_status, r.quote_status) << i;
      EXPECT_EQ(p.evaluation_status, r.evaluation_status) << i;
      EXPECT_EQ(p.admitted, r.admitted) << i;
      if (std::isfinite(p.market_iv) && std::isfinite(r.market_iv)) {
        ++compared;
        max_dsigma = std::max(max_dsigma, std::abs(p.market_iv - r.market_iv));
        EXPECT_LE(std::abs(p.market_iv - r.market_iv), 4.0 * tol) << i;
      }
    }
    // Band gate, k = 10: confirms skipped, the same rows served.
    QuoteSliceConfig band = screen;
    band.channel.deam_confirm = QuoteSliceDeamConfirm::Band;
    deam_iv_memo_clear();
    const auto gated = fit_quote_slice_channel(c.market, c.quotes, band, nullptr);
    ASSERT_TRUE(gated);
    band_skipped += gated->diagnostics.deam.confirms_skipped;
    EXPECT_EQ(gated->fit_status, reference->fit_status);
    for (std::size_t i = 0; i < reference->rows.size(); ++i) {
      EXPECT_EQ(gated->rows[i].quote_status, reference->rows[i].quote_status) << i;
      EXPECT_EQ(gated->rows[i].admitted, reference->rows[i].admitted) << i;
      if (std::isfinite(gated->rows[i].market_iv) && std::isfinite(reference->rows[i].market_iv)) {
        max_band_dsigma =
            std::max(max_band_dsigma, std::abs(gated->rows[i].market_iv - reference->rows[i].market_iv));
      }
    }
  }
  std::printf("[screen cache] %zu confirms, %zu ambiguous edges, %zu routed, %zu mid IVs compared, "
              "max |dsigma| vs Search %.3g; band gate skipped %zu confirms, max |dsigma| %.3g\n",
              confirms, ambiguous, routed, compared, max_dsigma, band_skipped, max_band_dsigma);
  EXPECT_GT(confirms, 0u);
  // SliceDeAmModel::seed_price_bound's Screen margin (2x the whole-grid bound 7.8e-4 max(P, 1))
  // is not inert: on these three chains 54 edges are re-priced with it, 34 with a zero margin
  // (bidless quotes sitting exactly on a zero floor price), so the threshold pins the margin.
  EXPECT_GE(ambiguous, 50u);
  EXPECT_GT(compared, 0u);
  EXPECT_GT(band_skipped, 0u);
  deam_iv_memo_clear();
  // Screen seeds exist only inside the Premium inversion; the confirm keys only on Screen.
  const Chain c = spy_chain(0.15, false, 0.0);
  const QuoteSliceConfig search_on_screen =
      vol_config(price_config(QuoteSliceDividendModel::ContinuousYield), true,
                 QuoteSliceDeamProfile::Screen);
  const auto refused = fit_quote_slice_channel(c.market, c.quotes, search_on_screen, nullptr);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  QuoteSliceConfig band_on_accurate =
      vol_config(price_config(QuoteSliceDividendModel::ContinuousYield), true);
  band_on_accurate.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
  band_on_accurate.channel.deam_confirm = QuoteSliceDeamConfirm::Band;
  const auto refused_band = fit_quote_slice_channel(c.market, c.quotes, band_on_accurate, nullptr);
  ASSERT_FALSE(refused_band);
  EXPECT_EQ(refused_band.error().code(), ErrorCode::InvalidArgument);
  deam_iv_memo_clear();
}

// Opt-in Fast certification grid (SPY1s Phase 1 arm D, QuoteSliceConfig::grid_kernel): the
// served prices are the grid's, so under grid_kernel Fast every served price moves by at most
// the Fast bound 1e-12 max(P, 1) (below the certificate's allowance) with the same rows served
// and the same statuses; the default is the accurate grid bit for bit; at least one price
// differs (the key is not inert).
TEST(QuoteSliceVolSpace, FastCertificationGridMovesServedPricesWithinTheFastBoundOnly) {
  std::size_t compared = 0, differing = 0;
  double max_rel = 0.0;
  for (const double years : {0.03, 0.15, 0.75}) {
    SCOPED_TRACE("T=" + std::to_string(years));
    const Chain c = spy_chain(years, false, 0.0);
    const QuoteSliceConfig off = price_config(QuoteSliceDividendModel::ContinuousYield);
    QuoteSliceConfig fast_grid = off;
    fast_grid.grid_kernel = QuoteSliceGridKernel::Fast;
    const auto reference = fit_quote_slice_channel(c.market, c.quotes, off, nullptr);
    const auto fast = fit_quote_slice_channel(c.market, c.quotes, fast_grid, nullptr);
    const auto again = fit_quote_slice_channel(c.market, c.quotes, off, nullptr);
    ASSERT_TRUE(reference && fast && again);
    expect_identical(*reference, *again, "accurate grid twice");
    EXPECT_EQ(fast->fit_status, reference->fit_status);
    ASSERT_EQ(fast->rows.size(), reference->rows.size());
    for (std::size_t i = 0; i < reference->rows.size(); ++i) {
      const QuoteSliceRow &a = reference->rows[i];
      const QuoteSliceRow &b = fast->rows[i];
      EXPECT_EQ(a.quote_status, b.quote_status) << i;
      EXPECT_EQ(a.evaluation_status, b.evaluation_status) << i;
      EXPECT_EQ(a.admitted, b.admitted) << i;
      if (std::isfinite(a.fitted_price) && std::isfinite(b.fitted_price)) {
        ++compared;
        const double rel = std::abs(a.fitted_price - b.fitted_price) / std::max(a.fitted_price, 1.0);
        max_rel = std::max(max_rel, rel);
        EXPECT_LE(rel, 1.0e-11) << i; // 10x the tested Fast price bound
        if (!same_bits(a.fitted_price, b.fitted_price)) ++differing;
      }
    }
  }
  std::printf("[fast grid] %zu served prices compared, %zu differ, max |dP| / max(P, 1) %.3g\n",
              compared, differing, max_rel);
  EXPECT_GT(compared, 0u);
  EXPECT_GT(differing, 0u);
}

// A scripted American model: an Accurate inversion returns 0.25 * target, a Fast
// one 0.25 * target + fast_offset (or fast_result when set); prices bound the
// identifiable range at [floor_price, cap_price].
class ScriptedModel final : public DeAmModel {
public:
  double fast_offset = 1.0e-9;
  std::optional<double> fast_result{};
  bool fast_fails = false;
  bool fast_kernel = true;
  double vega = 1.0;
  double floor_price = 0.0;
  double cap_price = 1.0e6;
  // Screen seed tier (SPY1s Phase 1): its edge prices sit screen_offset off the accurate
  // ones, inside the tested bound screen_bound.
  bool screen_kernel = true;
  double screen_offset = 0.0;
  double screen_bound = 0.0;
  mutable int fast_calls = 0;
  mutable int accurate_calls = 0;
  mutable int price_calls = 0;
  mutable int screen_price_calls = 0;
  mutable int throw_on_price_call = -1; // the price call (0-based) that throws bad_alloc

  double effective_yield_of(double calendar_yield) const override { return calendar_yield; }
  double local_forward_of(double, double) const override { return 100.0; }
  double reference_yield() const override { return 0.0; }
  Result<double> price(const QuoteSliceQuote &, double sigma, double,
                       DeAmKernel kernel) const override {
    if (price_calls++ == throw_on_price_call) throw std::bad_alloc();
    const double exact = sigma == kIvMin ? floor_price : cap_price;
    if (kernel == DeAmKernel::Screen) {
      ++screen_price_calls;
      return Ok(exact + screen_offset);
    }
    return Ok(exact);
  }
  bool has_screen_kernel() const override { return screen_kernel; }
  double seed_price_bound(double, DeAmKernel kernel) const override {
    return kernel == DeAmKernel::Screen ? screen_bound : 0.0;
  }
  Result<double> implied_vol(const QuoteSliceQuote &, double target, double,
                             DeAmKernel kernel) const override {
    if (kernel == DeAmKernel::Accurate) {
      ++accurate_calls;
      return Ok(0.25 * target);
    }
    ++fast_calls;
    if (fast_fails) return Err(ErrorCode::Unavailable, "scripted Fast failure");
    return Ok(fast_result.value_or(0.25 * target + fast_offset));
  }
  bool has_fast_kernel() const override { return fast_kernel; }
  double black_vega(const QuoteSliceQuote &, double, double) const override { return vega; }
};

struct ViewRun {
  Result<double> mid = Err(ErrorCode::Unavailable, "not run");
  QuoteSliceDeamDiagnostics work{};
  int fast_calls = 0;
  int accurate_calls = 0;
};

ViewRun run_mid(const ScriptedModel &model, QuoteSliceDeamProfile profile) {
  const std::vector<QuoteSliceQuote> quotes = {{100.0, Side::Call, 1.0, 1.2}};
  DeAmIvCache cache;
  cache.ivs.assign(quotes.size(), DeAmQuoteIv{});
  ViewRun run;
  DeAmIvView view(cache, model, quotes, profile, run.work);
  run.mid = view.mid_iv(0);
  run.fast_calls = model.fast_calls;
  run.accurate_calls = model.accurate_calls;
  return run;
}

TEST(DeAmIvCache, FastInversionWithoutAGuaranteedBoundIsRedoneOnTheAccurateKernel) {
  constexpr double kAccurate = 0.25 * 1.1;
  {
    ScriptedModel model; // usable vega: the Fast root is served
    const ViewRun run = run_mid(model, QuoteSliceDeamProfile::Fast);
    ASSERT_TRUE(run.mid);
    EXPECT_DOUBLE_EQ(*run.mid, kAccurate + model.fast_offset);
    EXPECT_EQ(run.fast_calls, 1);
    EXPECT_EQ(run.accurate_calls, 0);
    EXPECT_EQ(run.work.inversions, 1u);
    EXPECT_EQ(run.work.accurate_fallbacks, 0u);
  }
  {
    ScriptedModel model; // vega below kDeAmFastMinRelativeVega * max(target, 1)
    model.vega = 0.5 * kDeAmFastMinRelativeVega * 1.1;
    const ViewRun run = run_mid(model, QuoteSliceDeamProfile::Fast);
    ASSERT_TRUE(run.mid);
    EXPECT_DOUBLE_EQ(*run.mid, kAccurate);
    EXPECT_EQ(run.accurate_calls, 1);
    EXPECT_EQ(run.work.inversions, 2u);
    EXPECT_EQ(run.work.accurate_fallbacks, 1u);
  }
  for (const double bound : {kIvMin, kIvMax}) {
    ScriptedModel model; // a Fast root at a volatility bound
    model.fast_result = bound;
    const ViewRun run = run_mid(model, QuoteSliceDeamProfile::Fast);
    ASSERT_TRUE(run.mid);
    EXPECT_DOUBLE_EQ(*run.mid, kAccurate) << bound;
    EXPECT_EQ(run.work.accurate_fallbacks, 1u) << bound;
  }
  {
    ScriptedModel model; // a Fast failure
    model.fast_fails = true;
    const ViewRun run = run_mid(model, QuoteSliceDeamProfile::Fast);
    ASSERT_TRUE(run.mid);
    EXPECT_DOUBLE_EQ(*run.mid, kAccurate);
    EXPECT_EQ(run.work.accurate_fallbacks, 1u);
  }
  {
    ScriptedModel model; // no Fast kernel (the dated-cash lattice): accurate only
    model.fast_kernel = false;
    const ViewRun run = run_mid(model, QuoteSliceDeamProfile::Fast);
    ASSERT_TRUE(run.mid);
    EXPECT_DOUBLE_EQ(*run.mid, kAccurate);
    EXPECT_EQ(run.fast_calls, 0);
    EXPECT_EQ(run.work.inversions, 1u);
    EXPECT_EQ(run.work.accurate_fallbacks, 0u);
  }
  {
    ScriptedModel model; // the Accurate profile never calls the Fast kernel
    const ViewRun run = run_mid(model, QuoteSliceDeamProfile::Accurate);
    ASSERT_TRUE(run.mid);
    EXPECT_DOUBLE_EQ(*run.mid, kAccurate);
    EXPECT_EQ(run.fast_calls, 0);
    EXPECT_EQ(run.accurate_calls, 1);
  }
}

TEST(DeAmIvCache, BandEdgesFollowTheChannelBuilderAndAreComputedOnce) {
  const std::vector<QuoteSliceQuote> quotes = {
      {100.0, Side::Call, 1.0, 1.2},  // two finite edges
      {100.0, Side::Put, 0.4, 1.2},   // bid at or below the floor price: no lower edge
      {105.0, Side::Call, 1.0, 9.0},  // ask at or above the cap price: no upper edge
      {110.0, Side::Call, 0.1, 0.4},  // band entirely below the floor: unusable
  };
  ScriptedModel model;
  model.floor_price = 0.5;
  model.cap_price = 8.0;
  DeAmIvCache cache;
  cache.ivs.assign(quotes.size(), DeAmQuoteIv{});
  QuoteSliceDeamDiagnostics work;
  DeAmIvView view(cache, model, quotes, QuoteSliceDeamProfile::Accurate, work);
  const DeAmQuoteIv &both = view.band(0);
  EXPECT_TRUE(both.well_posed);
  EXPECT_DOUBLE_EQ(both.bid_iv, 0.25);
  EXPECT_DOUBLE_EQ(both.ask_iv, 0.30);
  const DeAmQuoteIv &upper_only = view.band(1);
  EXPECT_TRUE(upper_only.well_posed);
  EXPECT_EQ(upper_only.bid_iv, 0.0);
  EXPECT_DOUBLE_EQ(upper_only.ask_iv, 0.30);
  const DeAmQuoteIv &lower_only = view.band(2);
  EXPECT_TRUE(lower_only.well_posed);
  EXPECT_DOUBLE_EQ(lower_only.bid_iv, 0.25);
  EXPECT_TRUE(std::isinf(lower_only.ask_iv));
  EXPECT_FALSE(view.band(3).well_posed);
  EXPECT_EQ(work.model_prices, 8u);
  EXPECT_EQ(work.inversions, 4u);
  // Every value is computed at most once per cache.
  const int prices = model.price_calls;
  const int inversions = model.accurate_calls;
  for (std::size_t i = 0; i < quotes.size(); ++i) static_cast<void>(view.band(i));
  ASSERT_TRUE(view.mid_iv(0));
  ASSERT_TRUE(view.mid_iv(0));
  EXPECT_EQ(model.price_calls, prices);
  EXPECT_EQ(model.accurate_calls, inversions + 1);
}

// Bound-decided band edges on the Screen seed tier (SPY1s Phase 1, gap report 1a): an edge
// comparison is decided on the Screen price only when the quote clears it by more than
// the tier's tested bound; a quote inside the bound has THAT edge re-priced on the accurate
// kernel first, so the band is admitted / refused exactly as under the accurate profile.
TEST(DeAmIvCache, ScreenBandEdgesAreDecidedWithTheBoundOrRepricedAccurately) {
  const std::vector<QuoteSliceQuote> quotes = {
      {100.0, Side::Call, 0.505, 1.2}, // bid 0.005 above the accurate floor: inside the bound
      {100.0, Side::Put, 1.0, 1.2},    // clears both edges by far
      {105.0, Side::Call, 1.0, 7.995}, // ask 0.005 below the accurate cap: inside the bound
      {110.0, Side::Call, 0.1, 0.4},   // band entirely below the floor, far from it
  };
  ScriptedModel model;
  model.floor_price = 0.5;
  model.cap_price = 8.0;
  model.screen_offset = 0.01; // Screen prices the floor at 0.51 and the cap at 8.01 ...
  model.screen_bound = 0.02;  // ... inside a tested bound of 0.02
  QuoteSliceDeamDiagnostics accurate_work;
  DeAmIvCache accurate_cache;
  accurate_cache.ivs.assign(quotes.size(), DeAmQuoteIv{});
  DeAmIvView accurate(accurate_cache, model, quotes, QuoteSliceDeamProfile::Accurate,
                      accurate_work, QuoteSliceDeamInversion::Premium);
  QuoteSliceDeamDiagnostics screen_work;
  DeAmIvCache screen_cache;
  screen_cache.ivs.assign(quotes.size(), DeAmQuoteIv{});
  DeAmIvView screen(screen_cache, model, quotes, QuoteSliceDeamProfile::Screen, screen_work,
                    QuoteSliceDeamInversion::Premium);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    SCOPED_TRACE(i);
    const DeAmQuoteIv &a = accurate.band(i);
    const DeAmQuoteIv &s = screen.band(i);
    EXPECT_EQ(s.well_posed, a.well_posed);
    EXPECT_TRUE(same_bits(s.bid_iv, a.bid_iv));
    EXPECT_TRUE(same_bits(s.ask_iv, a.ask_iv));
  }
  // Strictly on the Screen prices quote 0's bid (0.505) would sit BELOW the Screen floor
  // (0.51) and lose its lower edge; the accurate floor (0.5) gives it one.
  EXPECT_GT(screen.band(0).bid_iv, 0.0);
  EXPECT_TRUE(std::isfinite(screen.band(2).ask_iv));
  // Two ambiguous edges (quote 0's floor, quote 2's cap) were re-priced accurately; the
  // other six edges were decided on Screen. Quote 3's floor is 0.1 away: decided on Screen.
  EXPECT_EQ(screen_work.ambiguous_edges, 2u);
  EXPECT_EQ(screen_work.model_prices, 2u * quotes.size() + 2u);
  EXPECT_EQ(model.screen_price_calls, 2 * static_cast<int>(quotes.size()));
  EXPECT_EQ(accurate_work.ambiguous_edges, 0u);
  // The published edges are the settled ones: the vol-space builder's own mid test
  // (mid > floor && mid < cap) then agrees with the accurate decision.
  EXPECT_DOUBLE_EQ(screen.band(0).floor_price, 0.5);
  EXPECT_DOUBLE_EQ(screen.band(0).cap_price, 8.01);
  EXPECT_DOUBLE_EQ(screen.band(2).cap_price, 8.0);
  EXPECT_DOUBLE_EQ(screen.band(1).floor_price, 0.51);
}

// review-L2a minor (fixed in lane L2c): a band fill that throws mid-way (here the cap
// price, after the floor price) publishes nothing, so the next request refills the
// whole band instead of serving a half-filled entry as final.
TEST(DeAmIvCache, ABandFillThatThrowsIsRefilledNotServedHalfFilled) {
  const std::vector<QuoteSliceQuote> quotes = {{100.0, Side::Call, 1.0, 1.2}};
  ScriptedModel model;
  model.floor_price = 0.5;
  model.cap_price = 8.0;
  model.throw_on_price_call = 1; // the second model price of the fill: the cap price
  DeAmIvCache cache;
  cache.ivs.assign(quotes.size(), DeAmQuoteIv{});
  QuoteSliceDeamDiagnostics work;
  DeAmIvView view(cache, model, quotes, QuoteSliceDeamProfile::Accurate, work);
  EXPECT_THROW(static_cast<void>(view.band(0)), std::bad_alloc);
  EXPECT_EQ(cache.ivs[0].computed & DeAmQuoteIv::kBand, 0);
  const DeAmQuoteIv &band = view.band(0);
  EXPECT_NE(band.computed & DeAmQuoteIv::kBand, 0);
  EXPECT_TRUE(band.well_posed);
  EXPECT_DOUBLE_EQ(band.floor_price, 0.5);
  EXPECT_DOUBLE_EQ(band.cap_price, 8.0);
  EXPECT_DOUBLE_EQ(band.bid_iv, 0.25);
  EXPECT_DOUBLE_EQ(band.ask_iv, 0.30);
  EXPECT_EQ(model.price_calls, 4); // two of the failed fill, two of the refill
}

// A prepared fit and its inputs with every key field away from its default.
struct KeyInputs {
  QuoteSliceFit fit;
  std::vector<QuoteSliceQuote> quotes;
  QuoteSliceConfig config;
};

KeyInputs key_inputs() {
  KeyInputs in;
  in.fit.market = QuoteSliceMarket{670.0, 0.15, 0.16, 0.04, 0.012, 3.0};
  in.fit.diagnostics.forward = 671.5;
  in.fit.diagnostics.discount = 0.99;
  in.fit.diagnostics.effective_rate = 0.042;
  in.fit.diagnostics.effective_yield = 0.013;
  in.fit.dividend_model = QuoteSliceDividendModel::DatedCash;
  in.fit.al_opts = AlOpts{};
  in.fit.discrete_div_steps = 301;
  in.fit.discrete_div_scheme = DiscreteDivScheme::SplicedCrr;
  in.fit.variance_clock_dividends = {{0.05, 1.8}, {0.12, 1.8}};
  in.quotes = {{650.0, Side::Call, 30.0, 30.5, 0.011}, {700.0, Side::Put, 35.0, 35.6, std::nullopt}};
  in.config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  in.config.channel.deam_cache = true;
  return in;
}

using KeyEdit = std::function<void(KeyInputs &)>;

TEST(DeAmIvCache, MemoKeyCoversEveryInputThatChangesAnInversion) {
  const KeyInputs base = key_inputs();
  const DeAmIvKey reference = deam_iv_key(base.fit, base.quotes, base.config);
  ASSERT_FALSE(reference.words.empty());
  {
    const KeyInputs same = key_inputs(); // a pure function of its inputs
    const DeAmIvKey again = deam_iv_key(same.fit, same.quotes, same.config);
    EXPECT_EQ(again.words, reference.words);
    EXPECT_EQ(again.hash, reference.hash);
  }
  const std::vector<std::pair<const char *, KeyEdit>> changes = {
      {"spot", [](KeyInputs &in) { in.fit.market.spot = 671.0; }},
      {"variance years", [](KeyInputs &in) { in.fit.market.variance_years = 0.151; }},
      {"calendar years", [](KeyInputs &in) { in.fit.market.calendar_years = 0.161; }},
      {"rate", [](KeyInputs &in) { in.fit.market.rate = 0.041; }},
      {"reference yield", [](KeyInputs &in) { in.fit.market.continuous_yield = 0.013; }},
      {"dividend pv", [](KeyInputs &in) { in.fit.market.dividend_pv = 3.5; }},
      {"forward", [](KeyInputs &in) { in.fit.diagnostics.forward = 671.6; }},
      {"discount", [](KeyInputs &in) { in.fit.diagnostics.discount = 0.991; }},
      {"effective rate", [](KeyInputs &in) { in.fit.diagnostics.effective_rate = 0.043; }},
      {"effective yield", [](KeyInputs &in) { in.fit.diagnostics.effective_yield = 0.014; }},
      {"dividend model",
       [](KeyInputs &in) { in.fit.dividend_model = QuoteSliceDividendModel::ContinuousYield; }},
      {"lattice steps", [](KeyInputs &in) { in.fit.discrete_div_steps = 401; }},
      {"lattice scheme",
       [](KeyInputs &in) { in.fit.discrete_div_scheme = DiscreteDivScheme::EscrowedBbs; }},
      {"AlOpts absent", [](KeyInputs &in) { in.fit.al_opts.reset(); }},
      {"AlOpts collocation", [](KeyInputs &in) { in.fit.al_opts->n_collocation = 16; }},
      {"AlOpts quadrature", [](KeyInputs &in) { in.fit.al_opts->n_quadrature = 32; }},
      {"AlOpts price quadrature", [](KeyInputs &in) { in.fit.al_opts->n_quad_price = 48; }},
      {"AlOpts newton", [](KeyInputs &in) { in.fit.al_opts->max_newton_iter = 9; }},
      {"AlOpts tol", [](KeyInputs &in) { in.fit.al_opts->tol = 1.0e-11; }},
      {"AlOpts profile", [](KeyInputs &in) { in.fit.al_opts->profile = AlProfile::Fast; }},
      {"iv tolerance", [](KeyInputs &in) { in.config.iv_tolerance = 1.0e-8; }},
      {"iv iterations", [](KeyInputs &in) { in.config.max_iv_iterations = 65; }},
      {"cache kernel",
       [](KeyInputs &in) { in.config.channel.deam_profile = QuoteSliceDeamProfile::Fast; }},
      {"dividend date", [](KeyInputs &in) { in.fit.variance_clock_dividends[0].tau = 0.06; }},
      {"dividend amount", [](KeyInputs &in) { in.fit.variance_clock_dividends[1].amount = 1.9; }},
      {"dividend count", [](KeyInputs &in) { in.fit.variance_clock_dividends.pop_back(); }},
      {"quote strike", [](KeyInputs &in) { in.quotes[0].strike = 655.0; }},
      {"quote side", [](KeyInputs &in) { in.quotes[0].side = Side::Put; }},
      {"quote bid", [](KeyInputs &in) { in.quotes[1].bid = 35.05; }},
      {"quote ask", [](KeyInputs &in) { in.quotes[1].ask = 35.65; }},
      {"quote carry value", [](KeyInputs &in) { in.quotes[0].continuous_yield = 0.0115; }},
      {"quote carry supplied", [](KeyInputs &in) { in.quotes[1].continuous_yield = 0.0; }},
      {"quote count", [](KeyInputs &in) { in.quotes.pop_back(); }},
      {"quote order", [](KeyInputs &in) { std::swap(in.quotes[0], in.quotes[1]); }},
  };
  for (const auto &[name, edit] : changes) {
    KeyInputs in = key_inputs();
    edit(in);
    const DeAmIvKey key = deam_iv_key(in.fit, in.quotes, in.config);
    EXPECT_NE(key.words, reference.words) << name;
  }
  // Rung-level fit settings never change an inversion: every rung of the same
  // chain shares one cache (the L2c rung memo reuses this key).
  const std::vector<std::pair<const char *, KeyEdit>> inert = {
      {"cache flag", [](KeyInputs &in) { in.config.channel.deam_cache = false; }},
      {"fit method", [](KeyInputs &in) { in.config.method = QuoteSliceFitMethod::ChannelSpline; }},
      {"curve family", [](KeyInputs &in) { in.config.channel.family = VolCurveKind::Essvi; }},
      {"repair budget", [](KeyInputs &in) { in.config.channel.repair_half_spreads = 0.5; }},
      {"quality gate", [](KeyInputs &in) { in.config.quality.min_bid = 0.05; }},
      {"min quotes", [](KeyInputs &in) { in.config.min_quotes = 8; }},
  };
  for (const auto &[name, edit] : inert) {
    KeyInputs in = key_inputs();
    edit(in);
    const DeAmIvKey key = deam_iv_key(in.fit, in.quotes, in.config);
    EXPECT_EQ(key.words, reference.words) << name;
  }
}

TEST(DeAmIvCache, MemoIsBoundedAndAHeldCacheOutlivesItsEviction) {
  deam_iv_memo_clear();
  KeyInputs in = key_inputs();
  bool reused = true;
  const auto held = deam_iv_memo(deam_iv_key(in.fit, in.quotes, in.config), in.quotes.size(),
                                 reused);
  ASSERT_TRUE(held);
  EXPECT_FALSE(reused);
  ASSERT_EQ(held->ivs.size(), in.quotes.size());
  held->ivs[0].mid_iv = 0.25;
  {
    const auto hit = deam_iv_memo(deam_iv_key(in.fit, in.quotes, in.config), in.quotes.size(),
                                  reused);
    EXPECT_TRUE(reused);
    EXPECT_EQ(hit.get(), held.get());
  }
  for (std::size_t i = 0; i < kDeAmIvMemoCapacity; ++i) {
    in.fit.market.spot += 1.0;
    static_cast<void>(deam_iv_memo(deam_iv_key(in.fit, in.quotes, in.config), in.quotes.size(),
                                   reused));
    EXPECT_FALSE(reused) << i;
  }
  EXPECT_EQ(deam_iv_memo_size(), kDeAmIvMemoCapacity);
  // The first cache was evicted: only this test still holds it, intact.
  EXPECT_EQ(held.use_count(), 1);
  EXPECT_EQ(held->ivs[0].mid_iv, 0.25);
  const KeyInputs first = key_inputs();
  const auto fresh = deam_iv_memo(deam_iv_key(first.fit, first.quotes, first.config),
                                  first.quotes.size(), reused);
  EXPECT_FALSE(reused);
  EXPECT_NE(fresh.get(), held.get());
  EXPECT_EQ(deam_iv_memo_size(), kDeAmIvMemoCapacity);
  deam_iv_memo_clear();
  EXPECT_EQ(deam_iv_memo_size(), 0u);
  EXPECT_EQ(held->ivs[0].mid_iv, 0.25);
}

} // namespace
} // namespace atx::vol
