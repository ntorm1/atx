#include "strategy_ic_theme_erc.hpp"

#include <cmath>
#include <string>
#include <utility>
#include "atx/engine/combine/group_erc.hpp"
#include "atx/engine/combine/group_shrink.hpp"

namespace atx::impl::strategy {
namespace {
namespace co=atx::core;
namespace cb=atx::engine::combine;
constexpr atx::usize max_members=256; // the IC library's candidate bound
constexpr atx::usize max_themes=32;   // the theme_standardise bound (strategy_ic_composition.cpp)
co::Error ruled(const co::Error& error) {
  return co::Error(error.code(),std::string(theme_erc_rule)+": "+error.message());
}
co::Error refused(const char* what) {
  return co::Error(co::ErrorCode::InvalidArgument,std::string(theme_erc_rule)+": "+what);
}
} // namespace

co::Result<ThemeErcFit> theme_erc_weights(std::span<const atx::f64> share,std::span<const atx::usize> theme,
                                          atx::usize themes,std::span<const atx::f64> covariance) {
  if (share.empty() || share.size()>max_members || themes==0 || themes>max_themes || theme.size()!=share.size())
    return co::Err(refused("1..256 members in 1..32 themes, one theme per member"));
  std::vector<atx::f64> total(themes,0.0);
  std::vector<atx::usize> count(themes,0U);
  for (atx::usize k=0;k<share.size();++k) {
    if (theme[k]>=themes) return co::Err(refused("a theme index out of range"));
    if (!std::isfinite(share[k]) || !(share[k]>=0.0))
      return co::Err(refused("within-theme shares must be finite and >= 0"));
    total[theme[k]]+=share[k];
    ++count[theme[k]];
  }
  for (atx::usize t=0;t<themes;++t) {
    if (count[t]==0U) return co::Err(refused("a theme without a member"));
    if (!(std::abs(total[t]-1.0)<=theme_erc_share_tolerance))
      return co::Err(refused("a theme's within-theme shares do not sum to 1"));
  }
  auto erc=cb::group_erc_shares(covariance,themes,theme_erc_sweeps);
  if (!erc) return co::Err(ruled(erc.error()));
  if (!(erc->dispersion<=theme_erc_dispersion))
    return co::Err(refused("the risk contributions did not equalise within the registered sweeps"));
  ThemeErcFit fit;
  const auto count_t=static_cast<atx::f64>(themes);
  fit.cap=1.0/(2.0*count_t);
  fit.weights.resize(share.size());
  for (atx::usize k=0;k<share.size();++k) fit.weights[k]=share[k]*erc->share[theme[k]];
  auto passes=cb::cap_across_groups(fit.weights,theme,themes,fit.cap,theme_erc_cap_tolerance);
  if (!passes) return co::Err(ruled(passes.error()));
  fit.cap_passes=*passes;
  fit.dispersion=erc->dispersion;
  fit.theme_share=std::move(erc->share);
  fit.contribution=std::move(erc->contribution);
  return co::Ok(std::move(fit));
}
} // namespace atx::impl::strategy
