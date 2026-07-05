#pragma once

// atx::impl — stage_metabook: the p8-S2 seam that wires the frozen `fund::MetaBook` /
// `fund::MetaAllocator` / netting / cross-sleeve-risk layer (already built, tested, GREEN,
// but never called from atx-impl) into the runnable pipeline as a NEW opt-in stage.
//
// ===========================================================================
//  What this file is
// ===========================================================================
//  `fund/*.hpp` (meta_book, meta_allocator, cross_sleeve_risk, netting, sleeve) is a
//  WIRING/adapter surface over FROZEN `src/fund/*.cpp` estimation/allocation math — S2
//  CALLS it, it does not re-derive it (sprint-2 plan, "Owns"). This header defines the
//  stage's OWN small config seam (`MetaBookStageConfig`) and the `assign_sleeves` /
//  `run_metabook` entry points that assemble the four `MetaBook::run` callbacks from a
//  runnable research panel + combo panel + admitted-alpha library and drive the frozen
//  two-pass engine driver. See `atx-engine/plans/p8/sprint-2-mega-alpha-metabook.md` for
//  the full architecture note and `atx-engine/plans/p8/sprint-2-progress.md` for the
//  per-unit ledger (confirmed frozen signatures, the R7 pin analysis, the S1/S5 seams).
//
// ===========================================================================
//  Why a stage-owned config, not a `RunConfig` field (S2-0 root cause)
// ===========================================================================
//  `RunConfig` (`config.hpp`) and `stage_run.cpp`'s `run_all` orchestration are
//  Sprint-5-owned hub files S2 must not edit. `MetaBookStageConfig` is S2's OWN POD,
//  carrying the engine `fund::MetaBookConfig` (alloc method + risk_lookback) plus a
//  sleeve-assignment policy and the gross/name-cap/risk-aversion knobs the per-sleeve
//  `MultiHorizonConfig` needs — the SAME pattern S1's `RiskModelConfig` used (an
//  engine/impl-internal config struct, consumed by a direct-call stage entry point,
//  threaded onto the CLI in Sprint 5).
//
// ===========================================================================
//  The inert default (R7 boundary pin — see the ledger for the full analysis)
// ===========================================================================
//  `SleeveAssignment::SingleSleeve` (== 0, pinned by static_assert below) partitions the
//  WHOLE admitted-alpha set into ONE sleeve. `run_metabook` additionally overrides
//  `meta.alloc.fractional_kelly` to 1.0 for this path ONLY (target_vol=0 and max_gross=4
//  are already the engine defaults and never bind at S=1) so the allocator's Kelly/cap
//  composition yields `c == [1.0]` every period — the boundary config
//  `fund_meta_book_integration_test.cpp`'s `R7_OneSleeveReducesToMultiHorizonByteIdentical`
//  pins. Combined with one-sleeve netting (net==gross, no crossing, structural) and
//  `Sleeve::run`'s pure delegation to `MultiHorizonOptimizer` (`sleeve.hpp`'s own R7 note),
//  the fund book at the inert default is BYTE-IDENTICAL to today's whole-panel deploy book.

#include <vector>

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u8, u32

#include "atx/engine/fund/meta_book.hpp" // fund::MetaBookConfig (full type: a value member below)
#include "atx/engine/fund/sleeve.hpp"    // fund::SleeveConfig (assign_sleeves' return element)
#include "atx/engine/library/fwd.hpp"    // library::Library (fwd decl; assign_sleeves takes a ref)
#include "atx/engine/risk/factor_model.hpp" // risk::RiskModelConfig (S2-2 3-arg overload param)

#include "config.hpp" // RunConfig
#include "stages.hpp" // StageResult

namespace atx::impl {

namespace fund = atx::engine::fund;
namespace library = atx::engine::library;

// ===========================================================================
//  SleeveAssignment — how the admitted-alpha set is partitioned into sleeves (S2-1).
//  SingleSleeve is the INERT default: one sleeve == the whole admitted set == today's
//  whole-panel book (the R7 pin). The other three are opt-in multi-sleeve partitions;
//  see `assign_sleeves`'s doc (stage_metabook.cpp, S2-1) for each mode's exact rule and
//  its documented fallback to SingleSleeve when it cannot form >=2 non-empty sleeves.
// ===========================================================================
enum class SleeveAssignment : atx::u8 {
  SingleSleeve = 0,  // inert => byte-identical to the single-blend optimize book (R7 pin)
  ByLibraryGroup,    // one sleeve per library flush-batch (Library::kFlushBatch grouping)
  ByCorrCluster,     // data-driven single-linkage clusters of the alpha-PnL corr matrix
  BySignalFamily,    // one sleeve per canonical DSL-family label (outermost op token)
};

static_assert(static_cast<atx::u8>(SleeveAssignment::SingleSleeve) == 0U,
              "SleeveAssignment::SingleSleeve must be 0 -- the inert R7-pin default");

// ===========================================================================
//  MetaBookStageConfig — the stage's own config seam (pure configuration; S2-0).
// ===========================================================================
struct MetaBookStageConfig {
  fund::MetaBookConfig meta{};  // engine driver knobs: alloc (method/Kelly/caps) + risk_lookback
  SleeveAssignment assignment = SleeveAssignment::SingleSleeve; // inert => one sleeve
  atx::u32 max_sleeves = 8U;    // cap on N for ByCorrCluster / ByLibraryGroup / BySignalFamily

  // Mirrors RunConfig's --gross/--name-cap/--risk-aversion resolution (stage_optimize.cpp's
  // gross_val/name_cap_val/risk_aversion locals). `run_metabook` OVERWRITES these three from
  // the RunConfig it is called with (the SAME resolution formula stage_optimize.cpp uses),
  // so the byte-identity pin does not depend on the caller hand-populating them; they exist
  // here so `assign_sleeves` (which takes no RunConfig) has a self-contained input, and so a
  // direct-call test can exercise sleeve assignment without a RunConfig at all.
  atx::f64 gross = 1.0;         // --gross          (dollar-neutral gross leverage ceiling)
  atx::f64 name_cap = 1.0;      // --name-cap       (per-name position cap)
  atx::f64 risk_aversion = 1.0; // --risk-aversion  (lambda; MultiHorizonConfig::risk_aversion)
};

// ===========================================================================
//  assign_sleeves — the SleeveSpec seam (S2-1): admitted AlphaIds -> N SleeveConfigs.
// ===========================================================================
//  Deterministic sleeve assignment. `SingleSleeve` => the WHOLE admitted set as ONE
//  sleeve, members ascending AlphaId (mirrors stage_combine.cpp's library-enumeration
//  order, :401-405) — the R7 boundary pin. Every non-single mode partitions the
//  admitted pool by a documented rule (library segment / correlation cluster / DSL
//  family), capped at `cfg.max_sleeves`, and falls back to `SingleSleeve` when it
//  cannot form >= 2 non-empty sleeves (documented `Ok`, not an error — a one-sleeve
//  partition IS the inert path). AlphaId order is ascending within each sleeve. Every
//  sleeve's `SleeveConfig::mh` is the SAME H=1/identity/minimal-constraint shape
//  (derived from `cfg.gross`/`cfg.name_cap`/`cfg.risk_aversion`) so a SingleSleeve
//  partition reduces byte-identically to the deployed book.
//
//  Pure function of (lib, cfg): order-fixed enumeration, stable cluster tie-break, no
//  RNG, no clock. Same library + same cfg => same std::vector<SleeveConfig> (member
//  lists, tags, capacities identical) — verified by a twice-run test.
//
//  Err(InvalidArgument): `lib.n_alphas() == 0` (nothing to partition).
[[nodiscard]] atx::core::Result<std::vector<fund::SleeveConfig>>
assign_sleeves(const library::Library &lib, const MetaBookStageConfig &cfg);

// ===========================================================================
//  build_metabook_result / run_metabook — the S2-2 producer (two-pass drive).
// ===========================================================================
//  Consumes the research panel (`cfg.panel`) + combo panel (`cfg.combo`) + (for
//  multi-sleeve assignment) the admitted-alpha library (`cfg.library_dir`), assembles
//  the four MetaBook::run callbacks, and calls the FROZEN fund::MetaBook driver.
//
//  SingleSleeve with NO `cfg.library_dir` (RunConfig's own default -- the TRUE inert
//  path) needs no library at all: sources_at(0, ·) reads the combo panel's "alpha"
//  field cross-section directly, ONE identity-horizon source -- the SAME slice
//  stage_optimize.cpp's alpha_at reads. Combined with the fractional_kelly=1 override
//  applied whenever the resolved partition is exactly one sleeve (the c==[1] boundary
//  config, R7), this path's fund book is byte-identical to today's deployed book (see
//  the ledger for the composed proof + the empirical stage-boundary test).
//
//  Any other invocation (multi-sleeve assignment, OR SingleSleeve WITH an explicit
//  `--library-dir`) requires `cfg.library_dir`: each resolved sleeve's own member
//  subset is re-evaluated from its DSL (compile_batch -> Engine::evaluate ->
//  extract_streams, restricted to the sleeve's members) and locally combined via an
//  UNWEIGHTED cross-sectional mean of the members' position streams -- a documented,
//  distinct "mega-alpha per sleeve" seam, NOT the calibrated stage_combine::AlphaCombiner
//  fit (Sprint-3-owned; not re-derived). This path does NOT claim byte-identity to
//  stage_optimize's book (a different, undocumented-elsewhere alpha input) -- it is the
//  on-path RED->GREEN multi-sleeve case (measured netting/diversification win).
//
//  model_at defaults to diag_risk.hpp's diagonal_risk_model (the SAME model
//  stage_optimize's Diagonal path uses) -- no Factor-model variant is wired by S2
//  (recorded as an S1/S5 seam in the ledger). returns_at is the realized per-instrument
//  simple return from research's "close" field (diag_risk.hpp's TRI-return convention).
//
//  build_metabook_result is the direct-call engine-facing entry point (what tests
//  call); run_metabook wraps it into the StageResult shape (digest + kvs) and writes
//  the netted fund book to `cfg.books_out` via the SAME write_panel + .meta.txt
//  sidecar shape stage_optimize.cpp uses (a drop-in for stage_report). Netting /
//  attribution / effective-bets telemetry (S2-3/S2-4) surface into StageResult::kvs
//  ONLY -- never folded into the digest.
[[nodiscard]] atx::core::Result<fund::MetaBookResult>
build_metabook_result(const RunConfig &cfg, const MetaBookStageConfig &scfg);

[[nodiscard]] atx::core::Result<StageResult>
run_metabook(const RunConfig &cfg, const MetaBookStageConfig &scfg);

// ===========================================================================
//  S2-2 (p9) — the RiskModelConfig-parameterized overloads (the deferred p8-S2 seam).
// ===========================================================================
//  Closes the seam p8 `sprint-2-progress.md:269-294` recorded as deferred ("A Factor-model
//  model_at variant is a straightforward follow-on... Recorded here as the integration seam
//  for whoever picks it up next"). These ADD a third `risk::RiskModelConfig` argument; the
//  2-arg overloads above become thin `cfg.risk_model`-aware forwarders (mirroring
//  stage_optimize.cpp's own run_optimize 1-arg/2-arg split), so `--risk-model factor` reaches
//  metabook through the real CLI/run_all path with zero edits to stage_run.cpp/dispatch.cpp.
//
//  `risk_cfg.kind==Diagonal` (the RunConfig{} default) keeps the exact single whole-panel
//  diagonal_risk_model(research) call every pre-S2 caller got -- byte-identical.
//  `risk_cfg.kind==Factor` drives `model_at` with ONE FactorModel per rebalance step, PIT-fit
//  via build_risk_model at fit_end=period+1 (the same per-step producer loop stage_optimize's
//  Factor branch runs), including its diagonal warm-up fallback for an under-determined early
//  step. The Factor loop also threads S1's shared dead-alpha crowding wire (R3/R4): the
//  mega-book path SUBSTITUTES run_optimize (stage_run.cpp:127), so this is the mega-book's only
//  build_risk_model site -- inert (byte-identical) when --dead-alpha-factors is off.
[[nodiscard]] atx::core::Result<fund::MetaBookResult>
build_metabook_result(const RunConfig &cfg, const MetaBookStageConfig &scfg,
                      const atx::engine::risk::RiskModelConfig &risk_cfg);

[[nodiscard]] atx::core::Result<StageResult>
run_metabook(const RunConfig &cfg, const MetaBookStageConfig &scfg,
             const atx::engine::risk::RiskModelConfig &risk_cfg);

} // namespace atx::impl
