#pragma once

// Per-expiry rung memo of the graded surface fit (lane L2c, 2026-10-19; opt-in
// QuoteSurfaceConfig::rung_memo). Internal header (src/fitting); not public API.
//
// fit_quote_surface owns one ExpiryRungMemo per expiry slot and installs it on the
// fitting thread (ScopedRungMemo) around every per-expiry fit of that expiry: pass A
// (or the ladder's knot and point rungs), the B1 / B2 channel refits, the repair rungs
// and every channel refit of the calendar and dominance passes. A slice fit consults the
// installed memo in two places, and nowhere else:
//
//  1. The de-Am IV cache (QuoteSliceChannelConfig::deam_cache under VolChannel). The
//     expiry's cache lives in its slot instead of the fitting thread's memo
//     (deam_iv_cache.hpp), so every rung of the expiry shares one inversion pass
//     whichever thread runs it (the per-expiry fan-out spawns fresh threads for every
//     stage, so the thread-local memo misses across stages above one worker). Lookup
//     is by the SAME exact key (deam_iv_key: market, derived carry, quotes with their
//     contract carry, dated dividends, American model, inverter controls, cache
//     kernel), compared word for word, so a hit returns exactly what a rebuild
//     computes. No rung changes a keyed input: the rungs of one expiry differ only in
//     the repair settings (QuoteSliceConfig::repair; pass A runs without it), the
//     channel prior and its strength - none of them is read by an inversion, and
//     DeAmIvCache.MemoKeyCoversEveryInputThatChangesAnInversion pins that the key
//     covers every input that is. A rung that changed a keyed input would simply miss.
//
//  2. The certification grid (certify_and_serve). A CANDIDATE is the fitted curve
//     about to be repriced; it is UNCHANGED when the fit-level inputs of the American
//     model (the deam_iv_key words of the prepared fit: market, derived carry and
//     clocks, dividend model and dated dividends, AlOpts, lattice controls) and, at
//     every certification strike, the strike, the fitted volatility and the contract
//     yield are bit-identical to a candidate already repriced for this expiry. Then
//     the 2N American grid prices are bit-identical too, and the memo returns them
//     instead of repricing. The certificate itself (AL-free) and everything after it
//     (repairs, row re-implication) still run on those prices, so every outcome and
//     message is produced by the same code. A repair rung that refits the pass-A
//     candidate with the channel repair turned on re-presents an unchanged candidate.
//
// Threading: a memo is touched only by the thread fitting its expiry, and one expiry is
// never fitted by two threads at once (the fan-out writes slot i from task i only and
// joins before any later stage; the serial stages run on the calling thread), so the
// memo needs no lock; the join is the happens-before between two stages' accesses.
// Bounded: kRungMemoDeamKeys caches and kRungMemoGrids grids per expiry, FIFO.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "fitting/deam_iv_cache.hpp"

namespace atx::vol::detail {

inline constexpr std::size_t kRungMemoDeamKeys = 4;
inline constexpr std::size_t kRungMemoGrids = 8;

class ExpiryRungMemo {
public:
  // The expiry's de-Am IV cache for `key` (ivs sized to `quote_count`), created empty on
  // a miss; `reused` is set on a hit. Never null.
  [[nodiscard]] std::shared_ptr<DeAmIvCache> deam_cache(DeAmIvKey key, std::size_t quote_count,
                                                        bool &reused) {
    for (const DeamEntry &entry : deam_) {
      if (entry.key.hash == key.hash && entry.key.words == key.words &&
          entry.cache->ivs.size() == quote_count) {
        reused = true;
        ++deam_hits_;
        return entry.cache;
      }
    }
    reused = false;
    ++deam_misses_;
    DeamEntry fresh{std::move(key), std::make_shared<DeAmIvCache>()};
    fresh.cache->quote_set_hash = fresh.key.hash;
    fresh.cache->ivs.assign(quote_count, DeAmQuoteIv{});
    std::shared_ptr<DeAmIvCache> out = fresh.cache;
    remember(deam_, deam_next_, kRungMemoDeamKeys, std::move(fresh));
    return out;
  }

  // The memoised American grid of an unchanged candidate (`key`: see the header
  // comment), or null. The pointer is valid until the next remember_grid().
  [[nodiscard]] const std::vector<AmericanPriceNode> *
  grid(std::span<const std::uint64_t> key) noexcept {
    for (const GridEntry &entry : grids_) {
      if (entry.key.size() == key.size() &&
          std::equal(entry.key.begin(), entry.key.end(), key.begin())) {
        ++grid_hits_;
        return &entry.prices;
      }
    }
    ++grid_misses_;
    return nullptr;
  }

  void remember_grid(std::vector<std::uint64_t> key, std::vector<AmericanPriceNode> prices,
                     std::vector<std::array<double, 2>> bounds = {}) {
    remember(grids_, grid_next_, kRungMemoGrids,
             GridEntry{std::move(key), std::move(prices), std::move(bounds)});
  }

  // K4b (review-K4 m6): the per-node (call, put) error bounds remembered with the grid of
  // `key` (certify_and_serve's per-contract metadata), or null (no such grid, or none were
  // remembered with it). Not a lookup for the hit / miss accounting.
  [[nodiscard]] const std::vector<std::array<double, 2>> *
  grid_bounds(std::span<const std::uint64_t> key) const noexcept {
    for (const GridEntry &entry : grids_) {
      if (entry.key.size() == key.size() &&
          std::equal(entry.key.begin(), entry.key.end(), key.begin())) {
        return entry.bounds.empty() ? nullptr : &entry.bounds;
      }
    }
    return nullptr;
  }

  // Work accounting (tests and diagnostics): lookups that hit or missed.
  [[nodiscard]] std::size_t deam_hits() const noexcept { return deam_hits_; }
  [[nodiscard]] std::size_t deam_misses() const noexcept { return deam_misses_; }
  [[nodiscard]] std::size_t grid_hits() const noexcept { return grid_hits_; }
  [[nodiscard]] std::size_t grid_misses() const noexcept { return grid_misses_; }

private:
  struct DeamEntry {
    DeAmIvKey key;
    std::shared_ptr<DeAmIvCache> cache; // never null
  };
  struct GridEntry {
    std::vector<std::uint64_t> key;
    std::vector<AmericanPriceNode> prices;
    std::vector<std::array<double, 2>> bounds; // K4b: per node (call, put); may be empty
  };

  template <class Entry>
  static void remember(std::vector<Entry> &entries, std::size_t &next, std::size_t capacity,
                       Entry entry) {
    if (entries.size() < capacity) {
      entries.push_back(std::move(entry));
      return;
    }
    entries[next] = std::move(entry);
    next = (next + 1) % capacity;
  }

  std::vector<DeamEntry> deam_;
  std::vector<GridEntry> grids_;
  std::size_t deam_next_ = 0;
  std::size_t grid_next_ = 0;
  std::size_t deam_hits_ = 0;
  std::size_t deam_misses_ = 0;
  std::size_t grid_hits_ = 0;
  std::size_t grid_misses_ = 0;
};

// The memo installed on this thread (null: no rung memo, the historical path).
inline thread_local ExpiryRungMemo *t_rung_memo = nullptr;

[[nodiscard]] inline ExpiryRungMemo *current_rung_memo() noexcept { return t_rung_memo; }

// RAII install and restore of the calling thread's rung memo (null installs none).
class ScopedRungMemo {
public:
  explicit ScopedRungMemo(ExpiryRungMemo *memo) noexcept : prev_(t_rung_memo) {
    t_rung_memo = memo;
  }
  ~ScopedRungMemo() { t_rung_memo = prev_; }
  ScopedRungMemo(const ScopedRungMemo &) = delete;
  ScopedRungMemo &operator=(const ScopedRungMemo &) = delete;
  ScopedRungMemo(ScopedRungMemo &&) = delete;
  ScopedRungMemo &operator=(ScopedRungMemo &&) = delete;

private:
  ExpiryRungMemo *prev_;
};

} // namespace atx::vol::detail
