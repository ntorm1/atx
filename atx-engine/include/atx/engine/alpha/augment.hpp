#pragma once

// atx::engine::alpha — production Alpha101 panel augmentation (S5-0).
//
// `with_alpha101_fields` takes a base OHLCV-shaped panel and returns one that
// carries the FULL Alpha101 input vocabulary (Kakushadze 2016, arXiv:1601.00991)
// required for parsing/compiling/evaluating the 101 canonical formulae verbatim:
//
//   * returns           = close[t]/close[t-1] - 1   (causal; NaN on day 0 /
//                         universe gaps / zero prev-close / NaN inputs)
//   * cap               = market_cap (copied if present); else close*1e8 for
//                         in-universe cells, NaN elsewhere (synthetic fallback —
//                         documented proxy, NOT a real market-cap estimate)
//   * IndClass.sector   = widened f64 copy of the GICS `sector` code; 0.0 column
//                         if `sector` absent (exercises group-aware DSL/VM path)
//   * IndClass.industry = ALIAS of IndClass.sector (see I5-HOOK below)
//   * IndClass.subindustry = ALIAS of IndClass.sector (see I5-HOOK below)
//
//   NOTE: `industry` and `subindustry` alias the GICS SECTOR granularity — this
//   is NOT a claim of finer GICS fidelity.  The ORATS panel materializes only the
//   sector granularity; these stand-ins ensure the industry-neutralization DSL/VM
//   path is exercised in tests.  A future sprint replaces them with true SIC/NAICS
//   industry data at the I5-HOOK marker below.
//
//   * dollar_volume / vwap / adv{d} = delegated to
//     atx::engine::alpha::datafields::with_datafields (same derivation the engine
//     uses for all other callers; adv{d} == ts_mean(dollar_volume, d) bit-for-bit).
//
// This function is the production lift of `atx_impl_test::augment_for_alpha101`
// from atx-impl/tests/alpha101_support.hpp.  The two are byte-identical until
// Task 2 makes the test helper delegate here (pinned by DelegationIdentity test).
//
// ADDITIVE / IDEMPOTENT: every derivation is guarded by a presence check.
// Re-calling on an already-augmented panel adds NO duplicate columns.
//
// Header-only; construction is a COLD path — std::vector allocations are fine.
// Errors travel in Result; nothing throws.

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::alpha {

// Canonical quiet NaN for missing-cell sentinels (same policy as datafields.hpp).
inline constexpr atx::f64 kAugNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Return a panel carrying every Alpha101 input field, derived from `base` (which
// must provide at least open/high/low/close/volume). `adv_windows` is the set of
// adv{d} columns to materialize (e.g. {5,20,60}).
//
// Err(NotFound) if `base` has no `close` field (the minimum required input).
// Ragged panel geometry propagates through Panel::create as Err(InvalidArgument).
[[nodiscard]] inline atx::core::Result<Panel>
with_alpha101_fields(const Panel &base, std::span<const atx::u16> adv_windows) {
  const atx::usize D = base.dates();
  const atx::usize I = base.instruments();
  const atx::usize cells = D * I;

  // Reconstruct mutable field vectors from base so we can append derived columns.
  // Reserve capacity upfront: base fields + up to 8 derived ones.
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  names.reserve(base.num_fields() + 8);
  data.reserve(base.num_fields() + 8);
  for (atx::usize f = 0; f < base.num_fields(); ++f) {
    names.emplace_back(base.field_name(f));
    const std::span<const atx::f64> col =
        base.field_all(static_cast<FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }

  // Reconstruct universe mask (date-major, 1 == in-universe).
  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < I; ++n) {
      universe[d * I + n] = base.in_universe(d, n) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }

  // close is required; all other base fields are optional.
  const atx::usize close_i =
      datafields::detail::field_index(names, "close");
  if (close_i == static_cast<atx::usize>(-1)) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "with_alpha101_fields: base panel has no 'close' field");
  }

  // --- returns = close[t]/close[t-1] - 1, per instrument (date-major stride I).
  // Causal: day 0 is always NaN (no prior observation). A cell is NaN when either
  // day is out-of-universe, when prev_close == 0, or when a close input is NaN.
  if (!datafields::detail::has_field(names, "returns")) {
    const std::vector<atx::f64> &close = data[close_i];
    std::vector<atx::f64> returns(cells, kAugNaN);
    for (atx::usize n = 0; n < I; ++n) {
      for (atx::usize d = 1; d < D; ++d) {
        const atx::usize i = d * I + n;
        const atx::usize p = (d - 1) * I + n;
        if (universe[i] != 0 && universe[p] != 0) {
          const atx::f64 c = close[i];
          const atx::f64 pc = close[p];
          // NaN inputs propagate through the division; zero prev-close -> NaN.
          returns[i] = (pc != 0.0) ? (c / pc - 1.0) : kAugNaN;
        }
      }
    }
    names.emplace_back("returns");
    data.push_back(std::move(returns));
  }

  // --- cap = market_cap (copied). Fall back to close*1e8 only if market_cap absent.
  // The fallback is documented as a synthetic proxy, NOT a real market-cap estimate.
  if (!datafields::detail::has_field(names, "cap")) {
    const atx::usize mc_i =
        datafields::detail::field_index(names, "market_cap");
    std::vector<atx::f64> cap(cells, kAugNaN);
    if (mc_i != static_cast<atx::usize>(-1)) {
      cap = data[mc_i]; // copy the market_cap column verbatim
    } else {
      const std::vector<atx::f64> &close = data[close_i];
      for (atx::usize i = 0; i < cells; ++i) {
        if (universe[i] != 0) {
          cap[i] = close[i] * 1.0e8;
        }
      }
    }
    names.emplace_back("cap");
    data.push_back(std::move(cap));
  }

  // --- IndClass group classifiers, derived from the GICS `sector` code.
  //
  // I5-HOOK: Replace sec (a widened f64 copy of the sector label) with true SIC /
  // NAICS industry and sub-industry codes once finer GICS data is materialized in
  // the panel.  Until then, all three classifiers alias the sector column so the
  // industry-neutralization DSL/VM path is exercised without claiming finer fidelity.
  {
    const atx::usize sec_i =
        datafields::detail::field_index(names, "sector");
    // If sector is absent, a constant 0.0 column makes the whole universe one group
    // — group-aware ops (e.g. group_neutralize) still run without error.
    std::vector<atx::f64> sec(cells, 0.0);
    if (sec_i != static_cast<atx::usize>(-1)) {
      sec = data[sec_i];
    }
    for (std::string_view g :
         {"IndClass.sector", "IndClass.industry", "IndClass.subindustry"}) {
      if (!datafields::detail::has_field(names, g)) {
        names.emplace_back(g);
        data.push_back(sec); // shared copy — all three start identical
      }
    }
  }

  // --- Delegate dollar_volume / vwap / adv{d} to the engine's own derivation.
  // This guarantees the derived columns are bit-for-bit identical to what every
  // other engine caller (stage_panel, etc.) would produce from the same inputs.
  return datafields::with_datafields(
      D, I, std::move(names), std::move(data), std::move(universe), adv_windows);
}

} // namespace atx::engine::alpha
