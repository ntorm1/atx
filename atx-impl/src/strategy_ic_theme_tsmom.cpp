#include "strategy_ic_theme_tsmom.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include "strategy_ic_rules.hpp"

namespace atx::impl::strategy {
namespace {
namespace co=atx::core;
co::Error refused(const std::string& what) {
  return co::Error(co::ErrorCode::InvalidArgument,std::string(theme_tsmom_rule)+": "+what);
}
} // namespace

co::Result<atx::usize> theme_tsmom_masses(std::span<const atx::f64> parent,std::span<const atx::f64> trailing,
                                          std::span<atx::f64> out) {
  if (parent.empty() || trailing.size()!=parent.size() || out.size()!=parent.size())
    return co::Err(refused("one parent mass and one trailing sum per theme"));
  atx::usize on=0;
  atx::f64 total=0.0,kept=0.0;
  for (atx::usize t=0;t<parent.size();++t) {
    if (!std::isfinite(parent[t]) || !(parent[t]>0.0) || !std::isfinite(trailing[t]))
      return co::Err(refused("parent masses must be finite and > 0 and trailing sums finite"));
    total+=parent[t];
    if (trailing[t]>0.0) { ++on; kept+=parent[t]; }
  }
  if (on==0U || on==parent.size()) {
    std::copy(parent.begin(),parent.end(),out.begin());
    return co::Ok(atx::usize{0});
  }
  const atx::f64 scale=total/kept;
  for (atx::usize t=0;t<parent.size();++t) out[t]=trailing[t]>0.0?parent[t]*scale:0.0;
  return co::Ok(parent.size()-on);
}
} // namespace atx::impl::strategy

namespace atx::impl::strategy::ic_detail {
// Optional top-level `theme_schedule` (fitter flag --theme-tsmom theme-tsmom-v1, platform v8 Y-2):
// {"rule":"theme-tsmom-v1","lookback":252,"lag":3,"step":21,"themes":[theme, ...],"blocks":[
// {"from_session":<i64 session key>,"trailing":[T per theme]}, ...]} beside a theme_standardise block
// with rerank true and without theme_residualise. `themes` names each weighted theme of that block
// exactly once, in ascending order; the constants are the registered ones; 1..4096 blocks with
// strictly increasing from_session and one finite trailing sum per theme. Each block's masses are
// theme_tsmom_masses of the parent's W_theme (the sum of the theme's pinned weights in library order,
// as IcComposition::create sums them) and its trailing sums, kept by composition theme index for
// score_role, which places each block at the first session >= from_session. Absent: nothing changes.
// Runs after composition_residualise, before any role payload.
co::Status composition_schedule(const Json& j,const RuleInputs& in,PinnedWeights& pinned) {
  const auto& lib=in.lib;
  if (!j.contains("theme_schedule")) return co::Ok();
  const auto& block=j.at("theme_schedule");
  const auto registered=[&block](const char* key,usize value) {
    return block.contains(key) && block.at(key).is_number_unsigned() && block.at(key).get<u64>()==value;
  };
  if (!block.is_object() || !block.contains("rule") || !block.at("rule").is_string() ||
      block.at("rule").get<std::string>()!=theme_tsmom_rule ||
      !registered("lookback",theme_tsmom_lookback) || !registered("lag",theme_tsmom_lag) ||
      !registered("step",theme_tsmom_step) || !block.contains("themes") || !block.at("themes").is_array() ||
      !block.contains("blocks") || !block.at("blocks").is_array() || block.at("blocks").empty() ||
      block.at("blocks").size()>theme_tsmom_max_blocks)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_schedule must be {rule: theme-tsmom-v1, "
        "lookback: 252, lag: 3, step: 21, themes: [theme, ...], blocks: [{from_session, trailing}, ...] (1..4096)}");
  if (pinned.std_themes.empty() || pinned.residualise)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_schedule needs a theme_standardise block with "
        "rerank true and no theme_residualise (it reweights the standardised theme composites)");
  // The standardise block's theme names by composition index (theme_indices: first appearance in
  // library order) and the parent's W_theme, summed as IcComposition::create sums std_mass.
  const auto& listed=j.at("theme_standardise").at("themes");
  std::vector<std::string> names(pinned.std_theme_count);
  std::vector<f64> mass(pinned.std_theme_count,0.0);
  for (usize k=0;k<lib.candidates.size();++k)
    if (pinned.values[k]>0) {
      names[pinned.std_themes[k]]=listed.at(lib.candidates[k].id).get<std::string>();
      mass[pinned.std_themes[k]]+=pinned.values[k];
    }
  auto sorted=names;
  std::sort(sorted.begin(),sorted.end());
  const auto& themes=block.at("themes");
  bool same=themes.size()==sorted.size();
  for (usize p=0;same && p<themes.size();++p) same=themes[p].is_string() && themes[p].get<std::string>()==sorted[p];
  if (!same) {
    std::string want;
    for (const auto& theme:sorted) want+=(want.empty()?"":", ")+theme;
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_schedule themes must be the weighted themes of "
        "theme_standardise in ascending order: ["+want+"], not "+themes.dump());
  }
  std::vector<usize> index(sorted.size());     // block position -> composition theme index
  std::vector<f64> parent(sorted.size()),trailing(sorted.size()),masses(sorted.size());
  for (usize p=0;p<sorted.size();++p) {
    index[p]=static_cast<usize>(std::find(names.begin(),names.end(),sorted[p])-names.begin());
    parent[p]=mass[index[p]];
  }
  std::vector<i64> from; std::vector<std::vector<f64>> scheduled; usize off=0;
  for (const auto& row:block.at("blocks")) {
    if (!row.is_object() || !row.contains("from_session") || !row.at("from_session").is_number_integer() ||
        !row.contains("trailing") || !row.at("trailing").is_array() || row.at("trailing").size()!=sorted.size())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_schedule block must be {from_session: integer, "
          "trailing: [one number per theme]}");
    const i64 session=row.at("from_session").get<i64>();
    if (!from.empty() && !(session>from.back()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_schedule from_session must increase strictly");
    for (usize p=0;p<sorted.size();++p) {
      const auto& value=row.at("trailing")[p];
      trailing[p]=value.is_number()?value.get<f64>():quiet_nan;
    }
    auto switched=theme_tsmom_masses(parent,trailing,masses);
    if (!switched) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_schedule: "+
                                  switched.error().message());
    off+=*switched;
    std::vector<f64> by_index(sorted.size(),0.0);
    for (usize p=0;p<sorted.size();++p) by_index[index[p]]=masses[p];
    from.push_back(session);
    scheduled.push_back(std::move(by_index));
  }
  pinned.schedule=std::string(theme_tsmom_rule);
  pinned.schedule_from=std::move(from);
  pinned.schedule_mass=std::move(scheduled);
  pinned.schedule_off=off;
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail
