#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace atx::engine::alpha {

// These are daily-bar proxies, not observed intraday volume-weighted prices.
enum class VwapRule : std::uint8_t {
  AdjustedTypicalV1 = 1, // caller-supplied value, else (high + low + close) / 3
  RawDailyCloseV2 = 2,  // (raw_close * volume) / volume, evaluated without overflow
};

// A generic field named close does not establish an as-traded price basis.
enum class ClosePriceBasis : std::uint8_t { Unknown = 0, Raw = 1 };

[[nodiscard]] constexpr std::string_view vwap_rule_name(VwapRule rule) noexcept {
  switch (rule) {
  case VwapRule::AdjustedTypicalV1: return "adjusted-typical-v1";
  case VwapRule::RawDailyCloseV2: return "raw-daily-close-v2";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::optional<VwapRule>
parse_vwap_rule(std::string_view name) noexcept {
  if (name == "adjusted-typical-v1") return VwapRule::AdjustedTypicalV1;
  if (name == "raw-daily-close-v2") return VwapRule::RawDailyCloseV2;
  return std::nullopt;
}

} // namespace atx::engine::alpha
