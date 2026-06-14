// factory_pool_view_test.cpp — S4b-2: the PoolView corr-to-pool seam.
//
// S4b fuses the S3 factory and the S4 library. factory::pool_aware_fitness scores
// a candidate's marginal redundancy = MAX |corr| against the pool. Today it runs
// against an EPHEMERAL combine::AlphaStore with an O(N) scan; the persistent
// library::Library owns an O(neighbors) SimHash corr index that the factory never
// touched. PoolView is the thin seam that lets fitness score against EITHER
// backing with ONE corr impl each (corr_to_pool(..., Max) for the store,
// Library::worst_corr_to_pool for the library). Both return MAX, so they AGREE
// EXACTLY when the SimHash recall is 1.0.
//
// RECALL CAVEAT (the load-bearing fixture choice): the library backing returns the
// MAX |corr| over the candidate's SimHash NEIGHBORS — it equals the AlphaStore's
// exhaustive MAX only when every true argmax neighbor is recalled (recall == 1.0).
// We therefore REUSE the S4-3 / S4-5 ORTHOGONAL, EQUAL-NORM DFT basis pnl fixture:
// each pnl is kDrift + kAmp * (sum of 3 distinct cos/sin basis vectors over the
// 64-point grid). Distinct candidates share at most 2 of 3 basis vectors, so any
// pair's |corr| is in {0, 1/3, 2/3} (NEVER a knife-edge), and SimHash recall on
// this family is measured at 1.00 (S4-3). That makes the AlphaStore-vs-library MAX
// corr EXACTLY equal — a NON-VACUOUS equality (the pool members are genuinely
// correlated to the probe, not a trivially-uncorrelated all-zero pool).
//
// Tests:
//   * LibraryBackedWorstCorrMatchesAlphaStore — the two backings return the same
//     MAX |corr| for a probe against an identical pool (recall == 1.0).
//   * FitnessOverloadAgreesAcrossBackings     — pool_aware_fitness(..., PoolView).
//     .diversify agrees across the AlphaStore and library backings.
//   * EmptyPoolWorstCorrIsZero                — empty library/store => worst_corr 0.

#include <array>      // std::array (basis-triple unranking)
#include <cmath>      // std::cos, std::sin (DFT basis)
#include <cstdint>    // std::uint64_t (LCG)
#include <filesystem> // per-test temp directory (Library catalog)
#include <numbers>    // std::numbers::pi (basis frequencies)
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp" // alpha::Engine

#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/pool_view.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/record.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_factory_pool_view_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::compute_metrics;
using atx::engine::combine::GateConfig;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::AlphaStorePool;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::LibraryPool;
using atx::engine::factory::pool_aware_fitness;

namespace lib = atx::engine::library;

// ---- temp dir (Library catalog) ---------------------------------------------

[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S4b") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s4b_pv" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// ===========================================================================
//  ORTHOGONAL, EQUAL-NORM DFT basis pnl (recall == 1.0) — reused from S4-5.
//
//  62 mutually-orthogonal, equal-norm basis vectors { cos(2πf t/T), sin(2πf t/T)
//  : f = 1..31 } over t = 0..63 (real DFT basis minus DC and Nyquist). Each pnl
//  selects a DISTINCT 3-subset and is the equal-weight sum of those three (plus a
//  drift). Two distinct 3-subsets share at most 2 of 3 vectors, so |corr| in
//  {0, 1/3, 2/3} — never on a knife-edge — and SimHash recall is 1.00 (S4-3). This
//  makes the AlphaStore-vs-library MAX corr EXACTLY equal.
// ===========================================================================
constexpr usize kT = 64;             // PnL stream length (== corr index T)
constexpr usize kN = 1;              // single instrument
constexpr u64 kMasterSeed = 0xB055u; // library corr-index seed
inline constexpr usize kBasisDim = 62;
inline constexpr f64 kPnlDrift = 0.004;
inline constexpr f64 kPnlAmp = 0.0015;
inline constexpr u64 kBasisCombos = 37820; // C(62,3)

[[nodiscard]] inline f64 basis_at(usize b, usize t) {
  const f64 freq = static_cast<f64>(b / 2u + 1u);
  const f64 ang = 2.0 * std::numbers::pi * freq * static_cast<f64>(t) / static_cast<f64>(kT);
  return ((b & 1u) == 0u) ? std::cos(ang) : std::sin(ang);
}

[[nodiscard]] inline std::array<usize, 3> unrank_triple(u64 r) {
  auto choose2 = [](u64 n) -> u64 { return n < 2 ? 0ull : n * (n - 1ull) / 2ull; };
  std::array<usize, 3> out{};
  u64 x = 0;
  while (choose2(static_cast<u64>(kBasisDim) - 1ull - x) <= r) {
    r -= choose2(static_cast<u64>(kBasisDim) - 1ull - x);
    ++x;
  }
  out[0] = static_cast<usize>(x);
  ++x;
  while ((static_cast<u64>(kBasisDim) - 1ull - x) <= r) {
    r -= (static_cast<u64>(kBasisDim) - 1ull - x);
    ++x;
  }
  out[1] = static_cast<usize>(x);
  ++x;
  out[2] = static_cast<usize>(x + r);
  return out;
}

// The deterministic basis pnl for candidate index k (pure in k — L7).
[[nodiscard]] std::vector<f64> basis_pnl(usize k) {
  std::vector<f64> pnl(kT, 0.0);
  const std::array<usize, 3> tri = unrank_triple(static_cast<u64>(k) % kBasisCombos);
  for (usize t = 0; t < kT; ++t) {
    const f64 ac = basis_at(tri[0], t) + basis_at(tri[1], t) + basis_at(tri[2], t);
    pnl[t] = kPnlDrift + kPnlAmp * ac;
  }
  return pnl;
}

// A near-static position cross-section for candidate k (clears the turnover floor;
// only matters for admit-time metrics, never for worst_corr).
[[nodiscard]] std::vector<f64> basis_pos(usize k) {
  std::vector<f64> pos(kT * kN, 0.0);
  for (usize t = 0; t < kT; ++t) {
    pos[t * kN] = 0.10 + 0.001 * static_cast<f64>((t + k) % 3u);
  }
  pos[0] = 0.10;
  return pos;
}

// ---- backings ---------------------------------------------------------------

// An AlphaStore holding the basis pnl of candidates [0, n) (exact MAX-corr ref).
[[nodiscard]] AlphaStore store_of(usize n) {
  AlphaStore store;
  for (usize k = 0; k < n; ++k) {
    const std::vector<f64> pnl = basis_pnl(k);
    const std::vector<f64> pos = basis_pos(k);
    const auto id = store.insert(nullptr, pnl, pos, compute_metrics(pnl, pos, kN, 1.0));
    EXPECT_TRUE(id.has_value());
  }
  return store;
}

// A permissive gate: every basis candidate admits (the gate floors are irrelevant
// to worst_corr_to_pool, which only reads the corr index + store). Using a wide-
// open gate keeps the pool membership of the two backings IDENTICAL by construction.
[[nodiscard]] GateConfig open_gate_cfg() {
  GateConfig g;
  g.min_sharpe = -1.0e18;
  g.min_fitness = -1.0e18;
  g.max_turnover = 1.0e18;
  g.max_pool_corr = 1.0e18;
  return g;
}

// A Library holding the SAME basis pnl of candidates [0, n). Admits each via the
// open gate so the corr index covers exactly the AlphaStore's members.
[[nodiscard]] lib::Library library_of(const std::string &dir, usize n) {
  lib::Library facade = lib::Library::open(dir, open_gate_cfg(), {kMasterSeed});
  const AlphaGate gate{open_gate_cfg()};
  for (usize k = 0; k < n; ++k) {
    const std::vector<f64> pnl = basis_pnl(k);
    const std::vector<f64> pos = basis_pos(k);
    lib::AlphaCandidate c{/*canon_hash*/ 0x9E3779B97F4A7C15ull * static_cast<u64>(k + 1),
                          pnl,
                          pos,
                          compute_metrics(pnl, pos, kN, 1.0),
                          lib::Provenance{},
                          /*as_of*/ 1,
                          /*source*/ nullptr};
    const auto v = facade.admit(c, gate);
    EXPECT_EQ(v.kind, lib::AdmitKind::Accept)
        << "basis candidate " << k << " rejected (kind=" << static_cast<int>(v.kind) << ")";
  }
  return facade;
}

// ---- panel / genome (for the fitness-overload test) -------------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

[[nodiscard]] Genome make_genome(std::string_view src, Library &dsl) {
  auto parsed = parse_expr(src, dsl);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// A tiny deterministic LCG -> uniform(-1, 1) (the S3-4/S3-5 idiom; no RNG dep).
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// A noisy momentum close matrix [dates*insts] (a genuine, finite-Sharpe edge).
[[nodiscard]] std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.004 - 0.0016 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// The candidate's full causal OOS pnl stream over `panel` (alpha 0).
[[nodiscard]] std::vector<f64> full_pnl_of(const Genome &g, const Panel &panel,
                                           const WeightPolicy &policy,
                                           const ExecutionSimulator &sim) {
  auto prog = atx::engine::alpha::compile(g.ast, g.analysis);
  EXPECT_TRUE(prog.has_value());
  atx::engine::alpha::Engine engine{panel};
  auto ss = engine.evaluate(*prog);
  EXPECT_TRUE(ss.has_value());
  auto strm = atx::engine::alpha::extract_streams(*ss, policy, panel, sim);
  EXPECT_TRUE(strm.has_value());
  const std::span<const f64> p = strm->pnl(0);
  return std::vector<f64>(p.begin(), p.end());
}

// Stage one explicit pnl stream (the candidate's OWN OOS stream) into a Library so
// corr-to-pool is ~1.0 — a high corr the SimHash recalls reliably. The position
// stream is near-static (clears the open gate); metrics come from compute_metrics.
[[nodiscard]] lib::Library library_with_pnl(const std::string &dir, const std::vector<f64> &pnl) {
  lib::Library facade = lib::Library::open(dir, open_gate_cfg(), {kMasterSeed});
  const AlphaGate gate{open_gate_cfg()};
  std::vector<f64> pos(pnl.size() * kN, 0.0);
  for (usize t = 0; t < pnl.size(); ++t) {
    pos[t * kN] = 0.10; // static -> ~zero turnover
  }
  lib::AlphaCandidate c{/*canon_hash*/ 0x1234567u,
                        pnl,
                        pos,
                        compute_metrics(pnl, pos, kN, 1.0),
                        lib::Provenance{},
                        /*as_of*/ 1,
                        /*source*/ nullptr};
  EXPECT_EQ(facade.admit(c, gate).kind, lib::AdmitKind::Accept);
  return facade;
}

[[nodiscard]] AlphaStore store_with_pnl(const std::vector<f64> &pnl) {
  AlphaStore store;
  const std::vector<f64> pos(pnl.size() * kN, 0.0);
  const auto id = store.insert(nullptr, pnl, pos, compute_metrics(pnl, pos, kN, 1.0));
  EXPECT_TRUE(id.has_value());
  return store;
}

// =============================================================================
//  LibraryBackedWorstCorrMatchesAlphaStore — the two backings agree (recall 1.0).
// =============================================================================
TEST(FactoryPoolView, LibraryBackedWorstCorrMatchesAlphaStore) {
  constexpr usize kPool = 32;
  const AlphaStore store = store_of(kPool);
  const lib::Library library = library_of(tmpdir("rt"), kPool);
  ASSERT_EQ(static_cast<usize>(library.n_alphas()), kPool);

  const AlphaStorePool sp{store};
  const LibraryPool lp{library};

  // Probe with several basis combinations: each is genuinely correlated to some
  // pool members (|corr| in {0, 1/3, 2/3}) so the MAX is non-trivial, and is itself
  // OUTSIDE the pool (k >= kPool) — the differential is whether the SimHash recalls
  // the true argmax neighbor, not a trivial self-match.
  for (usize k = kPool; k < kPool + 8; ++k) {
    const std::vector<f64> probe = basis_pnl(k);
    const f64 exact = sp.worst_corr(probe);
    const f64 incremental = lp.worst_corr(probe);
    EXPECT_NEAR(exact, incremental, 1e-12)
        << "probe k=" << k << " AlphaStore MAX=" << exact << " library MAX=" << incremental;
  }

  // A probe that IS a near-duplicate of pool member 0 (corr ~ 1.0) — both backings
  // must surface the high MAX (and agree). Adds a non-{0,1/3,2/3} corr value.
  std::vector<f64> dup = basis_pnl(0);
  for (usize t = 0; t < kT; ++t) {
    dup[t] += 1.0e-6; // a tiny constant shift (corr is shift-invariant -> still ~1)
  }
  EXPECT_NEAR(sp.worst_corr(dup), lp.worst_corr(dup), 1e-12);
  EXPECT_GT(sp.worst_corr(dup), 0.99) << "near-duplicate must read MAX |corr| ~ 1";
}

// =============================================================================
//  FitnessOverloadAgreesAcrossBackings — pool_aware_fitness(..., PoolView).diversify
//  is identical across the AlphaStore and library backings (same redundancy).
// =============================================================================
TEST(FactoryPoolView, FitnessOverloadAgreesAcrossBackings) {
  constexpr usize kDates = 96;
  constexpr usize kInsts = 6;
  Library dsl;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();

  const Panel panel =
      make_panel(kDates, kInsts, {"close"}, {noisy_close(kDates, kInsts, /*seed*/ 0xC0FFEEu)});
  const Genome cand = make_genome("rank(close)", dsl);

  // Pool member = the candidate's OWN realized OOS pnl, so MAX |corr-to-pool| ~ 1.0
  // (a high corr the SimHash reliably recalls). Both backings hold this one stream.
  const std::vector<f64> own = full_pnl_of(cand, panel, policy, sim);
  ASSERT_EQ(own.size(), kDates) << "candidate OOS pnl length must equal n_dates";

  const AlphaStore store = store_with_pnl(own);
  const lib::Library library = library_with_pnl(tmpdir("fit"), own);

  const AlphaStorePool sp{store};
  const LibraryPool lp{library};

  const FitnessCfg cfg;
  const auto fa = pool_aware_fitness(cand, sp, panel, policy, sim, cfg);
  const auto fb = pool_aware_fitness(cand, lp, panel, policy, sim, cfg);
  ASSERT_TRUE(fa.has_value()) << (fa ? "" : fa.error().message());
  ASSERT_TRUE(fb.has_value()) << (fb ? "" : fb.error().message());

  // Same candidate, same pool member, MAX corr served by both backings: the
  // diversification discount (and hence redundancy/raw) must match within the
  // recall == 1.0 caveat.
  EXPECT_NEAR(fa->redundancy, fb->redundancy, 1e-12);
  EXPECT_NEAR(fa->diversify, fb->diversify, 1e-12);
  EXPECT_NEAR(fa->raw, fb->raw, 1e-12);
  // Non-vacuous: the candidate is genuinely redundant with the pool (corr ~ 1 ->
  // diversify ~ 0), so this is not a trivial 0-vs-0 agreement.
  EXPECT_GT(fa->redundancy, 0.9) << "candidate must be redundant with its own pnl";
}

// =============================================================================
//  EmptyPoolWorstCorrIsZero — empty library/store => worst_corr 0 (the empty-pool
//  gate convention; diversify = 1).
// =============================================================================
TEST(FactoryPoolView, EmptyPoolWorstCorrIsZero) {
  const AlphaStore empty_store;            // no members
  const lib::Library empty_lib = lib::Library::open(tmpdir("empty"), open_gate_cfg(), {kMasterSeed});
  ASSERT_EQ(static_cast<usize>(empty_lib.n_alphas()), 0u);

  const std::vector<f64> probe = basis_pnl(0);
  EXPECT_NEAR(AlphaStorePool{empty_store}.worst_corr(probe), 0.0, 1e-12);
  EXPECT_NEAR(LibraryPool{empty_lib}.worst_corr(probe), 0.0, 1e-12);
}


}  // namespace atxtest_factory_pool_view_test
