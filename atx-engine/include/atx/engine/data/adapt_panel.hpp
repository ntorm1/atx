#pragma once

// atx::engine::data — adapt_panel (S6.4a): Price Dataset → alpha::Panel bridge.
//
// `price_to_panel` lowers a Role::Price Dataset into an alpha::Panel by
// forwarding its columns and universe mask verbatim to
// datafields::with_datafields.  Because it does NOT re-derive any data, the
// resulting Panel is BYTE-IDENTICAL to a hand call to with_datafields on the
// same arrays.  This is the boundary-pin guarantee: any price-only DataContext
// (S6.8) must reduce to the same Panel the existing fixed-panel path produces.
//
// Preconditions (forwarded to with_datafields):
//   * The Dataset must carry at least "close" and "volume" columns.
//   * V2 requires raw_close or the caller's explicit Raw close basis.
//   * Legacy V1 requires high/low only when vwap is absent.
//   * with_datafields Err(NotFound)s on the first missing required field.
// Cold path; copies are intentional.

#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/vwap_rule.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

// Lower a Price Dataset into an alpha::Panel.
//
// Repackages the Dataset's columns + mask and forwards them unchanged to
// datafields::with_datafields (defined in alpha/datafields.hpp), which appends
// the derived dollar_volume, vwap, and adv{d} columns before building the Panel.
// The Panel returned is byte-identical to a direct call to with_datafields on
// the same raw arrays — that identity is the boundary-pin seed for S6.8.
//
// Returns Err(NotFound) if a required base field (close, volume, …) is missing.
// Returns Err(InvalidArgument) for ragged column sizes (forwarded from Panel::create).
// Positive pit_delay is also InvalidArgument: align onto a canonical decision
// grid first and wrap the already-aligned values with zero delay. A Panel has
// no date keys with which to enforce the source dataset's availability.
[[nodiscard]] atx::core::Result<alpha::Panel> price_to_panel(const Dataset &price,
    std::span<const atx::u16> adv_windows,
    alpha::VwapRule vwap_rule = alpha::VwapRule::RawDailyCloseV2,
    alpha::ClosePriceBasis close_basis = alpha::ClosePriceBasis::Unknown);

} // namespace atx::engine::data
