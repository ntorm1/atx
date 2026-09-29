#pragma once

// PRIVATE to atx-impl. Seams of the NAV replay shared with the daily decide path
// (strategy_live.cpp) and their tests. The decide path calls exactly these functions,
// which the replay itself runs, so a decision is never a re-implementation of the
// replay's DECIDE. No nlohmann dependency: JSON travels as text.

#include <array>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_target_replay_detail.hpp"

namespace atx::impl::strategy::detail {
// One book's DECIDE at role row d from given positions, by the replay's own sequence:
// borrow tiers of d (rows <= d), the locate-in-aim mask, on a cadence decision the
// shared desired target (form_desired: neutralization and its skip guard), then
// plan_weights (the construction-rule dispatch site: update_weights, then the scenario's
// locate block). current_i = held_i / nav_post, the replay's expression, so fed the
// replay's end-of-session holdings and post-trade NAV of d it returns the replay's
// planned weights bit for bit.
struct NavDecision {
  bool cadence{};   // (d - decision_begin) % cadence == 0
  bool rebalance{}; // effective: a cadence decision the neutralization guard did not skip
  ConstructionDay construction{};
  std::vector<atx::f64> desired;  // shared desired target (all zero unless rebalance)
  std::vector<atx::f64> current;  // held / nav_post
  std::vector<atx::f64> rule;     // the target rule's plan (before the locate block)
  std::vector<atx::f64> target;   // the planned weights (after the locate block)
  std::vector<atx::u8> tier, tier_missing; // BorrowTier per name (empty without fields)
  std::vector<atx::u8> no_short;  // the aim mask used (empty unless locate-in-aim)
  // Diagnostics only: members' daily vol over [d-w, d) by the execution liquidity
  // definition (NaN: fewer than min_vol_pairs pairs, and every nonmember).
  std::vector<atx::f64> sigma;
  TargetReplayDay plan{};         // the rule's turnover, gross, net, held names
  atx::usize members{};           // N_d
  atx::usize blocked_short_names{};
  atx::f64 blocked_short_dollars{};
  std::array<atx::usize, 3> member_tiers{}; // GC, warm, special
  atx::usize member_missing_predictors{};
};
// `cfg` is ONE book's config (base + its scenario; the decide path uses the primary).
// held: one finite dollar amount per name; nav_post finite > 0; d in
// [decision_begin, decision_end). no_locate: empty, or one byte per name: 1 = may not
// open or grow a short at d, OR-ed with the special tier into the locate-in-aim mask
// and the locate block (the replay passes none). Refuses rate per-name-v1 and
// monthly-budget-v2: they carry book state (pre-trade NAV, month-to-date plan) that
// positions do not. Everything else is the replay's contract (validate_nav_input).
[[nodiscard]] atx::core::Result<NavDecision> nav_decide(const NavReplayInput& in,
                                                       const NavReplayConfig& cfg, atx::usize d,
                                                       std::span<const atx::f64> held,
                                                       atx::f64 nav_post,
                                                       std::span<const atx::u8> no_locate = {});

// A pinned role, blend and (optional) fields set loaded exactly as run_nav_replay admits
// and loads them (one book's workspace reserve), plus the compact recipe digest the NAV
// run of (cfg, base, limits, fields) publishes as summary.json recipe_sha256.
struct NavDeployLoad {
  LoadedSavedBlend blend;
  std::vector<atx::f64> shares_out, si_shares, industry;
  std::string recipe_sha256;
  // Borrowed view; valid while *this is alive and unmodified.
  [[nodiscard]] NavReplayInput view() const;
};
[[nodiscard]] atx::core::Result<NavDeployLoad> load_nav_deploy(const TargetReplayRunConfig& cfg,
                                                              const NavReplayConfig& base,
                                                              const NavTurnoverLimits& limits,
                                                              const NavFieldsPin& fields);

// v7 W4 (B7): the raw-dollar ADV the replay's EXECUTE at session t reads for every name,
// by the execution liquidity definition (present raw_close x volume over [t-w, t) / w, w
// = cfg.liquidity_window; rows < t only). t in [1, dates]: t = d + 1 is the session that
// fills a decision at d, t = dates the session after the last role row.
[[nodiscard]] atx::core::Result<std::vector<atx::f64>> execution_adv(const NavReplayInput& in,
                                                                   const NavReplayConfig& cfg,
                                                                   atx::usize t);

// Stable CSV spellings shared by holdings.csv and the decide outputs.
[[nodiscard]] const char* borrow_tier_label(atx::u8 tier); // gc|warm|special|none
[[nodiscard]] const char* fill_status_label(NavFillStatus fill);
// holdings.csv header (the decide positions reader locates its columns by name).
[[nodiscard]] const char* holdings_csv_columns();
} // namespace atx::impl::strategy::detail
