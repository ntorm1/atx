// library_verdict_deflation_test.cpp — p8 Sprint 5 (S5-1): wire GateDeflation into
// the LIVE library admission path (Library::verdict_for), closing the p7-S1
// dead-code carry-forward.
//
// Before this unit, AlphaGate::admit (combine/gate.hpp:201-215) screens
// DSR/PBO/split-stable, but Library::verdict_for (the facade every live caller
// routes through via Library::admit / try_admit) never consulted a GateDeflation —
// it ended at the corr screen. This file proves:
//   (1) a low-DSR candidate is REJECTED once GateConfig.min_dsr is set (RED before
//       the wire: verdict_for returned Accept regardless of defl.dsr).
//   (2) the inert defaults (min_dsr=0, max_pbo=1, require_split_stable=false)
//       replay the IDENTICAL admitted set + reject-histogram as pre-S5-1
//       (off-path byte-identity).
//   (3) PBO and split-stable screens fire independently, with the right AdmitKind.
//   (4) the AdmitKind enum's pre-S5 prefix (Accept..RejectDsrSubwindow, 0..7) is
//       frozen — the new enumerators are strictly appended (8, 9, 10).
//   (5) a differential check: verdict_for's DSR/PBO/split verdict class matches
//       AlphaGate::admit's over a seeded candidate batch (the facade now mirrors
//       the gate on the SAME defl scalars).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/combine/gate.hpp"    // AlphaGate, GateConfig, GateDeflation, GateVerdict
#include "atx/engine/combine/metrics.hpp" // AlphaMetrics
#include "atx/engine/library/library.hpp" // the unit under test

namespace atxtest_library_verdict_deflation {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::GateConfig;
using atx::engine::combine::GateDeflation;
using atx::engine::combine::GateVerdict;
using atx::engine::combine::kInertDeflation;

namespace lib = atx::engine::library;

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S5_1") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s5_1_verdict_defl" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

constexpr usize kT = 32; // PnL stream length
constexpr u64 kMasterSeed = 424242;

// A candidate whose floor metrics comfortably clear EVERY pre-existing AlphaGate
// floor (fitness/sharpe/turnover/holding_days) at the default GateConfig, so only
// the NEW deflation screens (and the trailing corr screen, harmless on an empty
// pool) can determine the verdict. Owns the buffers the AlphaCandidate spans alias.
struct CandidateData {
  u64 canon_hash;
  std::vector<f64> pnl;
  std::vector<f64> pos_flat;
  AlphaMetrics metrics;
  lib::Provenance prov;
  usize as_of;
  GateDeflation defl;
};

[[nodiscard]] CandidateData make_candidate(u64 canon_hash, GateDeflation defl) {
  CandidateData c;
  c.canon_hash = canon_hash;
  c.pnl.assign(kT, 0.001); // flat, mildly-positive stream (a fixed 1-instrument book)
  c.pos_flat.assign(kT, 1.0);
  c.metrics.fitness = 2.0;      // clears min_fitness=1.0
  c.metrics.sharpe = 1.0;       // clears min_sharpe=0.25
  c.metrics.turnover = 0.10;    // clears max_turnover=0.70
  c.metrics.holding_days = 100; // clears the inert min_holding_days=0 floor trivially
  c.prov = lib::Provenance{"close", {}, 0, 1};
  c.as_of = 1;
  c.defl = defl;
  return c;
}

[[nodiscard]] lib::AlphaCandidate view_of(const CandidateData &c) {
  return lib::AlphaCandidate{c.canon_hash, c.pnl,   c.pos_flat, c.metrics,
                             c.prov,       c.as_of, nullptr,    c.defl};
}

// ---------------------------------------------------------------------------
// (1) LowDsrRejectedWhenMinDsrSet — RED before the wire (verdict_for ignored
// defl entirely and would have returned Accept regardless of dsr).
// ---------------------------------------------------------------------------
TEST(LibraryVerdict, LowDsrRejectedWhenMinDsrSet) {
  GateConfig cfg;
  cfg.min_dsr = 0.5;
  lib::Library facade = lib::Library::open(tmpdir("low_dsr"), cfg, {kMasterSeed});
  const AlphaGate gate{cfg};

  GateDeflation defl;
  defl.dsr = 0.10; // below the 0.5 bar
  const CandidateData cd = make_candidate(0xAAAAu, defl);
  const lib::AdmitKind kind = facade.admit_verdict_only(view_of(cd), gate);
  EXPECT_EQ(kind, lib::AdmitKind::RejectDsr);
}

// A candidate whose dsr clears the bar is NOT rejected on DSR grounds (still Accept
// on an empty pool with every other floor cleared).
TEST(LibraryVerdict, HighDsrAdmittedWhenMinDsrSet) {
  GateConfig cfg;
  cfg.min_dsr = 0.5;
  lib::Library facade = lib::Library::open(tmpdir("high_dsr"), cfg, {kMasterSeed});
  const AlphaGate gate{cfg};

  GateDeflation defl;
  defl.dsr = 0.9; // clears the 0.5 bar
  const CandidateData cd = make_candidate(0xBBBBu, defl);
  const lib::AdmitKind kind = facade.admit_verdict_only(view_of(cd), gate);
  EXPECT_EQ(kind, lib::AdmitKind::Accept);
}

// ---------------------------------------------------------------------------
// (2) InertDeflation_ByteIdentical — off-path byte-identity: with the GateConfig
// left at its inert default (min_dsr=0, max_pbo=1, require_split_stable=false), a
// pool of candidates (default kInertDeflation, i.e. the exact pre-S5-1 construction
// every existing caller uses) admits IDENTICALLY to a hand-computed pre-S5-1
// expectation (Accept for every floor-clearing candidate — the new branches never
// fire because their guards are all false).
// ---------------------------------------------------------------------------
TEST(LibraryVerdict, InertDeflation_ByteIdentical) {
  const GateConfig cfg{}; // GateConfig{}: min_dsr=0.0, max_pbo=1.0, require_split_stable=false
  const AlphaGate gate{cfg};
  lib::Library facade = lib::Library::open(tmpdir("inert"), cfg, {kMasterSeed});

  for (u64 i = 0; i < 8; ++i) {
    // Default-constructed CandidateData.defl is NOT set here -> exercise the
    // struct's own default member initializer (kInertDeflation) by using the
    // 7-argument legacy AlphaCandidate brace-init (no defl arg at all), proving
    // existing call sites (which never mention .defl) are unaffected.
    CandidateData cd = make_candidate(0x1000u + i, kInertDeflation);
    const lib::AlphaCandidate legacy_view{cd.canon_hash, cd.pnl, cd.pos_flat, cd.metrics,
                                          cd.prov,       cd.as_of, nullptr};
    // legacy_view omits .defl entirely -> must default to kInertDeflation.
    EXPECT_EQ(legacy_view.defl.dsr, kInertDeflation.dsr);
    EXPECT_EQ(legacy_view.defl.pbo, kInertDeflation.pbo);
    EXPECT_EQ(legacy_view.defl.split_stable, kInertDeflation.split_stable);
    const lib::AdmitKind kind = facade.admit_verdict_only(legacy_view, gate);
    EXPECT_EQ(kind, lib::AdmitKind::Accept) << "candidate " << i;
  }
}

// ---------------------------------------------------------------------------
// (3) PboRejectAndSplitReject.
// ---------------------------------------------------------------------------
TEST(LibraryVerdict, PboRejectAndSplitReject) {
  {
    GateConfig cfg;
    cfg.max_pbo = 0.5;
    lib::Library facade = lib::Library::open(tmpdir("pbo_reject"), cfg, {kMasterSeed});
    const AlphaGate gate{cfg};
    GateDeflation defl;
    defl.pbo = 0.9; // above the 0.5 ceiling
    const CandidateData cd = make_candidate(0xCCCCu, defl);
    EXPECT_EQ(facade.admit_verdict_only(view_of(cd), gate), lib::AdmitKind::RejectPbo);
  }
  {
    GateConfig cfg;
    cfg.require_split_stable = true;
    lib::Library facade = lib::Library::open(tmpdir("split_reject"), cfg, {kMasterSeed});
    const AlphaGate gate{cfg};
    GateDeflation defl;
    defl.split_stable = false; // unstable, and the flag requires stability
    const CandidateData cd = make_candidate(0xDDDDu, defl);
    EXPECT_EQ(facade.admit_verdict_only(view_of(cd), gate), lib::AdmitKind::RejectSplitUnstable);
  }
  {
    // A split-stable candidate under require_split_stable=true is NOT rejected.
    GateConfig cfg;
    cfg.require_split_stable = true;
    lib::Library facade = lib::Library::open(tmpdir("split_pass"), cfg, {kMasterSeed});
    const AlphaGate gate{cfg};
    GateDeflation defl;
    defl.split_stable = true;
    const CandidateData cd = make_candidate(0xEEEEu, defl);
    EXPECT_EQ(facade.admit_verdict_only(view_of(cd), gate), lib::AdmitKind::Accept);
  }
}

// ---------------------------------------------------------------------------
// (4) AdmitKindEnumFrozenPrefix — the pre-S5 indices (0..7) are pinned; the new
// enumerators are strictly appended at 8, 9, 10.
// ---------------------------------------------------------------------------
TEST(LibraryVerdict, AdmitKindEnumFrozenPrefix) {
  static_assert(static_cast<int>(lib::AdmitKind::Accept) == 0);
  static_assert(static_cast<int>(lib::AdmitKind::Duplicate) == 1);
  static_assert(static_cast<int>(lib::AdmitKind::RejectSharpe) == 2);
  static_assert(static_cast<int>(lib::AdmitKind::RejectFitness) == 3);
  static_assert(static_cast<int>(lib::AdmitKind::RejectTurnover) == 4);
  static_assert(static_cast<int>(lib::AdmitKind::RejectCorrelated) == 5);
  static_assert(static_cast<int>(lib::AdmitKind::RejectPriceScale) == 6);
  static_assert(static_cast<int>(lib::AdmitKind::RejectDsrSubwindow) == 7);
  static_assert(static_cast<int>(lib::AdmitKind::RejectDsr) == 8);
  static_assert(static_cast<int>(lib::AdmitKind::RejectPbo) == 9);
  static_assert(static_cast<int>(lib::AdmitKind::RejectSplitUnstable) == 10);
  // p8 final-wave (Item 3): RejectRobustness APPENDED at 11 (never inserted mid-enum).
  static_assert(static_cast<int>(lib::AdmitKind::RejectRobustness) == 11);
  SUCCEED();
}

// ---------------------------------------------------------------------------
// (5) Differential test vs AlphaGate::admit — for a seeded candidate batch (a mix
// of dsr/pbo/split values, some deliberately failing, some passing), verdict_for
// (via admit_verdict_only, skipping the dedup gate AlphaGate has no analog for)
// and AlphaGate::admit return the SAME verdict class (Accept vs a specific
// Reject*), i.e. the facade now mirrors the gate's deflation screens exactly.
// ---------------------------------------------------------------------------
TEST(LibraryVerdict, MatchesAlphaGateAdmitAcrossDeflationBranches) {
  GateConfig cfg;
  cfg.min_dsr = 0.4;
  cfg.max_pbo = 0.6;
  cfg.require_split_stable = true;
  const AlphaGate gate{cfg};
  lib::Library facade = lib::Library::open(tmpdir("differential"), cfg, {kMasterSeed});

  struct Case {
    GateDeflation defl;
    bool expect_accept;
  };
  const std::vector<Case> cases = {
      {GateDeflation{/*dsr*/ 0.9, /*pbo*/ 0.1, /*split_stable*/ true}, true},
      {GateDeflation{/*dsr*/ 0.1, /*pbo*/ 0.1, /*split_stable*/ true}, false}, // dsr fails
      {GateDeflation{/*dsr*/ 0.9, /*pbo*/ 0.9, /*split_stable*/ true}, false}, // pbo fails
      {GateDeflation{/*dsr*/ 0.9, /*pbo*/ 0.1, /*split_stable*/ false}, false}, // split fails
  };

  u64 tag = 0x9000;
  for (const Case &c : cases) {
    const CandidateData cd = make_candidate(tag++, c.defl);
    const lib::AlphaCandidate av = view_of(cd);
    const lib::AdmitKind facade_kind = facade.admit_verdict_only(av, gate);
    const GateVerdict gate_verdict = gate.admit(cd.metrics, std::span<const f64>{cd.pnl},
                                                atx::engine::combine::AlphaStore{}, c.defl);
    const bool facade_accept = (facade_kind == lib::AdmitKind::Accept);
    const bool gate_accept = (gate_verdict == GateVerdict::Accept);
    EXPECT_EQ(facade_accept, c.expect_accept);
    EXPECT_EQ(facade_accept, gate_accept)
        << "facade/gate diverged for dsr=" << c.defl.dsr << " pbo=" << c.defl.pbo
        << " split_stable=" << c.defl.split_stable;
  }
}

} // namespace atxtest_library_verdict_deflation
