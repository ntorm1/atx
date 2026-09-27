#pragma once

#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/engine/eval/cpcv.hpp"

namespace atx::engine::eval {

// Explicit V2 API. LabelSpan coordinates and embargo_dates are session ordinals,
// not packed YYYYMMDD dates or observation-row counts. Equal t0 rows belong to
// one date and cannot be split across train/test groups. Missing dates do not
// compress the embargo. Existing cpcv_folds retains its V1 recipe.
struct DateCpcvConfig {
  atx::usize n_groups{6};
  atx::usize n_test_groups{2};
  atx::usize embargo_dates{0};
  atx::u64 max_working_bytes{64ULL * 1024ULL * 1024ULL};
};

struct DateCpcvPlan {
  static constexpr atx::u32 recipe_version = 2;
  DateCpcvConfig config;
  std::vector<CpcvFold> folds;
  // K+1 observation offsets: each group includes all rows of its dates.
  std::vector<atx::usize> group_offsets;
  // paths[p][g] is the fold supplying group g in path p. Every path includes
  // every group once, and every (fold, held-out group) occurrence is used once.
  // Path count = C(K-1,k-1) = (k/K)*C(K,k); folds are not independent paths.
  std::vector<std::vector<atx::usize>> paths;
};

// Ascending t0, nonempty half-open labels required. Embargo extends each test
// label's exclusive end by embargo_dates. Returns Err before combinatorial
// allocation on invalid geometry, timestamp overflow or budget excess.
// K is bounded to 20; k must be in [1,K). All indices refer to caller rows.
[[nodiscard]] atx::core::Result<DateCpcvPlan>
cpcv_date_plan(std::span<const LabelSpan> spans, const DateCpcvConfig& config);

// Compact provenance retained by factory caches and fitted learned models.
// V1 leaves paths/offsets empty; V2 exposes actual combinatorial paths, not a
// claim that each fold is an independent backtest.
struct CpcvMetadata {
  CpcvConfig config;
  atx::u64 recipe_identity{0};
  atx::u64 label_identity{0};
  atx::usize fold_count{0};
  std::vector<atx::usize> group_offsets;
  std::vector<std::vector<atx::usize>> paths;
};
struct CpcvPlan {
  std::vector<CpcvFold> folds;
  CpcvMetadata metadata;
};
// Stable recipe identity. Inactive knobs are omitted; callers add geometry or
// label support identity when caching plans across different datasets.
[[nodiscard]] atx::u64 cpcv_recipe_identity(const CpcvConfig& config) noexcept;
[[nodiscard]] atx::core::Result<CpcvPlan>
cpcv_plan(std::span<const LabelSpan> spans, const CpcvConfig& config);
// Arbitrary inner-validation subset with the SAME DateV2 endpoint embargo.
[[nodiscard]] atx::core::Result<std::vector<atx::usize>>
cpcv_date_train(std::span<const LabelSpan> spans,
                std::span<const atx::usize> candidates,
                std::span<const atx::usize> test, atx::usize embargo_dates);

} // namespace atx::engine::eval
