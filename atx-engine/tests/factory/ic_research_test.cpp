#include <bit>
#include <cmath>
#include <limits>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/factory/ic_research.hpp"
namespace {
using namespace atx;
namespace ex=engine::factory;
engine::alpha::Panel make_panel(usize d,usize n,std::vector<u8> mask={}) {
  std::vector<f64> price(d*n);
  for (usize t=0;t<d;++t) for (usize i=0;i<n;++i)
    price[t*n+i]=100*std::exp(.0001*static_cast<f64>((i+1)*t));
  auto p=engine::alpha::Panel::create(d,n,{"close"},{std::move(price)},std::move(mask));
  EXPECT_TRUE(p); return std::move(*p);
}
TEST(ResearchIc, LegacyFourHorizonSeriesRemainBitIdentical) {
  constexpr usize d=400,n=8;
  auto p=make_panel(d,n); auto cfg=ex::equivalence_ic_screen_config();
  cfg.horizons={1,3,5,7}; cfg.min_names=3; cfg.min_dates=8;
  auto old=ex::prepare_ic_screen(p,cfg); ASSERT_TRUE(old);
  auto os=ex::prepare_ic_screen_scratch(*old); ASSERT_TRUE(os);
  auto fresh=ex::prepare_research_ic(p,cfg,{4,false}); ASSERT_TRUE(fresh);
  auto fs=ex::prepare_research_ic_scratch(*fresh); ASSERT_TRUE(fs);
  std::vector<f64> signal(d*n);
  for (usize k=0;k<signal.size();++k) signal[k]=std::sin(static_cast<f64>(k)*.071);
  auto a=ex::screen_ic(signal,*old,*os); ASSERT_TRUE(a);
  auto b=ex::evaluate_research_ic(signal,*fresh,*fs); ASSERT_TRUE(b);
  EXPECT_EQ(a->reject,b->screen.reject); EXPECT_EQ(a->reason,b->screen.reason);
  for (usize h=0;h<4;++h) {
    const auto x=os->rank_series(h),y=fs->rank_series(h);
    ASSERT_EQ(x.size(),y.size());
    for (usize k=0;k<x.size();++k) EXPECT_EQ(std::bit_cast<u64>(x[k]),std::bit_cast<u64>(y[k]));
    const auto u=os->pearson_series(h),v=fs->pearson_series(h);
    ASSERT_EQ(u.size(),v.size());
    for (usize k=0;k<u.size();++k) EXPECT_EQ(std::bit_cast<u64>(u[k]),std::bit_cast<u64>(v[k]));
    EXPECT_EQ(std::bit_cast<u64>(a->horizons[h].rank.standard_error),
              std::bit_cast<u64>(b->screen.horizons[h].rank.standard_error));
  }
}
TEST(ResearchIc, EndpointPresenceAndCandidatePairsAreCountedWithoutFutureMembership) {
  constexpr usize d=12,n=4;
  std::vector<u8> presence(d*n,1),member(d*n,1);
  presence[n]=0; presence[3*n+1]=0; // finite backing values remain in the Panel
  auto p=make_panel(d,n,presence); auto cfg=ex::equivalence_ic_screen_config();
  cfg.horizons={1,2,3,0}; cfg.min_names=3; cfg.min_dates=8;
  auto c=ex::prepare_research_ic(p,cfg,{3,true},member); ASSERT_TRUE(c);
  auto s=ex::prepare_research_ic_scratch(*c); ASSERT_TRUE(s);
  std::vector<f64> signal(d*n,1); signal[2]=std::numeric_limits<f64>::quiet_NaN();
  auto r=ex::evaluate_research_ic(signal,*c,*s); ASSERT_TRUE(r);
  const auto& counts=r->coverage[0];
  EXPECT_EQ(counts.mature_dates,10U); EXPECT_EQ(counts.structural_tail_dates,2U);
  EXPECT_EQ(counts.decision_eligible_pairs,38U);
  EXPECT_EQ(counts.missing_entry_pairs,2U); EXPECT_EQ(counts.missing_exit_pairs,1U);
  EXPECT_EQ(counts.finite_label_pairs,35U); EXPECT_EQ(counts.paired_signal_pairs,34U);
  EXPECT_EQ(r->screen.horizons[3].horizon,0U); EXPECT_FALSE(r->screen.horizons[3].rank.defined);
  EXPECT_TRUE(s->rank_series(3).empty());
  // Decision window only d0: future member flags cannot remove its labels.
  cfg.window_end=4; cfg.maturity_end=4;
  auto base=ex::prepare_research_ic(p,cfg,{1,true},member); ASSERT_TRUE(base);
  for (usize t=2;t<d;++t) for (usize i=0;i<n;++i) member[t*n+i]=0;
  auto changed=ex::prepare_research_ic(p,cfg,{1,true},member); ASSERT_TRUE(changed);
  auto bs=ex::prepare_research_ic_scratch(*base); ASSERT_TRUE(bs);
  auto cs=ex::prepare_research_ic_scratch(*changed); ASSERT_TRUE(cs);
  auto br=ex::evaluate_research_ic(signal,*base,*bs); ASSERT_TRUE(br);
  auto cr=ex::evaluate_research_ic(signal,*changed,*cs); ASSERT_TRUE(cr);
  EXPECT_EQ(br->coverage[0].finite_label_pairs,cr->coverage[0].finite_label_pairs);
  EXPECT_FALSE(ex::evaluate_research_ic(signal,*changed,*bs));
}
TEST(ResearchIc, ShortLongHorizonIsUnavailableAndNeverRejected) {
  auto p=make_panel(12,8); auto cfg=ex::equivalence_ic_screen_config();
  cfg.horizons={5,21,63,0}; cfg.min_names=3; cfg.min_dates=8;
  auto c=ex::prepare_research_ic(p,cfg,{3,true}); ASSERT_TRUE(c);
  auto s=ex::prepare_research_ic_scratch(*c); ASSERT_TRUE(s);
  const std::vector<f64> signal(12*8,1);
  auto r=ex::evaluate_research_ic(signal,*c,*s); ASSERT_TRUE(r);
  EXPECT_FALSE(r->screen.reject); EXPECT_FALSE(r->screen.enough_evidence);
  EXPECT_EQ(r->screen.reason,ex::IcScreenReason::InsufficientEvidence);
  EXPECT_EQ(r->coverage[2].mature_dates,0U); EXPECT_EQ(r->coverage[2].structural_tail_dates,12U);
  EXPECT_EQ(r->screen.horizons[2].rank.valid_dates,0U);
  EXPECT_FALSE(ex::prepare_research_ic(p,cfg,{0,true}));
  EXPECT_FALSE(ex::prepare_research_ic(p,cfg,{4,true})); // inactive zero is invalid when activated
}
} // namespace
