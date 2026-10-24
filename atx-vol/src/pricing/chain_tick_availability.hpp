#pragma once

// Optional output metadata for tick routes. This records supplied estimates;
// it neither changes their arithmetic nor certifies derivative accuracy.
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <span>

#include "atx/vol/api/pricing/chain_tick.hpp"
#include "pricing/american_greek_availability.hpp"

namespace atx::vol::detail {

inline constexpr auto kTickGreekFields = GreekFieldMask::Price | GreekFieldMask::Delta |
    GreekFieldMask::Gamma | GreekFieldMask::Vega | GreekFieldMask::Theta;
inline constexpr std::array<GreekFieldMask, 4> kTickDerivativeFields{
    GreekFieldMask::Delta, GreekFieldMask::Gamma, GreekFieldMask::Vega, GreekFieldMask::Theta};

[[nodiscard]] inline bool tick_has_metadata(const ChainTickOutputs &out) noexcept {
  return !out.requested.empty() || !out.supported.empty();
}

inline void write_tick_metadata(const ChainTickOutputs &out, std::size_t i,
                                GreekFieldMask supplied) noexcept {
  if (!out.requested.empty()) out.requested[i] = kTickGreekFields;
  if (!out.supported.empty()) out.supported[i] = supplied & kTickGreekFields;
}

[[nodiscard]] inline GreekFieldMask tick_producer_support(const AmericanGreeks &g) noexcept {
  if (!american_greeks_support(g, GreekFieldMask::Price)) return GreekFieldMask::None;
  auto fields = GreekFieldMask::Price;
  for (const auto field : kTickDerivativeFields)
    if (american_greeks_support(g, field)) fields = fields | field;
  return fields;
}

// The raw record/ladder APIs predate per-field refusal reasons. Their finite
// outputs are estimates only; a failed lane or missing price supplies nothing.
[[nodiscard]] inline GreekFieldMask tick_computed_support(const ChainTickOutputs &out,
                                                         std::size_t i) noexcept {
  if (out.status[i] == TickStatus::Unpriceable || out.status[i] == TickStatus::OutOfWindow ||
      !std::isfinite(out.price[i])) return GreekFieldMask::None;
  auto fields = GreekFieldMask::Price;
  const std::array values{out.delta[i], out.gamma[i], out.vega[i], out.theta[i]};
  for (std::size_t j = 0; j < values.size(); ++j)
    if (std::isfinite(values[j])) fields = fields | kTickDerivativeFields[j];
  return fields;
}

[[nodiscard]] inline bool tick_bytes_overlap(std::span<const std::byte> a,
                                            std::span<const std::byte> b) noexcept {
  if (a.empty() || b.empty()) return false;
  // Total pointer ordering is defined across unrelated caller allocations.
  const auto less = std::less<const std::byte *>{};
  return less(a.data(), b.data() + b.size()) && less(b.data(), a.data() + a.size());
}

// Guard only the newly optional columns; existing numeric alias contracts are
// unchanged. Extra ranges include all input columns and any statement/scratch
// storage that this route reads or writes.
[[nodiscard]] inline bool tick_metadata_aliases(const ChainTickOutputs &out,
    std::initializer_list<std::span<const std::byte>> extra = {}) noexcept {
  if (!tick_has_metadata(out)) return false;
  const auto requested = std::as_bytes(out.requested);
  const auto supported = std::as_bytes(out.supported);
  if (tick_bytes_overlap(requested, supported)) return true;
  const std::array numeric{std::as_bytes(out.price), std::as_bytes(out.delta),
      std::as_bytes(out.gamma), std::as_bytes(out.vega), std::as_bytes(out.theta),
      std::as_bytes(out.status)};
  for (const auto metadata : {requested, supported}) {
    for (const auto bytes : numeric)
      if (tick_bytes_overlap(metadata, bytes)) return true;
    for (const auto bytes : extra)
      if (tick_bytes_overlap(metadata, bytes)) return true;
  }
  return false;
}

} // namespace atx::vol::detail
