#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/point_in_time_universe.hpp"
#include "panel_artifact.hpp"

namespace atx::impl {

inline constexpr atx::usize kEquityBaselineWarmup = 256;
inline constexpr std::array<std::string_view, 2> kEquityBaselineDsl{
    "ts_mean(delay(close, 21) / delay(close, 252) - 1, 5)",
    "ts_mean(delay(close, 21) / delay(close, 126) - 1, 5)"};
inline constexpr std::array<std::string_view, 2> kEquityBaselineSignalNames{
    "momentum_252", "momentum_126"};

// Checkpoint 17 signal families (Stage 3, batch 1). Adding a family is ONE DSL line
// here plus ONE name below; the IC stage derives the family program's warmup from
// its required lookback and the declared configuration count from this size.
// Signs follow the literature so that a POSITIVE spread is the documented premium:
// reversal buys past losers, high52 buys names near their 52-week high, low/idio vol
// buys the least volatile names, Amihud buys the most illiquid, volume-shock buys
// abnormal volume. Every family reads only close, raw_close and volume.
// Checkpoint 20: the 15 families declared at checkpoints 17-19 are RETAINED
// (re-measured on the liquidity-floored universe; not counted again) and ONE new
// configuration is declared: the equal-weight cross-sectional rank blend of
// residual momentum and vol-scaled momentum. ret := close / delay(close, 1) - 1.
// Long-term reversal (1,260-row lookback) still does not fit the 256-row warmup.
inline constexpr std::array<std::string_view, 29> kEquityFamilyDsl{
    "delay(close, 21) / close - 1",
    "delay(close, 5) / close - 1",
    "close / ts_max(close, 252)",
    "0 - ts_std(close / delay(close, 1) - 1, 63)",
    "0 - ts_std(normalize(close / delay(close, 1) - 1), 63)",
    "ts_mean(abs(close / delay(close, 1) - 1) / (raw_close * volume), 21)",
    "log(volume / ts_mean(volume, 63))",
    "close / delay(close, 5) - 1",
    "ts_std(close / delay(close, 1) - 1, 63)",
    "0 - log(volume / ts_mean(volume, 63))",
    "(delay(close, 21) / delay(close, 252) - 1) / ts_std(close / delay(close, 1) - 1, 252)",
    "delay(ts_mean(normalize(close / delay(close, 1) - 1), 231) / "
    "ts_std(normalize(close / delay(close, 1) - 1), 231), 21)",
    "0 - ts_max(close / delay(close, 1) - 1, 21)",
    "0 - log(ts_mean(raw_close * volume, 21))",
    "rank(delay(ts_mean(normalize(close / delay(close, 1) - 1), 231) / "
    "ts_std(normalize(close / delay(close, 1) - 1), 231), 21)) + "
    "rank((delay(close, 21) / delay(close, 252) - 1) / ts_std(close / delay(close, 1) - 1, 252))",
    // Checkpoint 20 (batch 3a): earnings-announcement and options-implied-vol
    // families from the context's optional fields (earnFlag, atmCenI_*, sector).
    // earnFlag is NaN on non-event sessions, so each read uses the NaN->0 form
    // (ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag).
    "ts_sum(delay(normalize(close / delay(close, 1) - 1), 1) * "
    "min(1, (ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag) + "
    "delay((ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag), 1) + "
    "delay((ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag), 2)), 21)",
    "ts_sum(delay(normalize(close / delay(close, 1) - 1), 1) * "
    "min(1, (ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag) + "
    "delay((ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag), 1) + "
    "delay((ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag), 2)), 63)",
    "ts_sum(delay((ts_count_nans(earnFlag, 1) > 0 ? 0 : earnFlag), 178), 21)",
    "log(atmCenI_21d) - log(ts_std(close / delay(close, 1) - 1, 21))",
    "log(atmCenI_126d / atmCenI_21d)",
    "log(atmCenI_21d / delay(atmCenI_21d, 21))",
    "group_neutralize(ts_mean(delay(close, 21) / delay(close, 252) - 1, 5), sector)",
    // Checkpoint 21 (batch 3c): OHLC-range families from the adjusted high/low/open
    // fields (adjusted like close, history_panel.hpp:34-36; price <= 0 / non-finite
    // -> NaN, never 0, so no inf path). Signs pre-registered in progress.md R21-2.
    "delay(ts_sum(log(close / open), 231), 21)",
    "delay(ts_sum(log(close / open), 231) - ts_sum(log(open / delay(close, 1)), 231), 21)",
    "0 - ts_mean(0.5 * log(high / low) * log(high / low) - "
    "0.3862944 * log(close / open) * log(close / open), 63)",
    "ts_mean((log(delay(close, 1)) - 0.5 * log(delay(high, 1) * delay(low, 1))) * "
    "(log(delay(close, 1)) - 0.5 * log(high * low)), 63)",
    // Checkpoint 22 (FINRA short interest via the R21-3 as-of field si_shares;
    // available from 2018-01-10 so finite only in the 2018/2019 cells). Signs
    // pre-registered in progress.md R22-1.
    "0 - si_shares * raw_close / market_cap",
    "0 - si_shares / ts_mean(volume, 21)",
    "0 - (si_shares - delay(si_shares, 63)) * raw_close / market_cap"};
inline constexpr std::array<std::string_view, 29> kEquityFamilySignalNames{
    "reversal_21",         "reversal_5",           "high52_proximity",
    "low_vol_63",          "idio_vol_63",          "amihud_21",
    "volume_shock_63",     "continuation_5",       "high_vol_63",
    "volume_shock_neg_63", "momentum_volscaled_252", "residual_momentum_252",
    "max_ret_21",          "low_dollar_volume_21", "mom_resid_vs_blend",
    "abr_21",              "abr_63",               "earn_premium",
    "iv_rv_log",           "iv_slope",             "d_iv_21",
    "mom252_sector_neutral", "intraday_mom_252",     "day_minus_night_252",
    "gk_low_vol_63",         "chl_spread_63",         "short_interest_ratio",
    "days_to_cover_21",      "d_sir_63"};
// Leading entries that were declared at an earlier checkpoint and are only
// re-measured here; the trial count of a run excludes them. From checkpoint 21 on
// this includes checkpoint 20's 7 families (22 retained = checkpoints 17-20); from
// checkpoint 22 on also checkpoint 21's 4 families (26 retained = checkpoints 17-21).
inline constexpr atx::usize kEquityFamilyRetainedCount = 26;
// Ancillary per-cell series for the cost/capacity view (R17-8): mean dollar
// volume over the prior 21 sessions. Evaluated on the same mask as the families;
// never a signal, never counted as a trial.
inline constexpr std::string_view kEquityDollarAdvDsl = "ts_mean(raw_close * volume, 21)";
inline constexpr std::string_view kEquityDollarAdvName = "dollar_adv_21";
static_assert(kEquityFamilyDsl.size() == kEquityFamilySignalNames.size());

enum class EquityBaselineObservationBasis {
    Unspecified,
    ArchiveRawVolumeAndPointwiseAdjustedCloseV1
};

struct EquityBaselineWindow {
    atx::i64 begin_session_key{}; // Must be an exact source observation.
    atx::i64 end_exclusive_session_key{}; // Label filter; source coverage checked by caller.
};

// ---------------------------------------------------------------------------
//  W0-I0b / D-12 — point-in-time membership for the equity views.
//
//  A context built with `panel --universe-membership` restricts its universe to the
//  UNION of the membership cut over the whole panel window (plus the last rebalance
//  before --universe-eval-start). That union is a within-window selection look-ahead:
//  a name that joins in November is already admitted in January. Under AsOfV2 the
//  views admit a cell only when the last rebalance whose effective session is on or
//  before the cell's session lists the security (the membership.bin semantics the
//  equity-universe stage publishes). ContextYearUnionV1 reproduces the pre-W0 mask
//  exactly, for re-deriving frozen artifacts. Frozen integer values.
//
//  The as-of mask gates admission AND every DSL-internal cross-sectional op,
//  including those inside a rolling feature's warmup window. The VM loads each
//  column's observed history independently: a joiner's price-only time-series
//  feature retains its public past prices. A cross-sectional feature is NaN while
//  the name is not a member; rolling that feature observes the actual historical
//  membership and cannot invent pre-entry normalized values.
// ---------------------------------------------------------------------------
enum class EquityMembershipRule : atx::u8 {
    ContextYearUnionV1 = 1, // pre-W0: the context mask alone (year-union allow-list)
    AsOfV2 = 2,             // default: the context mask AND as-of membership
};

// "context-year-union-v1" / "as-of-pit-membership-v2" (the published labels).
[[nodiscard]] std::string_view equity_membership_rule_label(EquityMembershipRule rule) noexcept;
// CLI spelling: "year-union-v1" | "as-of-v2". Err(InvalidArgument) otherwise.
[[nodiscard]] atx::core::Result<EquityMembershipRule>
parse_equity_membership_rule(std::string_view text);

// One cut of a membership image, reduced to what an as-of lookup needs.
struct EquityAsOfMembership {
    std::vector<atx::i64> effective_session_keys;    // strictly ascending
    std::vector<std::vector<atx::i64>> security_ids; // parallel; each strictly ascending
    // True iff the last rebalance with effective_session_key <= session_key lists
    // `security_id`. False before the first effective rebalance.
    [[nodiscard]] bool member(atx::i64 session_key, atx::i64 security_id) const noexcept;
};

// Reduce `image` to cut `cut` (index = top_n_index * band_count + band_index).
// Err(InvalidArgument) for an out-of-range cut, no rebalances, or two rebalances
// sharing an effective session key.
[[nodiscard]] atx::core::Result<EquityAsOfMembership>
equity_asof_membership(const atx::engine::data::PitMembershipImage &image, atx::usize cut);

// "<top_n>:<units>.<hh>" (e.g. "3000:0.00", the context recipe's universe_cut) ->
// the cut index inside `image`. Err(InvalidArgument) when malformed or absent.
[[nodiscard]] atx::core::Result<atx::usize>
equity_membership_cut_index(const atx::engine::data::PitMembershipImage &image,
                            std::string_view cut_text);

struct LoadedEquityMembership {
    EquityAsOfMembership asof;
    std::string sha256;     // of the exact bytes decoded
    atx::usize cut_index{};
    atx::usize rebalances{};
};

// Read, hash and decode membership.bin at `path` and reduce it to `cut_text`.
// Err(IoError) when unreadable or larger than 512 MiB; codec errors propagate.
[[nodiscard]] atx::core::Result<LoadedEquityMembership>
load_equity_membership(const std::string &path, std::string_view cut_text);

// The stage-boundary rule shared by equity-baseline and equity-ic. A context whose
// recipe declares a membership restriction (sha256 + cut) needs, under AsOfV2, the
// very image it names (`membership_path`, hash-checked against `recipe_sha256`);
// under ContextYearUnionV1 no image may be supplied. A context without a restriction
// accepts no image either. Returns the loaded membership when one applies.
[[nodiscard]] atx::core::Result<std::optional<LoadedEquityMembership>>
resolve_equity_membership(bool context_has_membership, std::string_view recipe_sha256,
                          std::string_view recipe_cut, EquityMembershipRule rule,
                          const std::string &membership_path);

// W0-I0b / I-17 — publication order shared by the equity stages. `write_manifest`
// publishes the manifest; only after it succeeds is `<directory>/.pending` removed.
// A failed write leaves `.pending` in place (the directory still advertises itself
// as incomplete) and returns that error. Err(IoError) when `.pending` is absent or
// cannot be removed.
[[nodiscard]] atx::core::Status
publish_manifest_then_release_pending(const std::filesystem::path &directory,
                                      const std::function<atx::core::Status()> &write_manifest);

struct EquityBaselineConfig {
    EquityBaselineWindow evaluation;
    EquityBaselineObservationBasis observation_basis{EquityBaselineObservationBasis::Unspecified};
    atx::u64 max_additional_bytes{3'000'000'000ULL};
    // Checkpoint 19 liquidity floor (R18-5): a cell is admitted only if the mean
    // of raw_close * volume over the trailing `dollar_adv_window` sessions ending
    // at t (inclusive, all finite and > 0) is >= min_dollar_adv. 0 = no floor
    // (checkpoints 14-18 behaviour, bit-identical).
    atx::f64 min_dollar_adv{0.0};
    atx::usize dollar_adv_window{21};
    // D-12. With AsOfV2 and a membership, a cell must also be an as-of member; the
    // context's instrument ids must then be canonical integers. With no membership
    // (a context whose universe is a daily screen, already as-of) the rules agree.
    // ContextYearUnionV1 with a membership is refused (InvalidArgument).
    EquityMembershipRule membership_rule{EquityMembershipRule::AsOfV2};
    std::optional<EquityAsOfMembership> membership;
};

struct EquityBaselinePlan {
    atx::usize feature_begin{}; // Original context row, exactly 256 before evaluation_begin.
    atx::usize evaluation_begin{};
    atx::usize evaluation_end{}; // Exclusive original context row.
    atx::i64 first_evaluation_session_key{};
    atx::i64 last_evaluation_session_key{};
    atx::usize feature_cells{};
    atx::usize evaluation_cells{};
    atx::usize vm_slots{};
    atx::u64 vm_slot_bytes{};
    atx::u64 additional_array_bytes{};
};

struct EquityBaselineEvaluation {
    // Numeric columns borrow the input artifact. Keep that artifact and its
    // underlying owned/borrowed storage alive and unchanged until this view dies.
    atx::engine::alpha::Panel panel;
    // Owned evaluation-only signals, both NaN unless decision-eligible and ready.
    atx::engine::alpha::SignalSet signals;
    std::vector<atx::i64> session_keys;
    std::vector<atx::usize> context_rows; // Evaluation row -> original context row.
    std::vector<atx::usize> ready_by_observation;
    std::vector<atx::usize> admitted_by_observation;
    EquityBaselinePlan plan;
    atx::usize observed_feature_cells{};
    atx::usize eligible_evaluation_cells{}; // Before common readiness.
    atx::usize liquidity_floor_rejected_cells{}; // eligible AND ready, but ADV below the floor
    atx::usize ready_evaluation_cells{}; // Both signals finite, before eligibility.
    atx::usize admitted_evaluation_cells{};
    // D-12: context-eligible cells refused because the security was not an as-of
    // member on that session (0 under ContextYearUnionV1 or without a membership).
    atx::usize membership_rejected_cells{};
};

// Validate identified axes/shape, source-basis declaration, exact window and
// complete 256-observation context. Compile only the two immutable DSLs above.
// Returns checked VM/cell allocation counts without evaluating market values.
// End is a half-open label filter, not proof of source/calendar completeness.
// The publishing driver must verify requested coverage against bound source
// metadata; the plan exposes the actual first/last selected observations.
// additional_array_bytes covers VM slots, masks, signals (including overlap),
// output indices and bounded time-series scratch. Input storage, compiler/container
// overhead and allocator/runtime RSS are excluded and must be budgeted separately.
[[nodiscard]] atx::core::Result<EquityBaselinePlan>
plan_equity_baseline(const PanelArtifact &context, const EquityBaselineConfig &config);

// Internally creates a borrowed feature view with numerical observation validity,
// independent of historical decision eligibility. Requires finite positive close
// and raw_close, finite nonnegative raw volume, and representable dollar volume.
// Optional observed must be exactly 0/1; 0 suppresses observations, and 1 with bad
// numerics is rejected. Otherwise invalid numerics represent observation gaps.
// Current decision eligibility must imply numerical observation validity.
// Only the returned evaluation rows can enter WeightPolicy/extract_streams:
// signals are explicitly NaN-gated AND the returned Panel mask uses the same gate.
// No fitting, weighting, orders, file writes, calendar inference or economic/PIT
// certification occurs. Allocation failures may throw; inputs are never changed.
[[nodiscard]] atx::core::Result<EquityBaselineEvaluation>
evaluate_equity_baseline(const PanelArtifact &context, const EquityBaselineConfig &config);

// A temporary artifact would leave the returned numeric columns dangling.
atx::core::Result<EquityBaselineEvaluation>
evaluate_equity_baseline(const PanelArtifact &&, const EquityBaselineConfig &) = delete;

struct EquityFamilyEvaluation {
    // Evaluation-only signals on the baseline's axes: NaN unless the cell is admitted
    // by the baseline mask AND this family's own value is finite (per-signal readiness;
    // the IC engine excludes and counts non-finite cells, never imputes).
    atx::engine::alpha::SignalSet signals;
    atx::usize warmup{}; // Derived: the compiled program's required lookback.
    atx::usize vm_slots{};
    atx::u64 additional_array_bytes{};
    std::vector<atx::usize> finite_admitted_cells; // Per signal.
};

// Evaluate the DSL family list over the same context and evaluation window as a
// completed baseline evaluation. The warmup is the program's required lookback,
// which must fit before the evaluation window; the admission gate is the baseline's
// (universe eligibility AND both momentum signals ready), so every family is
// measured on exactly the checkpoint-16 universe. Budget covers VM slots + signals.
// Under AsOfV2 with membership, every cross-sectional opcode uses the as-of set
// for its own feature date, including warmup dates. Raw time-series observations
// are retained independently. ContextYearUnionV1 preserves the old VM behavior.
[[nodiscard]] atx::core::Result<EquityFamilyEvaluation>
evaluate_equity_families(const PanelArtifact &context, const EquityBaselineConfig &config,
                         const EquityBaselineEvaluation &baseline,
                         std::span<const std::string_view> dsl,
                         std::span<const std::string_view> names);

} // namespace atx::impl
