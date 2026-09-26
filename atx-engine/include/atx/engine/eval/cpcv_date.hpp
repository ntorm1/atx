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

} // namespace atx::engine::eval
