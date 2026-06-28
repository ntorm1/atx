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

#include <cmath>
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

// ===========================================================================
//  S2-2 / S2-3 — opt-in derived signal families (IV-surface, liquidity).
//
//  These appended families are NEVER produced by the default panel-build path:
//  `with_alpha101_fields` does not call them. A panel built without an explicit
//  with_iv_fields / with_liquidity_fields call is byte-identical to pre-S2. Each
//  entry point is idempotent (guarded by detail::has_field) and appends its
//  columns at the END so existing FieldIds never renumber.
// ===========================================================================

namespace detail {

// Cross-sectional sample z-score (ddof=1) of one date's row, computed over the
// in-universe, non-NaN instruments in ASCENDING instrument-index order, matching
// the engine's cs_zscore_row EXACTLY (mean over the valid set; sample std with
// n-1; a valid count < 2 -> every cell on that date is NaN). Out-of-universe,
// NaN-input, and out-of-set cells stay NaN. `row` length == instruments; `univ`
// is the date's universe slice (empty span -> all in-universe).
inline void cs_zscore_row_aug(std::span<const atx::f64> row, std::span<const std::uint8_t> univ,
                              std::span<atx::f64> out) noexcept {
  const atx::usize n_inst = row.size();
  atx::f64 sum = 0.0;
  atx::usize n = 0;
  for (atx::usize i = 0; i < n_inst; ++i) {
    const bool in = univ.empty() || univ[i] != 0;
    if (in && !std::isnan(row[i])) {
      sum += row[i];
      ++n;
    }
  }
  if (n < 2) {
    return; // fewer than 2 valid obs -> leave the whole row NaN (matches engine)
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (atx::usize i = 0; i < n_inst; ++i) {
    const bool in = univ.empty() || univ[i] != 0;
    if (in && !std::isnan(row[i])) {
      const atx::f64 d = row[i] - mean;
      ss += d * d;
    }
  }
  const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(n - 1));
  for (atx::usize i = 0; i < n_inst; ++i) {
    const bool in = univ.empty() || univ[i] != 0;
    if (in && !std::isnan(row[i])) {
      out[i] = (row[i] - mean) / sd; // sd could be 0 -> +/-inf or NaN; propagates
    }
  }
}

// Causal trailing SAMPLE std (ddof=1) of `src` over a `window`-bar window, per
// instrument (date-major stride == instruments). Value at date t requires a FULL
// window [t-window+1, t] of in-universe, non-NaN observations, else NaN — the
// same full-window / any-NaN -> NaN / causal policy as datafields::rolling_mean
// (so the column equals the engine's ts_std(src, window) on this fixture class).
inline std::vector<atx::f64> rolling_sample_std(std::span<const atx::f64> src,
                                                std::span<const std::uint8_t> universe,
                                                atx::usize dates, atx::usize instruments,
                                                atx::usize window) {
  std::vector<atx::f64> out(dates * instruments, kAugNaN);
  if (window < 2) {
    return out; // sample std undefined for window < 2
  }
  for (atx::usize j = 0; j < instruments; ++j) {
    for (atx::usize t = 0; t < dates; ++t) {
      if (t + 1 < window) {
        continue; // incomplete window -> NaN
      }
      atx::f64 sum = 0.0;
      bool ok = true;
      for (atx::usize k = t + 1 - window; k <= t; ++k) {
        const atx::usize cell = k * instruments + j;
        const bool in = universe.empty() || universe[cell] != 0;
        const atx::f64 v = src[cell];
        if (!in || std::isnan(v)) {
          ok = false;
          break;
        }
        sum += v;
      }
      if (!ok) {
        continue;
      }
      const atx::f64 mean = sum / static_cast<atx::f64>(window);
      atx::f64 ss = 0.0;
      for (atx::usize k = t + 1 - window; k <= t; ++k) {
        const atx::f64 d = src[k * instruments + j] - mean;
        ss += d * d;
      }
      out[t * instruments + j] = std::sqrt(ss / static_cast<atx::f64>(window - 1));
    }
  }
  return out;
}

} // namespace detail

// Append three IV-surface columns derived from already-loaded ORATS options /
// earnings fields, and return the augmented Panel. Appended in this order:
//
//   iv_term = zscore(atmCenI_21d / atmCenI_126d)   — cross-sectional term slope
//   iv_vrp  = atmCenI_21d - ts_std(returns, 21)     — IV minus realized vol gap
//   iv_lo   = atmCenI_21d / (nEarnCnt_5d + 1.0)      — IV conditioned on earnings
//
// CONTRACT
//   * Requires `atmCenI_21d`: Err(NotFound) if absent (the family's anchor field).
//   * Requires `returns` (for iv_vrp): Err(NotFound) if absent. `returns` is
//     materialized by with_alpha101_fields, so call that FIRST — this function
//     does NOT call it (the caller owns the field set / adv windows).
//   * `nEarnCnt_5d` is OPTIONAL: when absent, iv_lo's denominator is 1.0 (a
//     documented fallback, NOT a silent error) so iv_lo == atmCenI_21d.
//   * zscore is cross-sectional sample z (ddof=1) per date over in-universe
//     non-NaN cells, NaN where that count < 2. ts_std is the causal trailing
//     sample std over 21 dates (full-window / any-NaN -> NaN). Any cell whose
//     required input is NaN or which is out-of-universe is NaN.
//   * IDEMPOTENT: each column is guarded by detail::has_field; re-calling adds no
//     duplicates and returns the panel unchanged for already-present columns.
//
// Determinism: opt-in only. No default path calls this; appending leaves every
// existing FieldId / column byte unchanged.
[[nodiscard]] inline atx::core::Result<Panel> with_iv_fields(const Panel &base) {
  const atx::usize D = base.dates();
  const atx::usize I = base.instruments();
  const atx::usize cells = D * I;

  // Reconstruct mutable field vectors so we can append derived columns.
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  names.reserve(base.num_fields() + 3);
  data.reserve(base.num_fields() + 3);
  for (atx::usize f = 0; f < base.num_fields(); ++f) {
    names.emplace_back(base.field_name(f));
    const std::span<const atx::f64> col = base.field_all(static_cast<FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }

  // Universe mask (date-major, 1 == in-universe).
  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < I; ++n) {
      universe[d * I + n] = base.in_universe(d, n) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  const std::span<const std::uint8_t> univ{universe};

  const atx::usize iv21_i = datafields::detail::field_index(names, "atmCenI_21d");
  if (iv21_i == static_cast<atx::usize>(-1)) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "with_iv_fields: panel has no 'atmCenI_21d' field");
  }
  const atx::usize ret_i = datafields::detail::field_index(names, "returns");
  if (ret_i == static_cast<atx::usize>(-1)) {
    return atx::core::Err(
        atx::core::ErrorCode::NotFound,
        "with_iv_fields: panel has no 'returns' field (call with_alpha101_fields first)");
  }
  const atx::usize iv126_i = datafields::detail::field_index(names, "atmCenI_126d");
  const atx::usize nearn_i = datafields::detail::field_index(names, "nEarnCnt_5d");

  // Snapshot the inputs we read (indices into `data` are stable until we append).
  const std::vector<atx::f64> iv21 = data[iv21_i];
  const std::vector<atx::f64> returns = data[ret_i];
  const std::vector<atx::f64> *iv126 =
      (iv126_i == static_cast<atx::usize>(-1)) ? nullptr : &data[iv126_i];
  const std::vector<atx::f64> *nearn =
      (nearn_i == static_cast<atx::usize>(-1)) ? nullptr : &data[nearn_i];

  // --- iv_term = zscore(atmCenI_21d / atmCenI_126d) per date.
  if (!datafields::detail::has_field(names, "iv_term")) {
    std::vector<atx::f64> iv_term(cells, kAugNaN);
    // The ratio (NaN if either input NaN, atmCenI_126d absent, or denom == 0).
    std::vector<atx::f64> ratio(cells, kAugNaN);
    for (atx::usize i = 0; i < cells; ++i) {
      if (univ[i] == 0 || iv126 == nullptr) {
        continue;
      }
      const atx::f64 num = iv21[i];
      const atx::f64 den = (*iv126)[i];
      if (!std::isnan(num) && !std::isnan(den) && den != 0.0) {
        ratio[i] = num / den;
      }
    }
    for (atx::usize d = 0; d < D; ++d) {
      detail::cs_zscore_row_aug(std::span<const atx::f64>{ratio}.subspan(d * I, I),
                                univ.subspan(d * I, I),
                                std::span<atx::f64>{iv_term}.subspan(d * I, I));
    }
    names.emplace_back("iv_term");
    data.push_back(std::move(iv_term));
  }

  // --- iv_vrp = atmCenI_21d - ts_std(returns, 21).
  if (!datafields::detail::has_field(names, "iv_vrp")) {
    const std::vector<atx::f64> rstd =
        detail::rolling_sample_std(std::span<const atx::f64>{returns}, univ, D, I, 21);
    std::vector<atx::f64> iv_vrp(cells, kAugNaN);
    for (atx::usize i = 0; i < cells; ++i) {
      if (univ[i] == 0) {
        continue;
      }
      const atx::f64 a = iv21[i];
      const atx::f64 s = rstd[i];
      if (!std::isnan(a) && !std::isnan(s)) {
        iv_vrp[i] = a - s;
      }
    }
    names.emplace_back("iv_vrp");
    data.push_back(std::move(iv_vrp));
  }

  // --- iv_lo = atmCenI_21d / (nEarnCnt_5d + 1.0); denom defaults to 1.0.
  if (!datafields::detail::has_field(names, "iv_lo")) {
    std::vector<atx::f64> iv_lo(cells, kAugNaN);
    for (atx::usize i = 0; i < cells; ++i) {
      if (univ[i] == 0) {
        continue;
      }
      const atx::f64 a = iv21[i];
      if (std::isnan(a)) {
        continue;
      }
      atx::f64 denom = 1.0;
      if (nearn != nullptr) {
        const atx::f64 ne = (*nearn)[i];
        if (std::isnan(ne)) {
          continue; // a present nEarnCnt_5d that is NaN propagates to NaN
        }
        denom = ne + 1.0;
      }
      iv_lo[i] = a / denom; // denom >= 1 when from nEarnCnt_5d (count is >= 0)
    }
    names.emplace_back("iv_lo");
    data.push_back(std::move(iv_lo));
  }

  return Panel::create(D, I, std::move(names), std::move(data), std::move(universe));
}

} // namespace atx::engine::alpha
