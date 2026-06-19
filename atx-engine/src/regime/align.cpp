#include "atx/engine/regime/align.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atx::engine::regime {

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] atx::usize index_of(const std::vector<std::string> &names, const std::string &n) {
  for (atx::usize i = 0; i < names.size(); ++i) {
    if (names[i] == n) return i;
  }
  return static_cast<atx::usize>(-1);
}
}  // namespace

std::vector<atx::i64> build_master_axis(std::span<const NamedSeries> series,
                                        atx::i64 min_date_nanos) {
  std::vector<atx::i64> all;
  for (const NamedSeries &s : series) {
    for (const auto &kv : s.obs) {
      if (kv.first >= min_date_nanos) all.push_back(kv.first);
    }
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  return all;
}

std::vector<atx::f64> forward_fill(std::span<const std::pair<atx::i64, atx::f64>> obs,
                                   std::span<const atx::i64> axis) {
  std::vector<atx::f64> out(axis.size(), kNaN);
  atx::usize oi = 0;
  atx::f64 last = kNaN;
  bool have = false;
  for (atx::usize i = 0; i < axis.size(); ++i) {
    while (oi < obs.size() && obs[oi].first <= axis[i]) {
      last = obs[oi].second;
      have = true;
      ++oi;
    }
    out[i] = have ? last : kNaN;
  }
  return out;
}

atx::core::Status apply_derived(std::vector<std::string> &names,
                                std::vector<std::vector<atx::f64>> &cols, const DerivedSpec &spec) {
  const atx::usize li = index_of(names, spec.lhs);
  const atx::usize ri = index_of(names, spec.rhs);
  if (li == static_cast<atx::usize>(-1) || ri == static_cast<atx::usize>(-1)) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          std::string{"apply_derived: missing operand for '"} + spec.name + "'");
  }
  const std::vector<atx::f64> &lhs = cols[li];
  const std::vector<atx::f64> &rhs = cols[ri];
  std::vector<atx::f64> out(lhs.size(), kNaN);
  for (atx::usize i = 0; i < lhs.size(); ++i) {
    const atx::f64 a = lhs[i];
    const atx::f64 b = rhs[i];
    switch (spec.op) {
      case '+': out[i] = a + b; break;
      case '-': out[i] = a - b; break;
      case '*': out[i] = a * b; break;
      case '/': out[i] = (b == 0.0) ? kNaN : a / b; break;
      default:
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              std::string{"apply_derived: bad op for '"} + spec.name + "'");
    }
  }
  names.push_back(spec.name);
  cols.push_back(std::move(out));
  return atx::core::Ok();
}

}  // namespace atx::engine::regime
