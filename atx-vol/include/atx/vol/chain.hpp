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
  double T{0.0};            // year-fraction to expiry
  double strike{0.0};
  Side side{Side::Call};
  double bid{0.0};
  double ask{0.0};
  double mid{0.0};          // 0.5*(bid+ask)
  std::int32_t bid_size{0};
  std::int32_t ask_size{0};
};

class OptionChain {
 public:
  OptionChain(OptionChain&&) noexcept = default;
  OptionChain& operator=(OptionChain&&) noexcept = default;
  OptionChain(const OptionChain&) = delete;
  OptionChain& operator=(const OptionChain&) = delete;

  // Install `frame` into an owned `Universe` (`data_install`) and resolve its
  // single underlying. `r` is the flat continuously-compounded rate carried for
  // downstream pricing/inversion. `spot` overrides the pricing spot (e.g. an
  // OPRA panel's PCP-implied spot) when > 0; otherwise `frame.spot` is used.
  // Propagates any install error; NotFound if the frame installs no usable
  // underlying.
  [[nodiscard]] static Result<OptionChain> from_frame(const QuoteFrame& frame,
                                                      double r, double spot = 0.0);

  // ── Market snapshot ────────────────────────────────────────────────────────
  [[nodiscard]] double spot() const noexcept { return S_; }
  [[nodiscard]] double rate() const noexcept { return r_; }
  [[nodiscard]] std::int64_t now_ns() const noexcept { return now_ns_; }
  [[nodiscard]] Uid uid() const noexcept { return uid_; }

  // ── Enumeration / decode ───────────────────────────────────────────────────

  // Every option id present (both sides, all strikes/expiries), in a stable
  // deterministic order: ascending expiry (chain order), then ascending strike,
  // then side (call before put). The order is independent of quote content, so a
  // valuation over `ids()` is reproducible.
  [[nodiscard]] std::vector<OptionId> ids() const;

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
  [[nodiscard]] Status update_quotes(std::span<const OptionId> ids,
                                     std::span<const double> bids,
                                     std::span<const double> asks);

  // Non-owning const view of the installed underlying (the fitter's input).
  [[nodiscard]] const Underlying& underlying() const;

 private:
  OptionChain() = default;

  Universe u_{};
  Uid uid_{kInvalidUid};
  double S_{0.0};
  double r_{0.0};
  std::int64_t now_ns_{0};
};

}  // namespace atx::vol
