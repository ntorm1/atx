#pragma once

// PRIVATE to atx-impl. Shared seams of the saved-blend target replay, used only by
// strategy_target_replay.cpp (definitions), strategy_nav_replay.cpp and their tests.
// The definitions forward to the target replay's file-local implementations, so the
// NAV replay reuses the exact loader, validation and target arithmetic rather than a
// copy. No nlohmann/SHA dependency here; the manifest travels as its JSON text.

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/target_shaping.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::detail {
// Construction state of the desired target across the rebalance decisions of one replay (or
// supplied by the decide path), shared by every book. Owned by the caller of form_desired.
// hold: hold-band-v1 (v8 R-4) per-name state; empty until the kernel first runs.
// adv_dollars, nav: adv-hold-v1 (v8 R-5) inputs of the decision, filled by the NAV replay before
// form_desired (one raw-dollar ADV per name, read where the desired weight is nonzero; the
// run's NAV); caps: the pass's scratch.
// sigma: inv-vol-v1 (v8 X) input of the decision, filled by the NAV replay before form_desired
// (one execution volatility per member, NaN for the cost model's fallback); sigma_sorted: the
// kernel's scratch.
struct DesiredState {
  atx::engine::book::HoldBandState hold;
  std::vector<atx::f64> adv_dollars;
  atx::f64 nav{};
  std::vector<atx::f64> caps;
  std::vector<atx::f64> sigma, sigma_sorted;
  // two-speed-v1 (v8 Y-5): the virtual fast sleeve F per name (empty until the first rebalance),
  // F entering the latest rebalance (what a scaled book's plan carries: engine::book::
  // two_speed_carry, Ruling PM8-16 #10) and the fast sleeve's desired target of the decision
  // (scratch); parent_desired: the mechanics diagnostic's scratch.
  std::vector<atx::f64> fast, fast_before, fast_desired, parent_desired;
};
// One externally pinned saved blend plus its bound price role, owned. Date-major.
// volume is empty unless requested; present => finite >= 0 raw shares, absent => NaN.
struct LoadedSavedBlend {
  atx::usize dates{}, names{}, begin{}, end{};
  std::vector<atx::f64> signal, close, raw, volume;
  std::vector<atx::u8> member, present;
  std::vector<atx::i64> sessions;
  std::vector<atx::u64> ids;
  // two-speed-v1 (v8 Y-5): the saved sleeves the combined manifest pins (loaded only under it).
  std::vector<atx::f64> sleeve_fast, sleeve_slow, sleeve_fast_share;
  std::string manifest_json; // the pinned combined manifest, re-serialized
  // Borrowed view; valid while *this is alive and unmodified.
  [[nodiscard]] TargetReplayInput view() const;
};
// Pinned load exactly as run_target_replay performs it (combined SHA, role SHA ==
// manifest.role_manifest_sha256, source SHA, axes, member == member&present&close>0).
// Admits equal-family/equal-within and pinned-candidate-weights blends; the latter's
// composition_weights_sha256 stays in manifest_json (published as source_bindings).
// with_volume additionally requires the role, volume_basis "raw-share-volume" and the
// volume contract, and charges 8 more admitted bytes per cell.
[[nodiscard]] atx::core::Result<LoadedSavedBlend> load_saved_blend(
    const TargetReplayRunConfig& cfg, bool with_volume);
// v8 E-25 (nav --label-role, Ruling E-25): a label role marks the NAV books while every decision
// input stays on cfg's role. check_label_role reads the two pinned manifests only (cfg.role_path
// / role_sha256 and `path` / `sha256`; no payload) and refuses, by name ("nav replay:
// --label-role refused: ..."), a label role whose score_end_ns (when declared) is past the
// research seal (atx/engine/data/research_window.hpp), whose manifest keys other than files and
// universe differ from the role's, whose file set or any extent differs, whose file SHA-256
// differs except close.f64, raw_close.f64, volume.f64 and present.u8 (sessions: the dates,
// ids: the instruments, member.u8: the membership), or whose universe block is not absent in
// both or the same id, base role (manifest, member), identity-bridge and SIC-events pins (and
// the role's delisting-stage pin when it has one). member.u8 (and then the manifest key
// score_member_counts) may differ only when the label role declares a delisting-return
// clearing (prepare_recent_research.py DELISTING_RETURN_RULE member[T] = 0):
// universe.delisting.returns_applied true and
// universe.delisting.applied.members_cleared_on_termination_session N > 0; load_label_role
// verifies the N cells. Before all of these, cfg's role itself is refused when it is a
// --delisting-returns build (review B-3: atx::engine::data::refuse_delisting_returns_signal_role,
// its own message), never the label role.
[[nodiscard]] atx::core::Status check_label_role(const TargetReplayRunConfig& cfg,
                                                 const std::string& path,
                                                 const std::string& sha256);
// The label role's marks, owned (date-major, blend geometry): close, raw close and presence.
// label_only_cells counts the cells present here and absent in the role (all rows);
// label_only_scored_cells those in rows [begin, end).
struct LoadedLabelRole {
  std::vector<atx::f64> close, raw;
  std::vector<atx::u8> present;
  atx::usize label_only_cells{}, label_only_scored_cells{};
};
// check_label_role, then (before any label payload is opened) no session of `blend` (the
// role's, loaded with prices) at or after the seal; then close.f64, raw_close.f64, present.u8
// and member.u8 against the label manifest's receipts (SHA-256 and extent), refusing a
// presence/price contract breach, a --role-present cell that is absent or at another close or
// raw close (bits), or member & present & close > 0 differing from blend.member. When the
// manifests pin different member.u8 (a declared clearing), it also loads the role's member.u8
// against the role's receipt and refuses unless the two differ on exactly the declared N cells,
// each one the role has absent and keeps a member and the label role presents and clears.
[[nodiscard]] atx::core::Result<LoadedLabelRole> load_label_role(
    const TargetReplayRunConfig& cfg, const std::string& path, const std::string& sha256,
    const LoadedSavedBlend& blend);
// What a label role's load holds beside the blend: per cell its close, raw close, presence, the
// member mask it verifies and, for a declared clearing, the role's member mask; and the two
// pinned manifests' text and parse.
inline constexpr atx::u64 label_role_cell_bytes = 2 * sizeof(atx::f64) + 3;
inline constexpr atx::u64 label_role_metadata_bytes = 8ULL << 20;
// The target replay's own recipe/geometry/axes/support validation and budget.
[[nodiscard]] atx::core::Status validate_replay_input(const TargetReplayInput& in,
                                                      const TargetReplayConfig& cfg);
// Centered tied rank of members, demeaned, gross 1; an all-tie row stays flat.
void desired_target(std::span<const atx::f64> signal, std::span<const atx::u8> member,
                    std::vector<std::pair<atx::f64, atx::usize>>& row,
                    std::vector<atx::f64>& target);
// Forced exits to zero every decision; partial move by the (v2 budget-capped)
// fraction on rebalance decisions, except members inside the no-trade band.
// aim-partial-v5: members move by theta_i toward aim_leverage * desired unless
// inside the dust band (counted in banded_names); theta_i = per_name_rate[i] when
// the span is non-empty (the NAV's rate per-name-v1, T36; out.applied_fraction is
// then the members' mean rate), else trade_fraction. A non-empty span must hold
// exactly in.instruments rates in [0, 1] (NaN refused) under aim-partial-v5; any
// other non-empty span is refused with InvalidArgument before anything moves (never a
// silent fixed-theta fallback). exit_rate < 1 (aim-partial-v5): present nonmembers
// decay instead of exiting (TargetReplayConfig::exit_rate); it needs in.present at full
// geometry, else InvalidArgument before anything moves. `current` is updated in place
// to the plan and `out` accumulates the planned turnover/exposure fields (and
// out.construction.banded_names).
[[nodiscard]] atx::core::Status update_weights(
    const TargetReplayInput& in, const TargetReplayConfig& cfg, atx::usize d, bool rebalance,
    atx::f64 spent, const std::vector<atx::f64>& desired, std::vector<atx::f64>& current,
    TargetReplayDay& out, std::span<const atx::f64> per_name_rate = {});
// N_d: the members of decision d (in.member[d * instruments + i] != 0).
[[nodiscard]] atx::usize members_at(const TargetReplayInput& in, atx::usize d);
// The construction of rebalance decision d, shared by the target and NAV replays:
// the tied-rank desired target, then the configured post-processing (price-risk-v1:
// neutralize_price_risk with exposures computed once for d from the role prices,
// reading only sessions <= d; the industry ids: neutralize_price_risk_within_groups on
// row d of in.industry, InvalidArgument without it). Returns false when the guard
// skips the rebalance (data refusal or cap breach); contract and allocation errors are
// returned as errors. `out` receives the neutralization record (banded_names is
// untouched).
// no_short (NAV locate-in-aim, v6 prereg C3): empty (the default: unchanged), or one byte per
// name; a member with no_short[i] != 0 and a negative tied-rank weight is set to 0
// BEFORE the post-processing, so price-risk-v1 re-balances net and beta around it
// (counted in out.locate_zeroed). A span of any other length is InvalidArgument. Under
// the industry ids no_short is also the within-groups hold mask (review I3): every
// member with no_short[i] != 0 whose aim is 0 there (the zeroed shorts, and an exact-0
// tied-rank aim) is reset to 0 after the group demeaning, before the OLS.
// state (v8): hold-band-v1 reads and advances state->hold between the tied ranks and the
// demean (out.hold_moved / hold_kept / hold_first_set); adv-hold-v1 caps the desired target
// after the post-processing of a rebalance that proceeds, from state->adv_dollars and
// state->nav (out.adv_*). InvalidArgument without a state (or its ADV row and NAV) when an
// option needs it. With both off the state is not read and the arithmetic is the pre-v8
// construction's, operation for operation. inv-vol-v1 (v8 X) scales the tied ranks by
// engine::book::scale_inverse_vol on state->sigma before the demean (out.inv_vol_*);
// InvalidArgument without a state whose sigma row has one entry per name.
[[nodiscard]] atx::core::Result<bool> form_desired(
    const TargetReplayInput& in, const TargetReplayConfig& cfg, atx::usize d,
    std::vector<std::pair<atx::f64, atx::usize>>& row, std::vector<atx::f64>& desired,
    PriceRiskScratch& scratch, ConstructionDay& out, std::span<const atx::u8> no_short = {},
    DesiredState* state = nullptr);
// True iff any construction option is non-default or the rule is aim-partial-v5:
// only then do recipes, CSVs and summaries carry construction keys/columns (the
// default path emits none).
[[nodiscard]] bool construction_active(const TargetReplayConfig& cfg);
// "<rule>[+neutral-<id>][+band-<X>][+hold-band-<B>][+adv-hold-<Q>][+inv-vol-v1]" (id:
// price-risk-v1 | price-risk-ind-v1 | price-risk-ind-v2; X, B, Q: shortest round-trip decimal; B
// only when > 0; inv-vol-v1: v8 X, TargetReplayConfig::inv_vol).
[[nodiscard]] std::string construction_rule_id(const TargetReplayConfig& cfg);
// Construction recipe keys as a JSON object text; empty when not active.
[[nodiscard]] std::string construction_recipe_json(const TargetReplayConfig& cfg);
// {"construction": {...}} diagnostics over the decisions' records (skip reasons,
// used names min/median, amplification, banded names) as JSON text; empty when
// not active.
[[nodiscard]] std::string construction_summary_json(const TargetReplayConfig& cfg,
                                                    std::span<const ConstructionDay> decisions);
// One aim-partial-v5 decision: the planned weights after the rule (gross, net and
// nonzero names) and the decision's members N_d.
struct AimPartialDecision {
  atx::f64 gross{}, net{};
  atx::usize held_names{}, members{};
};
// The summary's construction.v5 object as JSON text: theta, dust_multiple,
// aim_leverage, rate ("fixed"; the NAV's per-name-v1 overrides it and adds
// rate_stats) and, over the decisions, mean_gross,
// mean_net and mean_held_share (held_names / N_d over decisions with members);
// a mean over no decisions is null; exit_rate only when it is not 1. Empty unless
// rule == AimPartialV5.
[[nodiscard]] std::string aim_partial_summary_json(
    const TargetReplayConfig& cfg, std::span<const AimPartialDecision> decisions);
// Working bytes of the construction scratch for `instruments` names (0 unless
// neutralizing or a v8 option is on: the DesiredState); charged by both replays' admission.
[[nodiscard]] atx::u64 construction_scratch_bytes(const TargetReplayConfig& cfg,
                                                  atx::usize instruments);
// CSV header suffix ",neutralize,...,banded_names" and the matching row writer
// (appended by both replays only when construction_active).
[[nodiscard]] const char* construction_csv_columns();
void write_construction_csv(std::ostream& out, const ConstructionDay& day);
// CLI spelling: "none" | "price-risk-v1" | "price-risk-ind-v1" | "price-risk-ind-v2"
// sets cfg.neutralize (ind-v2 also its declared vol 126 / log-ADV 252 windows in
// cfg.price_risk); false (cfg untouched) otherwise.
[[nodiscard]] bool parse_neutralize(std::string_view value, TargetReplayConfig& cfg);
// Stable CSV spelling of a neutralization outcome.
[[nodiscard]] const char* neutralize_outcome_label(NeutralizeOutcome outcome);
// Linear interpolation at (n-1)q over ascending finite values (numpy default);
// NaN when empty.
[[nodiscard]] atx::f64 sorted_quantile(std::span<const atx::f64> sorted, atx::f64 q);
// YYYYMM of a UTC-midnight session key.
[[nodiscard]] atx::u32 calendar_month(atx::i64 session_ns);
// The aim_partial declarations' nonmember clause (both replays' recipes):
// "nonmembers exit to 0" at exit_rate 1 (the default text byte for byte), else
// "nonmembers follow exit_rate_rule".
[[nodiscard]] const char* nonmember_exit_clause(const TargetReplayConfig& cfg);
} // namespace atx::impl::strategy::detail
