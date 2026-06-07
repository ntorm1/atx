#pragma once

// atx::engine::data — universe selection helpers. Pure, deterministic, no I/O.

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace atx::engine::data {

// Median of a copy (does not mutate input). Empty -> 0.0.
[[nodiscard]] inline double median(std::vector<double> v) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Rank symbols by the median of their daily notionals (descending), tie-break by
// symbol ascending for determinism, return the first `n`.
[[nodiscard]] inline std::vector<std::string> top_n_by_median_notional(
    const std::unordered_map<std::string, std::vector<double>>& notionals_by_symbol,
    std::size_t n) {
  std::vector<std::pair<std::string, double>> scored;
  scored.reserve(notionals_by_symbol.size());
  for (const auto& [sym, notionals] : notionals_by_symbol) {
    scored.emplace_back(sym, median(notionals));
  }
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) {
      return a.second > b.second; // higher notional first
    }
    return a.first < b.first; // tie-break: symbol ascending
  });
  if (scored.size() > n) {
    scored.resize(n);
  }
  std::vector<std::string> out;
  out.reserve(scored.size());
  for (auto& kv : scored) {
    out.push_back(std::move(kv.first));
  }
  return out;
}

} // namespace atx::engine::data
