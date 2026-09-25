#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
//
// W0-I0b changes (each keeps the pre-W0 behaviour reachable for frozen artifacts):
//  * E-18: `--min-names-per-date` (default 50; checkpoint 14 used 2).
//  * E-09: `--ic-execution-delay` (default 1: the return runs from the entry close
//    t+1; 0 needs --allow-same-close) and the _common prefix is T - (max(H) + delay).
//  * E-02: `--ic-block-len-rule` (default two-horizon-v2; half-horizon-v1 is pre-W0).
//  * D-12: admission is as-of point-in-time membership (`--membership`), see
//    equity_baseline_views.hpp; `--membership-rule year-union-v1` is the pre-W0 mask.
//  * I-15: terminal pricing comes from a terminal-return table (`--terminal-returns`,
//    data from W2-D2); without one, the frozen 2013 table below applies. The 2013
//    required-mark audit is optional: when absent, the ex34 restriction is empty.
[[nodiscard]] atx::core::Result<StageResult> run_equity_ic(const RunConfig &config);

// ---------------------------------------------------------------------------
//  Terminal-return table (W0-I0b / I-15 interface; the data arrives in W2-D2).
//
//  One row per security with a terminal (delisting / cash-merger) event. The engine
//  never derives a terminal return: this table supplies the terminal cash VALUE per
//  share for every cell of the security, either directly (`terminal_value`) or as a
//  return on the security's last finite raw close in the evaluated view
//  (`terminal_return`, value = last_raw_close * (1 + return)). A special dividend is
//  added to the value for sessions at or before `record_session_key` (ruling AR-2).
//  An UNEVIDENCED row flags the security terminal without pricing it: its cells are
//  dropped and counted, never priced (ruling AR-1, the PCS rule).
// ---------------------------------------------------------------------------
struct EquityTerminalEvent {
    atx::i64 security_id{};
    std::optional<atx::f64> terminal_value;  // cash per share (exactly one of the two)
    std::optional<atx::f64> terminal_return; // on the last finite raw close, > -1
    atx::f64 special_dividend{};             // >= 0; needs record_session_key when > 0
    std::optional<atx::i64> record_session_key; // UTC-midnight nanoseconds
    std::string record_date;                    // the same date as YYYY-MM-DD, or empty
    bool evidenced{};
    std::string source;
};

struct EquityTerminalReturnTable {
    std::vector<EquityTerminalEvent> events; // unique security ids, in source order
    std::string source;                      // provenance label
    std::string sha256;                      // of the CSV bytes; empty for the frozen table
    [[nodiscard]] const EquityTerminalEvent *find(atx::i64 security_id) const noexcept;
};

// The checkpoint-14 table: HNZ, DELL (with its record-dated special dividend) and
// MOLX evidenced, PCS 146189 flagged terminal without evidence. Source:
// atx-engine/reviews/2026-09-20-equity-required-marks-audit.md:13-15.
[[nodiscard]] EquityTerminalReturnTable equity_ic_frozen_terminal_table();

// CSV contract (header required, exactly these columns, no quoting):
//   security_id,terminal_value,terminal_return,special_dividend,record_date,evidenced,source
// terminal_value / terminal_return: exactly one non-empty on an evidenced row, both
// empty on an unevidenced row; values finite, terminal_value > 0, return > -1;
// special_dividend empty or finite >= 0 (> 0 requires record_date YYYY-MM-DD);
// evidenced true|false; ids unique canonical integers. Err(ParseError) otherwise.
[[nodiscard]] atx::core::Result<EquityTerminalReturnTable>
parse_equity_terminal_table(std::string_view csv_text);

// Read (<= 64 MiB), hash and parse a table file. Err(IoError) when unreadable.
[[nodiscard]] atx::core::Result<EquityTerminalReturnTable>
load_equity_terminal_table(const std::string &path);

// Terminal cash value of `event` for a cell observed at `session_key_ns`, before any
// return-based resolution: the value (or 0 for a return row / unevidenced row) plus
// the special dividend when the session is at or before the record date.
[[nodiscard]] atx::f64 equity_terminal_cash(const EquityTerminalEvent &event,
                                            atx::i64 session_key_ns) noexcept;

// The cash consideration this stage supplies to the engine for one evidenced
// terminal event of the FROZEN 2013 table observed at `session_key_ns` (design §3.8,
// ruling AR-2). Kept for the checkpoint-14 pins.
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
