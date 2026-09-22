#pragma once

#include "stages.hpp"

namespace atx::impl {

// Pre-registered cross-sectional forecast evaluation of the frozen equity
// baseline. Reuses evaluate_equity_baseline and the published combo.bin
// unchanged; fits nothing, trades nothing and changes no prior artifact.
// Requires an identified context, a published baseline directory and the
// explicit training evaluation window; publishes a fresh directory and keeps
// failure.json plus partial evidence on a failed attempt.
// Frozen design: atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md
// (§4 pre-registration, §5 seal, §7 subcommand and schemas, §12 qualifications).
// The emitted information coefficients are model-skill statistics on a
// 189-observation training slice: sign-and-shape evidence only, never accepted
// alpha, a Sharpe, trading capacity or grounds for selecting anything.
[[nodiscard]] atx::core::Result<StageResult> run_equity_ic(const RunConfig &config);

// The cash consideration this stage supplies to the engine for one evidenced
// terminal event observed at `session_key_ns` (design §3.8, ruling AR-2). The
// engine never derives a terminal return: it takes this amount per cell.
//
// Ruling AR-2, the whole of it: a special dividend is included ONLY when the
// cell's observation date is at or before the event's record date. A cell that
// buys at a close AFTER the record date receives the consideration alone —
// "Its entitlement must not be inferred from holdings at a later missing mark"
// (2026-09-20-equity-required-marks-audit.md:14).
//
// Returns 0.0 for any id the audit does not evidence, PCS 146189 included: that
// id is flagged terminal WITHOUT evidence and its cells are dropped and counted,
// never priced (ruling AR-1). Declared here so the record-date boundary is
// directly pinnable from both sides rather than only through an aggregate.
[[nodiscard]] atx::f64 equity_ic_terminal_value(atx::i64 security_id, atx::i64 session_key_ns);

} // namespace atx::impl
