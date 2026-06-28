// gate_dsr_pbo_test.cpp — p7 Sprint 1: deflation gates in AlphaGate::admit.
//
// Sprint 1 wires three opt-in deflation / selection-bias screens into the
// stateless AlphaGate, each reading a per-candidate scalar from GateDeflation:
//   * S1-1 DSR floor       (cfg.min_dsr > 0 && defl.dsr < min_dsr   -> RejectDsr)
//   * S1-2 PBO ceiling     (cfg.max_pbo < 1 && defl.pbo > max_pbo   -> RejectPbo)
//   * S1-3 split-half guard (cfg.require_split_stable && !defl.split_stable
//                                                        -> RejectSplitUnstable)
// plus the S1-0 field plumbing and the S1-5 reject-histogram layout pin.
//
// DESIGN NOTE (see gate.hpp GateDeflation): the per-candidate dsr/pbo/split_stable
// scalars are carried in a NON-serialized GateDeflation struct, NOT on AlphaMetrics
// — AlphaMetrics is memcpy'd verbatim into the library segment record (frozen
// sizeof==56), so growing it would break on-disk byte-identity. The gate reads the
// scalars from `defl`, which defaults to the inert instance for every pre-S1 caller.
//
// Each screen ships the four mandated determinism classes:
//   (a) off-path byte-identity — inert GateConfig default => verdict unchanged;
//   (b) on-path RED->GREEN     — a non-inert bar flips a qualifying candidate;
//   (c) twice-run              — same inputs => same verdict (no hidden state);
//   (d) seq==parallel          — admit() is a pure const function, so a worker-thread
//                                call yields the same verdict as the serial call.
//
// All new GateVerdict enumerators are appended at the END (frozen reject-histogram
// index); HistogramIndicesAreFrozen pins that contract at compile time.

#include <future> // std::async (seq==parallel purity check)
#include <vector> // std::vector (fixture storage)

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/gate.hpp"    // GateConfig, GateVerdict, GateDeflation, AlphaGate
#include "atx/engine/combine/metrics.hpp" // AlphaMetrics
#include "atx/engine/combine/store.hpp"   // AlphaStore

namespace atxtest_gate_dsr_pbo {

using atx::f64;
using atx::usize;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::GateConfig;
using atx::engine::combine::GateDeflation;
using atx::engine::combine::GateVerdict;

// Metrics that clear ALL default floors (min_sharpe=0.25, min_fitness=1.0,
// max_turnover=0.70) so a test can isolate a single deflation gate. A short
// candidate PnL stream + empty pool means the correlation gate always clears
// (corr_to_pool = 0), so the deflation screen is the deciding gate.
[[nodiscard]] AlphaMetrics passing_metrics() {
  return AlphaMetrics{/*sharpe*/ 2.0,   /*turnover*/ 0.30, /*returns*/ 0.10,
                      /*drawdown*/ 0.1, /*margin*/ 1.0,    /*fitness*/ 2.0,
                      /*holding_days*/ 3.3};
}

// A short candidate PnL stream; the empty-pool corr gate ignores its contents.
[[nodiscard]] std::vector<f64> cand_pnl() { return {0.0, 0.01, -0.02, 0.03}; }

// ===========================================================================
//  S1-0 / S1-5 — field plumbing + reject-histogram layout pin
// ===========================================================================

// The reject-histogram treats GateVerdict's underlying value as a stable array
// index, so the enumerator order is FROZEN: S1 appends RejectDsr/RejectPbo/
// RejectSplitUnstable at the END (indices 5,6,7) and must never renumber an
// existing value. A reorder would silently move counts into the wrong bucket; the
// static_asserts make any reorder a COMPILE error.
TEST(GateVerdictLayout, HistogramIndicesAreFrozen) {
  static_assert(static_cast<int>(GateVerdict::Accept) == 0);
  static_assert(static_cast<int>(GateVerdict::RejectSharpe) == 1);
  static_assert(static_cast<int>(GateVerdict::RejectFitness) == 2);
  static_assert(static_cast<int>(GateVerdict::RejectTurnover) == 3);
  static_assert(static_cast<int>(GateVerdict::RejectCorrelated) == 4);
  static_assert(static_cast<int>(GateVerdict::RejectDsr) == 5);
  static_assert(static_cast<int>(GateVerdict::RejectPbo) == 6);
  static_assert(static_cast<int>(GateVerdict::RejectSplitUnstable) == 7);
  SUCCEED();
}

// The enum now spans 8 enumerators (was 5 pre-S1). Pin the count so a later
// addition that is not reflected in the layout pin is caught.
TEST(GateVerdictLayout, EnumeratorCountIsEight) {
  EXPECT_EQ(static_cast<int>(GateVerdict::RejectSplitUnstable) + 1, 8);
}

// S1-0 sentinel pin: a default GateDeflation is inert at the inert GateConfig.
TEST(GateDeflationSentinels, DefaultsAreInert) {
  const GateDeflation d;
  EXPECT_EQ(d.dsr, 1.0);        // DSR max => clears any min_dsr in [0,1]
  EXPECT_EQ(d.pbo, 0.0);        // PBO min => never exceeds any max_pbo in [0,1]
  EXPECT_FALSE(d.split_stable); // gated only when require_split_stable is set
}

// ===========================================================================
//  S1-1 — DSR floor
// ===========================================================================

// (a) off-path: min_dsr=0.0 (default) => guard never fires; synthetic alphas with
// dsr {0.1,0.4,0.9} all keep their non-RejectDsr verdict.
TEST(AlphaGateDsr, OffPathZeroFloorPreservesVerdict) {
  const AlphaGate gate{GateConfig{}}; // min_dsr defaults to 0.0
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  for (const f64 dsr : {0.1, 0.4, 0.9}) {
    const GateDeflation defl{/*dsr*/ dsr, /*pbo*/ 0.0, /*split_stable*/ false};
    EXPECT_EQ(gate.admit(m, cand, pool, defl), GateVerdict::Accept)
        << "min_dsr=0.0 must never produce RejectDsr (dsr=" << dsr << ")";
  }
  // The default-arg (no defl) path must equal the inert-defl path exactly.
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::Accept);
}

// (b) on-path RED->GREEN: min_dsr=0.5, defl.dsr=0.3 (below floor) => RejectDsr;
// the SAME candidate with min_dsr=0.0 => Accept (empty pool clears corr).
TEST(AlphaGateDsr, OnPathBelowFloorRejects) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  const GateDeflation defl{/*dsr*/ 0.3, /*pbo*/ 0.0, /*split_stable*/ false};

  GateConfig cfg_floor;
  cfg_floor.min_dsr = 0.5;
  EXPECT_EQ(AlphaGate{cfg_floor}.admit(m, cand, pool, defl), GateVerdict::RejectDsr);

  GateConfig cfg_off; // min_dsr=0.0
  EXPECT_EQ(AlphaGate{cfg_off}.admit(m, cand, pool, defl), GateVerdict::Accept);
}

// on-path PASS: min_dsr=0.5, defl.dsr=0.7 => DSR check passes; the verdict is
// decided by the later gates (here Accept), NOT RejectDsr.
TEST(AlphaGateDsr, OnPathAboveFloorPasses) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  const GateDeflation defl{/*dsr*/ 0.7, /*pbo*/ 0.0, /*split_stable*/ false};
  GateConfig cfg;
  cfg.min_dsr = 0.5;
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, defl), GateVerdict::Accept);
}

// sentinel: the inert GateDeflation default (dsr=1.0) clears any min_dsr in [0,1].
TEST(AlphaGateDsr, InertSentinelClearsAnyFloor) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.min_dsr = 0.99;
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool), GateVerdict::Accept);
}

// (c) twice-run: same inputs => same verdict on the second call.
TEST(AlphaGateDsr, TwiceRunSameVerdict) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.min_dsr = 0.5;
  const AlphaGate gate{cfg};
  const GateDeflation defl{/*dsr*/ 0.3, /*pbo*/ 0.0, /*split_stable*/ false};
  const GateVerdict v1 = gate.admit(m, cand, pool, defl);
  const GateVerdict v2 = gate.admit(m, cand, pool, defl);
  EXPECT_EQ(v1, GateVerdict::RejectDsr);
  EXPECT_EQ(v2, v1);
}

// (d) seq==parallel: a worker-thread admit() equals the serial verdict (pure const).
TEST(AlphaGateDsr, SeqEqualsParallel) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.min_dsr = 0.5;
  const AlphaGate gate{cfg};
  const GateDeflation defl{/*dsr*/ 0.3, /*pbo*/ 0.0, /*split_stable*/ false};
  const GateVerdict serial = gate.admit(m, cand, pool, defl);
  auto fut = std::async(std::launch::async, [&] { return gate.admit(m, cand, pool, defl); });
  EXPECT_EQ(serial, GateVerdict::RejectDsr);
  EXPECT_EQ(fut.get(), serial);
}

// ===========================================================================
//  S1-2 — PBO ceiling
// ===========================================================================

// (a) off-path: max_pbo=1.0 (default) => no RejectPbo even at pbo=0.45.
TEST(AlphaGatePbo, OffPathUnitCeilingPreservesVerdict) {
  const AlphaGate gate{GateConfig{}}; // max_pbo defaults to 1.0
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  const GateDeflation defl{/*dsr*/ 1.0, /*pbo*/ 0.45, /*split_stable*/ false};
  EXPECT_EQ(gate.admit(m, cand, pool, defl), GateVerdict::Accept);
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::Accept); // default-arg path identical
}

// (b) on-path RED->GREEN: max_pbo=0.40, pbo=0.45 (above ceiling) => RejectPbo.
TEST(AlphaGatePbo, OnPathAboveCeilingRejects) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  const GateDeflation defl{/*dsr*/ 1.0, /*pbo*/ 0.45, /*split_stable*/ false};
  GateConfig cfg;
  cfg.max_pbo = 0.40;
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, defl), GateVerdict::RejectPbo);

  GateConfig cfg_off; // max_pbo=1.0
  EXPECT_EQ(AlphaGate{cfg_off}.admit(m, cand, pool, defl), GateVerdict::Accept);
}

// on-path PASS: max_pbo=0.40, pbo=0.30 => PBO check passes; verdict by later gates.
TEST(AlphaGatePbo, OnPathBelowCeilingPasses) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  const GateDeflation defl{/*dsr*/ 1.0, /*pbo*/ 0.30, /*split_stable*/ false};
  GateConfig cfg;
  cfg.max_pbo = 0.40;
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, defl), GateVerdict::Accept);
}

// sentinel: the inert GateDeflation default (pbo=0.0) clears any max_pbo in [0,1].
TEST(AlphaGatePbo, InertSentinelClearsAnyCeiling) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.max_pbo = 0.01;
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool), GateVerdict::Accept);
}

// (c) twice-run.
TEST(AlphaGatePbo, TwiceRunSameVerdict) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.max_pbo = 0.40;
  const AlphaGate gate{cfg};
  const GateDeflation defl{/*dsr*/ 1.0, /*pbo*/ 0.45, /*split_stable*/ false};
  const GateVerdict v1 = gate.admit(m, cand, pool, defl);
  const GateVerdict v2 = gate.admit(m, cand, pool, defl);
  EXPECT_EQ(v1, GateVerdict::RejectPbo);
  EXPECT_EQ(v2, v1);
}

// (d) seq==parallel.
TEST(AlphaGatePbo, SeqEqualsParallel) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.max_pbo = 0.40;
  const AlphaGate gate{cfg};
  const GateDeflation defl{/*dsr*/ 1.0, /*pbo*/ 0.45, /*split_stable*/ false};
  const GateVerdict serial = gate.admit(m, cand, pool, defl);
  auto fut = std::async(std::launch::async, [&] { return gate.admit(m, cand, pool, defl); });
  EXPECT_EQ(serial, GateVerdict::RejectPbo);
  EXPECT_EQ(fut.get(), serial);
}

// ===========================================================================
//  S1-3 — require_split_stable guard
// ===========================================================================

// (a) off-path: require_split_stable=false (default) => no RejectSplitUnstable
// even when split_stable is false.
TEST(AlphaGateSplit, OffPathFlagFalsePreservesVerdict) {
  const AlphaGate gate{GateConfig{}}; // require_split_stable defaults to false
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  const GateDeflation defl{/*dsr*/ 1.0, /*pbo*/ 0.0, /*split_stable*/ false};
  EXPECT_EQ(gate.admit(m, cand, pool, defl), GateVerdict::Accept);
  EXPECT_EQ(gate.admit(m, cand, pool), GateVerdict::Accept); // default-arg path identical
}

// (b) on-path RED->GREEN: require_split_stable=true, split_stable=false => reject;
// split_stable=true => Accept.
TEST(AlphaGateSplit, OnPathUnstableRejects) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.require_split_stable = true;

  const GateDeflation unstable{/*dsr*/ 1.0, /*pbo*/ 0.0, /*split_stable*/ false};
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, unstable), GateVerdict::RejectSplitUnstable);

  const GateDeflation stable{/*dsr*/ 1.0, /*pbo*/ 0.0, /*split_stable*/ true};
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, stable), GateVerdict::Accept);
}

// sentinel: the S1-0 default split_stable=false is never rejected when the flag is off.
TEST(AlphaGateSplit, InertFlagNeverRejectsUnstable) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg; // require_split_stable=false
  const GateDeflation unstable{/*dsr*/ 1.0, /*pbo*/ 0.0, /*split_stable*/ false};
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, unstable), GateVerdict::Accept);
}

// (c) twice-run.
TEST(AlphaGateSplit, TwiceRunSameVerdict) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.require_split_stable = true;
  const AlphaGate gate{cfg};
  const GateDeflation unstable{/*dsr*/ 1.0, /*pbo*/ 0.0, /*split_stable*/ false};
  const GateVerdict v1 = gate.admit(m, cand, pool, unstable);
  const GateVerdict v2 = gate.admit(m, cand, pool, unstable);
  EXPECT_EQ(v1, GateVerdict::RejectSplitUnstable);
  EXPECT_EQ(v2, v1);
}

// (d) seq==parallel.
TEST(AlphaGateSplit, SeqEqualsParallel) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.require_split_stable = true;
  const AlphaGate gate{cfg};
  const GateDeflation unstable{/*dsr*/ 1.0, /*pbo*/ 0.0, /*split_stable*/ false};
  const GateVerdict serial = gate.admit(m, cand, pool, unstable);
  auto fut = std::async(std::launch::async, [&] { return gate.admit(m, cand, pool, unstable); });
  EXPECT_EQ(serial, GateVerdict::RejectSplitUnstable);
  EXPECT_EQ(fut.get(), serial);
}

// ===========================================================================
//  Fixed-order: DSR is checked before PBO before split before correlation.
// ===========================================================================
TEST(AlphaGateDeflationOrder, DsrBeforePboBeforeSplit) {
  const AlphaMetrics m = passing_metrics();
  const AlphaStore pool;
  const std::vector<f64> cand = cand_pnl();
  GateConfig cfg;
  cfg.min_dsr              = 0.5;
  cfg.max_pbo              = 0.40;
  cfg.require_split_stable = true;

  // Fails all three => the FIRST failing screen (DSR) is the verdict.
  const GateDeflation all_fail{/*dsr*/ 0.3, /*pbo*/ 0.45, /*split_stable*/ false};
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, all_fail), GateVerdict::RejectDsr);

  // Clear DSR only => PBO is next.
  const GateDeflation ok_dsr{/*dsr*/ 0.7, /*pbo*/ 0.45, /*split_stable*/ false};
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, ok_dsr), GateVerdict::RejectPbo);

  // Clear DSR + PBO => split is next.
  const GateDeflation ok_dsr_pbo{/*dsr*/ 0.7, /*pbo*/ 0.30, /*split_stable*/ false};
  EXPECT_EQ(AlphaGate{cfg}.admit(m, cand, pool, ok_dsr_pbo), GateVerdict::RejectSplitUnstable);
}

} // namespace atxtest_gate_dsr_pbo
