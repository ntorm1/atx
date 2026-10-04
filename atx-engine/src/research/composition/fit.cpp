// The fit side of the v8 composition rules on a K-P9-4 factor series (fit.hpp; P9 lane D2).
#include "atx/engine/research/composition/fit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "atx/engine/research/admission/screen.hpp"
#include "atx/engine/research/composition/theme_tsmom.hpp"

namespace atx::engine::research::composition {
namespace {
namespace co=atx::core;
using atx::f64;
using atx::i32;
using atx::i64;
using atx::u8;
using atx::usize;
constexpr usize pairwise_block=128U;    // numpy's PW_BLOCKSIZE
constexpr usize max_members=256U;       // the IC library's candidate bound
constexpr usize max_themes=32U;         // the theme_standardise bound
constexpr f64 sharpe_annualization=252.0;

co::Error refused(const std::string& what) {
  return co::Error(co::ErrorCode::InvalidArgument,"composition fit: "+what);
}

// numpy pairwise_sum_DOUBLE (loops_utils.h.src), term for term, on a contiguous vector.
f64 pairwise(const f64* a,usize n) noexcept {
  if (n<8U) {
    f64 res=0.0;
    for (usize i=0;i<n;++i) res+=a[i];
    return res;
  }
  if (n<=pairwise_block) {
    std::array<f64,8> r{a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]};
    usize i=8U;
    for (;i<n-(n%8U);i+=8U)
      for (usize j=0;j<8U;++j) r[j]+=a[i+j];
    f64 res=((r[0]+r[1])+(r[2]+r[3]))+((r[4]+r[5])+(r[6]+r[7]));
    for (;i<n;++i) res+=a[i];
    return res;
  }
  usize n2=n/2U;
  n2-=n2%8U;
  return pairwise(a,n2)+pairwise(a+n2,n-n2);
}

[[nodiscard]] bool shapes_ok(std::span<const std::vector<f64>> series,usize members,
                             usize decisions) {
  return series.size()==members &&
         std::all_of(series.begin(),series.end(),
                     [&](const std::vector<f64>& s) { return s.size()==decisions; });
}
} // namespace

f64 numpy_pairwise_sum(std::span<const f64> x) noexcept { return 0.0+pairwise(x.data(),x.size()); }

std::vector<f64> signed_series(const FactorSeriesView& view,usize column,i32 sign) {
  std::vector<f64> out(view.decisions,0.0);
  const auto s=static_cast<f64>(sign);
  for (usize d=0;d<view.decisions;++d) {
    const f64 f=view.factors[d*view.candidates+column];
    out[d]=s*(std::isnan(f)?0.0:f);
  }
  return out;
}

std::optional<f64> train_mean(const FactorSeriesView& view,usize column) {
  std::vector<f64> live;
  for (usize d=0;d<view.decisions;++d) {
    const f64 f=view.factors[d*view.candidates+column];
    if (view.train[d]!=0U && std::isfinite(f)) live.push_back(f);
  }
  return admission::mean_sharpe(live,sharpe_annualization).mean;
}

co::Result<std::vector<f64>> within_theme_shares(std::span<const f64> uncapped,
                                                 std::span<const usize> theme,usize themes) {
  if (uncapped.size()!=theme.size() || uncapped.empty() || themes==0U)
    return co::Err(refused("theme-erc-v1: one parent weight and one theme per member"));
  std::vector<f64> total(themes,0.0);
  for (usize k=0;k<uncapped.size();++k) {
    if (theme[k]>=themes) return co::Err(refused("theme-erc-v1: a theme index out of range"));
    total[theme[k]]+=uncapped[k];
  }
  for (const f64 t:total)
    if (!(t>0.0)) return co::Err(refused("theme-erc-v1: a theme of the parent with no weight"));
  std::vector<f64> out(uncapped.size());
  for (usize k=0;k<uncapped.size();++k) out[k]=uncapped[k]/total[theme[k]];
  return co::Ok(std::move(out));
}

co::Result<SleeveCovariance> theme_sleeve_covariance(std::span<const std::vector<f64>> series,
                                                     std::span<const u8> train,
                                                     std::span<const f64> share,
                                                     std::span<const usize> theme,usize themes) {
  const usize members=share.size();
  if (members==0U || theme.size()!=members || themes==0U ||
      !shapes_ok(series,members,train.size()))
    return co::Err(refused("theme-erc-v1: one series, share and theme per member"));
  std::vector<usize> keep;
  for (usize d=0;d<train.size();++d)
    if (train[d]!=0U) keep.push_back(d);
  const usize n=keep.size();
  if (n<2U) return co::Err(refused("theme-erc-v1: the theme sleeves need at least two decisions"));
  // composition_theme_erc.sleeve_series (r = r + a_k * x_k, members in input order), then
  // covariance's centring by the row mean.
  std::vector<std::vector<f64>> centred(themes,std::vector<f64>(n,0.0));
  for (usize t=0;t<themes;++t) {
    auto& r=centred[t];
    for (usize k=0;k<members;++k) {
      if (theme[k]>=themes) return co::Err(refused("theme-erc-v1: a theme index out of range"));
      if (theme[k]!=t) continue;
      for (usize i=0;i<n;++i) r[i]=r[i]+share[k]*series[k][keep[i]];
    }
    const f64 mean=numpy_pairwise_sum(r)/static_cast<f64>(n);
    for (auto& v:r) v=v-mean;
  }
  SleeveCovariance out;
  out.decisions=n;
  out.covariance.assign(themes*themes,0.0);
  const auto divisor=static_cast<f64>(n-1U);
  for (usize i=0;i<themes;++i)
    for (usize j=i;j<themes;++j) {
      f64 dot=0.0; // pinned: sequential in decision order (numpy uses BLAS ddot)
      for (usize d=0;d<n;++d) dot+=centred[i][d]*centred[j][d];
      const f64 value=dot/divisor;
      out.covariance[i*themes+j]=value;
      out.covariance[j*themes+i]=value;
    }
  return co::Ok(std::move(out));
}

co::Result<TsmomSchedule> theme_tsmom_schedule(std::span<const std::vector<f64>> series,
                                               std::span<const i64> sessions,
                                               std::span<const u8> train,
                                               std::span<const f64> weights,
                                               std::span<const usize> theme,usize themes) {
  const usize members=weights.size();
  const usize decisions=sessions.size();
  if (members==0U || theme.size()!=members || train.size()!=decisions || themes==0U ||
      !shapes_ok(series,members,decisions))
    return co::Err(refused("theme-tsmom-v1: one factor series and one weight per member"));
  for (usize k=0;k<members;++k)
    if (!std::isfinite(weights[k]) || !(weights[k]>=0.0) || theme[k]>=themes)
      return co::Err(refused("theme-tsmom-v1: weights must be finite and >= 0"));
  for (usize d=1;d<decisions;++d)
    if (!(sessions[d]>sessions[d-1U]))
      return co::Err(refused("theme-tsmom-v1: decision sessions must increase"));
  TsmomSchedule out;
  std::vector<u8> weighted(themes,0U);
  for (usize k=0;k<members;++k)
    if (weights[k]>0.0) weighted[theme[k]]=1U;
  for (usize t=0;t<themes;++t)
    if (weighted[t]!=0U) out.themes.push_back(t);
  if (out.themes.empty() || out.themes.size()>max_themes)
    return co::Err(refused("theme-tsmom-v1: 1..32 weighted themes"));
  const usize first=theme_tsmom_lookback+theme_tsmom_lag-1U;
  std::vector<usize> starts;
  for (usize j=first;j<decisions;j+=theme_tsmom_step) starts.push_back(j);
  if (starts.empty() || starts.size()>theme_tsmom_max_blocks)
    return co::Err(refused("theme-tsmom-v1: the role has "+std::to_string(decisions)+
                           " decisions; the first block needs more than "+std::to_string(first)));
  const usize count=out.themes.size();
  // composition_theme_tsmom.theme_masses and sleeves: W_t in member order; r = r + (w_k / W_t) x_k
  // over every decision, then 0 outside TRAIN.
  std::vector<std::vector<f64>> sleeve(count,std::vector<f64>(decisions,0.0));
  for (usize p=0;p<count;++p) {
    f64 total=0.0;
    for (usize k=0;k<members;++k)
      if (theme[k]==out.themes[p] && weights[k]>0.0) total+=weights[k];
    out.parent_mass.push_back(total);
    auto& r=sleeve[p];
    for (usize k=0;k<members;++k) {
      if (theme[k]!=out.themes[p] || !(weights[k]>0.0)) continue;
      const f64 c=weights[k]/total;
      for (usize d=0;d<decisions;++d) r[d]=r[d]+c*series[k][d];
    }
    for (usize d=0;d<decisions;++d)
      if (train[d]==0U) r[d]=0.0;
  }
  out.on_count.assign(count,0U);
  for (const usize j:starts) {
    TsmomBlock block;
    block.decision_index=j;
    block.from_session=sessions[j];
    block.trailing.assign(count,0.0);
    block.masses.assign(count,0.0);
    const usize from=j-theme_tsmom_lag-theme_tsmom_lookback+1U,to=j-theme_tsmom_lag;
    for (usize p=0;p<count;++p) {
      f64 s=0.0;
      for (usize d=from;d<=to;++d) s+=sleeve[p][d];
      block.trailing[p]=s;
      if (s>0.0) ++out.on_count[p];
    }
    ATX_TRY(block.off,theme_tsmom_masses(out.parent_mass,block.trailing,block.masses));
    out.off+=block.off;
    out.blocks.push_back(std::move(block));
  }
  return co::Ok(std::move(out));
}

co::Result<std::vector<std::string>> theme_resid_order(std::span<const std::string> registered,
                                                       std::span<const std::string> weighted) {
  std::set<std::string> seen;
  for (const auto& theme:registered)
    if (theme.empty() || !seen.insert(theme).second)
      return co::Err(
          refused("theme-resid-v1: the registered theme list must name each theme once"));
  if (registered.size()<theme_resid_frozen_prefix.size() ||
      !std::equal(theme_resid_frozen_prefix.begin(),theme_resid_frozen_prefix.end(),
                  registered.begin()))
    return co::Err(refused("theme-resid-v1: the registered theme order must begin with the "
                           "frozen ten (Ruling PM4-11)"));
  const std::set<std::string> present(weighted.begin(),weighted.end());
  for (const auto& theme:present)
    if (!seen.contains(theme))
      return co::Err(refused("theme-resid-v1: a theme outside the registered order: "+theme));
  if (present.empty()) return co::Err(refused("theme-resid-v1: no weighted theme"));
  std::vector<std::string> out;
  for (const auto& theme:registered)
    if (present.contains(theme)) out.push_back(theme);
  return co::Ok(std::move(out));
}

co::Result<CompositionFit> fit_composition(const FactorSeriesView& view,
                                           std::span<const std::string> ids,const FitSpec& spec) {
  const bool std_rule=
      spec.composition==ew_theme_std_rule || spec.composition==ew_theme_std_aim_rule;
  const bool shrink_rule=spec.composition==ic_shrink_rule || spec.composition==ic_shrink_aim_rule;
  const bool aim=spec.composition==ew_theme_std_aim_rule || spec.composition==ic_shrink_aim_rule;
  if (!std_rule && !shrink_rule)
    return co::Err(refused("composition must be ew-theme-std-v1, ew-theme-std-aim-v1, "
                           "ic-shrink-v1 or ic-shrink-aim-v1: "+spec.composition));
  if (spec.members.empty() || spec.members.size()>max_members)
    return co::Err(refused("1..256 members"));
  if (spec.theme_resid && (spec.theme_erc || spec.theme_tsmom))
    return co::Err(refused("theme-resid-v1 is refused beside theme-erc-v1 and theme-tsmom-v1"));
  if (view.factors.size()!=view.candidates*view.decisions || view.sessions.size()!=view.decisions ||
      view.train.size()!=view.decisions || ids.size()!=view.candidates)
    return co::Err(refused("factor series geometry"));
  CompositionFit fit;
  std::set<std::string> theme_set;
  for (const auto& m:spec.members) theme_set.insert(m.theme);
  fit.themes.assign(theme_set.begin(),theme_set.end());
  if (fit.themes.size()>max_themes) return co::Err(refused("at most 32 themes"));
  std::set<std::string> seen;
  std::vector<std::string> member_ids;
  std::vector<f64> gains;
  for (const auto& m:spec.members) {
    if (!seen.insert(m.id).second) return co::Err(refused("member listed twice: "+m.id));
    if (m.sign!=1) return co::Err(refused("the prior sign of "+m.id+" must be +1"));
    if (aim!=m.gain.has_value())
      return co::Err(refused("an aim gain exactly for the aim rules' members: "+m.id));
    const auto at=std::find(ids.begin(),ids.end(),m.id);
    if (at==ids.end()) return co::Err(refused("member not in the factor series: "+m.id));
    fit.column.push_back(static_cast<usize>(at-ids.begin()));
    fit.member_theme.push_back(static_cast<usize>(
        std::find(fit.themes.begin(),fit.themes.end(),m.theme)-fit.themes.begin()));
    member_ids.push_back(m.id);
    if (aim) gains.push_back(*m.gain);
  }
  for (usize d=0;d<view.decisions;++d)
    if (view.train[d]!=0U) ++fit.train_decisions;
  const usize themes=fit.themes.size();
  if (std_rule) {
    for (const auto& m:spec.members) fit.tiers.push_back(m.tier);
    ATX_TRY(fit.regrades,apply_tier_regrades(member_ids,fit.tiers));
    std::vector<f64> scored;
    for (usize k=0;k<fit.tiers.size();++k) {
      const auto row=std::find_if(spec.tier_scores.begin(),spec.tier_scores.end(),
                                  [&](const auto& entry) { return entry.first==fit.tiers[k]; });
      if (row==spec.tier_scores.end())
        return co::Err(refused("ew-theme-std-v1: tier without a declared score: "+member_ids[k]+":"+
                               fit.tiers[k]));
      fit.scores.push_back(row->second);
      scored.push_back(aim?row->second*gains[k]:row->second);
    }
    ATX_TRY(auto parent,theme_gain_weights(scored,fit.member_theme,themes));
    fit.parent_weights=std::move(parent.weights);
    fit.parent_uncapped=std::move(parent.uncapped);
    fit.parent_cap=parent.cap;
    fit.parent_passes=std::move(parent.passes);
  } else {
    std::vector<f64> total(themes,0.0);
    std::vector<usize> count(themes,0U);
    for (usize k=0;k<fit.column.size();++k) {
      const auto ic=train_mean(view,fit.column[k]);
      if (!ic)
        return co::Err(
            refused("ic-shrink-v1: member "+member_ids[k]+" has no live TRAIN decision"));
      fit.ic.push_back(*ic);
      total[fit.member_theme[k]]+=*ic;
      ++count[fit.member_theme[k]];
    }
    for (usize t=0;t<themes;++t) fit.theme_mean.push_back(total[t]/static_cast<f64>(count[t]));
    ATX_TRY(auto parent,ic_shrink_weights(fit.ic,fit.member_theme,themes,gains,true));
    fit.shrunk=std::move(parent.shrunk);
    fit.share=std::move(parent.share);
    fit.equal_theme=std::move(parent.equal_theme);
    fit.parent_weights=std::move(parent.weights);
    fit.parent_uncapped=std::move(parent.uncapped);
    fit.parent_cap=parent.cap;
    fit.parent_passes=std::move(parent.passes);
  }
  fit.weights=fit.parent_weights;
  std::vector<std::vector<f64>> series;
  for (usize k=0;k<fit.column.size();++k)
    series.push_back(signed_series(view,fit.column[k],spec.members[k].sign));
  if (spec.theme_erc) {
    ATX_TRY(fit.erc_share,within_theme_shares(fit.parent_uncapped,fit.member_theme,themes));
    ATX_TRY(auto sleeves,theme_sleeve_covariance(series,view.train,fit.erc_share,
                                                 fit.member_theme,themes));
    fit.covariance=std::move(sleeves.covariance);
    ATX_TRY(fit.erc_fit,
            theme_erc_weights(fit.erc_share,fit.member_theme,themes,fit.covariance,true));
    fit.weights=fit.erc_fit.weights;
    fit.erc=true;
  }
  if (spec.theme_tsmom) {
    ATX_TRY(fit.schedule,theme_tsmom_schedule(series,view.sessions,view.train,fit.weights,
                                              fit.member_theme,themes));
    fit.tsmom=true;
  }
  if (spec.theme_resid) {
    std::vector<std::string> weighted;
    for (usize k=0;k<fit.weights.size();++k)
      if (fit.weights[k]>0.0) weighted.push_back(fit.themes[fit.member_theme[k]]);
    ATX_TRY(fit.resid_order,theme_resid_order(spec.registered_themes,weighted));
    fit.resid=true;
  }
  return co::Ok(std::move(fit));
}
} // namespace atx::engine::research::composition
