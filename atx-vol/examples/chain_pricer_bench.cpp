// chain_pricer_bench.cpp — the unified library layer, end to end, on real SPY.
//
// Proves the goal's five-step lifecycle and its parallel-inversion evaluator:
//   1. build an OptionChain (each option a unique id) from the SPY OPRA slice;
//   2. pass it to a PricerFitter with a config;
//   3. the fitter fits and OWNS the surface (unique_ptr<FittedSurface>);
//   4. value the whole chain in parallel across {1,2,4,8} threads, reporting the
//      scaling and asserting the result is BIT-IDENTICAL across thread counts
//      (the deterministic-layout claim);
//   5. update bid/ask for a set of option ids and re-value — the tick-to-quote
//      path — showing the band IVs move.
//
// The heavy per-option work is the seeded American-IV inversion of bid/ask/mid
// through the fit's correction cache. Model price/IV/Greeks come from the
// fitted surface's cached hot path.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/parallel_for.hpp"
#include "atx/vol/pricer_fitter.hpp"

using namespace atx::vol;

namespace {

double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// Count populated (non-NaN) cells across the band columns — the actual cold
// inversions performed (bid/ask/mid with a live quote).
std::size_t count_band_inversions(const ChainValuation &v) {
  std::size_t n = 0;
  for (double x : v.bid_iv)
    n += std::isfinite(x) ? 1u : 0u;
  for (double x : v.ask_iv)
    n += std::isfinite(x) ? 1u : 0u;
  for (double x : v.mid_iv)
    n += std::isfinite(x) ? 1u : 0u;
  return n;
}

bool same(double a, double b) {
  if (std::isnan(a) && std::isnan(b))
    return true;
  return a == b;
}

bool identical(const ChainValuation &a, const ChainValuation &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a.ids[i] != b.ids[i])
      return false;
    if (!same(a.model_iv[i], b.model_iv[i]))
      return false;
    if (!same(a.model_price[i], b.model_price[i]))
      return false;
    if (!same(a.bid_iv[i], b.bid_iv[i]))
      return false;
    if (!same(a.ask_iv[i], b.ask_iv[i]))
      return false;
    if (!same(a.mid_iv[i], b.mid_iv[i]))
      return false;
    if (!same(a.greeks[i].price, b.greeks[i].price))
      return false;
    if (!same(a.greeks[i].delta, b.greeks[i].delta))
      return false;
  }
  return true;
}

struct FieldCase {
  const char *name;
  OutputField fields;
};

struct TimingStats {
  double median_ms{0.0};
  double p95_ms{0.0};
  double cv{0.0};
};

struct ValuationMeasurement {
  TimingStats timing{};
  ChainValuation sample{};
};

std::optional<ValuationMeasurement> measure_value_chain(const PricerFitter &fitter,
                                                        const OptionChain &chain,
                                                        OutputField fields, unsigned threads) {
  constexpr std::size_t kSamples = 5u;
  if (!fitter.value_chain(chain, fields, threads).has_value()) {
    return std::nullopt;
  }
  std::array<double, kSamples> elapsed{};
  ChainValuation sample;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (std::size_t i = 0u; i < kSamples; ++i) {
    const double t0 = now_ms();
    auto result = fitter.value_chain(chain, fields, threads);
    elapsed[i] = now_ms() - t0;
    if (!result.has_value()) {
      return std::nullopt;
    }
    if (i == 0u) {
      sample = std::move(*result);
    }
    sum += elapsed[i];
    sum_sq += elapsed[i] * elapsed[i];
  }
  std::sort(elapsed.begin(), elapsed.end());
  const double mean = sum / static_cast<double>(kSamples);
  const double variance = std::max(0.0, sum_sq / static_cast<double>(kSamples) - mean * mean);
  const TimingStats timing{elapsed[kSamples / 2u], elapsed.back(),
                           mean > 0.0 ? std::sqrt(variance) / mean : 0.0};
  return ValuationMeasurement{timing, std::move(sample)};
}

void print_field_breakdown(const char *tier_name, const PricerFitter &fitter,
                           const OptionChain &chain) {
  constexpr FieldCase cases[] = {
      {"model_iv", OutputField::ModelIV},
      {"model_price", OutputField::ModelPrice},
      {"greeks", OutputField::Greeks},
      {"model_trio", OutputField::ModelIV | OutputField::ModelPrice | OutputField::Greeks},
      {"mid_iv", OutputField::MidIV},
      {"bands", OutputField::Bands},
      {"all", OutputField::All},
  };
  constexpr unsigned thread_counts[] = {1u, 8u};

  std::printf("value_chain field breakdown (real SPY, %s):\n", tier_name);
  std::printf("%14s %8s %10s %10s %8s %14s\n", "fields", "threads", "med_ms", "p95_ms", "cv",
              "options/s");
  for (const FieldCase &c : cases) {
    for (const unsigned nt : thread_counts) {
      const auto measured = measure_value_chain(fitter, chain, c.fields, nt);
      if (!measured.has_value()) {
        std::fprintf(stderr, "value_chain(%s) measurement failed\n", c.name);
        continue;
      }
      const double options_per_s =
          measured->timing.median_ms > 0.0
              ? 1000.0 * static_cast<double>(measured->sample.size()) / measured->timing.median_ms
              : 0.0;
      std::printf("%14s %8u %10.1f %10.1f %8.3f %14.0f\n", c.name, nt, measured->timing.median_ms,
                  measured->timing.p95_ms, measured->timing.cv, options_per_s);
    }
  }
  std::printf("\n");
}

void print_authoritative_t8_comparison(const PricerFitter &cold, const PricerFitter &representative,
                                       const PricerFitter &bank, const OptionChain &chain) {
  constexpr FieldCase cases[] = {
      {"model_price", OutputField::ModelPrice},
      {"greeks", OutputField::Greeks},
      {"all", OutputField::All},
  };
  struct Tier {
    const char *name;
    const PricerFitter *fitter;
  };
  const Tier tiers[] = {
      {"ColdReference", &cold}, {"Representative", &representative}, {"CarryBank", &bank}};
  std::printf("authoritative warm+5 t8 comparison (same process):\n");
  std::printf("%13s %16s %10s %10s %8s %10s\n", "fields", "tier", "med_ms", "p95_ms", "cv",
              "speedup");
  for (const FieldCase &field : cases) {
    std::array<std::optional<ValuationMeasurement>, 3u> measured;
    for (std::size_t tier = 0u; tier < measured.size(); ++tier) {
      measured[tier] = measure_value_chain(*tiers[tier].fitter, chain, field.fields, 8u);
    }
    if (!measured[0].has_value()) {
      std::fprintf(stderr, "cold t8 comparison failed for %s\n", field.name);
      continue;
    }
    const double cold_ms = measured[0]->timing.median_ms;
    for (std::size_t tier = 0u; tier < measured.size(); ++tier) {
      if (!measured[tier].has_value()) {
        std::fprintf(stderr, "%s t8 comparison failed for %s\n", tiers[tier].name, field.name);
        continue;
      }
      const TimingStats &timing = measured[tier]->timing;
      const double speedup = timing.median_ms > 0.0 ? cold_ms / timing.median_ms : 0.0;
      std::printf("%13s %16s %10.1f %10.1f %8.3f %9.2fx\n", field.name, tiers[tier].name,
                  timing.median_ms, timing.p95_ms, timing.cv, speedup);
    }
  }
  std::printf("\n");
}

struct ErrorStats {
  double median{0.0};
  double p99{0.0};
  double maximum{0.0};
  std::size_t count{0u};
};

template <class FastValue, class ColdValue, class Include>
ErrorStats absolute_error_stats(std::size_t n, FastValue &&fast_value, ColdValue &&cold_value,
                                Include &&include) {
  std::vector<double> errors;
  errors.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!include(i)) {
      continue;
    }
    const double fast = fast_value(i);
    const double cold = cold_value(i);
    if (std::isfinite(fast) && std::isfinite(cold)) {
      errors.push_back(std::fabs(fast - cold));
    }
  }
  if (errors.empty()) {
    return {};
  }
  std::sort(errors.begin(), errors.end());
  const std::size_t p99_index = errors.size() - 1u - errors.size() / 100u;
  return ErrorStats{errors[errors.size() / 2u], errors[p99_index], errors.back(), errors.size()};
}

template <class FastValue, class ColdValue>
ErrorStats absolute_error_stats(std::size_t n, FastValue &&fast_value, ColdValue &&cold_value) {
  return absolute_error_stats(n, std::forward<FastValue>(fast_value),
                              std::forward<ColdValue>(cold_value),
                              [](std::size_t) noexcept { return true; });
}

void print_fast_approximation_error(const char *tier_name, const PricerFitter &fast_fitter,
                                    const OptionChain &chain) {
  constexpr OutputField fields = OutputField::ModelPrice | OutputField::Greeks;
  const auto fast = fast_fitter.value_chain(chain, fields, 8u);
  if (!fast.has_value()) {
    std::fprintf(stderr, "fast model comparison valuation failed\n");
    return;
  }
  const ChainSnapshot quotes = chain.snapshot();
  if (fast->size() != quotes.size()) {
    std::fprintf(stderr, "fast model comparison size mismatch\n");
    return;
  }
  const VolaSession &session = fast_fitter.surface()->session();
  const SessionInputs &inputs = session.inputs();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> cold_price(quotes.size(), nan);
  std::vector<AmericanGreeks> cold_greeks(quotes.size());
  parallel_for(quotes.size(), 8u, [&](std::size_t i) {
    const double K = quotes.strike[i];
    const double T = quotes.T[i];
    if (!(K > 0.0) || !(T > 0.0)) {
      cold_greeks[i].price = nan;
      return;
    }
    const double sigma = session.iv(K, T);
    const double rate = session.rate_at(T);
    const double q = session.q_eff_at(T);
    const auto price = american_price(inputs.S, K, T, sigma, rate, q, quotes.side[i],
                                      inputs.deam.method, inputs.deam.al_opts);
    const auto greeks = american_greeks_fd(inputs.S, K, T, sigma, rate, q, quotes.side[i],
                                           inputs.deam.method, inputs.deam.al_opts);
    cold_price[i] = price.has_value() ? *price : nan;
    if (greeks.has_value()) {
      cold_greeks[i] = *greeks;
    } else {
      cold_greeks[i].price = nan;
    }
  });

  std::vector<std::uint8_t> two_sided(quotes.size(), 0u);
  std::vector<std::uint8_t> otm(quotes.size(), 0u);
  std::vector<std::uint8_t> delta25(quotes.size(), 0u);
  std::vector<double> strangle_fast;
  std::vector<double> strangle_cold;
  std::vector<double> strangle_half_spread;
  for (std::size_t i = 0u; i < quotes.size(); ++i) {
    two_sided[i] =
        static_cast<std::uint8_t>(std::isfinite(quotes.bid[i]) && std::isfinite(quotes.ask[i]) &&
                                  quotes.bid[i] > 0.0 && quotes.ask[i] > quotes.bid[i]);
    const double forward = session.forward_at(quotes.T[i]);
    otm[i] =
        static_cast<std::uint8_t>(std::isfinite(forward) && forward > 0.0 &&
                                  ((quotes.side[i] == Side::Call && quotes.strike[i] >= forward) ||
                                   (quotes.side[i] == Side::Put && quotes.strike[i] <= forward)));
  }
  for (std::size_t begin = 0u; begin < quotes.size();) {
    std::size_t end = begin + 1u;
    while (end < quotes.size() && quotes.T[end] == quotes.T[begin]) {
      ++end;
    }
    std::array<std::size_t, 2u> strangle_legs{quotes.size(), quotes.size()};
    for (const Side side : {Side::Call, Side::Put}) {
      const double target = side == Side::Call ? 0.25 : -0.25;
      std::size_t best = quotes.size();
      double best_distance = std::numeric_limits<double>::infinity();
      for (std::size_t i = begin; i < end; ++i) {
        if (quotes.side[i] != side || two_sided[i] == 0u || otm[i] == 0u ||
            !std::isfinite(cold_greeks[i].delta)) {
          continue;
        }
        const double distance = std::fabs(cold_greeks[i].delta - target);
        if (distance < best_distance) {
          best = i;
          best_distance = distance;
        }
      }
      if (best != quotes.size()) {
        delta25[best] = 1u;
        strangle_legs[side == Side::Call ? 0u : 1u] = best;
      }
    }
    if (strangle_legs[0] != quotes.size() && strangle_legs[1] != quotes.size()) {
      const std::size_t call = strangle_legs[0];
      const std::size_t put = strangle_legs[1];
      strangle_fast.push_back(fast->model_price[call] + fast->model_price[put]);
      strangle_cold.push_back(cold_price[call] + cold_price[put]);
      strangle_half_spread.push_back(
          0.5 * ((quotes.ask[call] - quotes.bid[call]) + (quotes.ask[put] - quotes.bid[put])));
    }
    begin = end;
  }

  using GreekMember = double AmericanGreeks::*;
  struct GreekColumn {
    const char *name;
    GreekMember member;
  };
  constexpr GreekColumn greek_columns[] = {
      {"delta", &AmericanGreeks::delta}, {"gamma", &AmericanGreeks::gamma},
      {"vega", &AmericanGreeks::vega},   {"theta", &AmericanGreeks::theta},
      {"rho", &AmericanGreeks::rho},     {"vanna", &AmericanGreeks::vanna},
      {"volga", &AmericanGreeks::volga}, {"charm", &AmericanGreeks::charm},
  };

  std::printf("%s query error vs same-surface cold Andersen-Lake/FD:\n", tier_name);
  std::printf("%14s %14s %14s %14s %10s\n", "column", "median_abs", "p99_abs", "max_abs",
              "samples");
  const ErrorStats price_stats = absolute_error_stats(
      fast->size(), [&](std::size_t i) { return fast->model_price[i]; },
      [&](std::size_t i) { return cold_price[i]; });
  std::printf("%14s %14.6g %14.6g %14.6g %10zu\n", "price", price_stats.median, price_stats.p99,
              price_stats.maximum, price_stats.count);
  for (const GreekColumn &column : greek_columns) {
    const ErrorStats stats = absolute_error_stats(
        fast->size(), [&](std::size_t i) { return fast->greeks[i].*(column.member); },
        [&](std::size_t i) { return cold_greeks[i].*(column.member); });
    std::printf("%14s %14.6g %14.6g %14.6g %10zu\n", column.name, stats.median, stats.p99,
                stats.maximum, stats.count);
  }

  struct PriceSlice {
    const char *name;
    const std::vector<std::uint8_t> *mask;
  };
  const PriceSlice price_slices[] = {
      {"all", nullptr}, {"two-sided", &two_sided}, {"otm", &otm}, {"25-delta", &delta25}};
  std::printf("\n%12s %11s %11s %11s %11s %11s %11s %8s\n", "price slice", "med_px", "p99_px",
              "max_px", "med_halfSp", "p99_halfSp", "max_halfSp", "samples");
  for (const PriceSlice &slice : price_slices) {
    const auto included = [&](std::size_t i) noexcept {
      return slice.mask == nullptr || (*slice.mask)[i] != 0u;
    };
    const ErrorStats price = absolute_error_stats(
        fast->size(), [&](std::size_t i) { return fast->model_price[i]; },
        [&](std::size_t i) { return cold_price[i]; }, included);
    const ErrorStats spread = absolute_error_stats(
        fast->size(),
        [&](std::size_t i) {
          const double half_spread = 0.5 * (quotes.ask[i] - quotes.bid[i]);
          return std::fabs(fast->model_price[i] - cold_price[i]) / std::max(0.01, half_spread);
        },
        [](std::size_t) { return 0.0; }, included);
    std::printf("%12s %11.5g %11.5g %11.5g %11.5g %11.5g %11.5g %8zu\n", slice.name, price.median,
                price.p99, price.maximum, spread.median, spread.p99, spread.maximum, price.count);
  }
  const ErrorStats strangle_price = absolute_error_stats(
      strangle_fast.size(), [&](std::size_t i) { return strangle_fast[i]; },
      [&](std::size_t i) { return strangle_cold[i]; });
  const ErrorStats strangle_spread = absolute_error_stats(
      strangle_fast.size(),
      [&](std::size_t i) {
        return std::fabs(strangle_fast[i] - strangle_cold[i]) /
               std::max(0.02, strangle_half_spread[i]);
      },
      [](std::size_t) { return 0.0; });
  std::printf("\n25d OTM strangle by expiry: price med/p99/max %.5g / %.5g / %.5g; "
              "combined-half-spread %.5g / %.5g / %.5g (%zu expiries)\n",
              strangle_price.median, strangle_price.p99, strangle_price.maximum,
              strangle_spread.median, strangle_spread.p99, strangle_spread.maximum,
              strangle_price.count);
  std::printf("\n");
}

} // namespace

int main(int argc, char **argv) {
  const std::string path = argc > 1 ? argv[1] : "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet";
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  auto panel = load_opra_cbbo_parquet(spec);
  if (!panel.has_value()) {
    std::fprintf(stderr, "load failed: %s\n", panel.error().message().c_str());
    return 1;
  }

  // 1. OptionChain (unique-id board) from the panel frame + PCP-implied spot.
  auto chain_r = OptionChain::from_frame(panel->frame, spec.r, panel->implied_spot);
  if (!chain_r.has_value()) {
    std::fprintf(stderr, "chain build failed: %s\n", chain_r.error().message().c_str());
    return 1;
  }
  OptionChain chain = std::move(*chain_r);
  std::printf("SPY chain: spot %.2f, %zu option legs (unique ids)\n\n", chain.spot(), chain.size());

  // 2-3. PricerFitter fits and OWNS the surface.
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft,
                                   .use_correction_cache = true,
                                   .query_pricing_tier = QueryPricingTier::CarryBank}};
  const double t_fit0 = now_ms();
  const Status fit = fitter.fit(chain);
  const double fit_ms = now_ms() - t_fit0;
  if (!fit.has_value()) {
    std::fprintf(stderr, "fit failed: %s\n", fit.error().message().c_str());
    return 1;
  }
  std::printf("CarryBank fit: %.1f ms, surface owned=%s, %zu slices, worst frac-in-band %.3f\n\n",
              fit_ms, fitter.fitted() ? "yes" : "no", fitter.surface()->diagnostics().n_slices,
              fitter.surface()->diagnostics().worst_frac_within_bidask);

  PricerFitter representative{
      PricerConfig{.preset = FitPreset::Hft,
                   .use_correction_cache = true,
                   .query_pricing_tier = QueryPricingTier::RepresentativeFast}};
  const double representative_t0 = now_ms();
  const Status representative_fit = representative.fit(chain);
  const double representative_fit_ms = now_ms() - representative_t0;
  if (!representative_fit.has_value()) {
    std::fprintf(stderr, "representative fit failed: %s\n",
                 representative_fit.error().message().c_str());
    return 1;
  }
  std::printf("RepresentativeFast fit: %.1f ms\n\n", representative_fit_ms);

  PricerFitter cold{PricerConfig{.preset = FitPreset::Hft,
                                 .use_correction_cache = true,
                                 .query_pricing_tier = QueryPricingTier::ColdReference}};
  const double cold_t0 = now_ms();
  const Status cold_fit = cold.fit(chain);
  const double cold_fit_ms = now_ms() - cold_t0;
  if (!cold_fit.has_value()) {
    std::fprintf(stderr, "cold fit failed: %s\n", cold_fit.error().message().c_str());
    return 1;
  }
  std::printf("ColdReference fit: %.1f ms\n\n", cold_fit_ms);

  const AmericanCorrectionCaches caches = fitter.surface()->session().correction_caches();
  std::printf("correction caches: call=%s put=%s carry-bank=%zu pairs\n\n",
              caches.call != nullptr ? "yes" : "no", caches.put != nullptr ? "yes" : "no",
              fitter.surface()->session().query_cache_bank_size());

  print_field_breakdown("RepresentativeFast", representative, chain);
  print_field_breakdown("CarryBank", fitter, chain);
  print_authoritative_t8_comparison(cold, representative, fitter, chain);
  print_fast_approximation_error("RepresentativeFast", representative, chain);
  print_fast_approximation_error("CarryBank", fitter, chain);

  // 4. Parallel whole-chain valuation across thread counts. All fields: model
  //    price/IV/Greeks + bid/ask/mid cached American-IV inversions in parallel.
  //    Report scaling + prove determinism.
  const unsigned threads[] = {1u, 2u, 4u, 8u};
  std::printf("value_chain(All) warm+5 parallel scaling:\n");
  std::printf("%8s %10s %10s %8s %14s %9s\n", "threads", "med_ms", "p95_ms", "cv", "inversions/s",
              "speedup");
  double base_ms = 0.0;
  ChainValuation ref;
  bool have_ref = false;
  bool determinism_ok = true;
  for (const unsigned nt : threads) {
    auto measured = measure_value_chain(fitter, chain, OutputField::All, nt);
    if (!measured.has_value()) {
      std::fprintf(stderr, "value_chain scaling measurement failed\n");
      return 1;
    }
    const double ms = measured->timing.median_ms;
    const std::size_t n_inv = count_band_inversions(measured->sample);
    const double inv_per_s = ms > 0.0 ? 1000.0 * static_cast<double>(n_inv) / ms : 0.0;
    if (nt == 1u)
      base_ms = ms;
    const double speedup = ms > 0.0 ? base_ms / ms : 0.0;
    std::printf("%8u %10.1f %10.1f %8.3f %14.0f %8.2fx\n", nt, ms, measured->timing.p95_ms,
                measured->timing.cv, inv_per_s, speedup);
    if (!have_ref) {
      ref = std::move(measured->sample);
      have_ref = true;
    } else if (!identical(ref, measured->sample)) {
      determinism_ok = false;
    }
  }
  std::printf("\nDETERMINISM across thread counts: %s\n\n",
              determinism_ok ? "IDENTICAL (bit-for-bit)" : "*** MISMATCH ***");

  // 5. Tick-to-quote: replace bid/ask for a spread of option ids, re-value the
  //    band, and show the model IV band moved.
  const std::vector<OptionId> all_ids = chain.ids();
  std::vector<OptionId> upd_ids;
  std::vector<double> upd_bids;
  std::vector<double> upd_asks;
  for (std::size_t i = 0; i < all_ids.size() && upd_ids.size() < 8; i += all_ids.size() / 8 + 1) {
    const auto o = chain.at(all_ids[i]);
    if (!o.has_value() || !(o->mid > 0.5))
      continue;
    upd_ids.push_back(all_ids[i]);
    upd_bids.push_back(o->bid * 1.10); // widen the quote up 10%
    upd_asks.push_back(o->ask * 1.10);
  }
  const std::span<const OptionId> dirty_ids{upd_ids};
  const auto before = fitter.value_chain(chain, dirty_ids, OutputField::MidIV, 1);
  const Status us =
      chain.update_quotes(std::span<const OptionId>(upd_ids), std::span<const double>(upd_bids),
                          std::span<const double>(upd_asks));
  const auto after = fitter.value_chain(chain, dirty_ids, OutputField::MidIV, 1);
  std::printf("update_quotes on %zu dirty ids -> selected MidIV (status=%s):\n", upd_ids.size(),
              us.has_value() ? "ok" : "err");
  if (before.has_value() && after.has_value()) {
    for (const OptionId id : upd_ids) {
      const auto rb = before->row_of(id);
      const auto ra = after->row_of(id);
      if (rb && ra) {
        std::printf("  id %016llx  midIV %.4f -> %.4f\n", static_cast<unsigned long long>(id),
                    before->mid_iv[*rb], after->mid_iv[*ra]);
      }
    }
  }

  constexpr std::size_t kDirtyIterations = 100u;
  std::size_t dirty_inversions = 0u;
  bool dirty_ok = true;
  const double dirty_t0 = now_ms();
  for (std::size_t iteration = 0u; iteration < kDirtyIterations; ++iteration) {
    const Status updated =
        chain.update_quotes(std::span<const OptionId>(upd_ids), std::span<const double>(upd_bids),
                            std::span<const double>(upd_asks));
    const auto valued = fitter.value_chain(chain, dirty_ids, OutputField::Bands, 1);
    if (!updated.has_value() || !valued.has_value()) {
      dirty_ok = false;
      break;
    }
    dirty_inversions += count_band_inversions(*valued);
  }
  const double dirty_ms = now_ms() - dirty_t0;
  if (dirty_ok && dirty_ms > 0.0) {
    const double batches_per_s = 1000.0 * static_cast<double>(kDirtyIterations) / dirty_ms;
    const double options_per_s = batches_per_s * static_cast<double>(upd_ids.size());
    const double inversions_per_s = 1000.0 * static_cast<double>(dirty_inversions) / dirty_ms;
    std::printf("\nselected tick path (update + Bands, %zu dirty ids): %.1f us/batch, "
                "%.0f options/s, %.0f inversions/s\n",
                upd_ids.size(), 1.0e6 / batches_per_s, options_per_s, inversions_per_s);
  } else {
    std::printf("\nselected tick path benchmark failed\n");
  }

  std::printf("\nNOTE: whole-chain fan-out is for surface refreshes; quote ticks should use\n"
              "the selected-id overload above so work stays proportional to dirty options.\n"
              "Results remain deterministic by construction (disjoint output slots).\n");
  return 0;
}
