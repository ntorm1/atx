#include "atx/vol/api/marketdata/universe.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"  // kArchiveSymbolMax (static_assert only; this TU, not the header)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// "No value yet" fill for the derived/IV axis. Named locally so this TU does
// not redefine any `kQuietNaN` another atx-vol header may also declare.
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Canonical-symbol length cap for `uid_for_symbol`. MUST match
// `atx::vol::kArchiveSymbolMax` (surface_archive.hpp) — kept as a local constant
// so the PUBLIC header `universe.hpp` stays free of the archive's heavier
// `PricedSurface` dependency (surface_archive.hpp is pulled into THIS .cpp only,
// solely for the drift static_assert below). `MultinamePipeline.UidForSymbol_*`
// (multiname_pipeline_test.cpp) exercises the canonicalization this guards.
inline constexpr std::size_t kSymbolCanonMax = 32u;

// The canonical-symbol cap MUST equal the archive's inline symbol cap: the corpus
// writer canonicalizes to kArchiveSymbolMax bytes and `uid_for_symbol` must hash
// the identical truncation, else a >32-byte symbol would resolve to a different
// uid than the one stamped on disk. A static_assert (not just the comment above)
// turns any future drift between the two constants into a compile error.
static_assert(kSymbolCanonMax == static_cast<std::size_t>(kArchiveSymbolMax),
              "uid_for_symbol's canonical cap must match the archive symbol cap");

// FNV-1a 32-bit — a small, dependency-free, deterministic byte hash. Chosen
// over `atx::core::hash_bytes` (wyhash) because that helper's contract is
// explicitly "NOT stable across process restarts or platforms"; `uid_for_symbol`
// must agree across processes (the corpus writer and any later reader are
// typically different process invocations).
[[nodiscard]] constexpr std::uint32_t fnv1a32(std::string_view bytes) noexcept {
  constexpr std::uint32_t kOffsetBasis = 0x811C'9DC5u;
  constexpr std::uint32_t kPrime = 0x0100'0193u;
  std::uint32_t h = kOffsetBasis;
  for (const char c : bytes) {
    h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    h *= kPrime;
  }
  return h;
}

} // namespace

std::string canonical_symbol(std::string_view symbol) {
  // ASCII upper-case, truncated to kSymbolCanonMax — matches the archive's own
  // canonicalizer (surface_archive.cpp's file-local `canonicalize`): upper-case
  // then truncate, no trimming of interior bytes, and NO zero-pad. Returning
  // exactly the truncated-length bytes mirrors how the archive hashes
  // `plan.symbol_len` bytes, not the full padded array.
  const std::size_t n = std::min(symbol.size(), kSymbolCanonMax);
  std::string canon;
  canon.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    char c = symbol[i];
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    canon.push_back(c);
  }
  return canon;
}

std::uint32_t uid_for_symbol(std::string_view symbol) noexcept {
  // Hash exactly the canonical bytes (single source of truth: `canonical_symbol`).
  // The observable value is UNCHANGED from the prior inline canonicalization —
  // pinned by MultinamePipeline.UidForSymbolValuesArePinned.
  const std::uint32_t h = fnv1a32(canonical_symbol(symbol));
  return h == 0u ? 1u : h; // 0 is kInvalidUid, the reserved sentinel
}

// ── Construction ────────────────────────────────────────────────────────────

Universe::Universe() : Universe(Options{}) {}

Universe::Universe(const Options &opts) {
  // Reserve uid 0 as the INVALID sentinel: real uids start at 1 and index
  // `unders_` directly (matching the C, where `n_uids` starts at 1).
  unders_.emplace_back();
  if (opts.expected_unders > 0u) {
    ticker_index_.reserve(opts.expected_unders);
  }
}

// ── Internal lookup ─────────────────────────────────────────────────────────

Underlying *Universe::find_underlying(Uid uid) noexcept {
  if (uid == kInvalidUid || uid >= unders_.size()) {
    return nullptr;
  }
  return &unders_[uid];
}

const Underlying *Universe::find_underlying(Uid uid) const noexcept {
  if (uid == kInvalidUid || uid >= unders_.size()) {
    return nullptr;
  }
  return &unders_[uid];
}

// ── Underlying registration ─────────────────────────────────────────────────

Result<Uid> Universe::intern_ticker(std::string_view ticker) {
  if (ticker.empty()) {
    return Err(ErrorCode::InvalidArgument, "intern_ticker: empty ticker");
  }
  if (ticker.size() > kMaxTickerLen) {
    return Err(ErrorCode::InvalidArgument, "intern_ticker: ticker exceeds 16 bytes");
  }

  std::string key(ticker);
  if (const auto it = ticker_index_.find(key); it != ticker_index_.end()) {
    return Ok(it->second); // idempotent
  }

  const auto uid = static_cast<Uid>(unders_.size()); // next free; slot 0 reserved
  Underlying &under = unders_.emplace_back();
  under.uid = uid;
  under.ticker = key;
  ticker_index_.emplace(std::move(key), uid);
  return Ok(uid);
}

Result<std::string_view> Universe::ticker_for(Uid uid) const {
  const Underlying *under = find_underlying(uid);
  if (under == nullptr) {
    return Err(ErrorCode::NotFound, "ticker_for: unknown uid");
  }
  return Ok(std::string_view{under->ticker});
}

// ── Underlying access ───────────────────────────────────────────────────────

Result<Underlying *> Universe::get_underlying(Uid uid) {
  Underlying *under = find_underlying(uid);
  if (under == nullptr) {
    return Err(ErrorCode::NotFound, "get_underlying: unknown uid");
  }
  return Ok(under);
}

Result<const Underlying *> Universe::get_underlying(Uid uid) const {
  const Underlying *under = find_underlying(uid);
  if (under == nullptr) {
    return Err(ErrorCode::NotFound, "get_underlying: unknown uid");
  }
  return Ok(under);
}

// ── Chain layout ────────────────────────────────────────────────────────────

Result<ExpiryId> Universe::add_expiry(Uid uid, std::int64_t expiry_ns) {
  Underlying *under = find_underlying(uid);
  if (under == nullptr) {
    return Err(ErrorCode::NotFound, "add_expiry: unknown uid");
  }

  // Idempotent on a duplicate expiry instant. O(1) via the expiry index instead
  // of an O(E) scan over `chains` (the index maps expiry_ns -> current id and is
  // kept in lock-step with `chains` by add_expiry + install_sort_chains_by_T).
  if (const auto it = under->expiry_index.find(expiry_ns); it != under->expiry_index.end()) {
    return Ok(it->second);
  }
  if (under->chains.size() >= kMaxChainsPerUnderlying) {
    return Err(ErrorCode::OutOfRange, "add_expiry: chain capacity exhausted");
  }

  const auto eid = static_cast<ExpiryId>(under->chains.size());
  Chain &c = under->chains.emplace_back();
  c.uid = uid;
  c.expiry_id = eid;
  c.expiry_ns = expiry_ns;
  c.T = 0.0;
  under->expiry_index.emplace(expiry_ns, eid);
  return Ok(eid);
}

Result<std::uint16_t> Universe::add_strike(Uid uid, ExpiryId expiry_id, double strike) {
  Underlying *under = find_underlying(uid);
  if (under == nullptr) {
    return Err(ErrorCode::NotFound, "add_strike: unknown uid");
  }
  if (expiry_id >= under->chains.size()) {
    return Err(ErrorCode::NotFound, "add_strike: unknown expiry");
  }

  Chain &c = under->chains[expiry_id];
  if (c.strikes.size() >= kMaxStrikesPerChain) {
    return Err(ErrorCode::OutOfRange, "add_strike: strike capacity exhausted");
  }

  const auto idx = static_cast<std::uint16_t>(c.strikes.size());
  c.strikes.push_back(strike);

  // Grow the per-side axes in lock-step (two slots per strike). New slots are
  // value-initialized; `ivs` defaults to NaN (unfittable until calibrated).
  const std::size_t per_side = c.strikes.size() * 2u;
  c.bids.resize(per_side, 0.0);
  c.asks.resize(per_side, 0.0);
  c.bid_sizes.resize(per_side, 0);
  c.ask_sizes.resize(per_side, 0);
  c.mids.resize(per_side, 0.0);
  c.ivs.resize(per_side, kNaN);
  c.ts_ns.resize(per_side, 0);
  c.flags.resize(per_side, std::uint8_t{0});
  return Ok(idx);
}

// ── Bulk quote ingest ───────────────────────────────────────────────────────

Status Universe::apply_quotes(const QuoteBatch &batch) {
  const std::size_t n = batch.contracts.size();
  if (batch.bids.size() != n || batch.asks.size() != n || batch.bid_sizes.size() != n ||
      batch.ask_sizes.size() != n || batch.ts_ns.size() != n || batch.flags.size() != n) {
    return Err(ErrorCode::InvalidArgument, "apply_quotes: batch column length mismatch");
  }

  for (std::size_t i = 0; i < n; ++i) {
    const ContractId cid = batch.contracts[i];
    const Uid uid = cid_uid(cid);
    const ExpiryId exp = cid_expiry(cid);
    const std::uint16_t strike = cid_strike_idx(cid);
    const Side side = cid_side(cid);

    Underlying *under = find_underlying(uid);
    if (under == nullptr) {
      ++n_quotes_dropped_;
      ++quote_drops_.unknown_uid;
      continue;
    }
    if (exp >= under->chains.size()) {
      ++n_quotes_dropped_;
      ++quote_drops_.expiry_out_of_range;
      continue;
    }
    Chain &c = under->chains[exp];
    if (strike >= c.strikes.size()) {
      ++n_quotes_dropped_;
      ++quote_drops_.strike_out_of_range;
      continue;
    }

    const std::size_t idx = chain_index(strike, side);
    c.bids[idx] = batch.bids[i];
    c.asks[idx] = batch.asks[i];
    c.bid_sizes[idx] = batch.bid_sizes[i];
    c.ask_sizes[idx] = batch.ask_sizes[i];
    c.mids[idx] = 0.5 * (batch.bids[i] + batch.asks[i]);
    c.ts_ns[idx] = batch.ts_ns[i];
    c.flags[idx] = batch.flags[i];

    // Advance the LRU touch clock from the quote timestamps.
    if (batch.ts_ns[i] > under->last_touched_ns) {
      under->last_touched_ns = batch.ts_ns[i];
    }
    ++n_quotes_applied_;
  }
  return Ok();
}

// ── Stats ───────────────────────────────────────────────────────────────────

UniverseStats Universe::stats() const noexcept {
  UniverseStats out;
  out.n_underlyings = n_underlyings();
  out.n_quotes_applied = n_quotes_applied_;
  out.n_quotes_dropped = n_quotes_dropped_;
  out.drops = quote_drops_;

  std::uint32_t n_chains = 0u;
  std::uint64_t n_strikes = 0u;
  for (std::size_t uid = 1u; uid < unders_.size(); ++uid) {
    const Underlying &under = unders_[uid];
    n_chains += static_cast<std::uint32_t>(under.chains.size());
    for (const Chain &c : under.chains) {
      n_strikes += c.strikes.size();
    }
  }
  out.n_chains = n_chains;
  out.n_strikes = n_strikes;
  return out;
}

std::uint32_t Universe::n_underlyings() const noexcept {
  // `unders_` always holds at least the reserved slot 0.
  return static_cast<std::uint32_t>(unders_.size() - 1u);
}

// ── LRU residency policy ────────────────────────────────────────────────────

void Universe::mark_live(Uid uid, std::uint64_t bytes, std::int64_t last_touched_ns) noexcept {
  Underlying *under = find_underlying(uid);
  if (under == nullptr) {
    return;
  }
  under->bytes_live = bytes;
  under->last_touched_ns = last_touched_ns;
  under->evicted = false;
}

std::uint64_t Universe::bytes_live() const noexcept {
  std::uint64_t total = 0u;
  for (std::size_t uid = 1u; uid < unders_.size(); ++uid) {
    const Underlying &under = unders_[uid];
    if (!under.evicted) {
      total += under.bytes_live;
    }
  }
  return total;
}

EvictionStats Universe::evict_lru(const EvictionOptions &opts) {
  EvictionStats stats;
  if (n_underlyings() == 0u) {
    return stats;
  }

  // Snapshot live underliers into a sortable array.
  struct Row {
    Uid uid = 0u;
    std::int64_t last_touched_ns = 0;
    std::uint64_t bytes_live = 0u;
  };
  std::vector<Row> rows;
  rows.reserve(n_underlyings());
  std::uint64_t total = 0u;
  for (std::size_t uid = 1u; uid < unders_.size(); ++uid) {
    const Underlying &under = unders_[uid];
    if (under.evicted) {
      continue;
    }
    rows.push_back(Row{static_cast<Uid>(uid), under.last_touched_ns, under.bytes_live});
    total += under.bytes_live;
  }

  // Already under the high-water mark? Nothing to do.
  if (total <= opts.cap_bytes) {
    stats.bytes_remaining = total;
    return stats;
  }

  // Target: keep total <= cap * 0.9 so we do not churn around the boundary.
  const auto target = static_cast<std::uint64_t>(static_cast<double>(opts.cap_bytes) * 0.9);

  std::sort(rows.begin(), rows.end(),
            [](const Row &a, const Row &b) noexcept { return a.last_touched_ns < b.last_touched_ns; });

  for (const Row &row : rows) {
    if (total <= target) {
      break;
    }
    const std::int64_t age =
        (opts.now_ns > row.last_touched_ns) ? (opts.now_ns - row.last_touched_ns) : 0;
    if (age < opts.min_residency_ns) {
      ++stats.n_skipped_min_residency;
      continue;
    }
    Underlying *under = find_underlying(row.uid);
    if (under == nullptr) {
      continue;
    }
    // Logical eviction: mark evicted, zero the footprint, retain chain data.
    under->evicted = true;
    const std::uint64_t freed = under->bytes_live;
    under->bytes_live = 0u;
    total = (total > freed) ? (total - freed) : 0u;
    ++stats.n_evicted;
    stats.bytes_freed += freed;
  }

  stats.bytes_remaining = total;
  return stats;
}

} // namespace atx::vol
