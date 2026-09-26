#include "atx/engine/eval/cpcv_date.hpp"

#include <algorithm>
#include <bit>
#include <initializer_list>
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
atx::u64 cpcv_recipe_identity(const CpcvConfig& cfg) noexcept {
  atx::u64 h = 14695981039346656037ULL;
  const auto add = [&](atx::u64 value) {
    for (unsigned i = 0; i < 8U; ++i) {
      h = (h ^ (value & 255U)) * 1099511628211ULL;
      value >>= 8U;
    }
  };
  add(static_cast<atx::u64>(cfg.rule)); add(cfg.n_groups); add(cfg.n_test_groups);
  if (cfg.rule == CpcvRule::DateV2) {
    add(cfg.embargo_dates); add(cfg.max_working_bytes);
  } else { add(std::bit_cast<atx::u64>(cfg.embargo)); }
  return h;
}

co::Result<CpcvPlan> cpcv_plan(std::span<const LabelSpan> spans, const CpcvConfig& cfg) {
  CpcvPlan out;
  out.metadata.config = cfg;
  out.metadata.recipe_identity = cpcv_recipe_identity(cfg);
  if (cfg.rule == CpcvRule::ObservationV1) {
    out.folds = cpcv_folds(spans, cfg); // exact historical order/arithmetic
  } else if (cfg.rule == CpcvRule::DateV2) {
    ATX_TRY(auto plan, cpcv_date_plan(spans, DateCpcvConfig{
        cfg.n_groups, cfg.n_test_groups, cfg.embargo_dates, cfg.max_working_bytes}));
    out.metadata.label_identity = 14695981039346656037ULL;
    for (const auto span : spans) {
      for (atx::u64 value : {static_cast<atx::u64>(span.t0), static_cast<atx::u64>(span.t1)})
        for (unsigned b = 0; b < 8U; ++b) {
          out.metadata.label_identity = (out.metadata.label_identity ^ (value & 255U)) * 1099511628211ULL;
          value >>= 8U;
        }
    }
    out.folds = std::move(plan.folds);
    out.metadata.group_offsets = std::move(plan.group_offsets);
    out.metadata.paths = std::move(plan.paths);
  } else {
    return co::Err(co::ErrorCode::InvalidArgument, "CPCV: unknown rule");
  }
  out.metadata.fold_count = out.folds.size();
  return co::Ok(std::move(out));
}

co::Result<std::vector<usize>> cpcv_date_train(std::span<const LabelSpan> spans,
    std::span<const usize> candidates, std::span<const usize> test, usize embargo) {
  std::vector<LabelSpan> windows;
  windows.reserve(test.size());
  for (const auto i : test) {
    if (i >= spans.size() || spans[i].t0 >= spans[i].t1 ||
        embargo > std::numeric_limits<usize>::max() - spans[i].t1)
      return co::Err(co::ErrorCode::InvalidArgument, "date CPCV inner: invalid test endpoint");
    windows.push_back({spans[i].t0, spans[i].t1 + embargo});
  }
  std::sort(windows.begin(), windows.end(), [](const auto& a, const auto& b) { return a.t0 < b.t0; });
  usize merged = 0;
  for (const auto window : windows) {
    if (merged == 0U || window.t0 > windows[merged - 1U].t1) windows[merged++] = window;
    else windows[merged - 1U].t1 = std::max(windows[merged - 1U].t1, window.t1);
  }
  windows.resize(merged);
  std::vector<usize> out;
  out.reserve(candidates.size());
  for (const auto i : candidates) {
    if (i >= spans.size() || spans[i].t0 >= spans[i].t1)
      return co::Err(co::ErrorCode::InvalidArgument, "date CPCV inner: invalid candidate endpoint");
    const auto next = std::lower_bound(windows.begin(), windows.end(), spans[i].t0,
        [](const auto& window, usize t0) { return window.t1 <= t0; });
    if (next == windows.end() || spans[i].t1 <= next->t0) out.push_back(i);
  }
  return co::Ok(std::move(out));
}
} // namespace atx::engine::eval
