// risk_dead_factor_test.cpp — S7-3: dead-alpha → risk-factor extraction (Kakushadze).
//
// The S7-3 numeric path recycles RETIRED (dead) alphas into the risk model as
// orthogonal risk FACTORS so the operating book stops re-loading on directions that
// have already decayed (Kakushadze & Yu, arXiv:1709.06641):
//   extract_dead_factors(lib, dead_ids, as_of, M): build the M×M holdings-overlap
//     X_AB = Σ_{i∈dead} P_iA P_iB over the L1-normalized dead holdings, symmetric-
//     eigendecompose, FIX the sign (largest-|component| positive), truncate to the
//     effective rank, emit the kept loadings + eigenvalues (the dead variances).
//   augment_factor_model(base, dead): V_aug = [X|X_dead]·blockdiag(F,diag(var))·[.]ᵀ
//     + D, reusing FactorModel::create.
//   effective_rank(evals): round(exp(Shannon entropy of the normalized spectrum)).
//
// Coverage:
//   * DeadFactor.ExtractsFactorsFromDeadHoldings  — n_dead=12, M=16; shapes line up.
//   * DeadFactor.EmptyDeadSetYieldsNoFactors      — boundary: k_dead == 0.
//   * DeadFactor.SignConventionIsReproducible     — two extracts BIT-identical (R1).
//   * DeadFactor.AugmentedModelReducesDeadSubspaceExposure (LOAD-BEARING, R6):
//     raising the dead direction's variance STRICTLY reduces ‖loadingsᵀ w‖₂.
//   * DeadFactorBuild.BuildComponentsMatchesBuild — the §0.3 refactor is behavior-
//     preserving (build == build_components + create).

#include <cmath>        // std::cos, std::isfinite
#include <filesystem>   // per-test temp directory
#include <limits>       // std::numeric_limits (PanelFixture NaN fill)
#include <memory>       // std::unique_ptr (DeadLibFixture owns the Library)
#include <numbers>      // std::numbers::pi
#include <span>
#include <string>
#include <system_error> // std::error_code (filesystem remove_all/create_directories)
#include <utility>      // std::move (DeadCandidate / fixture moves)
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense> // Eigen::Index, Eigen::Map (covers <Eigen/Core>)

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/combine/gate.hpp"      // AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp"   // AlphaMetrics
#include "atx/engine/combine/store.hpp"     // combine::AlphaId
#include "atx/engine/library/library.hpp"   // library::Library, AlphaCandidate
#include "atx/engine/library/lifecycle.hpp" // LifecycleState
#include "atx/engine/library/record.hpp"    // Provenance

#include "atx/engine/loop/panel_types.hpp"  // PanelView, PanelField (BuildComponents fixture)
#include "atx/engine/loop/types.hpp"        // InstrumentId
#include "atx/engine/risk/dead_factor.hpp"  // the unit under test
#include "atx/engine/risk/factor_model.hpp" // FactorModel, FactorComponents, FactorModelBuilder
#include "atx/engine/risk/optimizer.hpp"    // PortfolioOptimizer, OptimizerConfig

namespace atxtest_risk_dead_factor_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::GateConfig;
using atx::engine::library::AdmitKind;
using atx::engine::library::LifecycleState;
using atx::engine::risk::augment_factor_model;
using atx::engine::risk::DeadAlphaFactors;
using atx::engine::risk::effective_rank;
using atx::engine::risk::extract_dead_factors;
using atx::engine::risk::FactorComponents;
using atx::engine::risk::FactorModel;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PortfolioOptimizer;

namespace lib = atx::engine::library;

[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S7") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s7_deadfac" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec); // fresh per construction
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// A PERMISSIVE gate so every structured dead-holdings candidate is admitted: this
// unit exercises the POSITIONS path (the dead holdings cross-section), not the gate.
// The dead alphas deliberately share a low-frequency holdings shape (so the overlap
// matrix has structure), which also makes their pnl streams correlated — we widen
// max_pool_corr past 1.0 and drop the sharpe/fitness floors so the corr / floor
// gates never reject a well-formed dead candidate.
[[nodiscard]] GateConfig permissive_gate_cfg() {
  GateConfig cfg;
  cfg.min_sharpe = -1e9;
  cfg.min_fitness = -1e9;
  cfg.max_turnover = 1e9;
  cfg.max_pool_corr = 1.1; // never reject on correlation (corr <= 1)
  return cfg;
}

// A metrics value that comfortably clears the default gate (sharpe>=1, fitness>=1,
// turnover<=0.7). We do not exercise the gate here — admit() must Accept so the
// alpha lands in the store and can be marked Dead.
[[nodiscard]] AlphaMetrics passing_metrics() {
  AlphaMetrics m{};
  m.sharpe = 5.0;
  m.turnover = 0.05;
  m.returns = 1.0;
  m.drawdown = 0.1;
  m.margin = 10.0;
  m.fitness = 5.0;
  m.holding_days = 20.0;
  return m;
}

// Owns the buffers an AlphaCandidate spans (the candidate spans must outlive the
// admit() call — §0.3). A single-period (T=1) holdings cross-section of length M is
// the dead alpha's holdings vector; pnl is a benign T=1 stream.
struct DeadCandidate {
  std::vector<f64> pnl;       // length T (== n_periods)
  std::vector<f64> pos_flat;  // length T*M (period-major)
  AlphaMetrics metrics;
  lib::Provenance prov;
};

// ===========================================================================
//  library_with_dead_alphas(n_dead, M): admit n_dead alphas, each carrying a KNOWN
//  holdings cross-section so the overlap matrix has REAL structure, then walk each
//  through the legal lifecycle chain Admitted -> Live -> Decaying -> Dead and return
//  the (Library, dead AlphaIds, as_of). The holdings concentrate on a few
//  instruments (a structured, not-isotropic dead pool) so the eigenspectrum has a
//  genuine effective rank > 1. T == 2 periods (admit needs a >= 1-period pnl; the
//  positions at the as_of period carry the holdings).
// ===========================================================================
struct DeadLibFixture {
  std::unique_ptr<lib::Library> lib;
  std::vector<AlphaId> dead_ids;
  usize as_of = 0U;
  usize m = 0U;
};

// A holdings pattern for dead alpha k over M instruments: a smooth low-frequency
// shape (a cosine bump centered at a k-dependent instrument) so distinct dead
// alphas overlap PARTIALLY — the overlap matrix then has a non-trivial spectrum
// (effective rank strictly between 1 and M). Deterministic in k.
[[nodiscard]] std::vector<f64> dead_holdings(usize k, usize m) {
  std::vector<f64> h(m, 0.0);
  const f64 center = static_cast<f64>(k % m);
  for (usize i = 0; i < m; ++i) {
    const f64 d = (static_cast<f64>(i) - center) / static_cast<f64>(m);
    const f64 w = std::cos(std::numbers::pi * d); // smooth bump, sign-varying
    h[i] = w;
  }
  return h;
}

[[nodiscard]] DeadLibFixture library_with_dead_alphas(usize n_dead, usize m) {
  DeadLibFixture fx;
  fx.m = m;
  fx.as_of = 1U; // the period whose holdings we read in extract_dead_factors
  const std::string dir = tmpdir(std::to_string(n_dead) + "_" + std::to_string(m));
  fx.lib = std::make_unique<lib::Library>(
      lib::Library::open(dir, permissive_gate_cfg(), {/*master seed*/ 4242ULL}));
  const AlphaGate gate{permissive_gate_cfg()};

  constexpr usize kT = 2U; // 2 periods: holdings at period 1 carry the dead direction
  std::vector<DeadCandidate> owners;
  owners.reserve(n_dead);
  std::vector<AlphaId> ids;
  ids.reserve(n_dead);

  for (usize k = 0; k < n_dead; ++k) {
    DeadCandidate c;
    c.pnl.assign(kT, 0.0);
    c.pnl[1] = 0.01 + 0.0001 * static_cast<f64>(k); // benign non-degenerate pnl
    c.pos_flat.assign(kT * m, 0.0);
    const std::vector<f64> h = dead_holdings(k, m);
    for (usize i = 0; i < m; ++i) {
      c.pos_flat[fx.as_of * m + i] = h[i]; // holdings at period as_of (period 1)
      c.pos_flat[0 * m + i] = 0.0;         // flat at period 0
    }
    c.metrics = passing_metrics();
    c.prov = lib::Provenance{"dead", std::vector<atx::u64>{}, /*op*/ 0, /*seed*/ 100 + k};
    owners.push_back(std::move(c));
  }

  for (usize k = 0; k < n_dead; ++k) {
    const DeadCandidate &c = owners[k];
    const lib::AlphaCandidate cand{/*canon_hash*/ 0x100ULL + k, c.pnl,    c.pos_flat,
                                   c.metrics,                   c.prov,   /*as_of*/ 0U,
                                   /*source*/ nullptr};
    const auto v = fx.lib->admit(cand, gate);
    EXPECT_EQ(v.kind, AdmitKind::Accept) << "dead candidate " << k << " not admitted";
    ids.push_back(v.id);
  }

  // Walk every admitted alpha down the legal lifecycle spine to Dead, ASCENDING by
  // AlphaId so extract_dead_factors' order-fixed accumulation matches.
  for (const AlphaId id : ids) {
    EXPECT_TRUE(fx.lib->mark(id, LifecycleState::Live, /*as_of*/ 2U).has_value());
    EXPECT_TRUE(fx.lib->mark(id, LifecycleState::Decaying, /*as_of*/ 3U).has_value());
    EXPECT_TRUE(fx.lib->mark(id, LifecycleState::Dead, /*as_of*/ 4U).has_value());
    // Confirm the alpha is Dead as of period 4 (PIT).
    const auto st = fx.lib->state_as_of(id, 4U);
    EXPECT_TRUE(st.has_value() && *st == LifecycleState::Dead);
  }
  fx.dead_ids = std::move(ids);
  return fx;
}

// ===========================================================================
//  #1 ExtractsFactorsFromDeadHoldings — shapes line up; k_dead in [1, M].
// ===========================================================================
TEST(DeadFactor, ExtractsFactorsFromDeadHoldings) {
  const usize n_dead = 12U;
  const usize m = 16U;
  DeadLibFixture fx = library_with_dead_alphas(n_dead, m);
  const auto df = extract_dead_factors(*fx.lib, std::span<const AlphaId>{fx.dead_ids}, fx.as_of, m);
  ASSERT_TRUE(df.has_value()) << (df ? "" : df.error().to_string());
  EXPECT_GE(df->k_dead, 1U);
  EXPECT_LE(df->k_dead, m);
  EXPECT_EQ(df->loadings.cols(), static_cast<Eigen::Index>(df->k_dead));
  EXPECT_EQ(df->loadings.rows(), static_cast<Eigen::Index>(m));
  EXPECT_EQ(df->variances.size(), static_cast<Eigen::Index>(df->k_dead));
  // Kept eigenvalues are non-negative (overlap is PSD) and descending.
  for (Eigen::Index j = 0; j < df->variances.size(); ++j) {
    EXPECT_GE(df->variances[j], -1e-12);
    if (j > 0) {
      EXPECT_LE(df->variances[j], df->variances[j - 1] + 1e-12);
    }
  }
}

// ===========================================================================
//  #2 EmptyDeadSetYieldsNoFactors — boundary: empty dead set → k_dead == 0.
// ===========================================================================
TEST(DeadFactor, EmptyDeadSetYieldsNoFactors) {
  DeadLibFixture fx = library_with_dead_alphas(1U, 16U); // a non-empty library...
  const auto df =
      extract_dead_factors(*fx.lib, std::span<const AlphaId>{}, /*as_of*/ 100U, /*M*/ 16U);
  ASSERT_TRUE(df.has_value());
  EXPECT_EQ(df->k_dead, 0U);
  EXPECT_EQ(df->loadings.cols(), 0);
  EXPECT_EQ(df->variances.size(), 0);
}

// ===========================================================================
//  #3 SignConventionIsReproducible — two identical extracts are BIT-identical (R1).
// ===========================================================================
TEST(DeadFactor, SignConventionIsReproducible) {
  const usize n_dead = 10U;
  const usize m = 14U;
  DeadLibFixture fx = library_with_dead_alphas(n_dead, m);
  const auto a = extract_dead_factors(*fx.lib, std::span<const AlphaId>{fx.dead_ids}, fx.as_of, m);
  const auto b = extract_dead_factors(*fx.lib, std::span<const AlphaId>{fx.dead_ids}, fx.as_of, m);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a->k_dead, b->k_dead);
  ASSERT_EQ(a->loadings.rows(), b->loadings.rows());
  ASSERT_EQ(a->loadings.cols(), b->loadings.cols());
  EXPECT_TRUE((a->loadings.array() == b->loadings.array()).all()); // bit-identical loadings
  EXPECT_TRUE((a->variances.array() == b->variances.array()).all());
}

// effective_rank sanity: a one-direction spectrum -> 1; a flat r-direction spectrum
// -> r; a zero spectrum -> 0. (eRank determinism + formula.)
TEST(DeadFactor, EffectiveRankMatchesEntropy) {
  VecX one(4);
  one << 10.0, 0.0, 0.0, 0.0;
  EXPECT_EQ(effective_rank(one), 1U);
  VecX flat4(4);
  flat4 << 1.0, 1.0, 1.0, 1.0; // entropy ln4 -> exp = 4
  EXPECT_EQ(effective_rank(flat4), 4U);
  VecX zero = VecX::Zero(5);
  EXPECT_EQ(effective_rank(zero), 0U);
  VecX neg(3);
  neg << -1.0, -2.0, -3.0; // all non-positive -> 0
  EXPECT_EQ(effective_rank(neg), 0U);
}

// ===========================================================================
//  RejectsOutOfRangePeriod — a too-large as_of_period (>= n_periods) must Err, NOT
//  silently OOB-read the store's pos_row (the NDEBUG heap-read hazard the guard
//  closes). The fixture has T == 2 periods, so period 100 is out of range.
// ===========================================================================
TEST(DeadFactor, RejectsOutOfRangePeriod) {
  const usize m = 16U;
  DeadLibFixture fx = library_with_dead_alphas(8U, m);
  ASSERT_FALSE(fx.dead_ids.empty());
  const auto df = extract_dead_factors(*fx.lib, std::span<const AlphaId>{fx.dead_ids},
                                       /*as_of*/ 100U, m); // >> n_periods (==2)
  ASSERT_FALSE(df.has_value());
  EXPECT_EQ(df.error().code(), atx::core::ErrorCode::OutOfRange);
}

// ===========================================================================
//  OrderIndependentOfDeadIdOrdering — the overlap sum is order-sensitive FP, so the
//  result must NOT depend on the caller's dead_ids ordering (extract sorts a local
//  copy ascending). Extract with the ids forward vs reversed → BIT-identical (R1).
// ===========================================================================
TEST(DeadFactor, OrderIndependentOfDeadIdOrdering) {
  const usize n_dead = 12U;
  const usize m = 16U;
  DeadLibFixture fx = library_with_dead_alphas(n_dead, m);
  std::vector<AlphaId> rev(fx.dead_ids.rbegin(), fx.dead_ids.rend()); // reversed order
  const auto fwd =
      extract_dead_factors(*fx.lib, std::span<const AlphaId>{fx.dead_ids}, fx.as_of, m);
  const auto bwd = extract_dead_factors(*fx.lib, std::span<const AlphaId>{rev}, fx.as_of, m);
  ASSERT_TRUE(fwd.has_value());
  ASSERT_TRUE(bwd.has_value());
  ASSERT_EQ(fwd->k_dead, bwd->k_dead);
  EXPECT_TRUE((fwd->loadings.array() == bwd->loadings.array()).all()); // bit-identical
  EXPECT_TRUE((fwd->variances.array() == bwd->variances.array()).all());
}

// ===========================================================================
//  R6 fixture helpers: build a base FactorComponents directly (full control over
//  the geometry) for the load-bearing reduction proof. M instruments, ONE base
//  factor (an all-ones market column) with small factor variance, plus uniform
//  specific variance D. This base risk model is nearly ISOTROPIC off the market
//  direction, so the unconstrained-risk book follows the alpha direction — letting
//  us inject an alpha that DELIBERATELY loads on a dead direction and then watch the
//  augmented model steer the book off it.
// ===========================================================================
[[nodiscard]] FactorComponents make_base_components(usize m, f64 spec_var) {
  FactorComponents comp;
  comp.X = MatX::Ones(static_cast<Eigen::Index>(m), 1); // one market factor (all-ones)
  comp.F = MatX::Constant(1, 1, 1e-6);                  // tiny market factor variance
  comp.D = VecX::Constant(static_cast<Eigen::Index>(m), spec_var); // uniform specific var
  comp.fit_end = 1U;
  return comp;
}

// dead-subspace exposure ‖loadingsᵀ w‖₂ of a book w on the dead loadings.
[[nodiscard]] f64 dead_subspace_exposure(const MatX &loadings, const std::vector<f64> &w) {
  Eigen::Map<const VecX> wv(w.data(), static_cast<Eigen::Index>(w.size()));
  const VecX proj = loadings.transpose() * wv;
  return proj.norm();
}

// ===========================================================================
//  #4 AugmentedModelReducesDeadSubspaceExposure (LOAD-BEARING, R6).
//
//  Build a base model V0 (no dead factors) and an augmented model V1 (with dead
//  factors). Optimize the SAME mega_alpha through BOTH at λ>0. The augmented book
//  must have STRICTLY LESS exposure ‖loadingsᵀ w‖₂ to the dead subspace.
//
//  Fixture geometry (why the reduction is clean): the alpha is built to point ALONG
//  the dominant dead loading (after dollar-neutral centering), and the base model is
//  near-isotropic, so V0's book loads heavily on that dead direction. V1 raises that
//  direction's variance by a large factor, so V1⁻¹ shrinks the book's component
//  there — measurably cutting the dead-subspace exposure.
// ===========================================================================
TEST(DeadFactor, AugmentedModelReducesDeadSubspaceExposure) {
  const usize n_dead = 12U;
  const usize m = 16U;
  DeadLibFixture fx = library_with_dead_alphas(n_dead, m);
  const auto df = extract_dead_factors(*fx.lib, std::span<const AlphaId>{fx.dead_ids}, fx.as_of, m);
  ASSERT_TRUE(df.has_value());
  ASSERT_GE(df->k_dead, 1U);

  // Base components: near-isotropic risk (uniform specific var, tiny market factor).
  const FactorComponents base = make_base_components(m, /*spec_var*/ 1e-4);

  // The mega_alpha points ALONG the dominant dead loading column so V0's book loads
  // on the dead subspace. (Dollar-neutral centering in the optimizer removes the
  // mean, which is why we use a sign-varying loading — its mean is ~0.)
  const VecX top = df->loadings.col(0);
  std::vector<f64> mega_alpha(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    mega_alpha[i] = top[static_cast<Eigen::Index>(i)];
  }

  // Scale the dead variances UP so the augmented model assigns real risk to the dead
  // directions (the overlap eigenvalues are O(1/n_dead); the optimizer's directional
  // tilt is variance-RATIO driven, so we lift the dead block well above the base
  // specific variance to make the steering measurable). This mirrors the operating-
  // book recycle: a heavily-traded dead direction gets a large penalty.
  DeadAlphaFactors scaled = *df;
  scaled.variances = scaled.variances * 1.0e4;

  const auto v0 = FactorModel::create(base.X, base.F, base.D, 0U, base.fit_end);
  ASSERT_TRUE(v0.has_value()) << (v0 ? "" : v0.error().to_string());
  const auto v1 = augment_factor_model(base, scaled);
  ASSERT_TRUE(v1.has_value()) << (v1 ? "" : v1.error().to_string());

  OptimizerConfig oc;
  oc.risk_aversion = 1.0; // λ>0 so the V⁻¹ directional tilt is active
  oc.turnover_penalty = 0.0;
  oc.gross_leverage = 1.0;
  oc.name_cap = 1.0; // effectively uncapped (>= L)
  oc.dollar_neutral = true;
  const PortfolioOptimizer opt{oc};

  const auto w0 = opt.solve(std::span<const f64>{mega_alpha}, *v0, std::span<const f64>{});
  const auto w1 = opt.solve(std::span<const f64>{mega_alpha}, *v1, std::span<const f64>{});
  ASSERT_TRUE(w0.has_value()) << (w0 ? "" : w0.error().to_string());
  ASSERT_TRUE(w1.has_value()) << (w1 ? "" : w1.error().to_string());

  const f64 e0 = dead_subspace_exposure(df->loadings, *w0);
  const f64 e1 = dead_subspace_exposure(df->loadings, *w1);
  EXPECT_TRUE(std::isfinite(e0));
  EXPECT_TRUE(std::isfinite(e1));
  // Observed magnitude (deterministic): e0 ~ 0.229, e1 ~ 0.125 (a ~45% reduction in
  // dead-subspace exposure) — a clean strict reduction, far off the knife-edge.
  EXPECT_LT(e1, e0) << "augmented book must load LESS on the dead subspace (e0=" << e0
                    << ", e1=" << e1 << ")";
}

// ===========================================================================
//  AugmentRejectsDimMismatch — dead loadings whose row count disagrees with X's must
//  return Err (NOT abort via Eigen's comma-initializer eigen_assert). M=8 base vs a
//  10-row dead loading column.
// ===========================================================================
TEST(DeadFactor, AugmentRejectsDimMismatch) {
  const usize m = 8U;
  const FactorComponents base = make_base_components(m, /*spec_var*/ 1e-4);
  DeadAlphaFactors bad;
  bad.loadings = MatX::Ones(static_cast<Eigen::Index>(m + 2U), 1); // wrong row count (10 != 8)
  bad.variances = VecX::Constant(1, 1.0);
  bad.k_dead = 1U;
  const auto v = augment_factor_model(base, bad);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ===========================================================================
//  PanelFixture (mirror of risk_factor_builder_test) for the build_components proof.
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(atx::core::domain::Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(atx::engine::kPanelFieldCount * cap_ * n_inst_,
                   std::numeric_limits<f64>::quiet_NaN());
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        set(atx::engine::PanelField::Open, phys, i, c);
        set(atx::engine::PanelField::High, phys, i, c);
        set(atx::engine::PanelField::Low, phys, i, c);
        set(atx::engine::PanelField::Close, phys, i, c);
        set(atx::engine::PanelField::Volume, phys, i, 1000.0);
        mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
      }
    }
  }

  [[nodiscard]] atx::engine::PanelView view() const noexcept {
    return atx::engine::PanelView{fields_.data(),
                                  mask_.data(),
                                  std::span<const atx::engine::InstrumentId>{universe_},
                                  cap_,
                                  head_(),
                                  n_rows_,
                                  mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }
  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }
  void set(atx::engine::PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }
  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<atx::engine::InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// ===========================================================================
//  #5 DeadFactorBuild.BuildComponentsMatchesBuild — the §0.3 refactor is behavior-
//  preserving: build_components + create == build (same K, same risk()).
// ===========================================================================
TEST(DeadFactorBuild, BuildComponentsMatchesBuild) {
  const usize window = 8U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) { // oldest -> newest
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{1U, 1U, 1U, 1U}; // one sector -> K=1

  atx::engine::risk::FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00; // sectors only (no lookback)
  atx::engine::risk::FactorModelBuilder builder{cfg};

  const auto comp = builder.build_components(fx.view(), window, std::span<const f64>{},
                                             std::span<const u32>{group});
  ASSERT_TRUE(comp.has_value()) << (comp ? "" : comp.error().to_string());
  const auto full =
      builder.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(full.has_value()) << (full ? "" : full.error().to_string());

  const auto re = FactorModel::create(comp->X, comp->F, comp->D, 0U, comp->fit_end);
  ASSERT_TRUE(re.has_value()) << (re ? "" : re.error().to_string());

  EXPECT_EQ(re->n_factors(), full->n_factors());
  EXPECT_EQ(re->n_instruments(), full->n_instruments());
  EXPECT_EQ(comp->fit_end, window);

  // risk(probe) is bit-identical between the re-assembled model and build()'s model.
  const std::vector<f64> probe{0.4, -0.1, 0.3, -0.2};
  EXPECT_NEAR(re->risk(std::span<const f64>{probe}), full->risk(std::span<const f64>{probe}),
              1e-12);
  // Determinism: build() twice -> identical risk readout (the refactor preserves it).
  const auto full2 =
      builder.build(fx.view(), window, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(full2.has_value());
  EXPECT_EQ(full2->risk(std::span<const f64>{probe}), full->risk(std::span<const f64>{probe}));
}


}  // namespace atxtest_risk_dead_factor_test
