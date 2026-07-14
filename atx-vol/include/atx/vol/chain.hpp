#pragma once

// OptionChain — the addressable, mutable option-chain handle at the top of the
// atx-vol library lifecycle (chain -> fit -> price -> update).
//
// The chain the caller "has" in the five-step mental model is a single
// underlier's board of listed options, each with a stable unique id. That id
// already exists in atx-vol as `ContractId` (universe.hpp) — the packed
// (uid:24, expiry:16, strike_idx:16, side:1) key that `Universe::apply_quotes`
// keys tick updates on. OptionChain reuses it verbatim (`using OptionId =
// ContractId`) so the chain, the fitter, and the tick-to-quote path all speak
// one identifier with no side table.
//
// OptionChain owns a `Universe` holding exactly one `Underlying` (installed from
// a `QuoteFrame`) plus the market snapshot (spot, rate, valuation time). It is
// the ingestion + mutation handle; `PricerFitter` consumes its `underlying()`
// to fit, and `value_chain` prices its `ids()`.
//
// ## Ownership / thread-safety
//
// Move-only (owns a `Universe`). `underlying()` is resolved from the stored uid
// on demand, so it stays valid across a move. Mutators (`update_quotes`) require
// exclusive access (they call `Universe::apply_quotes`); the const accessors
// (`ids`, `at`, `underlying`, snapshot getters) are safe for concurrent readers
// once no writer is active — the "many readers OR one writer" universe contract.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/data.hpp"       // QuoteFrame, data_install
#include "atx/vol/market_env.hpp" // MarketEnv (spot / rate-curve / divs / ts)
#include "atx/vol/types.hpp"      // Side, Result, Status
#include "atx/vol/universe.hpp"   // Universe, Uid, ContractId, Underlying

namespace atx::vol {

// Stable per-option handle == the packed universe contract id. Decode with the
// universe free functions (`cid_uid`/`cid_expiry`/`cid_strike_idx`/`cid_side`).
using OptionId = ContractId;

// Decoded read-only snapshot of one option (NOT a live handle — re-`at()` after
// an `update_quotes` to see fresh bid/ask).
struct OptionRef {
  OptionId id{};
  std::int64_t expiry_ns{};
  double T{0.0}; // year-fraction to expiry
  double strike{0.0};
  Side side{Side::Call};
  double bid{0.0};
  double ask{0.0};
  double mid{0.0}; // 0.5*(bid+ask)
  std::int32_t bid_size{0};
  std::int32_t ask_size{0};
};

// Cache-friendly immutable flattening of one board. Columns are aligned by row
// and ordered exactly like ids(). This is the valuation handoff: one linear walk
// over the Universe SoA, no per-id decode/lookups, and no 72-byte OptionRef AoS.
struct ChainSnapshot {
  std::vector<OptionId> ids;
  std::vector<double> T;
  std::vector<double> strike;
  std::vector<double> bid;
  std::vector<double> ask;
  std::vector<double> mid;
  std::vector<Side> side;

  [[nodiscard]] std::size_t size() const noexcept { return ids.size(); }
};

class OptionChain {
public:
  OptionChain(OptionChain &&) noexcept = default;
  OptionChain &operator=(OptionChain &&) noexcept = default;
  OptionChain(const OptionChain &) = delete;
  OptionChain &operator=(const OptionChain &) = delete;

  // Install `frame` into an owned `Universe` (`data_install`) and resolve its
  // single underlying, carrying the full `MarketEnv` (spot / rate-curve / divs /
  // valuation time). This is the self-contained entry point: everything the
  // fitter needs travels in `env`. `env.spot` overrides `frame.spot` when > 0.
  // Propagates any install error; NotFound if the frame installs no usable
  // underlying.
  [[nodiscard]] static Result<OptionChain> from_frame(const QuoteFrame &frame, MarketEnv env);

  // Legacy flat-scalar overload (bit-identical to a flat `MarketEnv`). `r` is the
  // flat continuously-compounded rate; `spot` overrides `frame.spot` when > 0.
  [[nodiscard]] static Result<OptionChain> from_frame(const QuoteFrame &frame, double r,
                                                      double spot = 0.0);

  // ── Market snapshot ────────────────────────────────────────────────────────
  [[nodiscard]] double spot() const noexcept { return env_.spot; }
  // Representative continuously-compounded rate for the fit pipeline (the env's
  // rate at the front listed expiry; == flat_rate for a flat env).
  [[nodiscard]] double rate() const noexcept { return r_repr_; }
  [[nodiscard]] std::int64_t now_ns() const noexcept { return env_.now_ns; }
  [[nodiscard]] const MarketEnv &env() const noexcept { return env_; }
  [[nodiscard]] Uid uid() const noexcept { return uid_; }
  // Process-unique logical identity plus monotonically increasing quote
  // revisions. Revisions are provenance only; callers must still obey the
  // chain's many-readers-or-one-writer synchronization contract.
  [[nodiscard]] std::uint64_t instance_id() const noexcept { return instance_id_; }
  [[nodiscard]] std::uint64_t quote_revision() const noexcept { return quote_revision_; }
  [[nodiscard]] std::span<const std::uint64_t> expiry_quote_revisions() const noexcept {
    return expiry_quote_revisions_;
  }

  // ── Enumeration / decode ───────────────────────────────────────────────────

  // Every option id present (both sides, all strikes/expiries), in a stable
  // deterministic order: ascending expiry (chain order), then ascending strike,
  // then side (call before put). The order is independent of quote content, so a
  // valuation over `ids()` is reproducible.
  [[nodiscard]] std::vector<OptionId> ids() const;

  // Flatten every valuation-relevant field in one cache-linear pass. The
  // snapshot is detached from later quote updates and safe to fan out across
  // worker threads. Empty only if the underlying can no longer be resolved.
  [[nodiscard]] ChainSnapshot snapshot() const;

  // Flatten only `selected_ids`, preserving caller order and duplicates. Work
  // and allocation are proportional to the selection, so a quote-update loop
  // does not scan or materialize the full board. The returned snapshot is
  // detached from later quote updates, exactly like snapshot().
  //
  // @return NotFound if any id is foreign or does not name a listed leg. An
  //         empty selection succeeds with an empty snapshot.
  [[nodiscard]] Result<ChainSnapshot> snapshot(std::span<const OptionId> selected_ids) const;

  // Number of option legs present (== ids().size(), without materializing).
  [[nodiscard]] std::size_t size() const noexcept;

  // Decode one option to a value view. NotFound if `id` does not resolve to a
  // known (uid, expiry, strike, side) leg of this chain.
  [[nodiscard]] Result<OptionRef> at(OptionId id) const;

  // ── Tick-to-quote update ───────────────────────────────────────────────────

  // Replace bid/ask for a batch of ids (mids recomputed by the universe). Ids
  // that do not decode to a known leg are silently dropped (mirrors
  // `Universe::apply_quotes`). Sizes default to 1, timestamp to `now_ns()`.
  //
  // @return InvalidArgument if the three spans differ in length; otherwise Ok().
  [[nodiscard]] Status update_quotes(std::span<const OptionId> ids, std::span<const double> bids,
                                     std::span<const double> asks);

  // Non-owning const view of the installed underlying (the fitter's input).
  [[nodiscard]] const Underlying &underlying() const;

private:
  OptionChain() = default;

  Universe u_{};
  Uid uid_{kInvalidUid};
  MarketEnv env_{};    // spot / rate-curve / divs / valuation time
  double r_repr_{0.0}; // representative pipeline rate (env_.rate_at(front T))
  std::uint64_t instance_id_{0u};
  std::uint64_t quote_revision_{0u};
  std::vector<std::uint64_t> expiry_quote_revisions_{};
};

} // namespace atx::vol
