#pragma once

// Contract universe: the registry of underlyings, expiries, and strikes that
// atx-vol knows about, plus the SoA chain layout the hot path streams over.
//
// Ported from the C `ats-vol` library (ats_universe.{h,c}, the Swiss-table
// ticker index in ats_universe_hashmap.{h,c}, and the LRU residency policy in
// ats_universe_eviction.{h,c}). The refactor to the atx house style
// (.agents/cpp/agent.md) drops the C's arena + hand-rolled Swiss-table +
// negative-integer `AtsVolStatus` channel and replaces them with std
// containers (Rule of Zero), `std::unordered_map`, and `Result<T>` / `Status`.
// The *observable* semantics are preserved bit-for-bit:
//
//   - O(1) ticker -> uid interning (idempotent) and uid -> ticker reverse
//     lookup. uid 0 is reserved as `kInvalidUid`; assigned uids start at 1.
//   - Contiguous SoA per (uid, expiry) `Chain`: a `strikes[]` axis plus
//     per-(strike, side) `bids/asks/bid_sizes/ask_sizes/mids/ivs/ts_ns/flags`
//     axes indexed `chain_index(strike_idx, side)` == `2*strike_idx + side`
//     (call=0 then put=1 interleaved, both legs in one cache line).
//   - `apply_quotes` updates mids/flags and silently drops quotes whose
//     contract id does not decode to a known (uid, expiry, strike) tuple,
//     exposing the drop count via `stats()`.
//   - LRU eviction by `last_touched_ns` with a min-residency floor.
//
// ## Ownership
//
// A `Universe` owns everything by value via RAII (`std::deque<Underlying>`,
// `std::unordered_map`, `std::vector`, `std::string`) — Rule of Zero, no arena,
// no raw owning pointers, no manual free. `Underlying*` handed back by
// `get_underlying` are non-owning views; they are stable across `intern_ticker`
// growth (the `std::deque` never relocates existing elements, matching the C's
// stable-`AtsVolUnderlying*` contract). A `Chain&` is stable only until the
// next `add_expiry` on the same underlier (that may grow the chain vector) —
// exactly the C contract, where the chains array reallocates on new expiries.
//
// ## Thread-safety
//
// "Many readers OR one writer", matching the C. After construction, ticker /
// uid / contract-id lookups and `stats()` are safe from any number of threads
// concurrently. Mutators (`intern_ticker`, `add_expiry`, `add_strike`,
// `apply_quotes`, `mark_live`, `evict_lru`) require exclusive access — the
// caller fences them across a session boundary or copy-on-write pivot.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/vol/types.hpp"

namespace atx::vol {

// ── Identifiers ─────────────────────────────────────────────────────────────

// Underlying id. 0 is reserved as `kInvalidUid`; the encode field is 24 bits.
using Uid = std::uint32_t;

// Expiry bucket id, per underlying. Also the 0-based index into
// `Underlying::chains`.
using ExpiryId = std::uint16_t;

// Stable contract id, packed (uid:24, expiry:16, strike_idx:16, side:1); the
// high bit is reserved. Build/decode with the free functions below.
using ContractId = std::uint64_t;

inline constexpr Uid kInvalidUid = 0u;
inline constexpr ExpiryId kInvalidExpiry = 0xFFFFu;

// Ticker length cap (ATS_VOL_UNIVERSE_MAX_TICKER in the C). Interning a longer
// ticker is rejected with InvalidArgument.
inline constexpr std::size_t kMaxTickerLen = 16u;

// Per-underlier / per-chain growth caps, mirroring the C's doubling ceilings
// (`ensure_chains_cap` at 0x8000, `ensure_chain_strikes_cap` at 0x4000). Both
// also keep the encoded expiry/strike fields within their 16-bit widths.
inline constexpr std::size_t kMaxChainsPerUnderlying = 0x8000u; // 32768
inline constexpr std::size_t kMaxStrikesPerChain = 0x4000u;     // 16384

// ── Symbol -> uid (archive stamping) ────────────────────────────────────────
//
// A STABLE, deterministic, NON-ZERO uid derived purely from a ticker's
// canonical form (ASCII-upper, truncated to the archive's symbol cap — see
// `kArchiveSymbolMax` in surface_archive.hpp; duplicated as a local constant in
// universe.cpp, not `#include`d, to keep this header free of the archive's
// heavier `PricedSurface` dependency — keep the two constants in lock-step).
//
// Every fresh single-symbol `Universe` assigns its sole interned ticker uid 1
// (`intern_ticker` below), so a corpus that fits one board per (date, symbol)
// in its own `Universe` produces surfaces that ALL carry uid=1. `uid_for_symbol`
// gives the corpus/archive WRITER (corpus.cpp) a symbol-derived uid to stamp
// on the persisted copy instead, so a date's archive holds distinct uids per
// symbol. Pure function of the canonical bytes: identical across calls,
// processes, dates, and thread counts (no process- or ASLR-seeded hash state,
// unlike `atx::core::hash_bytes`/wyhash). Maps a 0 digest to 1 (0 is
// `kInvalidUid`, this module's reserved sentinel).
[[nodiscard]] std::uint32_t uid_for_symbol(std::string_view symbol) noexcept;

// ── Contract-id codec ───────────────────────────────────────────────────────
//
// Bit-for-bit identical to the C `ats_vol_cid_make` and its decoders:
//   uid        -> bits [33, 56]  (24 bits, masked 0x00FFFFFF)
//   expiry     -> bits [17, 32]  (16 bits, masked 0x0000FFFF)
//   strike_idx -> bits [ 1, 16]  (16 bits, masked 0x0000FFFF)
//   side       -> bit  0

[[nodiscard]] constexpr ContractId make_contract_id(Uid uid, ExpiryId expiry,
                                                    std::uint16_t strike_idx,
                                                    Side side) noexcept {
  const auto side_bit = static_cast<ContractId>(static_cast<std::uint8_t>(side)) & 0x1ull;
  return (static_cast<ContractId>(uid & 0x00FF'FFFFu) << 33) |
         (static_cast<ContractId>(expiry & 0xFFFFu) << 17) |
         (static_cast<ContractId>(strike_idx & 0xFFFFu) << 1) | side_bit;
}

[[nodiscard]] constexpr Uid cid_uid(ContractId c) noexcept {
  return static_cast<Uid>((c >> 33) & 0x00FF'FFFFull);
}

[[nodiscard]] constexpr ExpiryId cid_expiry(ContractId c) noexcept {
  return static_cast<ExpiryId>((c >> 17) & 0xFFFFull);
}

[[nodiscard]] constexpr std::uint16_t cid_strike_idx(ContractId c) noexcept {
  return static_cast<std::uint16_t>((c >> 1) & 0xFFFFull);
}

[[nodiscard]] constexpr Side cid_side(ContractId c) noexcept {
  return static_cast<Side>(static_cast<std::uint8_t>(c & 0x1ull));
}

// SoA index for the per-(strike, side) axes of a `Chain`: call (side 0) then
// put (side 1) interleaved. Mirrors `ats_vol_chain_idx`.
[[nodiscard]] constexpr std::size_t chain_index(std::uint16_t strike_idx, Side side) noexcept {
  return static_cast<std::size_t>(strike_idx) * 2u +
         static_cast<std::size_t>(static_cast<std::uint8_t>(side));
}

// ── Chain (one (uid, expiry) bucket) ────────────────────────────────────────

// One chain = one (uid, expiry) bucket. Calls and puts share the strike axis;
// the per-side arrays are indexed `chain_index(strike_idx, side)` so both legs
// of a strike sit in the same cache line. `strikes` has one entry per strike;
// every per-side array is exactly `2 * strikes.size()` long and grows in
// lock-step in `add_strike`. `ivs` is NaN until the calibrator inverts it;
// `apply_quotes` updates every axis except `ivs` (that is a calibration-cadence
// concern, not a quote-ingest one — same split as the C).
//
// Owned by the enclosing `Underlying`/`Universe` via RAII (Rule of Zero).
struct Chain {
  Uid uid = kInvalidUid;
  ExpiryId expiry_id = 0u;
  std::int64_t expiry_ns = 0;                                 // expiry instant, UTC ns since epoch
  double T = 0.0;                                             // year-fraction to expiry (set at clock-sync)

  // Optional source-side ATM IV anchor (Sprint 26). NaN / absent unless a
  // loader surfaces it.
  double source_atm_vol = std::numeric_limits<double>::quiet_NaN();
  bool source_atm_vol_present = false;

  std::vector<double> strikes;         // [n_strikes]      ascending, insertion order
  std::vector<double> bids;            // [2 * n_strikes]
  std::vector<double> asks;            // [2 * n_strikes]
  std::vector<std::int32_t> bid_sizes; // [2 * n_strikes]
  std::vector<std::int32_t> ask_sizes; // [2 * n_strikes]
  std::vector<double> mids;            // [2 * n_strikes]  derived on quote ingest
  std::vector<double> ivs;             // [2 * n_strikes]  NaN until fitted
  std::vector<std::int64_t> ts_ns;     // [2 * n_strikes]  last update
  std::vector<std::uint8_t> flags;     // [2 * n_strikes]  ATS_VOL_QFLAG_* bits

  [[nodiscard]] std::size_t n_strikes() const noexcept { return strikes.size(); }
};

// ── Underlying ──────────────────────────────────────────────────────────────

// One underlying owns N chains (one per expiry, indexed by `ExpiryId`).
//
// The C's `curves` / `surface` / `profile_ptr` views live in other modules
// (curve set, surface, profile registry) that are out of scope for this port;
// only the residency bookkeeping those modules feed (`evicted` + `bytes_live`)
// is modelled here, which is all the eviction policy actually reads/writes.
//
// Owned by the `Universe` (`std::deque`, stable addresses); non-owning views
// are handed out by `Universe::get_underlying`.
struct Underlying {
  Uid uid = kInvalidUid;
  std::string ticker;         // exact interned ticker (reverse-lookup store)
  double spot = 0.0;          // last reference spot
  std::int64_t spot_ts_ns = 0;
  std::vector<Chain> chains;  // one per expiry; `expiry_id` indexes this
  std::uint32_t flags = 0u;   // ATS_VOL_UFLAG_* bitfield (e.g. HTB)

  // O(1) idempotency index for `add_expiry`: expiry_ns -> the chain's current
  // `ExpiryId` (== its index into `chains`). Kept in lock-step with `chains`:
  // `add_expiry` inserts on a new expiry, and `install_sort_chains_by_T`
  // rebuilds it after it reorders the chains and re-issues their ids. A plain
  // value member so Rule-of-Zero copy/move carries it with the underlier.
  std::unordered_map<std::int64_t, ExpiryId> expiry_index;

  // LRU residency bookkeeping (Sprint 09). `last_touched_ns` advances on every
  // `apply_quotes` ingest touching this uid; `evict_lru` chooses the dormant
  // underliers by it. `evicted` marks that the (out-of-scope) surface /
  // correction cache have been logically dropped — chain data is retained, so
  // a later quote still ingests and the next refit rebuilds cold.
  std::int64_t last_touched_ns = 0;
  bool evicted = false;
  std::uint64_t bytes_live = 0u;
};

// ── Bulk quote ingest (SoA) ─────────────────────────────────────────────────

// The hot-path quote representation: parallel columns the caller owns.
// `contracts` sets the batch length; every other column must be the same
// length (validated in `apply_quotes`). The C's `exch_masks` column is omitted:
// `apply_quotes` never consumed it and the `Chain` SoA stores no venue mask.
struct QuoteBatch {
  std::span<const ContractId> contracts;
  std::span<const double> bids;
  std::span<const double> asks;
  std::span<const std::int32_t> bid_sizes;
  std::span<const std::int32_t> ask_sizes;
  std::span<const std::int64_t> ts_ns;
  std::span<const std::uint8_t> flags;
};

// ── Stats ───────────────────────────────────────────────────────────────────

// Per-reason breakdown of quotes dropped by `apply_quotes`. Every drop path in
// `apply_quotes` bumps exactly one of these, so the fields sum to
// `UniverseStats::n_quotes_dropped`. Cheap (per-reason counter increments, no
// allocation, no per-quote logging) so it is safe in the ingest hot path. The
// only drop reasons `apply_quotes` has are contract-id decode failures — it does
// not filter on quote economics (non-finite / crossed / min-obs live at the
// calibration layer, not here).
struct QuoteDropTally {
  std::uint64_t unknown_uid = 0u;          // contract id decodes to no known uid
  std::uint64_t expiry_out_of_range = 0u;  // expiry index past the uid's chains
  std::uint64_t strike_out_of_range = 0u;  // strike index past the chain's strikes

  // Sum of all drop reasons; equals UniverseStats::n_quotes_dropped.
  [[nodiscard]] std::uint64_t total() const noexcept {
    return unknown_uid + expiry_out_of_range + strike_out_of_range;
  }
};

// Snapshot counters (AtsVolUniverseStats in the C, minus `bytes_arena_used` —
// there is no arena in this port).
struct UniverseStats {
  std::uint32_t n_underlyings = 0u;
  std::uint32_t n_chains = 0u;
  std::uint64_t n_strikes = 0u;      // summed across all chains
  std::uint64_t n_quotes_applied = 0u;
  std::uint64_t n_quotes_dropped = 0u; // unknown contract id (== drops.total())
  QuoteDropTally drops;                // per-reason breakdown of n_quotes_dropped
};

// ── LRU residency policy ────────────────────────────────────────────────────

// Eviction budget (AtsVolEvictionOpts). Defaults match the C
// `ats_vol_eviction_default_opts`: 4 GB cap, 60 s min-residency.
struct EvictionOptions {
  std::uint64_t cap_bytes = 4ull * 1024ull * 1024ull * 1024ull; // 4 GB
  std::int64_t min_residency_ns = 60ll * 1'000'000'000ll;       // 60 s
  std::int64_t now_ns = 0;                                       // reference timestamp
};

// Outcome of one `evict_lru` sweep (AtsVolEvictionStats).
struct EvictionStats {
  std::uint32_t n_evicted = 0u;
  std::uint32_t n_skipped_min_residency = 0u;
  std::uint64_t bytes_freed = 0u;
  std::uint64_t bytes_remaining = 0u;
};

// ── Universe ────────────────────────────────────────────────────────────────

// The registry. Rule of Zero: copyable/movable via its RAII members.
class Universe {
public:
  // Construction sizing hints (AtsVolUniverseOpts, minus the required arena).
  // `expected_unders` only pre-reserves the ticker index; the universe grows
  // on demand with no hard capacity cap.
  struct Options {
    std::uint32_t expected_unders = 1024u;
  };

  Universe();
  explicit Universe(const Options &opts);

  // ── Underlying registration ────────────────────────────────────────────

  // Register `ticker` -> uid. Idempotent: returns the existing uid if the
  // ticker is already interned.
  // @return InvalidArgument if `ticker` is empty or longer than
  //         `kMaxTickerLen`.
  [[nodiscard]] Result<Uid> intern_ticker(std::string_view ticker);

  // Reverse lookup uid -> ticker. The returned view is owned by the universe
  // and stable for the universe's lifetime.
  // @return NotFound if `uid` is 0 or was never assigned.
  [[nodiscard]] Result<std::string_view> ticker_for(Uid uid) const;

  // ── Underlying access ──────────────────────────────────────────────────

  // Non-owning view of an underlier's mutable state. The pointer is stable
  // across later `intern_ticker` calls (deque storage).
  // @return NotFound if `uid` is 0 or was never assigned.
  [[nodiscard]] Result<Underlying *> get_underlying(Uid uid);
  [[nodiscard]] Result<const Underlying *> get_underlying(Uid uid) const;

  // ── Chain layout ───────────────────────────────────────────────────────

  // Add an expiry bucket to an underlier. Idempotent on a duplicate
  // `expiry_ns` (returns the existing id). Returns the assigned `ExpiryId`.
  // @return NotFound if `uid` is unknown; OutOfRange at the chain cap.
  [[nodiscard]] Result<ExpiryId> add_expiry(Uid uid, std::int64_t expiry_ns);

  // Append a strike to a chain, growing the SoA grid. Returns the assigned
  // strike index.
  // @return NotFound if `uid` or `expiry_id` is unknown; OutOfRange at the
  //         strike cap.
  [[nodiscard]] Result<std::uint16_t> add_strike(Uid uid, ExpiryId expiry_id, double strike);

  // ── Bulk quote ingest ──────────────────────────────────────────────────

  // Apply a batch of quotes. Quotes whose contract id does not decode to a
  // known (uid, expiry, strike) tuple are dropped and counted in
  // `stats().n_quotes_dropped`, with a per-reason breakdown in `stats().drops`
  // (unknown uid / expiry out of range / strike out of range). Updates
  // mids/flags but not `ivs` (the
  // calibration cadence owns that). Also advances each touched underlier's
  // `last_touched_ns` to the max quote timestamp.
  // @return InvalidArgument if the batch columns are not all the same length.
  [[nodiscard]] Status apply_quotes(const QuoteBatch &batch);

  // ── Stats ──────────────────────────────────────────────────────────────

  [[nodiscard]] UniverseStats stats() const noexcept;

  // Count of registered uids (uids run 1..n_underlyings()).
  [[nodiscard]] std::uint32_t n_underlyings() const noexcept;

  // ── LRU residency policy ───────────────────────────────────────────────

  // Test/driver hook: declare an underlier "live" by stamping its footprint
  // and touch time directly (AtsVolUniverseMark_live). Clears `evicted`.
  // No-op for an unknown uid.
  void mark_live(Uid uid, std::uint64_t bytes, std::int64_t last_touched_ns) noexcept;

  // Total live footprint across non-evicted underliers, without mutation.
  [[nodiscard]] std::uint64_t bytes_live() const noexcept;

  // Evict dormant underliers until the live footprint falls below
  // `cap_bytes * 0.9` (10% headroom). Oldest `last_touched_ns` first; any
  // underlier touched within `min_residency_ns` of `now_ns` is skipped.
  // Eviction is logical: it sets `evicted`, zeroes `bytes_live`, and retains
  // the chain data (matching the C v1 policy, which frees the surface but not
  // the arena-backed chains).
  EvictionStats evict_lru(const EvictionOptions &opts = {});

private:
  [[nodiscard]] Underlying *find_underlying(Uid uid) noexcept;
  [[nodiscard]] const Underlying *find_underlying(Uid uid) const noexcept;

  // Index 0 is a reserved sentinel so a raw uid indexes directly (uid 0 is
  // `kInvalidUid`). Deque keeps element addresses stable across growth.
  std::deque<Underlying> unders_;
  std::unordered_map<std::string, Uid> ticker_index_; // ticker -> uid
  std::uint64_t n_quotes_applied_ = 0u;
  std::uint64_t n_quotes_dropped_ = 0u;
  QuoteDropTally quote_drops_; // per-reason breakdown; sums to n_quotes_dropped_
};

} // namespace atx::vol
