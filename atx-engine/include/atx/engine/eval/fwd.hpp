#pragma once

// atx::engine::eval — evaluation & validation spine forward declarations (Sprint S1).
//
// A lightweight header other engine headers include to NAME the evaluation spine
// types without pulling in their full definitions (and the stats, metrics, DSR,
// PBO, and CPCV machinery behind them). Keeping the forward set here means a
// header that only passes an `eval::ReturnMetrics` or an `eval::PboResult` around
// does not transitively include the distribution helpers, fold generators, or
// validation harness.
//
// Full definitions live in (added per unit):
//   eval/stats_ext.hpp      — norm_cdf/norm_ppf/skewness/excess_kurtosis/median   (S1-1)
//   eval/perf_metrics.hpp   — ReturnMetrics, compute_return_metrics              (S1-1)
//   eval/deflated_sharpe.hpp— DsrResult, deflated_sharpe                         (S1-2)
//   eval/pbo.hpp            — PboResult, pbo_cscv                                 (S1-3)
//   eval/cpcv.hpp           — CpcvConfig, CpcvFold, cpcv_folds                    (S1-4)

#include "atx/core/types.hpp" // atx::u8 (needed for enum underlying types)

namespace atx::engine::eval {

struct ReturnMetrics;   // perf_metrics.hpp (S1-1)
struct DsrResult;       // deflated_sharpe.hpp (S1-2)
struct PboResult;       // pbo.hpp (S1-3)
struct CpcvConfig;      // cpcv.hpp (S1-4)
struct CpcvFold;        // cpcv.hpp (S1-4)

} // namespace atx::engine::eval
