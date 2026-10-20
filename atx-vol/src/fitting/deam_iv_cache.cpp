#include "fitting/deam_iv_cache.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include "atx/vol/api/fitting/aggregate_arity.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "fitting/counters.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

// The key below spells out every field of these inputs. A new field must be
// added to deam_iv_key, or two different inputs would share a cache.
static_assert(detail::aggregate_arity_is_v<AlOpts, 6>, "AlOpts changed: update deam_iv_key");
static_assert(detail::aggregate_arity_is_v<QuoteSliceMarket, 6>,
              "QuoteSliceMarket changed: update deam_iv_key");
static_assert(detail::aggregate_arity_is_v<QuoteSliceQuote, 5>,
              "QuoteSliceQuote changed: update deam_iv_key");
static_assert(detail::aggregate_arity_is_v<CashDividend, 2>,
              "CashDividend changed: update deam_iv_key");

bool positive(double value) noexcept { return value > 0.0 && std::isfinite(value); }

// FNV-1a over the words, then a splitmix64 finaliser: a filter only, equality
// of the words decides a hit.
std::uint64_t hash_words(std::span<const std::uint64_t> words) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (const std::uint64_t word : words) {
    h = (h ^ word) * 0x100000001b3ull;
  }
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return h;
}

struct MemoEntry {
  DeAmIvKey key;
  std::shared_ptr<DeAmIvCache> cache; // never null
};

// Per-thread ring of caches (FIFO eviction at capacity). The caches are shared
// with the fits reading them, so eviction never invalidates a cache in use.
struct Memo {
  std::vector<MemoEntry> entries;
  std::size_t next_eviction = 0;
};

thread_local Memo t_memo;

} // namespace

DeAmIvView::DeAmIvView(DeAmIvCache &cache, const DeAmModel &model,
                       std::span<const QuoteSliceQuote> quotes, QuoteSliceDeamProfile profile,
                       QuoteSliceDeamDiagnostics &work, QuoteSliceDeamInversion inversion) noexcept
    : cache_(cache), model_(model), quotes_(quotes), profile_(profile), work_(work),
      inversion_(inversion) {}

bool DeAmIvView::premium() const noexcept {
  return inversion_ == QuoteSliceDeamInversion::Premium && kernel() == DeAmKernel::Accurate;
}

DeAmKernel DeAmIvView::kernel() const noexcept {
  return profile_ == QuoteSliceDeamProfile::Fast && model_.has_fast_kernel() ? DeAmKernel::Fast
                                                                             : DeAmKernel::Accurate;
}

DeAmQuoteIv &DeAmIvView::carry(std::size_t i) {
  DeAmQuoteIv &entry = cache_.ivs[i];
  if ((entry.computed & DeAmQuoteIv::kCarry) == 0u) {
    const QuoteSliceQuote &quote = quotes_[i];
    const double calendar_yield = quote.continuous_yield.value_or(model_.reference_yield());
    entry.q_effective = model_.effective_yield_of(calendar_yield);
    entry.local_forward = model_.local_forward_of(calendar_yield, entry.q_effective);
    entry.computed = static_cast<std::uint8_t>(entry.computed | DeAmQuoteIv::kCarry);
  }
  return entry;
}

Result<double> DeAmIvView::invert(const QuoteSliceQuote &quote, double target,
                                  const DeAmQuoteIv &entry, IvPremiumMemory *memory) {
  namespace ft = counters::fit_timing;
  namespace ledger = counters::ledger;
  ft::FitTimingCounters *const timing = ft::current_counts();
  const std::uint64_t al0 = timing ? ledger::this_thread(ledger::Solve::AlBoundarySolves) : 0u;
  IvPremiumStats stats;
  Result<double> out = premium() ? invert_premium(quote, target, entry, memory, stats)
                                 : invert_search(quote, target, entry);
  if (timing == nullptr) {
    return out;
  }
  const std::uint64_t al = ledger::this_thread(ledger::Solve::AlBoundarySolves) - al0;
  const std::uint32_t spent = ft::detail::saturate(al);
  ++timing->deam_inversions;
  timing->deam_inversion_al += spent;
  timing->deam_inversion_al_max = std::max(timing->deam_inversion_al_max, spent);
  if (!premium()) {
    return out;
  }
  ++timing->deam_premium;
  const bool fell_back = stats.fallback != IvPremiumFallback::None;
  if (fell_back) {
    ++timing->deam_premium_fallbacks;
    const auto reason = static_cast<std::size_t>(stats.fallback);
    if (reason < timing->deam_premium_fallback_by_reason.size()) {
      ++timing->deam_premium_fallback_by_reason[reason];
    }
  } else {
    timing->deam_premium_max_residual_usd =
        std::max(timing->deam_premium_max_residual_usd, stats.last_residual_usd);
  }
  if (ft::deam_audit_armed()) {
    // A fallback IS the Search answer (bit for bit, pinned by AmericanIvPremium tests):
    // a pair at 0 without a second inversion. An accepted root is compared with Search.
    double dsigma = 0.0;
    if (!fell_back) {
      const Result<double> search =
          model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Accurate);
      if (static_cast<bool>(search) != static_cast<bool>(out)) {
        ++timing->deam_audit_status_mismatch;
      } else if (search && out) {
        dsigma = std::fabs(*out - *search);
      }
      if (dsigma > 0.0) {
        const Result<double> pp = model_.price(quote, *out, entry.q_effective, DeAmKernel::Accurate);
        const Result<double> ps =
            model_.price(quote, *search, entry.q_effective, DeAmKernel::Accurate);
        if (pp && ps) {
          const double chord = std::fabs((*pp - *ps) / (*out - *search));
          if (chord > 0.0 && std::isfinite(chord)) {
            const double np = std::fabs(*pp - target) / chord;
            const double ns = std::fabs(*ps - target) / chord;
            timing->deam_audit_max_newton_premium =
                std::max(timing->deam_audit_max_newton_premium, np);
            timing->deam_audit_max_newton_search =
                std::max(timing->deam_audit_max_newton_search, ns);
            timing->deam_audit_search_farther += ns > np ? 1u : 0u;
          }
        }
      }
    }
    ++timing->deam_audit_pairs;
    timing->deam_audit_max_dsigma = std::max(timing->deam_audit_max_dsigma, dsigma);
    std::size_t bin = 0;
    if (dsigma > 0.0) {
      const double decade = std::floor(std::log10(dsigma)) + 17.0; // 1e-16 -> bin 1
      bin = static_cast<std::size_t>(std::clamp(decade, 1.0, 16.0));
    }
    ++timing->deam_audit_dsigma_decades[bin];
  }
  return out;
}

Result<double> DeAmIvView::invert_premium(const QuoteSliceQuote &quote, double target,
                                          const DeAmQuoteIv &entry, IvPremiumMemory *memory,
                                          IvPremiumStats &stats) {
  ++cache_.inversions;
  ++work_.inversions;
  ++work_.premium_inversions;
  Result<double> iv = model_.implied_vol_premium(quote, target, entry.q_effective, memory, stats);
  if (stats.fallback != IvPremiumFallback::None) {
    ++work_.premium_fallbacks;
  }
  return iv;
}

Result<double> DeAmIvView::invert_search(const QuoteSliceQuote &quote, double target,
                                         const DeAmQuoteIv &entry) {
  ++cache_.inversions;
  ++work_.inversions;
  if (kernel() == DeAmKernel::Accurate) {
    return model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Accurate);
  }
  auto fast = model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Fast);
  if (fast && *fast > kIvMin && *fast < kIvMax &&
      model_.black_vega(quote, *fast, entry.local_forward) >=
          kDeAmFastMinRelativeVega * std::max(target, 1.0)) {
    return fast;
  }
  ++cache_.accurate_fallbacks;
  ++work_.accurate_fallbacks;
  ++cache_.inversions;
  ++work_.inversions;
  return model_.implied_vol(quote, target, entry.q_effective, DeAmKernel::Accurate);
}

Result<double> DeAmIvView::mid_iv(std::size_t i) {
  DeAmQuoteIv &entry = carry(i);
  if ((entry.computed & DeAmQuoteIv::kMid) == 0u) {
    const QuoteSliceQuote &quote = quotes_[i];
    const double mid = 0.5 * quote.bid + 0.5 * quote.ask; // admission's own arithmetic
    // Premium: the mid from scratch; its cold points seed the band edges.
    const auto iv = invert(quote, mid, entry, premium() ? &entry.mid_memory : nullptr);
    entry.mid_iv = iv && positive(*iv) ? *iv : std::numeric_limits<double>::quiet_NaN();
    entry.computed = static_cast<std::uint8_t>(entry.computed | DeAmQuoteIv::kMid);
  }
  if (!positive(entry.mid_iv)) {
    return Err(ErrorCode::Unavailable, "de-Am IV cache: no identifiable mid volatility");
  }
  return Ok(entry.mid_iv);
}

const DeAmQuoteIv &DeAmIvView::band(std::size_t i) {
  DeAmQuoteIv &entry = carry(i);
  if ((entry.computed & DeAmQuoteIv::kBand) != 0u) {
    return entry;
  }
  entry.computed = static_cast<std::uint8_t>(entry.computed | DeAmQuoteIv::kBand);
  const QuoteSliceQuote &quote = quotes_[i];
  if (!positive(entry.local_forward) || !std::isfinite(entry.q_effective)) {
    return entry;
  }
  const DeAmKernel k = kernel();
  const auto floor_price = model_.price(quote, kIvMin, entry.q_effective, k);
  const auto cap_price = model_.price(quote, kIvMax, entry.q_effective, k);
  cache_.model_prices += 2;
  work_.model_prices += 2;
  if (!floor_price || !cap_price) {
    return entry;
  }
  entry.floor_price = *floor_price;
  entry.cap_price = *cap_price;
  if (!(quote.bid < entry.cap_price) || !(quote.ask > entry.floor_price)) {
    return entry;
  }
  // Premium: the mid first (exactly as the channel builder would ask for it: a
  // two-sided quote whose mid lies inside the identifiable range), then each edge from
  // a copy of the mid's cold points; without a mid, the edges from scratch.
  bool seeded = false;
  if (premium() && quote.bid > 0.0) {
    const double mid = 0.5 * quote.bid + 0.5 * quote.ask;
    if (mid > entry.floor_price && mid < entry.cap_price) {
      static_cast<void>(mid_iv(i));
      seeded = entry.mid_memory.points > 0u;
    }
  }
  IvPremiumMemory bid_memory = entry.mid_memory;
  IvPremiumMemory ask_memory = entry.mid_memory;
  // A bid at or below the minimum-volatility price carries no lower
  // information; an ask at or above the maximum-volatility price no upper.
  if (quote.bid > entry.floor_price) {
    const auto lo = invert(quote, quote.bid, entry, seeded ? &bid_memory : nullptr);
    entry.bid_iv = lo && positive(*lo) ? *lo : 0.0;
  }
  if (quote.ask < entry.cap_price) {
    const auto hi = invert(quote, quote.ask, entry, seeded ? &ask_memory : nullptr);
    if (hi && positive(*hi)) {
      entry.ask_iv = *hi;
    }
  }
  entry.well_posed = entry.ask_iv > entry.bid_iv &&
                     (entry.bid_iv > 0.0 || std::isfinite(entry.ask_iv));
  return entry;
}

DeAmIvKey deam_iv_key(const QuoteSliceFit &prepared, std::span<const QuoteSliceQuote> quotes,
                      const QuoteSliceConfig &config) {
  DeAmIvKey key;
  std::vector<std::uint64_t> &words = key.words;
  words.reserve(28 + 2 * prepared.variance_clock_dividends.size() + 6 * quotes.size());
  const auto real = [&words](double value) { words.push_back(std::bit_cast<std::uint64_t>(value)); };
  const auto word = [&words](std::uint64_t value) { words.push_back(value); };
  const QuoteSliceMarket &m = prepared.market;
  real(m.spot);
  real(m.variance_years);
  real(m.calendar_years);
  real(m.rate);
  real(m.continuous_yield);
  real(m.dividend_pv);
  // The derived carry the model reads (prepare_fit): functions of the words
  // above and the dividends, keyed directly so no rounding path is left out.
  const QuoteSliceDiagnostics &d = prepared.diagnostics;
  real(d.forward);
  real(d.discount);
  real(d.effective_rate);
  real(d.effective_yield);
  word(static_cast<std::uint64_t>(prepared.dividend_model));
  word(static_cast<std::uint64_t>(static_cast<std::int64_t>(prepared.discrete_div_steps)));
  word(static_cast<std::uint64_t>(prepared.discrete_div_scheme));
  word(prepared.al_opts ? 1u : 0u);
  if (prepared.al_opts) {
    const AlOpts &opts = *prepared.al_opts;
    word(opts.n_collocation);
    word(opts.n_quadrature);
    word(opts.n_quad_price);
    word(opts.max_newton_iter);
    real(opts.tol);
    word(static_cast<std::uint64_t>(opts.profile));
  }
  real(config.iv_tolerance);
  word(config.max_iv_iterations);
  word(static_cast<std::uint64_t>(config.channel.deam_profile));
  word(static_cast<std::uint64_t>(config.channel.deam_inversion)); // a Premium cache never
                                                                   // answers a Search fit
  word(prepared.variance_clock_dividends.size());
  for (const CashDividend &event : prepared.variance_clock_dividends) {
    real(event.tau);
    real(event.amount);
  }
  word(quotes.size());
  for (const QuoteSliceQuote &quote : quotes) {
    real(quote.strike);
    word(static_cast<std::uint64_t>(quote.side));
    real(quote.bid);
    real(quote.ask);
    word(quote.continuous_yield ? 1u : 0u);
    real(quote.continuous_yield.value_or(0.0));
  }
  key.hash = hash_words(words);
  return key;
}

std::shared_ptr<DeAmIvCache> deam_iv_memo(DeAmIvKey key, std::size_t quote_count,
                                           bool &reused) {
  Memo &memo = t_memo;
  for (const MemoEntry &entry : memo.entries) {
    if (entry.key.hash == key.hash && entry.key.words == key.words &&
        entry.cache->ivs.size() == quote_count) {
      reused = true;
      return entry.cache;
    }
  }
  reused = false;
  MemoEntry fresh{std::move(key), std::make_shared<DeAmIvCache>()};
  fresh.cache->quote_set_hash = fresh.key.hash;
  fresh.cache->ivs.assign(quote_count, DeAmQuoteIv{});
  std::shared_ptr<DeAmIvCache> out = fresh.cache;
  if (memo.entries.size() < kDeAmIvMemoCapacity) {
    memo.entries.push_back(std::move(fresh));
    return out;
  }
  memo.entries[memo.next_eviction] = std::move(fresh);
  memo.next_eviction = (memo.next_eviction + 1) % kDeAmIvMemoCapacity;
  return out;
}

void deam_iv_memo_clear() noexcept {
  t_memo.entries.clear();
  t_memo.next_eviction = 0;
}

std::size_t deam_iv_memo_size() noexcept { return t_memo.entries.size(); }

} // namespace atx::vol
