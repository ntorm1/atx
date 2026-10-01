#include "strategy_ic_shrink.hpp"

#include <string>
#include <utility>
#include "atx/engine/combine/group_shrink.hpp"

namespace atx::impl::strategy {
namespace {
namespace co=atx::core;
namespace cb=atx::engine::combine;
constexpr atx::usize max_members=256; // the IC library's candidate bound
constexpr atx::usize max_themes=32;   // the theme_standardise bound (strategy_ic_composition.cpp)
co::Error ruled(const co::Error& error) { return co::Error(error.code(),"ic-shrink-v1: "+error.message()); }
} // namespace

co::Result<IcShrinkFit> ic_shrink_weights(std::span<const atx::f64> ic,std::span<const atx::usize> theme,
                                          atx::usize themes) {
  if (ic.empty() || ic.size()>max_members || themes==0 || themes>max_themes)
    return co::Err(co::ErrorCode::InvalidArgument,"ic-shrink-v1: 1..256 members in 1..32 themes");
  auto shares=cb::group_shrink_shares(ic,theme,themes,ic_shrink_intensity,ic_shrink_floor);
  if (!shares) return co::Err(ruled(shares.error()));
  IcShrinkFit fit;
  fit.cap=1.0/(2.0*static_cast<atx::f64>(themes));
  fit.weights.resize(ic.size());
  for (atx::usize k=0;k<ic.size();++k) fit.weights[k]=shares->share[k]/static_cast<atx::f64>(themes);
  auto passes=cb::cap_across_groups(fit.weights,theme,themes,fit.cap,ic_shrink_cap_tolerance);
  if (!passes) return co::Err(ruled(passes.error()));
  fit.cap_passes=*passes;
  fit.shrunk=std::move(shares->shrunk);
  fit.share=std::move(shares->share);
  fit.equal_theme=std::move(shares->equal);
  return co::Ok(std::move(fit));
}
} // namespace atx::impl::strategy
