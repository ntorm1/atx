// ew-theme-std-v1's fit side in C++ (ew_theme_std.hpp; P9 lane D2).
#include "atx/engine/research/composition/ew_theme_std.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace atx::engine::research::composition {
namespace {
namespace co=atx::core;
namespace cb=atx::engine::combine;
constexpr atx::usize max_members=256; // the IC library's candidate bound
constexpr atx::usize max_themes=32;   // the theme_standardise bound
co::Error refused(const std::string& what) {
  return co::Error(co::ErrorCode::InvalidArgument,std::string(ew_theme_std_rule)+": "+what);
}
} // namespace

co::Result<std::vector<RegradeApplied>> apply_tier_regrades(std::span<const std::string> ids,
                                                            std::span<std::string> tiers) {
  if (ids.size()!=tiers.size()) return co::Err(refused("one source tier per member"));
  std::vector<RegradeApplied> applied;
  for (const auto& row:tier_regrades_v8) {
    RegradeApplied out{std::string(row.id),std::string(row.from),std::string(row.to),{},{},false};
    const auto at=std::find(ids.begin(),ids.end(),row.id);
    if (at==ids.end()) {
      out.status="not-a-member";
      applied.push_back(std::move(out));
      continue;
    }
    auto& tier=tiers[static_cast<atx::usize>(at-ids.begin())];
    if (tier!=row.from && tier!=row.to)
      return co::Err(co::ErrorCode::InvalidArgument,"tier re-grade of "+std::string(row.id)+": source tier "+
                     tier+" is neither the declared "+std::string(row.from)+" nor "+std::string(row.to));
    out.member=true;
    out.source_tier=tier;
    out.status=row.from==row.to?"declared-unchanged":(tier==row.from?"applied":"already-at-target");
    tier=std::string(row.to);
    applied.push_back(std::move(out));
  }
  return co::Ok(std::move(applied));
}

co::Result<ThemeGainFit> theme_gain_weights(std::span<const atx::f64> scores,
                                            std::span<const atx::usize> group,atx::usize groups) {
  if (scores.empty() || scores.size()>max_members || groups==0U || groups>max_themes ||
      group.size()!=scores.size())
    return co::Err(refused("1..256 members in 1..32 themes, one theme per member"));
  std::vector<atx::f64> total(groups,0.0);
  std::vector<atx::usize> count(groups,0U);
  for (atx::usize k=0;k<scores.size();++k) {
    if (group[k]>=groups) return co::Err(refused("a theme index out of range"));
    if (!std::isfinite(scores[k]) || !(scores[k]>0.0))
      return co::Err(refused("member scores must be finite and > 0"));
    total[group[k]]+=scores[k];
    ++count[group[k]];
  }
  if (std::any_of(count.begin(),count.end(),[](atx::usize n) { return n==0U; }))
    return co::Err(refused("a theme without a member"));
  // composition_rules.tier_weights: s / (T * total[t]); theme_gain_weights: cap 1.0 / (2 * T).
  const auto count_t=static_cast<atx::f64>(groups);
  ThemeGainFit fit;
  fit.uncapped.resize(scores.size());
  for (atx::usize k=0;k<scores.size();++k) fit.uncapped[k]=scores[k]/(count_t*total[group[k]]);
  fit.cap=1.0/(2.0*count_t);
  fit.weights=fit.uncapped;
  auto passes=cb::cap_across_groups(fit.weights,group,groups,fit.cap,member_cap_tolerance,&fit.passes);
  if (!passes) return co::Err(refused(passes.error().message()));
  return co::Ok(std::move(fit));
}
} // namespace atx::engine::research::composition
