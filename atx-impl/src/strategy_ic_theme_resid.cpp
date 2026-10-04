// The theme_residualise parse of the IC runner (theme-resid-v1). The rule's per-session kernel
// add_theme_residualised lives in atx-engine-research-composition since P9 lane D2
// (atx/engine/research/composition/theme_resid.hpp; strategy_ic_theme_resid.hpp is its thin
// include).
#include "strategy_ic_theme_resid.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "strategy_ic_rules.hpp"

namespace atx::impl::strategy::ic_detail {
// Optional top-level `theme_residualise` (fitter flag --theme-resid, platform v8 R-11):
// {"rule":"theme-resid-v1","order":[theme, ...]} beside a theme_standardise block with rerank true,
// `order` naming each of that block's weighted themes exactly once (the registered theme order the
// fitter wrote). Every weighted candidate's theme index becomes its theme's position in `order`, so
// the composition residualises the theme at position t on the themes before it. Ruling PM4-11
// (finding R6B-O-4): every weighted theme must be registered and `order` must be the registered
// order restricted to the weighted themes; the order is recorded (pinned.residualise_order: the
// recipe and the combined manifest). The registered order is the theme table's (P9 lane D1,
// strategy_ic_rules.hpp: the alpha registry's themes, or the built-in table, the same thirteen
// themes in the order the rule was registered on). Absent: nothing changes. Runs after
// composition_standardise, before any role payload.
co::Status composition_residualise(const Json& j,const RuleInputs& in,PinnedWeights& pinned) {
  const auto& lib=in.lib;
  if (!j.contains("theme_residualise")) return co::Ok();
  const auto& block=j.at("theme_residualise");
  if (!block.is_object() || !block.contains("rule") || block.at("rule")!=theme_residualise_rule ||
      !block.contains("order") || !block.at("order").is_array())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise must be {rule: theme-resid-v1, "
        "order: [theme, ...]}");
  if (pinned.std_themes.empty())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise needs a theme_standardise block with "
        "rerank true (it residualises the standardised theme composites)");
  // The standardise block's theme names by index (theme_indices: first appearance in library
  // order); composition_standardise has checked every weighted candidate's entry.
  const auto& listed=j.at("theme_standardise").at("themes");
  std::vector<std::string> names(pinned.std_theme_count);
  for (usize k=0;k<lib.candidates.size();++k)
    if (pinned.values[k]>0) names[pinned.std_themes[k]]=listed.at(lib.candidates[k].id).get<std::string>();
  for (const auto& theme:names)
    if (!registered_theme(in.themes,theme))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise: weighted theme "+theme+
          " is outside the registered theme order of theme-resid-v1 (Ruling PM4-11)");
  std::vector<std::string> registered; // the registered order restricted to the weighted themes
  for (const std::string_view theme:in.themes.names) {
    const auto weighted=[theme](const std::string& name) { return std::string_view(name)==theme; };
    if (std::any_of(names.begin(),names.end(),weighted)) registered.emplace_back(theme);
  }
  const auto refuse=[](const std::string& why) {
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise order must name each weighted theme "
        "of theme_standardise exactly once ("+why+")");
  };
  const auto& order=block.at("order");
  if (order.size()!=names.size())
    return refuse(std::to_string(order.size())+" entries for "+std::to_string(names.size())+" themes");
  std::vector<usize> position(names.size(),names.size()); // names.size(): not yet placed
  for (usize p=0;p<order.size();++p) {
    if (!order[p].is_string()) return refuse("an entry is not a string");
    const auto theme=order[p].get<std::string>();
    const auto at=std::find(names.begin(),names.end(),theme);
    if (at==names.end()) return refuse("not a weighted theme: "+theme);
    auto& slot=position[static_cast<usize>(at-names.begin())];
    if (slot!=names.size()) return refuse("repeated: "+theme);
    slot=p;
  }
  // `order` is a permutation of the weighted themes here; it must be the registered one.
  for (usize p=0;p<order.size();++p)
    if (order[p].get<std::string>()!=registered[p]) {
      std::string want;
      for (const auto& theme:registered) want+=(want.empty()?"":", ")+theme;
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise order must be the registered "
          "theme order restricted to the weighted themes (Ruling PM4-11): ["+want+"], not "+order.dump());
    }
  for (usize k=0;k<lib.candidates.size();++k)
    if (pinned.values[k]>0) pinned.std_themes[k]=position[pinned.std_themes[k]];
  pinned.residualise=true;
  pinned.residualise_order=std::move(registered);
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail
