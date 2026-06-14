#pragma once

// atx::engine::alpha — derived datafields (S3.3): vwap / dollar_volume / adv{d}.
//
// Alpha101 and the BRAIN vocabulary reference price-derived columns —
// `vwap`, `dollar_volume`, `adv20` — as if they were raw inputs (e.g.
// `rank(close / adv20)`). They are NOT operators: baking them into the ISA
// would pollute the otherwise price-volume-clean opcode set (impl plan §0.5).
// Instead they are DERIVED PANEL COLUMNS — computed once at panel-construction
// time from the base OHLCV fields and appended to the field dictionary, so a
// formula loads `adv20` through the same `LoadField` path as `close`.
//
// PIT / universe discipline (identical to OHLC): a derived cell is NaN whenever
// it is out-of-universe at that date OR any input is NaN — never imputed. adv is
// computed from the ALREADY-masked dollar_volume so an out-of-universe day
// breaks the trailing window exactly as a missing observation would (the
// rolling mean is the engine's full-window / any-NaN -> NaN / causal policy, so
// the derived `adv{d}` column equals `ts_mean(dollar_volume, d)` bit-for-bit).
//
//   * dollar_volume = close · volume
//   * vwap          = supplied `vwap` field if present, else the typical-price
//                     proxy (high + low + close) / 3 (documented as a proxy when
//                     a true volume-weighted price is unavailable)
//   * adv{d}        = ts_mean(dollar_volume, d)  — causal trailing mean
//
// Header-only; construction is a COLD path (once per backtest window), so the
// std::vector allocation here is fine. Errors travel in Result; nothing throws.

#include <charconv>
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

#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::alpha::datafields {

// Canonical derived-field names recognized by the panel builder.
inline constexpr std::string_view kClose = "close";
inline constexpr std::string_view kVolume = "volume";
inline constexpr std::string_view kHigh = "high";
inline constexpr std::string_view kLow = "low";
inline constexpr std::string_view kVwap = "vwap";
inline constexpr std::string_view kDollarVolume = "dollar_volume";
inline constexpr std::string_view kAdvPrefix = "adv";

// Parse an "adv{d}" field name into its window `d` (e.g. "adv20" -> 20). Returns
// true and writes `window_out` iff `name` is exactly "adv" followed by a
// positive integer that fits a u16; false otherwise (not an adv field, or a
// degenerate / overflowing window).
[[nodiscard]] inline bool parse_adv_field(std::string_view name, atx::u16 &window_out) noexcept {
  if (name.size() <= kAdvPrefix.size() || name.substr(0, kAdvPrefix.size()) != kAdvPrefix) {
    return false;
  }
  const std::string_view digits = name.substr(kAdvPrefix.size());
  unsigned value = 0;
  const char *first = digits.data();
  const char *last = digits.data() + digits.size();
  const std::from_chars_result r = std::from_chars(first, last, value);
  if (r.ec != std::errc{} || r.ptr != last) {
    return false; // non-numeric suffix or trailing junk
  }
  if (value < 1 || value > 0xFFFFu) {
    return false;
  }
  window_out = static_cast<atx::u16>(value);
  return true;
}

namespace detail {

inline constexpr atx::f64 kDfNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] inline bool df_in_universe(std::span<const std::uint8_t> universe,
                                         atx::usize idx) noexcept {
  return universe.empty() || universe[idx] != 0; // empty == all in-universe
}

[[nodiscard]] inline bool has_field(const std::vector<std::string> &names,
                                    std::string_view name) noexcept {
  for (const std::string &n : names) {
    if (n == name) {
      return true;
    }
  }
  return false;
}

// Index of `name` in `names`, or SIZE_MAX if absent.
[[nodiscard]] inline atx::usize field_index(const std::vector<std::string> &names,
                                            std::string_view name) noexcept {
  for (atx::usize i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      return i;
    }
  }
  return static_cast<atx::usize>(-1);
}

// Causal trailing mean of `src` over a `d`-bar window, matching the engine's
// ts_mean policy EXACTLY: per instrument column, value at date t requires a full
// window [t-d+1, t] with NO NaN, else NaN; the mean sums oldest->newest /d. The
// per-instrument stride is `instruments` (date-major layout).
[[nodiscard]] inline std::vector<atx::f64> rolling_mean(std::span<const atx::f64> src,
                                                        atx::usize dates, atx::usize instruments,
                                                        atx::usize d) {
  std::vector<atx::f64> out(dates * instruments, kDfNaN);
  if (d == 0) {
    return out;
  }
  for (atx::usize j = 0; j < instruments; ++j) {
    for (atx::usize t = 0; t < dates; ++t) {
      if (t + 1 < d) {
        continue; // incomplete window -> NaN
      }
      atx::f64 sum = 0.0;
      bool ok = true;
      for (atx::usize k = t + 1 - d; k <= t; ++k) { // oldest -> newest
        const atx::f64 v = src[k * instruments + j];
        if (std::isnan(v)) {
          ok = false;
          break;
        }
        sum += v;
      }
      if (ok) {
        out[t * instruments + j] = sum / static_cast<atx::f64>(d);
      }
    }
  }
  return out;
}

} // namespace detail

// Build an augmented Panel: copy the base field set, then append the derived
// `dollar_volume`, `vwap`, and one `adv{d}` column per requested window. A
// derived name that is ALREADY supplied in `field_names` is left untouched (the
// caller's column wins — e.g. a true `vwap` overrides the typical-price proxy).
//
// Requires `close` and `volume` for dollar_volume/adv; if `vwap` is absent it is
// derived from `high`/`low`/`close` (so those are required only in that case).
// Err(NotFound) names the first missing required base field; ragged input maps
// to Panel::create's Err(InvalidArgument).
[[nodiscard]] inline atx::core::Result<Panel>
with_datafields(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
                std::vector<std::vector<atx::f64>> field_data, std::vector<std::uint8_t> universe,
                std::span<const atx::u16> adv_windows) {
  const atx::usize cells = dates * instruments;
  const std::span<const std::uint8_t> univ{universe};

  auto require = [&](std::string_view name) -> atx::core::Result<atx::usize> {
    const atx::usize idx = detail::field_index(field_names, name);
    if (idx == static_cast<atx::usize>(-1)) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            std::string{"with_datafields: missing required base field '"} +
                                std::string{name} + "'");
    }
    return atx::core::Ok(idx);
  };

  ATX_TRY(const atx::usize close_i, require(datafields::kClose));
  ATX_TRY(const atx::usize volume_i, require(datafields::kVolume));
  const std::span<const atx::f64> close{field_data[close_i]};
  const std::span<const atx::f64> volume{field_data[volume_i]};

  // dollar_volume = close · volume, masked to the universe (NaN out-of-universe).
  std::vector<atx::f64> dvol(cells, detail::kDfNaN);
  for (atx::usize i = 0; i < cells; ++i) {
    if (detail::df_in_universe(univ, i)) {
      dvol[i] = close[i] * volume[i]; // NaN inputs propagate
    }
  }

  // vwap: keep a supplied column; else derive the typical-price proxy.
  const bool derive_vwap = !detail::has_field(field_names, datafields::kVwap);
  std::vector<atx::f64> vwap;
  if (derive_vwap) {
    ATX_TRY(const atx::usize high_i, require(datafields::kHigh));
    ATX_TRY(const atx::usize low_i, require(datafields::kLow));
    const std::span<const atx::f64> high{field_data[high_i]};
    const std::span<const atx::f64> low{field_data[low_i]};
    vwap.assign(cells, detail::kDfNaN);
    for (atx::usize i = 0; i < cells; ++i) {
      if (detail::df_in_universe(univ, i)) {
        vwap[i] = (high[i] + low[i] + close[i]) / 3.0;
      }
    }
  }

  // adv{d} = ts_mean(dollar_volume, d) per requested window (skip duplicates and
  // any window whose adv name is already supplied).
  std::vector<std::pair<std::string, std::vector<atx::f64>>> adv_cols;
  for (const atx::u16 d : adv_windows) {
    std::string name = std::string{datafields::kAdvPrefix} + std::to_string(d);
    if (detail::has_field(field_names, name)) {
      continue; // caller supplied this adv column
    }
    bool already = false;
    for (const auto &c : adv_cols) {
      already = already || c.first == name;
    }
    if (already) {
      continue; // duplicate window in the request
    }
    adv_cols.emplace_back(std::move(name), detail::rolling_mean(dvol, dates, instruments, d));
  }

  // Append derived columns (only those not already supplied) and build.
  if (!detail::has_field(field_names, datafields::kDollarVolume)) {
    field_names.emplace_back(datafields::kDollarVolume);
    field_data.push_back(std::move(dvol));
  }
  if (derive_vwap) {
    field_names.emplace_back(datafields::kVwap);
    field_data.push_back(std::move(vwap));
  }
  for (auto &c : adv_cols) {
    field_names.push_back(std::move(c.first));
    field_data.push_back(std::move(c.second));
  }
  return Panel::create(dates, instruments, std::move(field_names), std::move(field_data),
                       std::move(universe));
}

} // namespace atx::engine::alpha::datafields
