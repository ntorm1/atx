#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/factory/execution_stock_transition_streams.hpp"
#include "atx/engine/cost/cost_surface.hpp"

namespace {
using namespace atx;
namespace ex=atx::engine::factory;
namespace cost=atx::engine::cost;
constexpr usize D=10,N=4;
constexpr i64 day=86'400'000'000'000;
struct StockInput {
  ex::ExecutionObjectiveConfig cfg;
  std::vector<f64> close=std::vector<f64>(D*N,100),raw=std::vector<f64>(D*N,50);
  std::vector<f64> signal=std::vector<f64>(D*N);
  std::vector<u8> present=std::vector<u8>(D*N,1),member=std::vector<u8>(D*N,1);
  std::vector<u32> guard=std::vector<u32>(D*N,0);
  std::vector<i64> marks=std::vector<i64>(D),decisions=std::vector<i64>(D);
  std::array<u64,N> ids{10,20,30,40};
  f64 borrow{};
  bool missing_successor_borrow{};
  StockInput() {
    cfg.rule=ex::ExecutionObjectiveRule::DelayedSurfaceV2;
    cfg.initial_nav=1000; cfg.window_end=D; cfg.maturity_end=D;
    cfg.rebalance_sessions=252;
    for (usize t=0;t<D;++t) {
      marks[t]=(100+static_cast<i64>(t))*day; decisions[t]=marks[t]+10;
      for (usize i=0;i<N;++i) signal[t*N+i]=2*static_cast<f64>(i)-3;
      raw[t*N+2]=20; close[t*N+2]=40;
    }
  }
  void successor_move(usize t) {
    for (;t<D;++t) { raw[t*N+2]=22; close[t*N+2]=44; }
  }
  void extinct(usize name,usize t) {
    for (;t<D;++t) { present[t*N+name]=0; close[t*N+name]=777; raw[t*N+name]=777; }
  }
  void swap_names(usize a,usize b) {
    std::swap(ids[a],ids[b]);
    for (usize t=0;t<D;++t) {
      std::swap(close[t*N+a],close[t*N+b]); std::swap(raw[t*N+a],raw[t*N+b]);
      std::swap(signal[t*N+a],signal[t*N+b]); std::swap(present[t*N+a],present[t*N+b]);
      std::swap(member[t*N+a],member[t*N+b]); std::swap(guard[t*N+a],guard[t*N+b]);
    }
  }
  core::Result<atx::engine::alpha::Panel> panel() const {
    return atx::engine::alpha::Panel::create(D,N,{"close","raw_close"},{close,raw},present);
  }
  ex::ExecutionStockTransitionEvent event(usize predecessor=3,usize t=2) const {
    ex::ExecutionStockTransitionEvent e;
    e.event_id="synthetic-stock-completion"; e.revision=1;
    e.predecessor_id=ids[predecessor]; e.successor_id=ids[2];
    e.security_id_namespace="synthetic-id";
    e.predecessor_identity="synthetic-old-common"; e.successor_identity="synthetic-new-class-a";
    e.panel_source_sha256=std::string(64,'b');
    e.predecessor_identity_evidence_sha256=std::string(64,'c');
    e.successor_identity_evidence_sha256=std::string(64,'d');
    e.completion_evidence_sha256=std::string(64,'e'); e.basis_evidence_sha256=std::string(64,'f');
    e.evidence=ex::ExecutionCashClaimEvidence::SyntheticFixtureV1;
    e.reference_mark_ns=marks[t-1]; e.reference_raw_close=raw[(t-1)*N+predecessor];
    e.reference_adjusted_close=close[(t-1)*N+predecessor];
    e.effective_after_ns=marks[t-1]; e.effective_by_ns=marks[t]-2;
    e.available_at_ns=marks[t]-1; e.recognition_mark_ns=marks[t];
    e.stock_ratio_numerator=2; e.stock_ratio_denominator=1;
    e.successor_raw_close=raw[t*N+2]; e.successor_adjusted_close=close[t*N+2];
    e.stock_and_cash_excluded_from_adjusted_close=true;
    return e;
  }
  core::Result<ex::ExecutionObjectiveContext> context(const atx::engine::alpha::Panel& p,
      std::span<const ex::ExecutionStockTransitionEvent> stocks={},
      std::span<const ex::ExecutionCashClaimEvent> cash={},bool old_cash=false) const {
    cost::CostSurfaceRecipe recipe; recipe.rule=cost::CostSurfaceRule::ModeledInputsV2;
    recipe.impact_y=0; recipe.commission_bps=0;
    std::array<cost::CostSurfaceRow,N> rows{};
    for (usize i=0;i<N;++i) {
      rows[i].instrument_id=ids[i]; rows[i].state=cost::CostInputState::Available;
      rows[i].available_at_ns=1; rows[i].adv_dollars=1e9;
      rows[i].borrow_state=missing_successor_borrow && ids[i]==30?
          cost::CostInputState::Unavailable:cost::CostInputState::Available;
      rows[i].borrow_available_at_ns=1; rows[i].borrow_annual_fraction=borrow;
    }
    std::vector<cost::CostSurface> surfaces;
    for (usize t=cfg.window_begin;t<D-cfg.delay-1;++t) {
      ATX_TRY(auto s,cost::CostSurface::create(recipe,
          {decisions[t],std::string(64,'a'),"synthetic-prior","explicit-test"},rows));
      surfaces.push_back(std::move(s));
    }
    atx::engine::WeightPolicy policy; policy.winsorize_limit=0;
    policy.transform=atx::engine::Transform::Raw;
    const ex::ExecutionObjectiveIdentity identity{std::string(64,'b'),"synthetic-stock","tri"};
    if (old_cash) return ex::prepare_execution_objective_claims(p,policy,cfg,surfaces,marks,
        decisions,ids,identity,cash,member,guard);
    return ex::prepare_execution_objective_transitions(p,policy,cfg,surfaces,marks,decisions,
        ids,identity,cash,stocks,member,guard);
  }
};
ex::ExecutionCashClaimEvent cash_event(const ex::ExecutionStockTransitionEvent& se) {
  ex::ExecutionCashClaimEvent ce;
  ce.event_id="cash-only"; ce.revision=1; ce.instrument_id=se.predecessor_id;
  ce.security_id_namespace=se.security_id_namespace; ce.historical_identity=se.predecessor_identity;
  ce.panel_source_sha256=se.panel_source_sha256;
  ce.identity_evidence_sha256=se.predecessor_identity_evidence_sha256;
  ce.completion_evidence_sha256=se.completion_evidence_sha256; ce.basis_evidence_sha256=se.basis_evidence_sha256;
  ce.evidence=se.evidence; ce.reference_mark_ns=se.reference_mark_ns;
  ce.reference_raw_close=se.reference_raw_close; ce.reference_adjusted_close=se.reference_adjusted_close;
  ce.effective_after_ns=se.effective_after_ns; ce.effective_by_ns=se.effective_by_ns;
  ce.available_at_ns=se.available_at_ns; ce.recognition_mark_ns=se.recognition_mark_ns;
  ce.cash_usd_per_raw_share=60; ce.cash_excluded_from_adjusted_close=true;
  return ce;
}
void equal_bits(const std::vector<f64>& a,const std::vector<f64>& b) {
  ASSERT_EQ(a.size(),b.size());
  for (usize k=0;k<a.size();++k) EXPECT_EQ(std::bit_cast<u64>(a[k]),std::bit_cast<u64>(b[k]));
}

TEST(ExecutionStockTransition, MarksExistingSuccessorThenAddsSignedRawShareDelivery) {
  StockInput f; f.successor_move(2); const auto e=f.event(); f.extinct(3,2);
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_transitions(f.signal,*c); ASSERT_TRUE(r);
  ASSERT_EQ(r->stock_recognitions.size(),1U); const auto& v=r->stock_recognitions[0];
  EXPECT_DOUBLE_EQ(v.removed_equity_dollars,375);
  EXPECT_DOUBLE_EQ(v.predecessor_share_equivalents,7.5); // raw50, not adjusted100
  EXPECT_DOUBLE_EQ(v.delivered_successor_share_equivalents,15);
  EXPECT_DOUBLE_EQ(v.delivered_successor_dollars,330); // raw22, not adjusted44
  EXPECT_DOUBLE_EQ(v.recognition_pnl_dollars,-45);
  EXPECT_NEAR(r->cash.streams.end_nav_flat[2],967.5,1e-12); // existing125 gained12.5
  EXPECT_NEAR(r->cash.streams.positions(0,3)[2],467.5/967.5,1e-15);
  EXPECT_DOUBLE_EQ(r->cash.streams.positions(0,3)[3],0);
  EXPECT_DOUBLE_EQ(r->cash.settled_cash_dollars[2],1000);
  EXPECT_DOUBLE_EQ(r->cash.streams.turnover_flat[3],0);
  EXPECT_DOUBLE_EQ(r->cash.streams.execution_cost_flat[2],0);
  EXPECT_TRUE(r->cash.recognitions.empty()); EXPECT_TRUE(r->cash.event_uses.empty());
  EXPECT_TRUE(r->physical_delivery_and_fraction_cash_unresolved);
  EXPECT_FALSE(ex::extract_execution_signal_claims(f.signal,*c));
  auto strict=f.context(*p); ASSERT_TRUE(strict);
  EXPECT_FALSE(ex::extract_execution_signal_claims(f.signal,*strict));
  // Swap predecessor/successor axis order, including every signal/support cell.
  f.swap_names(2,3); auto pp=f.panel(); ASSERT_TRUE(pp);
  auto cc=f.context(*pp,{&e,1}); ASSERT_TRUE(cc);
  auto rr=ex::extract_execution_signal_transitions(f.signal,*cc); ASSERT_TRUE(rr);
  EXPECT_NEAR(rr->cash.streams.end_nav_flat[2],967.5,1e-12);
  EXPECT_NEAR(rr->cash.streams.positions(0,3)[3],467.5/967.5,1e-15);
}

TEST(ExecutionStockTransition, DelayedSuccessorTargetNetsDeliveredBookAndCancelsPredecessor) {
  StockInput f; f.cfg.delay=2; f.cfg.rebalance_sessions=1; f.successor_move(3);
  const auto e=f.event(3,3); f.extinct(3,3);
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_transitions(f.signal,*c); ASSERT_TRUE(r);
  // d0 fills at2. Mark3 adds330 to existing137.5; d1's already queued successor
  // target125 sells342.5, rather than adding another125 or wiping the delivery.
  EXPECT_NEAR(r->cash.streams.end_nav_flat[3],967.5,1e-12);
  EXPECT_NEAR(r->cash.streams.positions(0,4)[2],125.0/967.5,1e-15);
  EXPECT_NEAR(r->cash.streams.turnover_flat[4],342.5/967.5,1e-15);
  for (usize t=4;t<D;++t) EXPECT_DOUBLE_EQ(r->cash.streams.positions(0,t)[3],0);
}

TEST(ExecutionStockTransition, ShortDeliveryAndFixedCashLiabilityRetainModeledBorrow) {
  StockInput f; f.borrow=.365; f.successor_move(2); auto e=f.event();
  e.fixed_cash_usd_per_raw_share=10; f.extinct(3,2);
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_transitions(f.signal,*c,-1); ASSERT_TRUE(r);
  const auto& v=r->stock_recognitions[0];
  EXPECT_DOUBLE_EQ(v.delivered_successor_dollars,-330);
  EXPECT_DOUBLE_EQ(v.fixed_cash_claim_dollars,-75);
  EXPECT_DOUBLE_EQ(r->cash.payable_dollars[2],75);
  EXPECT_DOUBLE_EQ(r->cash.recognition_pnl_dollars[2],0); // stock bridge separate
  EXPECT_NEAR(r->cash.streams.end_nav_flat[2],957,1e-12);
  EXPECT_NEAR(r->cash.claim_borrow_dollars[3],.075,1e-14);
  EXPECT_NEAR(r->cash.streams.borrow_cost_flat[3],.5425/957.0,1e-15);
  EXPECT_NEAR(r->cash.streams.end_nav_flat[3],956.4575,1e-12);
  EXPECT_NEAR(r->cash.settled_cash_dollars[3],998.9575,1e-12);
  // Missing modeled successor borrow is not a free inherited loan.
  f.missing_successor_borrow=true; auto refused=f.context(*p,{&e,1}); ASSERT_TRUE(refused);
  EXPECT_FALSE(ex::extract_execution_signal_transitions(f.signal,*refused,-1));
}

TEST(ExecutionStockTransition, MultiplePredecessorsAddToSameSuccessorWithoutDoubleMark) {
  StockInput f; f.successor_move(2); auto a=f.event(0),b=f.event(3); b.event_id+="-two";
  f.extinct(0,2); f.extinct(3,2); const std::array events{a,b};
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,events); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_transitions(f.signal,*c); ASSERT_TRUE(r);
  ASSERT_EQ(r->stock_recognitions.size(),2U);
  EXPECT_DOUBLE_EQ(r->signed_delivered_dollars[2],0); // equal signed deliveries net
  EXPECT_NEAR(r->cash.streams.end_nav_flat[2],1012.5,1e-12);
  EXPECT_NEAR(r->cash.streams.positions(0,3)[2],137.5/1012.5,1e-15);
}

TEST(ExecutionStockTransition, RefusesClockIdentityRawBasisAndAbsentSuccessorMark) {
  StockInput f; f.successor_move(2); auto e=f.event(); auto p=f.panel(); ASSERT_TRUE(p);
  e.available_at_ns=e.recognition_mark_ns; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.recognition_mark_ns=f.marks[3]; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.reference_raw_close=100; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.successor_raw_close=44; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.successor_id=999; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.panel_source_sha256=std::string(64,'c'); EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); e.stock_and_cash_excluded_from_adjusted_close=false; EXPECT_FALSE(f.context(*p,{&e,1}));
  e=f.event(); f.present[2*N+2]=0; // finite matching backing price is not observation
  auto absent=f.panel(); ASSERT_TRUE(absent); EXPECT_FALSE(f.context(*absent,{&e,1}));
  f.present[2*N+2]=1; f.extinct(0,2); f.extinct(3,2);
  auto gap=f.panel(); ASSERT_TRUE(gap); auto gc=f.context(*gap,{&e,1}); ASSERT_TRUE(gc);
  EXPECT_FALSE(ex::extract_execution_signal_transitions(f.signal,*gc)); // unrelated short hole
  f.cfg.max_working_bytes=1; EXPECT_FALSE(f.context(*p,{&e,1}));
}

TEST(ExecutionStockTransition, PreRoleOutsideAndFutureEventsNeedNoIrrelevantSuccessorAxis) {
  StockInput f; auto e=f.event(); e.successor_id=999; f.cfg.window_begin=4;
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&e,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_transitions(f.signal,*c); ASSERT_TRUE(r);
  EXPECT_EQ(r->stock_event_uses[0].use,ex::ExecutionCashClaimUse::PreRoleRetired);
  EXPECT_TRUE(r->stock_recognitions.empty());
  EXPECT_DOUBLE_EQ(r->cash.streams.positions(0,6)[3],0);
  EXPECT_DOUBLE_EQ(r->cash.signed_claim_dollars[6],0);
  auto outside=e; outside.predecessor_id=998;
  auto oc=f.context(*p,{&outside,1}); ASSERT_TRUE(oc);
  auto rr=ex::extract_execution_signal_transitions(f.signal,*oc); ASSERT_TRUE(rr);
  EXPECT_EQ(rr->stock_event_uses[0].use,ex::ExecutionCashClaimUse::OutsideAxis);
  auto future=e; future.reference_mark_ns=f.marks.back()+day;
  future.effective_after_ns=future.reference_mark_ns;
  future.effective_by_ns=future.reference_mark_ns+1; future.available_at_ns=future.effective_by_ns+1;
  future.recognition_mark_ns=future.available_at_ns+1;
  auto fc=f.context(*p,{&future,1}); ASSERT_TRUE(fc);
  auto fr=ex::extract_execution_signal_transitions(f.signal,*fc); ASSERT_TRUE(fr);
  EXPECT_EQ(fr->stock_event_uses[0].use,ex::ExecutionCashClaimUse::AfterRole);
  auto late=e; late.recognition_mark_ns=f.marks[5]; EXPECT_FALSE(f.context(*p,{&late,1}));
}

TEST(ExecutionStockTransition, FutureTermsDoNotChangeEarlierExecutionPrefix) {
  StockInput f; f.successor_move(6); auto e=f.event(3,6); f.extinct(3,6);
  auto p=f.panel(); ASSERT_TRUE(p); auto a=f.context(*p,{&e,1}); ASSERT_TRUE(a);
  auto ar=ex::extract_execution_signal_transitions(f.signal,*a); ASSERT_TRUE(ar);
  e.stock_ratio_numerator=3; e.fixed_cash_usd_per_raw_share=1;
  auto b=f.context(*p,{&e,1}); ASSERT_TRUE(b);
  auto br=ex::extract_execution_signal_transitions(f.signal,*b); ASSERT_TRUE(br);
  EXPECT_NE(a->identity_sha256(),b->identity_sha256());
  for (usize t=2;t<6;++t) {
    EXPECT_EQ(std::bit_cast<u64>(ar->cash.streams.pnl_flat[t]),std::bit_cast<u64>(br->cash.streams.pnl_flat[t]));
    EXPECT_EQ(std::bit_cast<u64>(ar->cash.streams.end_nav_flat[t]),std::bit_cast<u64>(br->cash.streams.end_nav_flat[t]));
  }
}

TEST(ExecutionStockTransition, CashAndStockRecordsHaveIndependentIndexesAndCombinedBalances) {
  StockInput f; f.successor_move(2); auto se=f.event();
  se.fixed_cash_usd_per_raw_share=10;
  const auto ce=cash_event(f.event(0)); f.extinct(0,2); f.extinct(3,2);
  auto p=f.panel(); ASSERT_TRUE(p); auto c=f.context(*p,{&se,1},{&ce,1}); ASSERT_TRUE(c);
  auto r=ex::extract_execution_signal_transitions(f.signal,*c); ASSERT_TRUE(r);
  ASSERT_EQ(r->cash.recognitions.size(),1U); ASSERT_EQ(r->stock_recognitions.size(),1U);
  EXPECT_EQ(r->cash.recognitions[0].event_index,0U);
  EXPECT_EQ(r->cash.recognitions[0].instrument_id,10U);
  EXPECT_EQ(r->stock_recognitions[0].event_index,0U);
  EXPECT_EQ(r->stock_recognitions[0].predecessor_id,40U);
  EXPECT_DOUBLE_EQ(r->cash.recognition_pnl_dollars[2],-75);
  EXPECT_DOUBLE_EQ(r->recognition_pnl_dollars[2],30);
  EXPECT_DOUBLE_EQ(r->cash.payable_dollars[2],450);
  EXPECT_DOUBLE_EQ(r->cash.receivable_dollars[2],75);
  EXPECT_NEAR(r->cash.streams.end_nav_flat[2],967.5,1e-12);
  EXPECT_DOUBLE_EQ(r->cash.settled_cash_dollars[2],1000);
}

TEST(ExecutionStockTransition, EmptyStockPreservesCashRecipeAndAllNumericalArrays) {
  StockInput f; const auto se=f.event(); f.extinct(3,2);
  const auto ce=cash_event(se);
  auto p=f.panel(); ASSERT_TRUE(p);
  auto a=f.context(*p,{}, {&ce,1},true); ASSERT_TRUE(a);
  auto b=f.context(*p,{}, {&ce,1}); ASSERT_TRUE(b);
  EXPECT_EQ(a->identity_sha256(),b->identity_sha256());
  const auto ar=ex::extract_execution_signal_claims(f.signal,*a); ASSERT_TRUE(ar);
  const auto br=ex::extract_execution_signal_transitions(f.signal,*b); ASSERT_TRUE(br);
  const auto& x=ar->streams; const auto& y=br->cash.streams;
  for (const auto pair:{std::pair{&x.pnl_flat,&y.pnl_flat},std::pair{&x.gross_flat,&y.gross_flat},
      std::pair{&x.pos_flat,&y.pos_flat},std::pair{&x.end_nav_flat,&y.end_nav_flat},
      std::pair{&x.execution_cost_flat,&y.execution_cost_flat},std::pair{&x.borrow_cost_flat,&y.borrow_cost_flat},
      std::pair{&x.turnover_flat,&y.turnover_flat},std::pair{&x.pretrade_nav_flat,&y.pretrade_nav_flat},
      std::pair{&ar->signed_claim_dollars,&br->cash.signed_claim_dollars},
      std::pair{&ar->receivable_dollars,&br->cash.receivable_dollars},
      std::pair{&ar->payable_dollars,&br->cash.payable_dollars},
      std::pair{&ar->recognition_pnl_dollars,&br->cash.recognition_pnl_dollars},
      std::pair{&ar->claim_borrow_dollars,&br->cash.claim_borrow_dollars},
      std::pair{&ar->settled_cash_dollars,&br->cash.settled_cash_dollars}}) equal_bits(*pair.first,*pair.second);
  EXPECT_EQ(x.valid_flat,y.valid_flat); EXPECT_EQ(x.names_flat,y.names_flat);
  EXPECT_EQ(x.capped_names_flat,y.capped_names_flat);
  EXPECT_EQ(x.execution_context_sha256,y.execution_context_sha256);
  EXPECT_TRUE(br->stock_recognitions.empty()); EXPECT_TRUE(br->signed_delivered_dollars.empty());
}
} // namespace
