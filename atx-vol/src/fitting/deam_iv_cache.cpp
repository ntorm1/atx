#include "fitting/deam_iv_cache.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include "atx/vol/api/fitting/aggregate_arity.hpp"
#include "atx/vol/api/pricing/american.hpp"

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
  DeAmIvCache cache;
};

// Per-thread ring of caches (FIFO eviction at capacity). unique_ptr keeps every
// cache at a stable address while the vector grows.
struct Memo {
  std::vector<std::unique_ptr<MemoEntry>> entries;
  std::size_t next_eviction = 0;
};

thread_local Memo t_memo;

} // namespace

DeAmIvView::DeAmIvView(DeAmIvCache &cache, const DeAmModel &model,
                       std::span<const QuoteSliceQuote> quotes, QuoteSliceDeamProfile profile,
                       QuoteSliceDeamDiagnostics &work) noexcept
    : cache_(cache), model_(model), quotes_(quotes), profile_(profile), work_(work) {}

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
    const auto iv = invert(quote, mid, entry);
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
  // A bid at or below the minimum-volatility price carries no lower
  // information; an ask at or above the maximum-volatility price no upper.
  if (quote.bid > entry.floor_price) {
    const auto lo = invert(quote, quote.bid, entry);
    entry.bid_iv = lo && positive(*lo) ? *lo : 0.0;
  }
  if (quote.ask < entry.cap_price) {
    const auto hi = invert(quote, quote.ask, entry);
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
  words.reserve(24 + 2 * prepared.variance_clock_dividends.size() + 6 * quotes.size());
  const auto real = [&words](double value) { words.push_back(std::bit_cast<std::uint64_t>(value)); };
  const auto word = [&words](std::uint64_t value) { words.push_back(value); };
  const QuoteSliceMarket &m = prepared.market;
  real(m.spot);
  real(m.variance_years);
  real(m.calendar_years);
  real(m.rate);
  real(m.continuous_yield);
  real(m.dividend_pv);
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

DeAmIvCache &deam_iv_memo(DeAmIvKey key, std::size_t quote_count, bool &reused) {
  Memo &memo = t_memo;
  for (const std::unique_ptr<MemoEntry> &entry : memo.entries) {
    if (entry->key.hash == key.hash && entry->key.words == key.words &&
        entry->cache.ivs.size() == quote_count) {
      reused = true;
      return entry->cache;
    }
  }
  reused = false;
  auto fresh = std::make_unique<MemoEntry>();
  fresh->cache.quote_set_hash = key.hash;
  fresh->cache.ivs.assign(quote_count, DeAmQuoteIv{});
  fresh->key = std::move(key);
  if (memo.entries.size() < kDeAmIvMemoCapacity) {
    memo.entries.push_back(std::move(fresh));
    return memo.entries.back()->cache;
  }
  std::unique_ptr<MemoEntry> &slot = memo.entries[memo.next_eviction];
  slot = std::move(fresh);
  memo.next_eviction = (memo.next_eviction + 1) % kDeAmIvMemoCapacity;
  return slot->cache;
}

void deam_iv_memo_clear() noexcept {
  t_memo.entries.clear();
  t_memo.next_eviction = 0;
}

std::size_t deam_iv_memo_size() noexcept { return t_memo.entries.size(); }

} // namespace atx::vol
