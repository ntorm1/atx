#pragma once

// atx::engine::quant — parse an OSI (Options Symbology Initiative) 21-char
// option symbol "RRRRRRYYMMDDTSSSSSSSS": 6-char space-padded root, 6-digit
// YYMMDD expiry, 1-char type (C/P), 8-digit strike in thousandths of a dollar.
// Pure and header-only.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace atx::engine::quant {

struct OsiOption {
  std::string root;   // e.g. "AAPL", "BRKB"
  int year{};         // full year, e.g. 2027
  int month{};        // 1-12
  int day{};          // 1-31
  bool is_call{};
  double strike{};    // dollars, e.g. 250.0
};

[[nodiscard]] inline std::optional<OsiOption> parse_osi(std::string_view sym) {
  if (sym.size() != 21) {
    return std::nullopt;
  }
  const auto all_digits = [](std::string_view s) {
    for (const char c : s) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    return true;
  };
  if (!all_digits(sym.substr(6, 6)) || !all_digits(sym.substr(13, 8))) {
    return std::nullopt;
  }
  const char type = sym[12];
  if (type != 'C' && type != 'P') {
    return std::nullopt;
  }
  std::size_t root_end = 6;
  while (root_end > 0 && sym[root_end - 1] == ' ') {
    --root_end;
  }
  if (root_end == 0) {
    return std::nullopt;
  }
  const auto to_int = [](std::string_view s) {
    int v = 0;
    for (const char c : s) {
      v = v * 10 + (c - '0');
    }
    return v;
  };
  OsiOption o;
  o.root = std::string{sym.substr(0, root_end)};
  o.year = 2000 + to_int(sym.substr(6, 2));
  o.month = to_int(sym.substr(8, 2));
  o.day = to_int(sym.substr(10, 2));
  if (o.month < 1 || o.month > 12 || o.day < 1 || o.day > 31) {
    return std::nullopt;
  }
  o.is_call = (type == 'C');
  o.strike = static_cast<double>(to_int(sym.substr(13, 8))) / 1000.0;
  return o;
}

} // namespace atx::engine::quant
