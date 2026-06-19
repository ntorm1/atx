#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"

namespace atx::engine::regime {

// Field-name prefix reserved for broadcast regime columns in a Panel.
inline constexpr std::string_view kRegimePrefix = "regime_";

// Documented v1 series (config can request a subset/superset; this is the
// canonical naming reference, mirrored in regime/README.md).
inline constexpr std::array<std::string_view, 11> kCanonicalSeriesArr = {
    "vix", "vvix", "move",                     // vol complex
    "dgs2", "dgs10", "t10y2y", "t3m10y",       // rates / curve
    "hy_oas", "ig_oas", "nfci",                // credit / liquidity
    "spx_dist_200dma"};                        // breadth / trend
[[nodiscard]] inline std::span<const std::string_view> kCanonicalSeries() noexcept {
  return kCanonicalSeriesArr;
}

// A composite series defined as `name = lhs OP rhs` over two other series.
struct DerivedSpec {
  std::string name;
  std::string lhs;
  char op{'-'};
  std::string rhs;
};

// Parse "name = lhs OP rhs" (whitespace-insensitive; OP in {+,-,*,/}).
// Err(InvalidArgument) on any shape mismatch.
[[nodiscard]] inline atx::core::Result<DerivedSpec> parse_derived_spec(std::string_view s) {
  auto trim = [](std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
  };
  const std::size_t eq = s.find('=');
  if (eq == std::string_view::npos) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "parse_derived_spec: missing '='");
  }
  const std::string_view name = trim(s.substr(0, eq));
  std::string_view rhs_expr = trim(s.substr(eq + 1));
  std::size_t op_pos = std::string_view::npos;
  char op = 0;
  for (std::size_t i = 0; i < rhs_expr.size(); ++i) {
    const char c = rhs_expr[i];
    if (i > 0 && (c == '+' || c == '-' || c == '*' || c == '/')) {
      op_pos = i;
      op = c;
      break;
    }
  }
  if (name.empty() || op_pos == std::string_view::npos) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "parse_derived_spec: expected 'name = lhs OP rhs'");
  }
  const std::string_view lhs = trim(rhs_expr.substr(0, op_pos));
  const std::string_view rhs = trim(rhs_expr.substr(op_pos + 1));
  if (lhs.empty() || rhs.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "parse_derived_spec: empty operand");
  }
  return atx::core::Ok(DerivedSpec{std::string{name}, std::string{lhs}, op, std::string{rhs}});
}

}  // namespace atx::engine::regime
