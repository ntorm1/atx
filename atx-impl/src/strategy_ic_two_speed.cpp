#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "strategy_ic_detail.hpp"
#include "strategy_two_speed.hpp"

namespace atx::impl::strategy::ic_detail {
// Optional top-level `theme_sleeves` (fitter flag --two-speed two-speed-v1, platform v8 Y-5; Ruling
// PM8-12: the fitter only writes the spec): exactly {"rule":"two-speed-v1"}, beside a theme_standardise
// block with rerank true and without theme_residualise. The rule itself is here: every weighted theme
// of that block takes its registered half-life (two_speed_half_lives; a weighted theme outside the
// table is refused), a theme of at most engine::book::two_speed_fast_bound sessions is fast, and at
// least one weighted theme must be fast and one slow. Fills pinned.sleeves (the rule),
// pinned.sleeve_fast (one flag per composition theme index) and pinned.sleeve_fast_themes (ascending).
// Absent: nothing changes. Runs after composition_schedule, before any role payload.
co::Status composition_sleeves(const Json& j,const Library& lib,PinnedWeights& pinned) {
  if (!j.contains("theme_sleeves")) return co::Ok();
  const auto& block=j.at("theme_sleeves");
  if (!block.is_object() || block.size()!=1 || !block.contains("rule") || !block.at("rule").is_string() ||
      block.at("rule").get<std::string>()!=two_speed_rule)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_sleeves must be {rule: two-speed-v1}");
  if (pinned.std_themes.empty() || pinned.residualise)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_sleeves needs a theme_standardise block with "
        "rerank true and no theme_residualise (it splits the standardised theme composites)");
  // The standardise block's theme names by composition index (theme_indices: first appearance in
  // library order); composition_standardise has checked every weighted candidate's entry.
  const auto& listed=j.at("theme_standardise").at("themes");
  std::vector<std::string> names(pinned.std_theme_count);
  for (usize k=0;k<lib.candidates.size();++k)
    if (pinned.values[k]>0) names[pinned.std_themes[k]]=listed.at(lib.candidates[k].id).get<std::string>();
  std::vector<u8> fast(names.size(),0);
  std::vector<std::string> fast_names;
  for (usize t=0;t<names.size();++t) {
    f64 half_life=0;
    if (!two_speed_half_life(names[t],half_life))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_sleeves: weighted theme "+names[t]+
          " has no registered half-life (two-speed-v1)");
    if (two_speed_fast(half_life)) { fast[t]=1; fast_names.push_back(names[t]); }
  }
  if (fast_names.empty() || fast_names.size()==names.size())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_sleeves needs at least one fast and one slow "
        "weighted theme");
  std::sort(fast_names.begin(),fast_names.end());
  pinned.sleeves=std::string(two_speed_rule);
  pinned.sleeve_fast=std::move(fast);
  pinned.sleeve_fast_themes=std::move(fast_names);
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail
