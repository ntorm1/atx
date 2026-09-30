#include <bit>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/factory/ic_research.hpp"
#include "atx/engine/parallel/det_pool.hpp"
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

void same_estimate(const ex::IcScreenEstimate& a,const ex::IcScreenEstimate& b) {
  EXPECT_EQ(std::bit_cast<u64>(a.mean),std::bit_cast<u64>(b.mean));
  EXPECT_EQ(std::bit_cast<u64>(a.standard_error),std::bit_cast<u64>(b.standard_error));
  EXPECT_EQ(std::bit_cast<u64>(a.upper_abs_ic),std::bit_cast<u64>(b.upper_abs_ic));
  EXPECT_EQ(std::bit_cast<u64>(a.max_segment_abs_ic),std::bit_cast<u64>(b.max_segment_abs_ic));
  EXPECT_EQ(a.valid_dates,b.valid_dates); EXPECT_EQ(a.calendar_dates,b.calendar_dates);
  EXPECT_EQ(a.hac_lag,b.hac_lag); EXPECT_EQ(a.defined,b.defined);
  EXPECT_EQ(a.suggestive_direction,b.suggestive_direction);
}
void same_coverage(const ex::ResearchIcCoverage& a,const ex::ResearchIcCoverage& b) {
  EXPECT_EQ(a.mature_dates,b.mature_dates); EXPECT_EQ(a.structural_tail_dates,b.structural_tail_dates);
  EXPECT_EQ(a.decision_eligible_pairs,b.decision_eligible_pairs);
  EXPECT_EQ(a.finite_label_pairs,b.finite_label_pairs); EXPECT_EQ(a.paired_signal_pairs,b.paired_signal_pairs);
  EXPECT_EQ(a.missing_entry_pairs,b.missing_entry_pairs); EXPECT_EQ(a.missing_exit_pairs,b.missing_exit_pairs);
  EXPECT_EQ(a.guard_excluded_pairs,b.guard_excluded_pairs); EXPECT_EQ(a.invalid_price_pairs,b.invalid_price_pairs);
  EXPECT_EQ(a.nonfinite_return_pairs,b.nonfinite_return_pairs);
}
void same_result(const ex::ResearchIcResult& a,const ex::ResearchIcResult& b,
                 const ex::ResearchIcScratch& x,const ex::ResearchIcScratch& y) {
  EXPECT_EQ(a.active_horizons,b.active_horizons); EXPECT_EQ(a.screen.reject,b.screen.reject);
  EXPECT_EQ(a.screen.enough_evidence,b.screen.enough_evidence); EXPECT_EQ(a.screen.reason,b.screen.reason);
  for (usize h=0;h<4;++h) {
    SCOPED_TRACE(h); same_coverage(a.coverage[h],b.coverage[h]);
    EXPECT_EQ(a.screen.horizons[h].horizon,b.screen.horizons[h].horizon);
    EXPECT_EQ(a.screen.horizons[h].enough_evidence,b.screen.horizons[h].enough_evidence);
    same_estimate(a.screen.horizons[h].pearson,b.screen.horizons[h].pearson);
    same_estimate(a.screen.horizons[h].rank,b.screen.horizons[h].rank);
    const auto xp=x.pearson_series(h),yp=y.pearson_series(h),xr=x.rank_series(h),yr=y.rank_series(h);
    ASSERT_EQ(xp.size(),yp.size()); ASSERT_EQ(xr.size(),yr.size());
    usize mismatches=0;
    for (usize t=0;t<xp.size();++t) mismatches+=std::bit_cast<u64>(xp[t])!=std::bit_cast<u64>(yp[t]);
    for (usize t=0;t<xr.size();++t) mismatches+=std::bit_cast<u64>(xr[t])!=std::bit_cast<u64>(yr[t]);
    EXPECT_EQ(mismatches,0U);
  }
}

TEST(ResearchIc, DateParallelTwoAndFourWorkersPreserveEverySeriesAndHacBit) {
  constexpr usize d=413,n=96;
  std::vector<f64> price(d*n),signal(d*n);
  std::vector<u8> presence(d*n,1),member(d*n,1);
  std::vector<u32> guard(d*n,0);
  for (usize t=0;t<d;++t) for (usize i=0;i<n;++i) {
    const auto at=t*n+i; const auto a=static_cast<f64>(t),b=static_cast<f64>(i);
    price[at]=100*std::exp(.00003*(b+1)*a+.015*std::sin(.037*a+.17*b));
    signal[at]=static_cast<f64>((i+3*t)%17)-8; // many exact ties
    if (at%173==0 || t==83 || t==147) signal[at]=std::numeric_limits<f64>::quiet_NaN();
    if (t==53 && i==3) signal[at]=std::numeric_limits<f64>::infinity();
    if (at%211==0) presence[at]=0; // finite absent backing and horizon-varying pairs
    if (t<35 && i<9) member[at]=0;
    if (t>=101 && i%13==0) guard[at]=1;
  }
  auto p=engine::alpha::Panel::create(d,n,{"close"},{std::move(price)},presence); ASSERT_TRUE(p);
  auto cfg=ex::equivalence_ic_screen_config(); cfg.horizons={5,21,63,0};
  cfg.window_begin=11; cfg.window_end=407; cfg.maturity_end=399; cfg.min_dates=128;
  auto serial=ex::prepare_research_ic(*p,cfg,{3,true,1},member,guard); ASSERT_TRUE(serial);
  auto ss=ex::prepare_research_ic_scratch(*serial); ASSERT_TRUE(ss);
  for (usize workers : {usize{2},usize{4}}) {
    SCOPED_TRACE(workers);
    auto parallel=ex::prepare_research_ic(*p,cfg,{3,true,workers},member,guard); ASSERT_TRUE(parallel);
    auto ps=ex::prepare_research_ic_scratch(*parallel); ASSERT_TRUE(ps);
    EXPECT_EQ(parallel->bytes(),serial->bytes());
    EXPECT_GT(ps->bytes(),ss->bytes());
    EXPECT_LT(ps->bytes()-ss->bytes(),workers*(128*n+1024)); // O(workers*N), no dense series duplication
    engine::parallel::DetPool pool{workers};
    auto first=signal;
    for (usize pass=0;pass<2;++pass) {
      SCOPED_TRACE(pass);
      if (pass==1) for (usize at=0;at<first.size();++at) {
        first[at]=-first[at];
        if (at%197==0) first[at]=std::numeric_limits<f64>::quiet_NaN();
      }
      auto a=ex::evaluate_research_ic(first,*serial,*ss); ASSERT_TRUE(a);
      auto b=ex::evaluate_research_ic(first,*parallel,*ps,&pool); ASSERT_TRUE(b);
      same_result(*a,*b,*ss,*ps);
    }
  }
}

TEST(ResearchIc, ParallelAdmissionRefusesUnboundedWorkersMismatchedPoolAndAggregateBudget) {
  auto p=make_panel(180,12); auto cfg=ex::equivalence_ic_screen_config();
  cfg.horizons={5,21,63,0}; cfg.min_names=3; cfg.min_dates=8;
  EXPECT_FALSE(ex::prepare_research_ic(p,cfg,{3,true,0}));
  EXPECT_FALSE(ex::prepare_research_ic(p,cfg,{3,true,ex::max_research_ic_workers+1}));
  EXPECT_FALSE(ex::prepare_research_ic(p,cfg,{3,true,std::numeric_limits<usize>::max()}));
  auto serial=ex::prepare_research_ic(p,cfg,{3,true,1}); ASSERT_TRUE(serial);
  auto ss=ex::prepare_research_ic_scratch(*serial); ASSERT_TRUE(ss);
  auto parallel=ex::prepare_research_ic(p,cfg,{3,true,4}); ASSERT_TRUE(parallel);
  auto ps=ex::prepare_research_ic_scratch(*parallel); ASSERT_TRUE(ps);
  const std::vector<f64> signal(180*12,1);
  EXPECT_FALSE(ex::evaluate_research_ic(signal,*parallel,*ps));
  engine::parallel::DetPool wrong{2};
  EXPECT_FALSE(ex::evaluate_research_ic(signal,*parallel,*ps,&wrong));
  EXPECT_FALSE(ex::evaluate_research_ic(signal,*serial,*ss,&wrong));
  // Cache construction fits its own transient sorting scratch, but the combined
  // immutable cache + all worker rows does not. Refuse before row allocations.
  cfg.max_cache_bytes=parallel->bytes()+12*sizeof(usize);
  auto limited=ex::prepare_research_ic(p,cfg,{3,true,4}); ASSERT_TRUE(limited);
  EXPECT_FALSE(ex::prepare_research_ic_scratch(*limited));
  // Short/zero mature support does not touch uninitialized worker scratch.
  auto tiny=make_panel(8,12); cfg.max_cache_bytes=1ULL<<20;
  auto empty=ex::prepare_research_ic(tiny,cfg,{3,true,2}); ASSERT_TRUE(empty);
  auto es=ex::prepare_research_ic_scratch(*empty); ASSERT_TRUE(es);
  const std::vector<f64> short_signal(8*12,1);
  auto e=ex::evaluate_research_ic(short_signal,*empty,*es,&wrong); ASSERT_TRUE(e);
  EXPECT_FALSE(e->screen.reject); EXPECT_EQ(e->coverage[2].mature_dates,0U);
}
} // namespace
