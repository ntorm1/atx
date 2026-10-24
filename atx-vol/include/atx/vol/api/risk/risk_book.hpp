#pragma once

// RiskBook — a portfolio of listed option legs AND share legs over N underliers,
// deduplicated into unique option contracts; PreparedRiskBook — the grouped,
// aligned execution view a kernel evaluates.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner).
//
// ## Keys
//
// An option is keyed by (uid, K, expiry_ns, side) with K and expiry_ns compared by
// EXACT bits. The year-fraction T is NOT part of the key: it is derived per
// MarketState from expiry_ns under the state's TimeSpec, so the same book ages
// through a day of snapshots with no retime step and a leg that expires between two
// states is reported Rolled, never priced at a negative T.
//
// ## Shares
//
// A share leg is a delta-one position in `uid`: mark = spot, delta = qty, every
// other Greek 0. Shares are never deduplicated against options; several share legs
// on one uid stay separate rows (the explain reports each).
//
// ## Thread-safety
//
// Both types are immutable after `create`; concurrent const reads are safe.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "atx/vol/api/core/types.hpp" // Result, Side
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol {

struct OptionLeg {
  std::uint64_t id{0};      // caller key, echoed in every output row
  std::uint32_t uid{0};
  double K{0.0};            // strike (> 0)
  std::int64_t expiry_ns{0}; // absolute expiry instant (epoch ns, UTC)
  Side side{Side::Call};
  double qty{0.0};          // signed contracts: + long / - short
  double multiplier{100.0}; // deliverable; <= 0 or non-finite => 100
  [[nodiscard]] bool operator==(const OptionLeg &) const = default;
};

struct ShareLeg {
  std::uint64_t id{0};
  std::uint32_t uid{0};
  double qty{0.0}; // signed shares
  [[nodiscard]] bool operator==(const ShareLeg &) const = default;
};

class RiskBook {
public:
  RiskBook();
  ~RiskBook();
  RiskBook(RiskBook &&) noexcept;
  RiskBook &operator=(RiskBook &&) noexcept;
  RiskBook(const RiskBook &);
  RiskBook &operator=(const RiskBook &);

  // Build from legs (any order). An empty book is valid. Leg counts are bounded by
  // UINT32_MAX (InvalidArgument beyond). A leg with non-finite qty / K or K <= 0 is
  // NOT rejected here: it is kept and reported InvalidContract by every evaluation,
  // so a book never silently loses a row.
  [[nodiscard]] static Result<RiskBook> create(std::span<const OptionLeg> options,
                                               std::span<const ShareLeg> shares);

  [[nodiscard]] std::size_t n_option_legs() const noexcept;
  [[nodiscard]] std::size_t n_share_legs() const noexcept;
  [[nodiscard]] std::size_t n_unique_options() const noexcept;
  [[nodiscard]] std::size_t n_underliers() const noexcept;

  // Legs in input order (borrowed for the book's lifetime).
  [[nodiscard]] std::span<const OptionLeg> option_legs() const noexcept;
  [[nodiscard]] std::span<const ShareLeg> share_legs() const noexcept;
  // Per option leg: its unique-contract slot (index into the unique table, insertion order).
  [[nodiscard]] std::span<const std::uint32_t> option_unique_slot() const noexcept;
  // The unique contracts in slot order (K/expiry/side/uid of the first leg that named it).
  [[nodiscard]] std::span<const OptionLeg> unique_options() const noexcept;
  // Distinct uids, ascending.
  [[nodiscard]] std::span<const std::uint32_t> uids() const noexcept;

  // Process-unique identity (fresh per create / copy; moves transfer it), used by
  // PreparedRiskBook / RiskWorkspace to refuse a stale substrate.
  [[nodiscard]] std::uint64_t logical_id() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A contiguous run of unique contracts sharing (uid, expiry_ns, side), ascending K.
struct RiskGroup {
  std::uint32_t uid{0};
  std::int64_t expiry_ns{0};
  Side side{Side::Call};
  std::uint32_t begin{0}; // [begin, end) in prepared order
  std::uint32_t end{0};
  std::uint32_t uid_slot{0}; // index into PreparedRiskBook::uids()
};

// Grouped, 64-byte-aligned SoA over the book's unique options, in PREPARED order
// (groups by (uid, expiry_ns, side), ascending K inside). `prepared_to_unique()` is
// the permutation back to RiskBook unique-slot order; every RiskFrame row is in
// prepared order and every per-leg output is scattered through it, so leg input
// order is never permuted.
class PreparedRiskBook {
public:
  PreparedRiskBook();
  ~PreparedRiskBook();
  PreparedRiskBook(PreparedRiskBook &&) noexcept;
  PreparedRiskBook &operator=(PreparedRiskBook &&) noexcept;
  PreparedRiskBook(const PreparedRiskBook &) = delete;
  PreparedRiskBook &operator=(const PreparedRiskBook &) = delete;

  // Borrows `book` (it must outlive the prepared view).
  [[nodiscard]] static Result<PreparedRiskBook> create(const RiskBook &book);

  [[nodiscard]] const RiskBook &book() const noexcept;
  [[nodiscard]] std::size_t n_unique() const noexcept;
  [[nodiscard]] std::size_t n_groups() const noexcept;
  [[nodiscard]] std::span<const RiskGroup> groups() const noexcept;

  // Prepared-order columns (length n_unique()).
  [[nodiscard]] std::span<const std::uint32_t> uid() const noexcept;
  [[nodiscard]] std::span<const double> strike() const noexcept;
  [[nodiscard]] std::span<const std::int64_t> expiry_ns() const noexcept;
  [[nodiscard]] std::span<const Side> side() const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> prepared_to_unique() const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> unique_to_prepared() const noexcept;

  // Per option leg (input order): prepared row and weight = qty * multiplier.
  [[nodiscard]] std::span<const std::uint32_t> leg_prepared_row() const noexcept;
  [[nodiscard]] std::span<const double> leg_weight() const noexcept;

  // Distinct uids ascending and, per share leg (input order), its uid slot.
  [[nodiscard]] std::span<const std::uint32_t> uids() const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> share_uid_slot() const noexcept;

  [[nodiscard]] std::uint64_t book_logical_id() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol
