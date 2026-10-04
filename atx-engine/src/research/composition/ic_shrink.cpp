// ic-shrink-v1 / ic-shrink-aim-v1 (ic_shrink.hpp), relocated verbatim from
// atx-impl/src/strategy_ic_shrink.cpp (P9 lane D2); `trace` added.
#include "atx/engine/research/composition/ic_shrink.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace atx::engine::research::composition {
namespace {
namespace co=atx::core;
namespace cb=atx::engine::combine;
constexpr atx::usize max_members=256; // the IC library's candidate bound
constexpr atx::usize max_themes=32;   // the theme_standardise bound (strategy_ic_composition.cpp)
co::Error ruled(std::string_view rule,const co::Error& error) {
  return co::Error(error.code(),std::string(rule)+": "+error.message());
}
} // namespace

co::Result<IcShrinkFit> ic_shrink_weights(std::span<const atx::f64> ic,std::span<const atx::usize> theme,
                                          atx::usize themes,std::span<const atx::f64> gains,
                                          bool trace) {
  const bool aim=!gains.empty();
  const std::string rule(aim?ic_shrink_aim_rule:ic_shrink_rule);
  if (ic.empty() || ic.size()>max_members || themes==0 || themes>max_themes)
    return co::Err(co::ErrorCode::InvalidArgument,rule+": 1..256 members in 1..32 themes");
  if (aim && gains.size()!=ic.size())
    return co::Err(co::ErrorCode::InvalidArgument,rule+": one aim gain per member");
  for (const atx::f64 g:gains)
    if (!std::isfinite(g) || !(g>0))
      return co::Err(co::ErrorCode::InvalidArgument,rule+": aim gains must be finite and > 0");
  auto shares=cb::group_shrink_shares(ic,theme,themes,ic_shrink_intensity,ic_shrink_floor);
  if (!shares) return co::Err(ruled(rule,shares.error()));
  IcShrinkFit fit;
  const auto count=static_cast<atx::f64>(themes);
  fit.cap=1.0/(2.0*count);
  fit.weights.resize(ic.size());
  if (!aim) {
    for (atx::usize k=0;k<ic.size();++k) fit.weights[k]=shares->share[k]/count;
  } else {
    // E-27a (composition_rules.tier_weights, same expressions in the same order): share x gain,
    // renormalised inside the theme so each theme keeps 1 / T.
    std::vector<atx::f64> total(themes,0.0);
    for (atx::usize k=0;k<ic.size();++k) {
      fit.weights[k]=shares->share[k]*gains[k];
      total[theme[k]]+=fit.weights[k];
    }
    for (const atx::f64 t:total)
      if (!std::isfinite(t) || !(t>0))
        return co::Err(co::ErrorCode::InvalidArgument,rule+": a theme's gain-weighted shares do not sum above 0");
    for (atx::usize k=0;k<ic.size();++k) fit.weights[k]=fit.weights[k]/(count*total[theme[k]]);
  }
  if (trace) fit.uncapped=fit.weights;
  auto passes=cb::cap_across_groups(fit.weights,theme,themes,fit.cap,ic_shrink_cap_tolerance,
                                    trace?&fit.passes:nullptr);
  if (!passes) return co::Err(ruled(rule,passes.error()));
  fit.cap_passes=*passes;
  fit.shrunk=std::move(shares->shrunk);
  fit.share=std::move(shares->share);
  fit.equal_theme=std::move(shares->equal);
  return co::Ok(std::move(fit));
}
} // namespace atx::engine::research::composition
