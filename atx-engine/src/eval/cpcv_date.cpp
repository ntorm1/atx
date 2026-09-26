#include "atx/engine/eval/cpcv_date.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace atx::engine::eval {
namespace {
using atx::usize;
namespace co = atx::core;

// Conservative live-capacity accounting, including geometric vector growth,
// merged-window/date/index scratch and path headers. No F*N mask matrix.
bool fits_budget(usize rows, usize groups, usize folds, usize paths, atx::u64 budget) {
  const auto limit = std::min<atx::u64>(budget, std::numeric_limits<usize>::max());
  atx::u64 used = 4096U;
  const auto charge = [&](atx::u64 count, atx::u64 bytes) {
    if (used > limit || (bytes != 0U && count > (limit - used) / bytes)) return false;
    used += count * bytes;
    return true;
  };
  return charge(rows, 128U) && charge(folds, 128U) && charge(paths, 64U) &&
      charge(paths, static_cast<atx::u64>(groups) * 16U) &&
      charge(rows, static_cast<atx::u64>(folds) * 16U);
}
} // namespace

co::Result<DateCpcvPlan> cpcv_date_plan(std::span<const LabelSpan> spans,
                                     const DateCpcvConfig& cfg) {
  const auto K = cfg.n_groups, k = cfg.n_test_groups;
  if (K < 2U || K > 20U || k == 0U || k >= K || spans.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "date CPCV: invalid nonempty group geometry");
  const auto fold_count = detail::binomial(K, k);
  const auto path_count = detail::binomial(K - 1U, k - 1U);
  if (!fits_budget(spans.size(), K, fold_count, path_count, cfg.max_working_bytes))
    return co::Err(co::ErrorCode::OutOfRange, "date CPCV: working-byte budget exceeded");
  usize dates = 0;
  for (usize i = 0; i < spans.size(); ++i) {
    const auto& span = spans[i];
    if (span.t0 >= span.t1 || (i != 0U && span.t0 < spans[i - 1U].t0) ||
        cfg.embargo_dates > std::numeric_limits<usize>::max() - span.t1)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "date CPCV: unsorted/empty label or embargo endpoint overflow");
    if (i == 0U || span.t0 != spans[i - 1U].t0) ++dates;
  }
  if (dates < K)
    return co::Err(co::ErrorCode::InvalidArgument, "date CPCV: fewer distinct dates than groups");
  std::vector<usize> date_offsets;
  date_offsets.reserve(dates + 1U);
  for (usize i = 0; i < spans.size(); ++i)
    if (i == 0U || spans[i].t0 != spans[i - 1U].t0) date_offsets.push_back(i);
  date_offsets.push_back(spans.size());

  DateCpcvPlan out;
  out.config = cfg;
  out.group_offsets.reserve(K + 1U);
  for (usize g = 0; g <= K; ++g) {
    // floor(g*dates/K) without a multiply of the full observation count.
    const auto date_index = (dates / K) * g + ((dates % K) * g) / K;
    out.group_offsets.push_back(date_offsets[date_index]);
  }
  out.folds.reserve(fold_count);
  out.paths.assign(path_count, std::vector<usize>(K));
  std::vector<usize> occurrence(K, 0U), combo(k);
  for (usize i = 0; i < k; ++i) combo[i] = i;
  std::vector<bool> is_test(spans.size(), false);
  std::vector<LabelSpan> merged;
  merged.reserve(spans.size());
  do {
    const auto fold_index = out.folds.size();
    std::fill(is_test.begin(), is_test.end(), false);
    CpcvFold fold;
    for (const auto group : combo) {
      out.paths[occurrence[group]++][group] = fold_index;
      for (auto i = out.group_offsets[group]; i < out.group_offsets[group + 1U]; ++i) {
        fold.test_idx.push_back(i);
        is_test[i] = true;
      }
    }
    // Already ordered by t0, even for disconnected test groups. Merge only
    // overlapping/abutting information windows, never their global convex hull.
    merged.clear();
    for (const auto i : fold.test_idx) {
      const LabelSpan expanded{spans[i].t0, spans[i].t1 + cfg.embargo_dates};
      if (merged.empty() || expanded.t0 > merged.back().t1) merged.push_back(expanded);
      else merged.back().t1 = std::max(merged.back().t1, expanded.t1);
    }
    usize next_window = 0;
    for (usize i = 0; i < spans.size(); ++i) {
      if (is_test[i]) continue;
      while (next_window < merged.size() && merged[next_window].t1 <= spans[i].t0)
        ++next_window;
      if (next_window == merged.size() || spans[i].t1 <= merged[next_window].t0)
        fold.train_idx.push_back(i);
    }
    out.folds.push_back(std::move(fold));
  } while (detail::next_combination(combo, K));
  return co::Ok(std::move(out));
}
} // namespace atx::engine::eval
