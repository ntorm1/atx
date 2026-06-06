#pragma once

// atx::engine::combine — alpha-combination + risk layer forward declarations (Phase 4a).
//
// A lightweight header other engine headers include to NAME the combination spine
// types without pulling in their full definitions (and the alpha store, gate,
// combiner, and combined-signal-source machinery behind them). Keeping the forward
// set here means a header that only passes a `combine::AlphaCombiner*` or a
// `combine::AlphaStore&` around does not transitively include the metrics tables,
// gate policy, or weight-fitting machinery.
//
// Full definitions live in (added per phase unit):
//   combine/store.hpp          — AlphaId, AlphaRecord, AlphaStore              (P4-1)
//   combine/metrics.hpp        — AlphaMetrics                                   (P4-2)
//   combine/gate.hpp           — GateConfig, GateVerdict, AlphaGate             (P4-3)
//   combine/combiner.hpp       — CombineMethod, CombinerConfig, Combination,
//                                AlphaCombiner                                  (P4-4)
//   combine/combined_source.hpp — CombinedSignalSource                          (P4-5)
//
// NOTE: GateVerdict and CombineMethod are scoped enums with an explicit underlying
// type (atx::u8). They are forward-declared here so callers that store or pass
// these values by type can include only this header rather than the full definition
// headers.

#include "atx/core/types.hpp" // atx::u8 (needed for enum underlying types)

namespace atx::engine::combine {

// =====================================================================
//  Scoped enums — forward declarations with explicit underlying type
// =====================================================================

// Gate verdict: whether an alpha passes the correlation/turnover/fitness screen.
// Full definition in combine/gate.hpp (P4-3).
enum class GateVerdict : atx::u8;

// Weight-combination method (e.g. equal, IC-weighted, shrinkage).
// Full definition in combine/combiner.hpp (P4-4).
enum class CombineMethod : atx::u8;

// =====================================================================
//  Alpha store — insertion-ordered registry of live alphas (P4-1)
// =====================================================================

// Stable insertion-order handle for a registered alpha.
// Full definition in combine/store.hpp (P4-1).
struct AlphaId;

// One registered alpha's bookkeeping row (id + expression + compile artifact).
// Full definition in combine/store.hpp (P4-1).
struct AlphaRecord;

// Thread-compatible registry mapping AlphaId → AlphaRecord.
// Full definition in combine/store.hpp (P4-1).
class AlphaStore;

// =====================================================================
//  Metrics — per-alpha fitness statistics (P4-2)
// =====================================================================

// Per-alpha fitness statistics computed over AlphaStreams::pnl(a).
// Includes IC, turnover, Sharpe, drawdown, and correlation summary.
// Full definition in combine/metrics.hpp (P4-2).
struct AlphaMetrics;

// =====================================================================
//  Gate — correlation / turnover / fitness screen (P4-3)
// =====================================================================

// Thresholds governing when an alpha is admitted to the live pool
// (IC floor, max pairwise correlation, max turnover, min Sharpe).
// Full definition in combine/gate.hpp (P4-3).
struct GateConfig;

// Stateless gate: given an AlphaMetrics and GateConfig, returns a GateVerdict.
// Full definition in combine/gate.hpp (P4-3).
struct AlphaGate;

// =====================================================================
//  Combiner — weight-fitting over the admitted pool (P4-4)
// =====================================================================

// Parameters governing the combiner (method, shrinkage, turnover cap, etc.).
// Full definition in combine/combiner.hpp (P4-4).
struct CombinerConfig;

// Output of one combination run: per-alpha weights + combined-signal metadata.
// Full definition in combine/combiner.hpp (P4-4).
struct Combination;

// Fits per-alpha weights over a batch SignalSet → extract_streams pass.
// Full definition in combine/combiner.hpp (P4-4).
class AlphaCombiner;

// =====================================================================
//  Production source adapter (P4-5)
// =====================================================================

// Wraps a compiled Program (via compile_batch) as an ISignalSource that
// forwards Program::required_lookback as max_lookback() and feeds the
// Phase-2 backtest loop directly.
// Full definition in combine/combined_source.hpp (P4-5).
class CombinedSignalSource;

} // namespace atx::engine::combine
