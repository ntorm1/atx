#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/factory/execution_cash_claim_streams.hpp"
#include "atx/engine/cost/cost_surface.hpp"

namespace {
using namespace atx;
namespace ex=atx::engine::factory;
namespace cost=atx::engine::cost;
constexpr usize D=10,N=3;
constexpr i64 day=86'400'000'000'000;
struct ClaimInput {
  ex::ExecutionObjectiveConfig cfg;
  std::vector<f64> close=std::vector<f64>(D*N,100),raw=std::vector<f64>(D*N,50);
  std::vector<f64> signal=std::vector<f64>(D*N);
  std::vector<u8> present=std::vector<u8>(D*N,1),member=std::vector<u8>(D*N,1);
  std::vector<u32> guard=std::vector<u32>(D*N,0);
  std::vector<i64> marks=std::vector<i64>(D),decisions=std::vector<i64>(D);
  std::array<u64,N> ids{10,20,30};
  f64 borrow{};
  ClaimInput() {
    cfg.rule=ex::ExecutionObjectiveRule::DelayedSurfaceV2;
    cfg.initial_nav=1000; cfg.window_end=D; cfg.maturity_end=D;
    cfg.rebalance_sessions=252;
    for (usize t=0;t<D;++t) {
      marks[t]=(100+static_cast<i64>(t))*day; decisions[t]=marks[t]+1;
      signal[t*N]=-1; signal[t*N+1]=0; signal[t*N+2]=1;
    }
  }
  core::Result<atx::engine::alpha::Panel> panel() const {
    return atx::engine::alpha::Panel::create(D,N,{"close","raw_close"},{close,raw},present);
  }
  ex::ExecutionCashClaimEvent event(usize name=2,usize t=2) const {
    ex::ExecutionCashClaimEvent e;
    e.event_id="synthetic-cash-completion"; e.revision=1; e.instrument_id=ids[name];
    e.security_id_namespace="synthetic-id"; e.historical_identity="synthetic-common";
    e.panel_source_sha256=std::string(64,'b');
    e.identity_evidence_sha256=std::string(64,'c');
    e.completion_evidence_sha256=std::string(64,'d');
    e.basis_evidence_sha256=std::string(64,'e');
    e.evidence=ex::ExecutionCashClaimEvidence::SyntheticFixtureV1;
    e.reference_mark_ns=marks[t-1]; e.reference_raw_close=raw[(t-1)*N+name];
    e.reference_adjusted_close=close[(t-1)*N+name];
    e.effective_after_ns=marks[t-1]; e.effective_by_ns=marks[t]-2;
    e.available_at_ns=marks[t]-1; e.recognition_mark_ns=marks[t];
    e.cash_usd_per_raw_share=60; e.cash_excluded_from_adjusted_close=true;
    return e;
  }
  void extinct(usize name,usize t) {
    for (;t<D;++t) { present[t*N+name]=0; close[t*N+name]=777; raw[t*N+name]=777; }
  }
  core::Result<ex::ExecutionObjectiveContext> context(const atx::engine::alpha::Panel& p,
      std::span<const ex::ExecutionCashClaimEvent> events={},bool legacy=false) const {
    cost::CostSurfaceRecipe recipe; recipe.rule=cost::CostSurfaceRule::ModeledInputsV2;
    recipe.impact_y=0; recipe.commission_bps=0;
    std::array<cost::CostSurfaceRow,N> rows{};
    for (usize i=0;i<N;++i) {
      rows[i].instrument_id=ids[i]; rows[i].state=cost::CostInputState::Available;
      rows[i].available_at_ns=1; rows[i].adv_dollars=1e9;
      rows[i].borrow_state=cost::CostInputState::Available;
      rows[i].borrow_available_at_ns=1; rows[i].borrow_annual_fraction=borrow;
    }
    std::vector<cost::CostSurface> surfaces;
    for (usize t=cfg.window_begin;t<D-cfg.delay-1;++t) {
      ATX_TRY(auto s,cost::CostSurface::create(recipe,
          {decisions[t],std::string(64,'a'),"synthetic-prior","explicit-test"},rows));
      surfaces.push_back(std::move(s));
    }
    atx::engine::WeightPolicy policy; policy.winsorize_limit=0;
    const ex::ExecutionObjectiveIdentity identity{std::string(64,'b'),"synthetic-claim","tri"};
    if (legacy) return ex::prepare_execution_objective(p,policy,cfg,surfaces,marks,decisions,
                                                      ids,identity,member,guard);
    return ex::prepare_execution_objective_claims(p,policy,cfg,surfaces,marks,decisions,
                                                 ids,identity,events,member,guard);
  }
};

TEST(ExecutionCashClaim, LongRecognizesRawShareReceivableWithoutCashOrTurnover) {
  ClaimInput f; const auto e=f.event(); f.extinct(2,2);
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto result=ex::extract_execution_signal_claims(f.signal,*c); ASSERT_TRUE(result);
  ASSERT_EQ(result->recognitions.size(),1U);
  const auto& r=result->recognitions[0];
  EXPECT_DOUBLE_EQ(r.removed_equity_dollars,500);
  EXPECT_DOUBLE_EQ(r.research_share_equivalents,10); // raw50, NOT adjusted100
  EXPECT_DOUBLE_EQ(r.signed_claim_dollars,600);
  EXPECT_DOUBLE_EQ(r.recognition_pnl_dollars,100);
  EXPECT_DOUBLE_EQ(result->settled_cash_dollars[2],1000);
  EXPECT_DOUBLE_EQ(result->streams.end_nav_flat[2],1100);
  EXPECT_DOUBLE_EQ(result->streams.gross_flat[2],.1);
  EXPECT_DOUBLE_EQ(result->streams.turnover_flat[3],0);
  EXPECT_DOUBLE_EQ(result->streams.execution_cost_flat[2],0);
  EXPECT_DOUBLE_EQ(result->receivable_dollars.back(),600);
  EXPECT_DOUBLE_EQ(result->payable_dollars.back(),0);
  EXPECT_TRUE(result->payment_dates_unknown);
  EXPECT_DOUBLE_EQ(result->streams.positions(0,3)[2],0);
  EXPECT_FALSE(ex::extract_execution_signal(f.signal,*c)); // reporting cannot be bypassed
  auto strict=f.context(*p,{},true); ASSERT_TRUE(strict);
  EXPECT_FALSE(ex::extract_execution_signal(f.signal,*strict));
}

TEST(ExecutionCashClaim, ShortKeepsSignedLiabilityCashReserveAndCalendarBorrow) {
  ClaimInput f; f.borrow=.365; const auto e=f.event(0); f.extinct(0,2);
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_claims(f.signal,*c); ASSERT_TRUE(r);
  ASSERT_EQ(r->recognitions.size(),1U);
  EXPECT_DOUBLE_EQ(r->recognitions[0].signed_claim_dollars,-600);
  EXPECT_DOUBLE_EQ(r->payable_dollars[2],600);
  EXPECT_DOUBLE_EQ(r->receivable_dollars[2],0);
  EXPECT_NEAR(r->settled_cash_dollars[2],999.5,1e-12);
  EXPECT_NEAR(r->streams.end_nav_flat[2],899.5,1e-12);
  EXPECT_DOUBLE_EQ(r->claim_borrow_dollars[2],0); // old equity paid recognition interval
  EXPECT_NEAR(r->claim_borrow_dollars[3],.6,1e-14);
  EXPECT_NEAR(r->streams.end_nav_flat[3],898.9,1e-12);
  EXPECT_DOUBLE_EQ(r->streams.positions(0,3)[0],0); // liability was not forgiven
  auto costly=e; costly.cash_usd_per_raw_share=110; // payable1100 exceeds cash1000
  auto bounded=f.context(*p,{&costly,1}); ASSERT_TRUE(bounded);
  const auto refused=ex::extract_execution_signal_claims(f.signal,*bounded);
  ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code(),core::ErrorCode::Unavailable);
}

TEST(ExecutionCashClaim, RetiresPendingOrdersAndExcludesReceivableFromNewTargets) {
  ClaimInput f; f.cfg.rebalance_sessions=1;
  const auto e=f.event(); f.extinct(2,2);
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_claims(f.signal,*c); ASSERT_TRUE(r);
  // At d2 NAV1100 includes a nonspendable600 claim. Only500 funds new target
  // gross: +/-250, rather than +/-550. d1's queued extinct-name order canceled.
  EXPECT_NEAR(r->streams.positions(0,4)[0],-250.0/1100.0,1e-15);
  EXPECT_NEAR(r->streams.positions(0,4)[1],250.0/1100.0,1e-15);
  for (usize t=3;t<D;++t) EXPECT_DOUBLE_EQ(r->streams.positions(0,t)[2],0);
  ClaimInput delayed; delayed.cfg.delay=3; delayed.cfg.rebalance_sessions=1;
  const auto de=delayed.event(2,4); delayed.extinct(2,4);
  auto dp=delayed.panel(); ASSERT_TRUE(dp); auto dc=delayed.context(*dp,{&de,1}); ASSERT_TRUE(dc);
  auto dr=ex::extract_execution_signal_claims(delayed.signal,*dc); ASSERT_TRUE(dr);
  ASSERT_EQ(dr->recognitions.size(),1U);
  EXPECT_DOUBLE_EQ(dr->recognitions[0].removed_equity_dollars,500);
  for (usize t=5;t<D;++t) EXPECT_DOUBLE_EQ(dr->streams.positions(0,t)[2],0);
}

TEST(ExecutionCashClaim, RejectsLateKnowledgeBadBasisAndPreservesFuturePrefix) {
  ClaimInput f; auto e=f.event(); auto p=f.panel(); ASSERT_TRUE(p);
  e.available_at_ns=e.recognition_mark_ns; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.recognition_mark_ns=f.marks[3]; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.reference_raw_close=100; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.panel_source_sha256=std::string(64,'f'); EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.cash_excluded_from_adjusted_close=false; EXPECT_FALSE(f.context(*p,{&e,1}));
  const std::array duplicate{f.event(),f.event()}; EXPECT_FALSE(f.context(*p,duplicate));
  e=f.event(2,6); auto a=f.context(*p,{&e,1}); ASSERT_TRUE(a);
  auto first=ex::extract_execution_signal_claims(f.signal,*a); ASSERT_TRUE(first);
  e.cash_usd_per_raw_share=65;
  auto b=f.context(*p,{&e,1}); ASSERT_TRUE(b);
  auto second=ex::extract_execution_signal_claims(f.signal,*b); ASSERT_TRUE(second);
  EXPECT_NE(a->identity_sha256(),b->identity_sha256());
  for (usize t=2;t<6;++t) {
    EXPECT_EQ(std::bit_cast<u64>(first->streams.pnl_flat[t]),
              std::bit_cast<u64>(second->streams.pnl_flat[t]));
    EXPECT_EQ(std::bit_cast<u64>(first->streams.end_nav_flat[t]),
              std::bit_cast<u64>(second->streams.end_nav_flat[t]));
  }
  // A known event exempts only its own name, not another absent held endpoint.
  ClaimInput gap; const auto ge=gap.event(); gap.extinct(2,2); gap.extinct(0,2);
  auto gp=gap.panel(); ASSERT_TRUE(gp); auto gc=gap.context(*gp,{&ge,1}); ASSERT_TRUE(gc);
  EXPECT_FALSE(ex::extract_execution_signal_claims(gap.signal,*gc));
}

TEST(ExecutionCashClaim, PreRoleExtinctionCreatesNoOpeningClaimAndOtherAxesAreExplicit) {
  ClaimInput f; const auto e=f.event(); f.cfg.window_begin=4;
  // Keep finite/present backing cells: known extinction must still block orders.
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_claims(f.signal,*c); ASSERT_TRUE(r);
  ASSERT_EQ(r->event_uses.size(),1U);
  EXPECT_EQ(r->event_uses[0].use,ex::ExecutionCashClaimUse::PreRoleRetired);
  EXPECT_TRUE(r->recognitions.empty());
  EXPECT_DOUBLE_EQ(r->signed_claim_dollars[6],0);
  EXPECT_DOUBLE_EQ(r->streams.positions(0,6)[2],0);
  auto absent=e; absent.instrument_id=999;
  auto ac=f.context(*p,{&absent,1}); ASSERT_TRUE(ac);
  auto ar=ex::extract_execution_signal_claims(f.signal,*ac); ASSERT_TRUE(ar);
  EXPECT_EQ(ar->event_uses[0].use,ex::ExecutionCashClaimUse::OutsideAxis);
  auto future=e; future.reference_mark_ns=f.marks.back()+day;
  future.effective_after_ns=future.reference_mark_ns;
  future.effective_by_ns=future.reference_mark_ns+1;
  future.available_at_ns=future.effective_by_ns+1;
  future.recognition_mark_ns=future.available_at_ns+1;
  auto fc=f.context(*p,{&future,1}); ASSERT_TRUE(fc);
  auto fr=ex::extract_execution_signal_claims(f.signal,*fc); ASSERT_TRUE(fr);
  EXPECT_EQ(fr->event_uses[0].use,ex::ExecutionCashClaimUse::AfterRole);
}

TEST(ExecutionCashClaim, EmptyEventsPreserveDefaultDigestAndAllOutputBits) {
  ClaimInput f; auto p=f.panel(); ASSERT_TRUE(p);
  auto old=f.context(*p,{},true); ASSERT_TRUE(old); auto empty=f.context(*p); ASSERT_TRUE(empty);
  EXPECT_EQ(old->identity_sha256(),empty->identity_sha256());
  auto a=ex::extract_execution_signal(f.signal,*old); ASSERT_TRUE(a);
  auto b=ex::extract_execution_signal_claims(f.signal,*empty); ASSERT_TRUE(b);
  ASSERT_EQ(a->pnl_flat.size(),b->streams.pnl_flat.size());
  for (usize t=0;t<D;++t) {
    EXPECT_EQ(std::bit_cast<u64>(a->pnl_flat[t]),std::bit_cast<u64>(b->streams.pnl_flat[t]));
    EXPECT_EQ(std::bit_cast<u64>(a->end_nav_flat[t]),std::bit_cast<u64>(b->streams.end_nav_flat[t]));
  }
  for (usize k=0;k<a->pos_flat.size();++k)
    EXPECT_EQ(std::bit_cast<u64>(a->pos_flat[k]),std::bit_cast<u64>(b->streams.pos_flat[k]));
  EXPECT_TRUE(b->signed_claim_dollars.empty()); EXPECT_TRUE(b->recognitions.empty());
  f.cfg.max_working_bytes=1;
  EXPECT_FALSE(f.context(*p));
}
} // namespace
