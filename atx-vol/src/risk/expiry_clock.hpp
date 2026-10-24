#pragma once

// expiry_clock — THE year-fraction of one (uid, expiry) in a MarketState (design D22).
//
// A uid's exact-expiry ExpiryCarry row may carry a finite `T_override`; it then replaces
// time_to_expiry(expiry_ns) under the state's TimeSpec for every contract of that expiry.
// Every place the engine derives an option's T from (state instant, expiry, TimeSpec)
// goes through `expiry_T`: MarketState::resolve (so the family geometry, the family
// lanes and the cold lanes of both kernels), MarketState::carry_at's row match, the
// kernel's structural clock check, and the explain's per-expiry time step. One helper,
// so no route can price an expiry on one clock and explain it on another.

#include <cmath>
#include <cstdint>
#include <span>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/risk/market_state.hpp"

namespace atx::vol::risk_detail {

// The row of `rows` (sorted ascending by expiry_ns) with exactly `expiry_ns`, or nullptr.
[[nodiscard]] const ExpiryCarry *expiry_row(std::span<const ExpiryCarry> rows,
                                            std::int64_t expiry_ns) noexcept;

// The exact-expiry row of `uid` in `state` under ANY CarryFrom (a T override is a clock,
// not carry), or nullptr when the uid or the row is absent.
[[nodiscard]] const ExpiryCarry *expiry_row(const MarketState &state, std::uint32_t uid,
                                            std::int64_t expiry_ns) noexcept;

[[nodiscard]] inline bool has_T_override(const ExpiryCarry *row) noexcept {
  return row != nullptr && std::isfinite(row->T_override);
}

// `row`'s finite T_override, else state.time_to_expiry(expiry_ns) (which may refuse:
// VolTime OutOfRange). `row` must be the exact-expiry row of the contract's uid, or null.
[[nodiscard]] Result<double> expiry_T(const MarketState &state, const ExpiryCarry *row,
                                      std::int64_t expiry_ns) noexcept;

} // namespace atx::vol::risk_detail
