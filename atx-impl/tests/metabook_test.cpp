// metabook_test.cpp — p8 Sprint 2: stage_metabook (the fund:: mega-alpha layer wired into
// atx-impl). S2-0/S2-1/S2-2/S2-4/S2-5 land their accept tests here (S2-3's netting-specific
// tests live in metabook_netting_test.cpp per the sprint's test-home split).
//
// S2-0: config-surface + FROZEN-signature confirmation only (no stage behavior yet).
// S2-1: assign_sleeves -- the admitted-alpha -> N-sleeve partition seam.
// S2-2: build_metabook_result / run_metabook -- the two-pass drive producer + the R7
// stage-boundary pin (SingleSleeve, no --library-dir, == stage_optimize's book).

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/fund/meta_allocator.hpp"
#include "atx/engine/library/library.hpp"

#include "serialize_panel.hpp"
#include "stage_metabook.hpp"

namespace atxtest_metabook_test {

using atx::impl::MetaBookStageConfig;
using atx::impl::SleeveAssignment;

namespace lib = atx::engine::library;
namespace combine = atx::engine::combine;

// ===========================================================================
//  S2-1 fixture helpers — a small on-disk library::Library with admitted synthetic alphas.
// ===========================================================================
namespace {

constexpr atx::usize kPnlT = 64; // PnL series length shared by every fixture alpha

[[nodiscard]] std::string tmp_lib_dir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::string base = std::string(info != nullptr ? info->test_suite_name() : "S2") + "_" +
                           std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s2_metabook" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// A permissive gate so synthetic fixture PnL is admitted regardless of its Sharpe/fitness,
// and max_pool_corr is effectively OFF (>1.0, above any real correlation value) so
// deliberately-correlated cluster fixtures are not rejected by the library's own corr gate
// (a genuine ByCorrCluster test NEEDS highly-correlated admitted alphas within a cluster --
// the default max_pool_corr=0.7 would reject the 2nd/3rd member of such a cluster).
[[nodiscard]] combine::GateConfig permissive_gate_cfg() {
  combine::GateConfig g;
  g.min_sharpe = -1e9;
  g.min_fitness = -1e9;
  g.max_turnover = 1e9;
  g.max_pool_corr = 10.0;
  return g;
}

// Orthonormal-equal-norm DFT basis vector (matches library_integration_test.cpp's proven
// construction): b even -> cos, odd -> sin, of frequency f = b/2+1, over kPnlT samples. Two
// DISJOINT index sets from this family are EXACTLY orthogonal (standard DFT orthogonality)
// and each vector is exactly zero-mean over the full kPnlT-sample period -- so a PnL series
// built from one basis index never correlates with a series built from a DISJOINT index (an
// EXACT, computed 0.0 correlation, not an empirical approximation).
[[nodiscard]] atx::f64 basis_at(atx::usize b, atx::usize t) {
  const atx::f64 freq = static_cast<atx::f64>(b / 2U + 1U);
  const atx::f64 ang =
      2.0 * std::numbers::pi * freq * static_cast<atx::f64>(t) / static_cast<atx::f64>(kPnlT);
  return ((b & 1U) == 0U) ? std::cos(ang) : std::sin(ang);
}

// A PnL series driven by ONE basis index (small drift for a nonzero Sharpe; the permissive
// gate does not require it, but a nonzero, non-vacuous series is good hygiene).
[[nodiscard]] std::vector<atx::f64> basis_pnl(atx::usize basis_idx) {
  std::vector<atx::f64> v(kPnlT);
  for (atx::usize t = 0; t < kPnlT; ++t) {
    v[t] = 0.01 + 0.05 * basis_at(basis_idx, t);
  }
  return v;
}

// Admit one alpha with the given PnL series + DSL expr_source into `facade` under a
// permissive gate. pos_flat is a trivial constant single-instrument position stream (small,
// near-static -> negligible turnover). ASSERT-fails (via gtest macros) if admission is
// rejected -- a fixture that cannot admit is a broken fixture, not a legitimate test case.
void admit_pnl(lib::Library &facade, std::span<const atx::f64> pnl, const std::string &expr_source,
               atx::u64 canon_hash) {
  static const combine::AlphaGate kGate{permissive_gate_cfg()};
  std::vector<atx::f64> pos(pnl.size(), 0.10);
  const combine::AlphaMetrics m = combine::compute_metrics(pnl, pos, /*n_instruments*/ 1, /*book*/ 1.0);
  const lib::Provenance prov{expr_source, {}, 0, canon_hash};
  const lib::AlphaCandidate cand{canon_hash, pnl, std::span<const atx::f64>{pos}, m, prov,
                                 /*as_of*/ 1, nullptr};
  const auto v = facade.admit(cand, kGate);
  ASSERT_EQ(v.kind, lib::AdmitKind::Accept)
      << "fixture admit rejected (kind=" << static_cast<int>(v.kind) << "): " << expr_source;
}

// A 6-alpha library, each alpha's PnL driven by a DISTINCT basis index (pairwise near-
// orthogonal/decorrelated; no cluster structure) -- used by the SingleSleeve whole-set test.
[[nodiscard]] lib::Library make_flat_library(const std::string &tag) {
  lib::Library facade = lib::Library::open(tmp_lib_dir(tag), lib::GateConfig{}, {/*seeds*/});
  for (atx::usize k = 0; k < 6; ++k) {
    const auto pnl = basis_pnl(k);
    admit_pnl(facade, pnl, "ts_rank(field_" + std::to_string(k) + ")", 1000ULL + k);
  }
  return facade;
}

// A 6-alpha library with TWO constructed correlation clusters (3 alphas each): cluster A's
// members all share basis index 0 (corr == 1.0 within-cluster, exactly); cluster B's members
// all share basis index 10 -- DISJOINT from {0}, hence EXACTLY orthogonal / 0-correlated
// across clusters. (Identical PnL across members of one cluster is legal here: the library's
// dedup gate keys on the DSL canonical hash, not PnL content, so distinct canon_hash/expr per
// member is a genuinely distinct admitted alpha despite the shared PnL shape.)
[[nodiscard]] lib::Library make_two_cluster_library(const std::string &tag) {
  lib::Library facade = lib::Library::open(tmp_lib_dir(tag), lib::GateConfig{}, {/*seeds*/});
  const auto pnl_a = basis_pnl(0);
  const auto pnl_b = basis_pnl(10);
  for (atx::usize k = 0; k < 3; ++k) {
    admit_pnl(facade, pnl_a, "ts_rank(momentum_" + std::to_string(k) + ")", 2000ULL + k);
  }
  for (atx::usize k = 0; k < 3; ++k) {
    admit_pnl(facade, pnl_b, "cs_rank(reversal_" + std::to_string(k) + ")", 3000ULL + k);
  }
  return facade;
}

[[nodiscard]] bool has_alpha(const std::vector<lib::AlphaId> &members, atx::u32 id) {
  for (const auto &m : members) {
    if (m.value == id) {
      return true;
    }
  }
  return false;
}

// ===========================================================================
//  S2-2 fixture helpers — small research/combo panels (mirrors optimize_test.cpp's
//  make_research_panel / make_combo_panel exactly, so the R7 stage-boundary comparison is
//  apples-to-apples with run_optimize's own fixture shape).
// ===========================================================================

namespace alpha = atx::engine::alpha;

[[nodiscard]] atx::core::Result<std::string> make_research_panel_mb(const std::filesystem::path &out,
                                                                    atx::usize M, atx::usize D) {
  std::vector<atx::f64> close_data;
  close_data.reserve(D * M);
  for (atx::usize t = 0; t < D; ++t) {
    for (atx::usize i = 0; i < M; ++i) {
      const atx::f64 drift = 0.0002 * (1.0 + static_cast<atx::f64>(i) * 0.1);
      close_data.push_back(100.0 * std::exp(drift * static_cast<atx::f64>(t)));
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1U);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close"}, {close_data}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

[[nodiscard]] atx::core::Result<std::string> make_combo_panel_mb(const std::filesystem::path &out,
                                                                 atx::usize M, atx::usize D) {
  std::vector<atx::f64> alpha_data;
  alpha_data.reserve(D * M);
  for (atx::usize t = 0; t < D; ++t) {
    const atx::f64 wobble = 0.01 * static_cast<atx::f64>(t % 5);
    for (atx::usize i = 0; i < M; ++i) {
      alpha_data.push_back((static_cast<atx::f64>(i) - static_cast<atx::f64>(M) / 2.0) + wobble);
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1U);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

[[nodiscard]] std::string tmp_dir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::string base = std::string(info != nullptr ? info->test_suite_name() : "S2") + "_" +
                           std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s2_metabook_stage" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

} // namespace

// SleeveAssignment::SingleSleeve MUST be 0 -- the inert R7-pin default (also enforced by a
// static_assert in stage_metabook.hpp; re-checked here as a runtime regression net).
TEST(MetabookConfig, SingleSleeveIsZero) {
  EXPECT_EQ(static_cast<atx::u8>(SleeveAssignment::SingleSleeve), 0U);
}

// MetaBookStageConfig default-constructs to SingleSleeve + the engine MetaBookConfig's own
// defaults (untouched) + the stage's own gross/name_cap/risk_aversion defaults (1.0 each,
// mirroring RunConfig's resolved gross_val/name_cap_val/risk_aversion in stage_optimize.cpp).
TEST(MetabookConfig, DefaultsAreInert) {
  const MetaBookStageConfig cfg;
  EXPECT_EQ(cfg.assignment, SleeveAssignment::SingleSleeve);
  EXPECT_EQ(cfg.max_sleeves, 8U);
  EXPECT_DOUBLE_EQ(cfg.gross, 1.0);
  EXPECT_DOUBLE_EQ(cfg.name_cap, 1.0);
  EXPECT_DOUBLE_EQ(cfg.risk_aversion, 1.0);

  // The wrapped engine MetaBookConfig's own defaults (meta_allocator.hpp / meta_book.hpp),
  // untouched by S2-0 -- confirms the field is plumbed by VALUE, not reinterpreted.
  EXPECT_EQ(cfg.meta.alloc.method, atx::engine::fund::RiskBudgetMethod::EqualRiskContribution);
  EXPECT_DOUBLE_EQ(cfg.meta.alloc.fractional_kelly, 0.3);
  EXPECT_DOUBLE_EQ(cfg.meta.alloc.target_vol, 0.0);
  EXPECT_DOUBLE_EQ(cfg.meta.alloc.max_gross, 4.0);
  EXPECT_EQ(cfg.meta.alloc.solve_iters, 64U);
  EXPECT_EQ(cfg.meta.risk_lookback, 60U);
}

// ===========================================================================
//  S2-1 — assign_sleeves accept tests.
// ===========================================================================

// SingleSleeve over a 6-alpha fixture library yields exactly ONE SleeveConfig whose
// `members` are [0..5] ascending.
TEST(MetabookAssignSleeves, SingleSleeveIsWholeSet) {
  const lib::Library facade = make_flat_library("single_whole_set");
  MetaBookStageConfig cfg;
  cfg.assignment = SleeveAssignment::SingleSleeve;

  auto got = atx::impl::assign_sleeves(facade, cfg);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->size(), 1U);
  ASSERT_EQ((*got)[0].members.size(), 6U);
  for (atx::u32 i = 0; i < 6U; ++i) {
    EXPECT_EQ((*got)[0].members[i].value, i) << "member " << i << " out of ascending order";
  }
}

// ByCorrCluster on a fixture with two constructed correlation clusters (three alphas each)
// yields exactly 2 sleeves with the expected memberships, and a second call yields
// byte-identical members (stable tie-break / twice-run determinism).
TEST(MetabookAssignSleeves, CorrClusterDeterministic) {
  const lib::Library facade = make_two_cluster_library("corr_cluster");
  MetaBookStageConfig cfg;
  cfg.assignment = SleeveAssignment::ByCorrCluster;
  cfg.max_sleeves = 8U;

  auto got1 = atx::impl::assign_sleeves(facade, cfg);
  ASSERT_TRUE(got1.has_value()) << (got1 ? "" : got1.error().to_string());
  ASSERT_EQ(got1->size(), 2U) << "expected exactly 2 clusters from the constructed fixture";

  // Each sleeve must be EXACTLY one of the two constructed 3-member clusters {0,1,2} /
  // {3,4,5} -- order between sleeves is not asserted (canonical min-AlphaId ascending, but
  // the important invariant is membership correctness), only membership + no cross-mixing.
  for (const auto &sleeve : *got1) {
    ASSERT_EQ(sleeve.members.size(), 3U);
    const bool is_cluster_a = has_alpha(sleeve.members, 0) && has_alpha(sleeve.members, 1) &&
                              has_alpha(sleeve.members, 2);
    const bool is_cluster_b = has_alpha(sleeve.members, 3) && has_alpha(sleeve.members, 4) &&
                              has_alpha(sleeve.members, 5);
    EXPECT_TRUE(is_cluster_a || is_cluster_b) << "sleeve mixed the two constructed clusters";
  }

  // Twice-run: identical (lib, cfg) -> byte-identical SleeveConfig vector (member lists).
  auto got2 = atx::impl::assign_sleeves(facade, cfg);
  ASSERT_TRUE(got2.has_value()) << (got2 ? "" : got2.error().to_string());
  ASSERT_EQ(got1->size(), got2->size());
  for (atx::usize s = 0; s < got1->size(); ++s) {
    ASSERT_EQ((*got1)[s].members.size(), (*got2)[s].members.size()) << "sleeve " << s;
    for (atx::usize i = 0; i < (*got1)[s].members.size(); ++i) {
      EXPECT_EQ((*got1)[s].members[i].value, (*got2)[s].members[i].value)
          << "sleeve " << s << " member " << i << " differs across runs";
    }
  }
}

// A library where a mode would produce a single non-empty group (here: ByLibraryGroup on a
// small, never-explicitly-flushed library -- every alpha is still in the unflushed memtable,
// so segment_crc_per_alpha reports one crc==0 group for all of them) falls back to
// SingleSleeve: documented Ok, NOT an error.
TEST(MetabookAssignSleeves, DegenerateFallsBackToSingleSleeve) {
  const lib::Library facade = make_flat_library("degenerate_fallback");
  MetaBookStageConfig cfg;
  cfg.assignment = SleeveAssignment::ByLibraryGroup;

  auto got = atx::impl::assign_sleeves(facade, cfg);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->size(), 1U) << "ByLibraryGroup on an unsealed library must fall back to one sleeve";
  EXPECT_EQ((*got)[0].members.size(), 6U);
}

// Empty library -> Err(InvalidArgument), not a vacuous empty sleeve vector.
TEST(MetabookAssignSleeves, EmptyLibraryIsErr) {
  const lib::Library facade = lib::Library::open(tmp_lib_dir("empty"), lib::GateConfig{}, {});
  const MetaBookStageConfig cfg;
  auto got = atx::impl::assign_sleeves(facade, cfg);
  ASSERT_FALSE(got.has_value());
}

// ===========================================================================
//  S2-2 — the R7 STAGE-BOUNDARY pin: run_metabook(SingleSleeve, no --library-dir) produces
//  the byte-identical book run_optimize produces on the SAME research+combo panel. This is
//  the empirical half of the R7 claim (b) the ledger's composed-proof argues for; the engine
//  half (MetaBook one-sleeve == MultiPeriodOptimizer) is pinned in
//  atx-engine/tests/fund/fund_metabook_wire_test.cpp's SingleSleeveByteIdenticalTo
//  MultiPeriodOptimizer.
// ===========================================================================
TEST(MetabookStageBoundary, SingleSleeveByteIdenticalToStageOptimizeBook) {
  const std::string dir = tmp_dir("boundary");
  constexpr atx::usize kMi = 6;
  constexpr atx::usize kDi = 20;

  const auto research_r = make_research_panel_mb(std::filesystem::path(dir) / "research.bin", kMi, kDi);
  ASSERT_TRUE(research_r.has_value()) << (research_r ? "" : research_r.error().message());
  const auto combo_r = make_combo_panel_mb(std::filesystem::path(dir) / "combo.bin", kMi, kDi);
  ASSERT_TRUE(combo_r.has_value()) << (combo_r ? "" : combo_r.error().message());

  atx::impl::RunConfig cfg;
  cfg.panel = *research_r;
  cfg.combo = *combo_r;
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.books_out = (std::filesystem::path(dir) / "optimize_books.bin").string();

  auto opt_result = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(opt_result.has_value()) << (opt_result ? "" : opt_result.error().message());
  auto opt_books = atx::impl::read_panel(cfg.books_out);
  ASSERT_TRUE(opt_books.has_value()) << (opt_books ? "" : opt_books.error().message());

  const MetaBookStageConfig scfg; // default: SingleSleeve, cfg.library_dir left empty
  cfg.books_out = (std::filesystem::path(dir) / "metabook_books.bin").string();
  auto mb_result = atx::impl::run_metabook(cfg, scfg);
  ASSERT_TRUE(mb_result.has_value()) << (mb_result ? "" : mb_result.error().message());
  auto mb_books = atx::impl::read_panel(cfg.books_out);
  ASSERT_TRUE(mb_books.has_value()) << (mb_books ? "" : mb_books.error().message());

  ASSERT_EQ(opt_books->dates(), mb_books->dates());
  ASSERT_EQ(opt_books->instruments(), mb_books->instruments());
  const auto opt_wid = opt_books->field_id("weight");
  const auto mb_wid = mb_books->field_id("weight");
  ASSERT_TRUE(opt_wid.has_value());
  ASSERT_TRUE(mb_wid.has_value());

  bool some_nonzero = false;
  for (atx::usize s = 0; s < opt_books->dates(); ++s) {
    const auto ows = opt_books->field_cross_section(*opt_wid, s);
    const auto mws = mb_books->field_cross_section(*mb_wid, s);
    ASSERT_EQ(ows.size(), mws.size());
    for (atx::usize i = 0; i < ows.size(); ++i) {
      if (ows[i] != 0.0) {
        some_nonzero = true;
      }
      EXPECT_EQ(std::bit_cast<std::uint64_t>(ows[i]), std::bit_cast<std::uint64_t>(mws[i]))
          << "R7 stage-boundary BYTE DIVERGENCE period " << s << " name " << i;
    }
  }
  EXPECT_TRUE(some_nonzero) << "R7 vacuous: the pinned books are all zero";
  EXPECT_EQ(opt_result->digest, mb_result->digest) << "R7 stage-boundary digest diverged";
}

} // namespace atxtest_metabook_test
