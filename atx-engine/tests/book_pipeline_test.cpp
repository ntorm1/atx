// book_pipeline_test.cpp — S7-5 CAPSTONE: the end-to-end BookPipeline all-invariants
// gate (suite BookPipeline). The v2 done-gate: ONE real orchestrator (book::BookPipeline)
// composes mine -> admit -> promote -> combine -> augment-risk(dead factors) -> size ->
// multi-period optimize -> monitor decay -> recycle -> report, and these tests prove the
// operating-book invariants SIMULTANEOUSLY on its real output (no faked invariant).
//
// Invariants proven (each NON-VACUOUS):
//   1. EndToEndRunIsByteIdentical      (R1/R8) — two runs, same cfg+seed => equal digests.
//   2. NoLookAheadTruncationInvariant  (R2)    — the fitted factor model on <=t data is
//        byte-identical when >t data is appended (truncation-invariance at the fitted
//        boundary; scope documented — a FULL book-prefix truncation harness is deferred).
//   3. LifecycleDrivenEndToEnd         (R5)    — a planted decaying alpha reaches Dead AND
//        a pre-retired alpha is Recycled (census[Dead] > 0 AND census[Recycled] > 0).
//   4. NetBelowGrossCostHonest         (R3)    — sum(pnl_net) < sum(pnl_gross).
//   5. GrossNeverExceedsCapacity       (R7)    — every capacity_utilization[s] <= 1 + eps.
//   6. NoiseLibraryProducesFlatBook    (non-vacuity) — a pure-noise research config admits
//        <= a small ceiling, so ~nothing operates.
//   + DeadFactorAugmentationIsReal     (R6)    — a pre-retired dead pool yields k_dead > 0.
//
// The fixtures MIRROR factory_research_driver_test.cpp (the momentum vs pure-noise panels +
// the seed grammar + the temp-dir library) so the engine's planted-edge / pure-noise
// discrimination is the SAME the S3/S4b suites prove.

#include <cmath>        // std::isfinite, std::cos
#include <cstdint>      // std::uint64_t
#include <filesystem>   // per-test temp directory
#include <limits>       // std::numeric_limits (quiet_NaN panel cell)
#include <numbers>      // std::numbers::pi
#include <span>
#include <string>
#include <system_error> // std::error_code
#include <utility>      // std::move
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library

#include "atx/engine/combine/combiner.hpp" // combine::CombinerConfig, CombineMethod
#include "atx/engine/combine/gate.hpp"     // combine::AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp"  // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"    // combine::AlphaId

#include "atx/engine/exec/execution_sim.hpp"

#include "atx/engine/factory/factory.hpp"          // factory::FactoryConfig
#include "atx/engine/factory/research_driver.hpp"  // factory::ResearchConfig, ResearchReport

#include "atx/engine/library/library.hpp"   // library::Library, AlphaCandidate, AdmitKind
#include "atx/engine/library/lifecycle.hpp" // library::LifecycleState
#include "atx/engine/library/record.hpp"    // library::Provenance

#include "atx/engine/loop/panel_types.hpp"  // PanelView (truncation-invariance fixture)
#include "atx/engine/loop/types.hpp"        // InstrumentId
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/risk/exposures.hpp"    // risk::FactorModelConfig
#include "atx/engine/risk/factor_model.hpp" // FactorModelBuilder, FactorComponents (R2 boundary)
#include "atx/engine/risk/multi_period.hpp" // risk::RebalanceSchedule, MultiPeriodConfig

#include "atx/engine/book/pipeline.hpp" // the unit under test

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::InstrumentId;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::CombinerConfig;
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
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::ResearchConfig;
using atx::engine::book::BookPipeline;
using atx::engine::book::PipelineConfig;
using atx::engine::book::PipelineReport;
using atx::engine::risk::RebalanceSchedule;

namespace lib = atx::engine::library;
namespace risk = atx::engine::risk;
namespace factory = atx::engine::factory;

// ---- builders (mirrored from factory_research_driver_test.cpp) --------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.1, 0.5, 0.05}, // small non-zero impact so the capacity curve bites
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

// A tiny deterministic LCG -> uniform(-1, 1) (the S3-4/S3-5 idiom).
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

[[nodiscard]] std::vector<f64> momentum_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.010 - 0.0040 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.008 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

[[nodiscard]] std::vector<f64> noise_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + 0.008 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

[[nodiscard]] Panel two_field_panel(usize dates, usize insts, std::vector<f64> close) {
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

constexpr usize kDates = 120U;
constexpr usize kInsts = 8U;

[[nodiscard]] Panel real_signal_panel() {
  return two_field_panel(kDates, kInsts, momentum_close(kDates, kInsts, 0xA11Cu));
}
[[nodiscard]] Panel pure_noise_panel() {
  return two_field_panel(kDates, kInsts, noise_close(kDates, kInsts, 0xBADC0FFEEu));
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)", "rank(rev)",         "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}
[[nodiscard]] std::vector<std::string> panel_fields() { return {"close", "rev"}; }
[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

// A PERMISSIVE gate so a planted dead candidate (a benign near-zero pnl stream with a
// structured holdings cross-section) is always admitted — the planting exercises the
// POSITIONS path, not the gate (mirrors risk_dead_factor_test's permissive_gate_cfg).
[[nodiscard]] GateConfig permissive_gate_cfg() {
  GateConfig cfg;
  cfg.min_sharpe = -1e9;
  cfg.min_fitness = -1e9;
  cfg.max_turnover = 1e9;
  cfg.max_pool_corr = 1.1;
  return cfg;
}

constexpr u64 kLibSeed = 0xC0FFEEu;
constexpr f64 kMinDsr = 0.5;

[[nodiscard]] FactoryConfig per_run_cfg(usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = 0;
  cfg.search.population = pop;
  cfg.search.generations = gens;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.novelty_w = 0.1;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = seed_exprs();
  cfg.panel_fields = panel_fields();
  cfg.min_dsr = kMinDsr;
  return cfg;
}

[[nodiscard]] ResearchConfig real_signal_research_cfg(u64 master_seed, usize max_runs,
                                                      usize patience) {
  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(16, 4);
  cfg.max_runs = max_runs;
  cfg.patience = patience;
  cfg.master_seed = master_seed;
  return cfg;
}

// The pure-noise config: a LARGE deflation N drives even the in-sample-best noise
// candidate below the dsr bar, so EVERY run admits 0 (the noise guard).
[[nodiscard]] ResearchConfig noise_research_cfg(u64 master_seed, usize max_runs, usize patience) {
  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(24, 6);
  cfg.per_run.search.fitness.trial_count = 64;
  cfg.max_runs = max_runs;
  cfg.patience = patience;
  cfg.master_seed = master_seed;
  return cfg;
}

[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S7") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s7_pipeline" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  (void)ec;
  std::filesystem::create_directories(dir, ec);
  (void)ec;
  return dir.string();
}

// A short rebalance schedule, every period < kDates (accumulate_report reads the panel
// return cross-section at each). Ascending, unique (PIT).
[[nodiscard]] RebalanceSchedule book_schedule() {
  return RebalanceSchedule{{60U, 70U, 80U, 90U, 100U}};
}

// The standard pipeline config over the real-signal panel. forward_window is long enough
// for the DecayController to drive a positive-baseline Live alpha to Dead (ph_min_obs 30 +
// dsr_confirm_run 10 + confirm_periods 5 <= the window).
[[nodiscard]] PipelineConfig real_pipeline_cfg(u64 research_seed) {
  PipelineConfig cfg;
  cfg.research = real_signal_research_cfg(research_seed, /*max_runs*/ 2, /*patience*/ 0);
  cfg.library_master_seed = kLibSeed;
  cfg.schedule = book_schedule();
  cfg.combiner = CombinerConfig{}; // default ShrinkageMv
  // R7 must BIND non-vacuously: the configured gross (2.0) DELIBERATELY exceeds the
  // capacity-derived ceiling. reference_aum/max_gross are chosen so capacity_gross clamps
  // to a small leverage (1.0) below single.gross_leverage, so the MultiPeriodOptimizer's
  // `min(gross_leverage, capacity_gross)` clip actually binds (every book's L1 ≈ 1.0).
  cfg.optimizer.single.gross_leverage = 2.0;
  cfg.optimizer.single.turnover_penalty = 0.0; // overridden to cost.kappa by run()
  cfg.optimizer.trade_rate = 1.0;
  cfg.optimizer.capacity_bound_gross = true;
  cfg.alloc.max_gross = 1.0;     // the capacity-leverage ceiling clamps here (< gross 2.0)
  cfg.reference_aum = 1.0e5;     // AUM->leverage divisor (deployed book capital)
  cfg.forward_window = 80U;
  cfg.returns_field = "rev";
  cfg.dead_lifecycle_as_of = 5U;  // a pre-retired alpha (planted) reads Dead at journal period 5
  cfg.dead_holdings_period = 1U;  // its holdings cross-section lives at positions period 1
  return cfg;
}

// =============================================================================
//  A fixture bundle (one DSL Library / Panel / sim / policy / gate) + the library.
// =============================================================================
struct Fixture {
  Library dsl{};
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  AlphaGate gate{default_gate_cfg()};

  explicit Fixture(Panel p) : panel{std::move(p)} {}
};

// Plant n_dead pre-retired alphas into `library` (the DeadLibFixture recipe from
// risk_dead_factor_test): each carries a known low-frequency holdings cross-section so
// the overlap matrix has real structure, walked Admitted->Live->Decaying->Dead by journal
// period 5 (so cfg.dead_lifecycle_as_of==5 reads them Dead). T == kDates so the planted
// candidates share the mined alphas' period shape (the store fixes T on the first insert).
// Holdings live at positions period 1 (== cfg.dead_holdings_period). Returns the ids.
[[nodiscard]] std::vector<AlphaId> plant_dead_alphas(lib::Library &library, usize n_dead, usize m,
                                                     usize t) {
  const AlphaGate gate{permissive_gate_cfg()}; // planting exercises positions, not the gate
  struct Owner {
    std::vector<f64> pnl;
    std::vector<f64> pos_flat;
  };
  std::vector<Owner> owners(n_dead);
  std::vector<AlphaId> ids;
  ids.reserve(n_dead);
  for (usize k = 0; k < n_dead; ++k) {
    Owner &o = owners[k];
    o.pnl.assign(t, 0.0);
    o.pnl[1] = 0.01 + 0.0001 * static_cast<f64>(k); // benign non-degenerate pnl
    o.pos_flat.assign(t * m, 0.0);
    const f64 center = static_cast<f64>(k % m);
    for (usize i = 0; i < m; ++i) {
      const f64 d = (static_cast<f64>(i) - center) / static_cast<f64>(m);
      o.pos_flat[1 * m + i] = std::cos(std::numbers::pi * d); // holdings at positions period 1
    }
    AlphaMetrics metrics{};
    metrics.sharpe = 5.0;
    metrics.turnover = 0.05;
    metrics.returns = 1.0;
    metrics.drawdown = 0.1;
    metrics.margin = 10.0;
    metrics.fitness = 5.0;
    metrics.holding_days = 20.0;
    const lib::Provenance prov{"planted_dead", std::vector<u64>{}, /*op*/ 0,
                               /*seed*/ static_cast<u64>(900 + k)};
    const lib::AlphaCandidate cand{/*canon_hash*/ 0x900ULL + k, o.pnl,  o.pos_flat,
                                   metrics,                     prov,   /*as_of*/ 0U,
                                   /*source*/ nullptr};
    const auto v = library.admit(cand, gate);
    EXPECT_EQ(v.kind, lib::AdmitKind::Accept) << "planted dead candidate " << k;
    ids.push_back(v.id);
  }
  // Walk each down the legal spine to Dead by journal period 4 (so a query at period 5
  // reads Dead). Ascending id so extract_dead_factors' order-fixed accumulation matches.
  for (const AlphaId id : ids) {
    EXPECT_TRUE(library.mark(id, lib::LifecycleState::Live, /*as_of*/ 2U).has_value());
    EXPECT_TRUE(library.mark(id, lib::LifecycleState::Decaying, /*as_of*/ 3U).has_value());
    EXPECT_TRUE(library.mark(id, lib::LifecycleState::Dead, /*as_of*/ 4U).has_value());
  }
  return ids;
}

// =============================================================================
//  #1 EndToEndRunIsByteIdentical (R1/R8) — two runs, same cfg+seed => equal digests.
// =============================================================================
TEST(BookPipeline, EndToEndRunIsByteIdentical) {
  Fixture fa{real_signal_panel()};
  Fixture fb{real_signal_panel()};
  lib::Library la = lib::Library::open(tmpdir("a"), default_gate_cfg(), {kLibSeed});
  lib::Library lb = lib::Library::open(tmpdir("b"), default_gate_cfg(), {kLibSeed});
  BookPipeline pa{la, fa.dsl, fa.panel, fa.sim, fa.policy, fa.gate};
  BookPipeline pb{lb, fb.dsl, fb.panel, fb.sim, fb.policy, fb.gate};

  const auto ra = pa.run(real_pipeline_cfg(/*seed*/ 21));
  const auto rb = pb.run(real_pipeline_cfg(/*seed*/ 21));
  ASSERT_TRUE(ra.has_value()) << (ra ? "" : ra.error().to_string());
  ASSERT_TRUE(rb.has_value()) << (rb ? "" : rb.error().to_string());

  EXPECT_EQ(ra->book_digest, rb->book_digest);     // byte-identical realized book chain
  EXPECT_EQ(ra->report_digest, rb->report_digest); // byte-identical report series
  EXPECT_EQ(ra->research.digest, rb->research.digest);
  // Non-vacuity: the book chain is non-empty and the digest is a real fold (not the seed).
  EXPECT_EQ(ra->report.pnl_gross.size(), book_schedule().periods.size());
  EXPECT_NE(ra->book_digest, 0u);

  // COMBINER DRIVES THE BOOK (the combine step is NOT inert): a run that differs ONLY in
  // the combiner blend method (RankAverage's rank-space kernel vs the default linear
  // ShrinkageMv) produces a DIFFERENT book. If the optimizer ignored the fitted
  // CombinedSignalSource output, the book_digest would be identical regardless of method,
  // so this pins that combine -> optimize is wired through (the fitted blend drives the book).
  Fixture fc{real_signal_panel()};
  lib::Library lc = lib::Library::open(tmpdir("c"), default_gate_cfg(), {kLibSeed});
  BookPipeline pc{lc, fc.dsl, fc.panel, fc.sim, fc.policy, fc.gate};
  PipelineConfig cc = real_pipeline_cfg(/*seed*/ 21);
  cc.combiner.method = atx::engine::combine::CombineMethod::RankAverage; // different blend kernel
  const auto rc = pc.run(cc);
  ASSERT_TRUE(rc.has_value()) << (rc ? "" : rc.error().to_string());
  EXPECT_NE(ra->book_digest, rc->book_digest)
      << "the combiner blend method must change the book (combine -> optimize is wired)";
}

// =============================================================================
//  #2 NoLookAheadTruncationInvariant (R2) — the fitted factor model on <=t data is
//  byte-identical when >t data is appended. SCOPE: a full book-prefix truncation harness
//  (re-running the whole pipeline at a truncated date and bit-comparing the book prefix)
//  is intractable to wire deterministically here (the persistent library + research seed
//  axis would have to be replayed identically at two horizons). We instead prove the
//  load-bearing no-look-ahead property the pipeline RELIES ON: FactorModelBuilder over the
//  newest `window` rows of a PanelView is INVARIANT to rows appended beyond window — i.e.
//  the fitted (X, F, D) the optimizer consumes cannot see the future. This is the exact
//  PanelView/build path the BookPipeline uses (RESIDUAL #2). The book-prefix truncation
//  harness is the documented deferred extension.
// =============================================================================
namespace {
// Minimal PanelView backing (the PanelFixture pattern). Row 0 == newest.
class PanelBack {
public:
  PanelBack(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2(n_rows)}, mask_words_{(n_inst + 63U) / 64U} {
    for (usize i = 0; i < n_inst; ++i) {
      uni_.push_back(InstrumentId{static_cast<u32>(i + 1U)});
    }
    fields_.assign(atx::engine::kPanelFieldCount * cap_ * n_inst_,
                   std::numeric_limits<f64>::quiet_NaN());
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r;
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, 1.0e6);
        mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
      }
    }
  }
  [[nodiscard]] PanelView view(usize valid_rows) const noexcept {
    const usize head = (valid_rows == 0U) ? 0U : valid_rows - 1U;
    return PanelView{fields_.data(),    mask_.data(), std::span<const InstrumentId>{uni_},
                     cap_,              head,         valid_rows,
                     mask_words_};
  }

private:
  static usize pow2(usize n) {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }
  void set(PanelField f, usize phys, usize inst, f64 v) {
    fields_[static_cast<usize>(f) * cap_ * n_inst_ + phys * n_inst_ + inst] = v;
  }
  usize n_rows_, n_inst_, cap_, mask_words_;
  std::vector<InstrumentId> uni_;
  std::vector<f64> fields_;
  std::vector<u64> mask_;
};
} // namespace

TEST(BookPipeline, NoLookAheadTruncationInvariant) {
  constexpr usize kRows = 40U;
  constexpr usize kM = 6U;
  // A deterministic close grid (row 0 == newest). Rows [0, window) are the fit window.
  std::vector<std::vector<f64>> close(kRows, std::vector<f64>(kM));
  Lcg rng{0x5151u};
  for (usize r = 0; r < kRows; ++r) {
    for (usize i = 0; i < kM; ++i) {
      close[r][i] = 100.0 + 5.0 * rng.next() + 0.5 * static_cast<f64>(i);
    }
  }
  // FULL grid vs a grid where rows beyond the fit READ HORIZON are CORRUPTED (a "future"
  // mutation). A trailing return at fit row r reads close(r) AND close(r+1), so a window of
  // `kWindow` fit rows reads close rows [0, kWindow] (inclusive). Truncation-invariance
  // means rows STRICTLY beyond that horizon (r >= kWindow+1) are PIT-invisible — poison
  // exactly those so the test pins the real no-look-ahead boundary the builder honors.
  std::vector<std::vector<f64>> corrupt = close;
  constexpr usize kWindow = 20U;
  for (usize r = kWindow + 1U; r < kRows; ++r) {
    for (usize i = 0; i < kM; ++i) {
      corrupt[r][i] = -999.0; // poison rows strictly beyond the fit read horizon
    }
  }
  PanelBack clean{kRows, kM, close};
  PanelBack poisoned{kRows, kM, corrupt};

  risk::FactorModelConfig fc;
  fc.sector_factors = true;
  fc.style_mask = 0x00; // sectors-only (K==1), the BookPipeline base-model config
  const risk::FactorModelBuilder builder{fc};
  const std::vector<f64> mkt(kM, 1.0e9);
  const std::vector<u32> grp(kM, 0U);

  // Build over the newest `window` rows of BOTH panels (poisoned rows are >= window, so
  // PIT-invisible). The fitted components must be BYTE-IDENTICAL.
  const auto a = builder.build_components(clean.view(kRows), kWindow,
                                          std::span<const f64>{mkt}, std::span<const u32>{grp});
  const auto b = builder.build_components(poisoned.view(kRows), kWindow,
                                          std::span<const f64>{mkt}, std::span<const u32>{grp});
  ASSERT_TRUE(a.has_value()) << (a ? "" : a.error().to_string());
  ASSERT_TRUE(b.has_value()) << (b ? "" : b.error().to_string());
  // Bit-identical X, F, D (the no-look-ahead truncation pin).
  ASSERT_EQ(a->X.size(), b->X.size());
  EXPECT_TRUE(a->X == b->X); // Eigen exact equality (no future leak)
  ASSERT_EQ(a->F.size(), b->F.size());
  EXPECT_TRUE(a->F == b->F);
  ASSERT_EQ(a->D.size(), b->D.size());
  EXPECT_TRUE(a->D == b->D);
  EXPECT_EQ(a->fit_end, b->fit_end);

  // MUTATION / SANITY (the test must actually catch a look-ahead leak): poisoning a row
  // WITHIN the fit horizon (row 10 <= kWindow) MUST change the fitted components. If it
  // did not, the truncation-invariance above would be vacuous (the builder would be
  // ignoring ALL rows, not just the future ones). So at least one of (X, F, D) must differ.
  std::vector<std::vector<f64>> in_window = close;
  for (usize i = 0; i < kM; ++i) {
    in_window[10][i] = -999.0; // poison an IN-HORIZON row (10 < kWindow)
  }
  PanelBack mutated{kRows, kM, in_window};
  const auto c = builder.build_components(mutated.view(kRows), kWindow,
                                          std::span<const f64>{mkt}, std::span<const u32>{grp});
  ASSERT_TRUE(c.has_value()) << (c ? "" : c.error().to_string());
  const bool x_changed = !(a->X == c->X);
  const bool f_changed = !(a->F == c->F);
  const bool d_changed = !(a->D == c->D);
  EXPECT_TRUE(x_changed || f_changed || d_changed)
      << "an in-horizon mutation must change the fit (else truncation-invariance is vacuous)";
}

// =============================================================================
//  #3 LifecycleDrivenEndToEnd (R5) — a planted decaying alpha reaches Dead AND a
//  pre-retired alpha is Recycled. The mined alphas (positive admitted Sharpe) are fed a
//  genuinely decaying realized stream by the REAL DecayController over the forward window,
//  so they walk Live -> Decaying -> Dead; the planted Dead alphas are then Recycled.
// =============================================================================
TEST(BookPipeline, LifecycleDrivenEndToEnd) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {kLibSeed});
  // Mine + admit FIRST (the store fixes T = kDates), then plant pre-retired Dead alphas
  // sharing that T so they are insertable and the recycle step has a non-empty Dead set.
  {
    factory::ResearchDriver seeder{library, fx.dsl, fx.panel, fx.sim, fx.policy, fx.gate};
    const auto rep = seeder.run(real_signal_research_cfg(/*seed*/ 31, /*max_runs*/ 2, 0));
    ASSERT_GT(rep.total_admitted, 0u) << "real-signal panel must admit live alphas to decay";
  }
  const std::vector<AlphaId> planted = plant_dead_alphas(library, /*n_dead*/ 4U, kInsts, kDates);
  ASSERT_FALSE(planted.empty());

  // Run the pipeline. It promotes the mined Admitted alphas Live, drives them to Dead via
  // the real decay monitor, then recycles the planted Dead alphas.
  BookPipeline pipe{library, fx.dsl, fx.panel, fx.sim, fx.policy, fx.gate};
  PipelineConfig cfg = real_pipeline_cfg(/*seed*/ 31);
  cfg.research.max_runs = 0U; // do NOT re-mine inside run() (we seeded above); just operate
  const auto r = pipe.run(cfg);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  EXPECT_GT(r->lifecycle_census[static_cast<usize>(lib::LifecycleState::Dead)], 0u)
      << "a decaying alpha must reach Dead (R5)";
  EXPECT_GT(r->lifecycle_census[static_cast<usize>(lib::LifecycleState::Recycled)], 0u)
      << "a pre-retired Dead alpha must be Recycled (R5)";
}

// =============================================================================
//  #4 NetBelowGrossCostHonest (R3) — sum(pnl_net) < sum(pnl_gross): the calibrated cost
//  is genuinely charged (a strictly-positive round-trip cost on non-zero turnover).
// =============================================================================
TEST(BookPipeline, NetBelowGrossCostHonest) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {kLibSeed});
  BookPipeline pipe{library, fx.dsl, fx.panel, fx.sim, fx.policy, fx.gate};
  const auto r = pipe.run(real_pipeline_cfg(/*seed*/ 41));
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  f64 sum_gross = 0.0;
  f64 sum_net = 0.0;
  f64 sum_cost = 0.0;
  for (const f64 v : r->report.pnl_gross) {
    sum_gross += v;
  }
  for (const f64 v : r->report.pnl_net) {
    sum_net += v;
  }
  for (const f64 v : r->report.pnl_cost) {
    sum_cost += v;
  }
  ASSERT_GT(sum_cost, 0.0) << "non-vacuous: a positive cost was actually charged";
  EXPECT_LT(sum_net, sum_gross) << "net must sit strictly below gross (cost charged, R3)";
}

// =============================================================================
//  #5 GrossNeverExceedsCapacity (R7) — NON-VACUOUS: the capacity ceiling is a real GROSS-
//  LEVERAGE bound (AUM->leverage converted), the optimizer's configured gross (2.0)
//  EXCEEDS it (1.0), so the `min(gross_leverage, capacity_gross)` clip BINDS. We assert
//  (a) every book's L1 gross <= capacity_gross + eps, AND (b) the clip actually binds —
//  at least one period's gross ~= capacity_gross (utilization ~= 1). The test would FAIL
//  if the cap were removed (the book would lever to 2.0 and (a) would break).
// =============================================================================
TEST(BookPipeline, GrossNeverExceedsCapacity) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {kLibSeed});
  BookPipeline pipe{library, fx.dsl, fx.panel, fx.sim, fx.policy, fx.gate};
  const PipelineConfig cfg = real_pipeline_cfg(/*seed*/ 51);
  const auto r = pipe.run(cfg);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  ASSERT_FALSE(r->report.gross_leverage.empty());
  ASSERT_GT(r->capacity_gross, 0.0);
  // The capacity ceiling is a leverage bound STRICTLY below the configured gross (so the
  // clip can bind — proves the test is exercising a real constraint, not a no-op).
  ASSERT_LT(r->capacity_gross, cfg.optimizer.single.gross_leverage)
      << "fixture must set capacity_gross below the configured gross so the clip binds";

  f64 max_util = 0.0;
  for (usize s = 0; s < r->report.gross_leverage.size(); ++s) {
    // (a) the realized L1 gross never exceeds the capacity ceiling.
    EXPECT_LE(r->report.gross_leverage[s], r->capacity_gross + 1e-9)
        << "gross exceeded the capacity ceiling at period " << s;
    EXPECT_LE(r->report.capacity_utilization[s], 1.0 + 1e-9);
    if (r->report.capacity_utilization[s] > max_util) {
      max_util = r->report.capacity_utilization[s];
    }
  }
  // (b) the clip BINDS: some period's gross is AT the ceiling (utilization ~= 1). Without
  // the capacity clip the optimizer would lever to single.gross_leverage (2x the ceiling),
  // so this binding assertion is what makes R7 fail if the cap were deleted.
  EXPECT_NEAR(max_util, 1.0, 1e-6) << "the capacity clip must actually bind (R7 non-vacuous)";
}

// =============================================================================
//  #6 NoiseLibraryProducesFlatBook (non-vacuity) — a pure-noise research config admits
//  <= a small ceiling, so ~nothing operates (the S3/S4 noise guard, end-to-end).
// =============================================================================
constexpr usize kNoiseAdmitCeiling = 1U;

TEST(BookPipeline, NoiseLibraryProducesFlatBook) {
  Fixture fx{pure_noise_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {kLibSeed});
  BookPipeline pipe{library, fx.dsl, fx.panel, fx.sim, fx.policy, fx.gate};
  PipelineConfig cfg = real_pipeline_cfg(/*seed*/ 7);
  cfg.research = noise_research_cfg(/*seed*/ 7, /*max_runs*/ 10, /*patience*/ 2);
  const auto r = pipe.run(cfg);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  EXPECT_LE(r->research.total_admitted, kNoiseAdmitCeiling)
      << "pure noise must admit <= the ceiling (nothing meaningful operates)";
  EXPECT_LT(r->research.runs, 10u) << "the patience early-stop must fire (non-vacuous)";
}

// =============================================================================
//  + DeadFactorAugmentationIsReal (R6) — a pre-retired dead pool yields k_dead > 0, i.e.
//  the extract_dead_factors -> augment_factor_model path is genuinely exercised (the
//  augmented risk model carries dead directions; on a fresh run with no dead alphas the
//  path is a documented passthrough — proven non-vacuous here by planting a dead pool).
// =============================================================================
TEST(BookPipeline, DeadFactorAugmentationIsReal) {
  Fixture fx{real_signal_panel()};
  lib::Library library = lib::Library::open(tmpdir(), default_gate_cfg(), {kLibSeed});
  {
    factory::ResearchDriver seeder{library, fx.dsl, fx.panel, fx.sim, fx.policy, fx.gate};
    const auto rep = seeder.run(real_signal_research_cfg(/*seed*/ 61, /*max_runs*/ 1, 0));
    ASSERT_GE(rep.total_admitted, 0u);
  }
  const std::vector<AlphaId> planted = plant_dead_alphas(library, /*n_dead*/ 6U, kInsts, kDates);
  ASSERT_FALSE(planted.empty());

  BookPipeline pipe{library, fx.dsl, fx.panel, fx.sim, fx.policy, fx.gate};
  PipelineConfig cfg = real_pipeline_cfg(/*seed*/ 61);
  cfg.research.max_runs = 0U; // operate-only (we seeded + planted above)
  const auto r = pipe.run(cfg);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  EXPECT_GT(r->dead_factors, 0u)
      << "the pre-retired dead pool must yield >=1 extracted dead risk factor (R6)";
}

} // namespace
