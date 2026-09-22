#pragma once

// Append-only, SHA-256 hash-chained trial ledger for pre-registered trials.
//
// Frozen design: atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md
// §4.5 (schema, the two lines per run, the chain, the exact key set and ordering,
// "Nothing is rewritten"), §8 T4 (scope and acceptance), §11.1 OQ-3 (the ledger lives at
// atx-engine/reviews/trial-ledger.jsonl; this library takes the path, the stage supplies
// it). One JSON object per line, UTF-8, LF endings, no trailing whitespace.
//
// CHAIN RULE, quoted from §4.5: "Each line's `prev_sha256` is the SHA-256 of the
// *previous line's bytes including its LF*; the first line uses 64 zeros." `head_sha256`
// in the sidecar is read the same way, so it is exactly the `prev_sha256` that the next
// appended line must carry, and the empty ledger's head is the 64-zero genesis.
//
// WHAT IS AND IS NOT DETECTABLE. §4.5 claims "A deletion or in-place edit breaks the
// chain and is detectable". The chain alone does not deliver that for the ledger's TAIL:
// line i is checked against line i-1, so the newest line's bytes are compared with
// nothing, and deleting the newest k lines leaves a self-consistent chain. The sidecar
// `<stem>.manifest.json` is therefore a REQUIRED verification input, not a convenience:
//   * an edit to ANY line, the last included .............. detected (chain or head)
//   * deletion of any line, the last included ............. detected (chain or count)
//   * a truncated / torn final line ....................... detected (ParseError, with
//                                                           the repair offset)
//   * a sidecar missing beside a non-empty ledger ......... detected (NotFound; writing
//                                                           is refused)
//   * a tail edit WITH a consistently regenerated sidecar . NOT detected — the one
//     documented limit. Both files are git-tracked, so such an edit is still visible in
//     history; `TrialLedger.LastLineDeletionWithAConsistentSidecarIsTheDocumentedLimit`
//     pins the behaviour so it cannot regress into a silent surprise.
//
// CONCURRENCY. `append_trial` is the only mutator and it is SINGLE-WRITER by exclusive
// lock file `<ledger_path>.lock` (`fopen(…, "wx")`), held across verify + write + sidecar
// refresh. A second concurrent writer is refused with `Unavailable` naming the lock path:
// this API never waits and never removes a lock it did not create, so a lock left behind
// by a dead writer is a deliberate stop sign. The read APIs take no lock and may observe
// a torn tail, which they report rather than repair.
//
// REPAIR. Nothing in this library ever rewrites or shortens the ledger. A torn final line
// (an interrupted append) is repaired by hand: truncate the ledger to the byte offset the
// `ParseError` message names — the end of the last complete line — then regenerate the
// sidecar with that line count and the SHA-256 of the last line including its LF.
//
// WHY THE STRUCT DEFAULTS CARRY VALUES. Every defaulted member below is a frozen §4.5
// recipe constant (with §11.2 AR-3/AR-11, §11.3 R-A/R-B/R-C and §11.4 applied). They are
// defaults rather than stage arguments precisely so a stage cannot silently drift a
// pre-registered constant: changing one is a new trial (§4.4), never an edit to a line
// that has already been appended.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl {

inline constexpr std::string_view kTrialLedgerSchema = "atx-trial-ledger-v1";

// §4.5: "the first line uses 64 zeros".
inline constexpr std::string_view kTrialLedgerGenesisSha256 =
    "0000000000000000000000000000000000000000000000000000000000000000";

// One `parents` element. Exactly one of artifact_id / sha256 must be non-empty: an
// artifact parent is named by its store id, a file parent by its content digest.
struct TrialLedgerParent {
    std::string role;
    std::string artifact_id;
    std::string sha256;
};

// One `recipe.signals` element (§4.1). The DSL text and its digest are recorded so a
// later reader can prove which expression was pre-registered.
struct TrialLedgerSignal {
    std::string name;
    std::string dsl;
    std::string dsl_sha256;
};

// §4.5 `recipe.bootstrap`. block_lens is L_h = max(5, ceil(h/2)) evaluated on the frozen
// horizon set (ruling AR-3); stream_key is the six-field byte-aligned layout (ruling R-C).
struct TrialLedgerBootstrap {
    atx::i64 draws{2000};
    atx::u64 seed{20260920};
    std::string block_len_rule{"max(5, ceil(h/2))"};
    std::vector<atx::i64> block_lens{5, 5, 5, 11, 32};
    std::vector<atx::f64> percentiles{2.5, 97.5};
    std::string reportable_rule{"draws>=1 && n>=20 && floor(n/L)>=10"};
    std::vector<std::pair<std::string, atx::i64>> statistic_ids{
        {"IcMean", 0}, {"Icir", 1}, {"RankIcMean", 2},
        {"RankIcir", 3}, {"SpreadGross", 4}, {"SpreadNet", 5}};
    std::string stream_key{
        "seed ^ (stat<<8) ^ (horizon_index<<16) ^ (signal<<24) ^ (variant_id<<32)"
        " ^ (restriction_id<<40) ^ (sample_id<<48)"};
    std::vector<std::pair<std::string, atx::i64>> sample_ids{{"full", 0}, {"common", 1}};
    std::vector<atx::i64> predicted_null_horizons{63};
};

// §4.5 `recipe.cost`, with ruling AR-11 (the priced book is the gross-2.0 book the spread
// describes) and ruling AR-3 part 2 (calendar days from the panel's own session keys).
struct TrialLedgerCost {
    atx::f64 trade_bps{5.0};
    std::string trade_bps_source{"constexpr EquityAllocationConfig{}.trade_bps"};
    atx::f64 annual_borrow_bps{365.0};
    std::string annual_borrow_bps_source{"deployed equity-book run config (handoff)"};
    atx::f64 short_leg_gross{1.0};
    atx::f64 priced_book_gross{2.0};
    std::string decile_weights{"+1.0/n_top, -1.0/n_bottom"};
    std::string borrow_day_convention{"calendar_days_from_session_keys"};
    std::string borrow_day_formula{
        "(session_keys[t+h]-session_keys[t])/86400000000000 integer division"};
    std::string provenance{
        "constants-restated-not-a-replay-call; replay.cpp borrow_charge is file-local"};
};

// §4.5 `recipe.seal` (§5, policy RejectSealedV1).
struct TrialLedgerSeal {
    std::string policy{"RejectSealedV1"};
    std::string validation_begin{"2020-01-01"};
    std::string sealed_begin{"2023-01-01"};
};

// §4.5 `recipe`. signals is the only member a caller must supply; everything else is a
// frozen constant (see the file header on why these are defaults).
struct TrialLedgerRecipe {
    std::vector<TrialLedgerSignal> signals;
    std::vector<atx::i64> horizons{1, 5, 10, 21, 63};
    atx::i64 quantiles{10};
    TrialLedgerBootstrap bootstrap{};
    std::vector<atx::i64> autocorr_lags{1};
    std::string turnover_source{"rho_rank"};
    std::vector<std::string> forward_variants{"DropMissingForward", "IncludeAuditedTerminalV1"};
    std::vector<std::string> restrictions{"full", "_ex34"};
    std::vector<std::string> samples{"full", "_common"};
    std::string common_sample_rule{
        "prefix t < (T - max(H)); incomplete prefix => _common unreportable"};
    std::string alignment{"signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1"};
    std::string net_spread_rule{
        "per-date net_h(t) = gross_h(t) - cost_drag_h(t); no rebalance grid"};
    TrialLedgerCost cost{};
    TrialLedgerSeal seal{};
};

// §4.5 `window`. Dates are the caller's own YYYY-MM-DD text; this library records them
// verbatim and never reinterprets them as a calendar.
struct TrialLedgerWindow {
    std::string start;
    std::string end_exclusive;
    atx::i64 observations{};
};

// §4.5 `source_exclusions`, carrying the §11.3 R-A partition: 34 = 3 terminal-cash-event
// hypothesis + 2 evidenced non-terminal + 29 unclassified, with PCS (146189) the one
// unevidenced-terminal id. ex34_restriction_ids is caller-supplied: it is the loaded
// required-mark set, which the stage asserts against these counts before appending.
struct TrialLedgerSourceExclusions {
    atx::i64 required_mark_id_count{34};
    std::vector<atx::i64> terminal_hypothesis_ids{37648, 35715, 39970};
    std::string terminal_hypothesis_note{
        "terminal-cash-event hypothesis per the audit's wording,"
        " not a settled classification"};
    std::vector<std::pair<std::string, std::string>> terminal_evidenced_record_dates{
        {"35715", "2013-10-28"}};
    std::vector<atx::i64> evidenced_non_terminal_ids{150340, 351548};
    atx::i64 unclassified_id_count{29};
    std::vector<atx::i64> terminal_unevidenced_ids{146189};
    std::vector<std::string> ex34_restriction_ids;
    std::string pcs_statement{"PCS is never applied; admission remains rejected"};
};

// §4.5 `fit_boundary`. Stage 1 fits nothing, so the defaults are the Stage-1 truth.
struct TrialLedgerFitBoundary {
    std::string fit_kind{"unfit-no-fitting-performed"};
    atx::i64 fitted_observations{0};
};

// §4.5 `runtime`. Both members are null on the pre-registration line and filled on the
// completion line; an empty optional serializes as JSON null.
struct TrialLedgerRuntime {
    std::optional<atx::f64> wall_seconds;
    std::optional<atx::u64> peak_working_set_bytes;
};

// §4.5 `result`. outcome is "pending" | "completed" | "failed" and must agree with the
// entry's status; the matching digest is required on a terminal line.
struct TrialLedgerResult {
    std::string outcome{"pending"};
    std::optional<std::string> manifest_sha256;
    std::optional<std::string> failure_sha256;
};

// One ledger line. Serialization emits exactly these keys, in exactly this order (§4.5);
// there is no optional key and no key is ever omitted.
struct TrialLedgerEntry {
    std::string schema{std::string(kTrialLedgerSchema)};
    std::string trial_id;
    std::string appended_utc;
    // Set by append_trial from the live chain head; any caller value is overwritten.
    // Callers that serialize without appending must set it themselves.
    std::string prev_sha256;
    atx::i64 checkpoint{};
    std::string purpose;
    // "pre-registered" (before the run) | "completed" | "failed" (after it). §4.5:
    // "Two lines per run, never one."
    std::string status;
    atx::i64 trial_count_declared{};
    std::vector<TrialLedgerParent> parents;
    TrialLedgerRecipe recipe{};
    TrialLedgerWindow window{};
    TrialLedgerSourceExclusions source_exclusions{};
    TrialLedgerFitBoundary fit_boundary{};
    TrialLedgerRuntime runtime{};
    TrialLedgerResult result{};
    std::string producer_executable_sha256;
    std::string notes;
};

// Chain state of an existing ledger. head_sha256 is the genesis constant when the ledger
// is absent or empty, so a caller never special-cases the first append.
struct TrialLedgerHead {
    atx::usize lines{};
    std::string head_sha256;
};

// Canonical, byte-deterministic serialization of one line, WITHOUT the trailing LF.
// Validates the entry first (required fields, status/outcome agreement, finite reals,
// plain-text object keys) and returns InvalidArgument on a violation. The same entry
// always yields the same bytes:
// fixed key order, locale-independent integer and real formatting.
[[nodiscard]] atx::core::Result<std::string> serialize_trial_entry(const TrialLedgerEntry &entry);

// Walk an existing ledger, check every link, then anchor the tail against the sidecar.
// Returns the line count and the head digest. A ledger and sidecar that are both absent
// are a valid empty chain (genesis head).
//   ErrorCode::Internal   — a line's prev_sha256 does not match the previous line's
//                           digest (the message names the 0-based line index), or the
//                           sidecar's {lines, head_sha256} disagree with the walk (the
//                           message names both the recorded and the walked values).
//   ErrorCode::ParseError — a truncated final line (the message carries the byte offset
//                           to truncate to), an empty line, a CR, a line whose
//                           prev_sha256 is absent or not 64 lowercase hex, or a sidecar
//                           that cannot be read as {lines, head_sha256, created_utc}.
//   ErrorCode::NotFound   — the sidecar is missing beside a non-empty ledger, so the
//                           tail cannot be anchored (and append is therefore refused).
//   ErrorCode::IoError    — a file exists but cannot be read.
[[nodiscard]] atx::core::Result<TrialLedgerHead>
verify_trial_ledger(const std::string &ledger_path);

// Take the single-writer lock, re-verify the whole chain, then append exactly one line
// and refresh the sidecar. Refuses to write when the chain is broken, truncated or
// unanchored, propagating the verify error unchanged (§4.5: append "re-verifies the whole
// chain before writing and fails with ErrorCode::Internal on a break"). Opens in append
// mode only; it never truncates and never rewrites a prior line. Returns the appended
// entry with prev_sha256 stamped.
//   ErrorCode::Unavailable — `<ledger_path>.lock` already exists; no wait, no takeover.
// The write is fail-closed, not atomic: see REPAIR in the file header for the torn-tail
// contract. On the FIRST append both the ledger and its sidecar are created.
[[nodiscard]] atx::core::Result<TrialLedgerEntry>
append_trial(const std::string &ledger_path, const TrialLedgerEntry &entry);

// Total trial count declared for one checkpoint, over a chain verified exactly as
// verify_trial_ledger verifies it (sidecar anchor included — this is the field a tail
// edit would target): the sum of trial_count_declared over
// every "pre-registered" line whose checkpoint matches. This is the N that
// eval::deflated_sharpe must be fed (§4.4). Summing is the conservative reading — a
// retried run is pre-registered again under a new trial_id, and §4.4 requires a later
// selection to be ledgered as a new trial. NotFound when the checkpoint has no line.
[[nodiscard]] atx::core::Result<atx::u64>
declared_trials_for_checkpoint(const std::string &ledger_path, atx::i64 checkpoint);

// Checkpoint 15 (design 2026-09-20-iteration15 §5.7, ruling R15-3): the explicit
// NON-TRIAL purpose allow-list. A line whose purpose is listed here declares no forecast
// trial and MUST carry trial_count_declared == 0; every other purpose must declare a
// positive count exactly as before. Growing this list is a design change, never a
// convenience: a purpose added here removes that purpose's lines from every N_k sum.
inline constexpr std::array<std::string_view, 1> kNonTrialPurposes = {
    "point-in-time-universe-construction"};
[[nodiscard]] bool is_non_trial_purpose(std::string_view purpose);

// Number of "pre-registered" lines carrying `checkpoint`, over the same verified walk
// declared_trials_for_checkpoint uses (§5.7). Ok(0) when none: unlike the trial sum, an
// absent checkpoint is a count, not NotFound, because the cp15 trial_id ordinal is
// 1 + this value and the cp14 `declared / N` rule cannot divide by a declared 0.
[[nodiscard]] atx::core::Result<atx::u64>
pre_registered_lines_for_checkpoint(const std::string &ledger_path, atx::i64 checkpoint);

// Sidecar path for a ledger: "<stem>.manifest.json" when the ledger ends in ".jsonl",
// otherwise "<path>.manifest.json" (§4.5 names trial-ledger.manifest.json).
[[nodiscard]] std::string trial_ledger_manifest_path(const std::string &ledger_path);

} // namespace atx::impl
