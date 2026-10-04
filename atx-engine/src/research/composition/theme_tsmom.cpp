// theme-tsmom-v1's mass kernel (theme_tsmom.hpp), relocated verbatim from
// atx-impl/src/strategy_ic_theme_tsmom.cpp (P9 lane D2).
#include "atx/engine/research/composition/theme_tsmom.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace atx::engine::research::composition {
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
} // namespace atx::engine::research::composition
