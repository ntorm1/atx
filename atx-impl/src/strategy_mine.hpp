#pragma once
// atx::impl::strategy — `atx-equity-strategy-mine` (platform v8 H-3): the mining verb. Built and
// self-tested on a synthetic role; a campaign on real data runs only under owner decision OD-7.
//
// One campaign on one pinned research role (strategy_research_role.hpp) and its pool
// (strategy_mine_pool.hpp), with two windows inside TRAIN of research-window-v2:
//   discover  the search's fitness (research_ic_fitness.hpp): f1 = |h 21 rank IC t|,
//             f2 = sign x marginal IC HAC t against the pool's regressors;
//   confirm   one read of each promoted expression (strategy_mine_rule.hpp, mined-v1).
// Search: stage 1 evaluates the templates rank(f), rank(ts_mean(f, w)) and rank(delta(f, w)),
// w in {5, 21, 63, 126, 252}, over the mined fields in one generation; stage 2 is an NSGA-II run
// seeded with the stage-1 front (parsimony on, novelty off, literature operators on, the miner's
// deny list). Both run on the signal-fitness path of SearchDriver with the role's decision
// membership as the cross-section mask and race on instrument strides only.
// Registry: every distinct expression of the campaign is one trial of a V3 TrialRegistry
// (--registry; an existing log is reopened only against --registry-head), in first-seen order:
// evaluated trials with their oriented daily h 21 rank IC over the discover label rows (NaN as
// 0), screen-rejected, racing-rejected and failed ones as screened observations. A trial's
// identity is (recipe, expression); the recipe (campaign.json recipe, recipe_sha256) binds the
// role, the fields manifest, the library (VM and IC source pins), the pool and both windows, so a
// campaign any of whose trials the registry holds under its recipe is a second confirm read on
// that identity and is refused before the confirm read (review MINE-3). The registry's
// own chain head (records, u64) is written to OUTPUT/registry_head.txt (the next campaign's
// --registry-head); the ledger's chain head is the SHA-256 of the registry log's bytes as the
// campaign left it (review MINE-1), in campaign.json with the byte count and in ledger_line.json.
//
// Outputs (OUTPUT must be new): campaign.json (atx.mine-campaign/v1), trials.csv,
// mined_members.json (atx.mined-members/v1, theme `mined`), ledger_line.json (the Ruling E-33
// atx.trial-ledger/v1 line of backtest_integrity.campaign_line, strategy_mine_ledger.hpp: kind
// mining-campaign, count 0, registry {path, chain_head, bytes, count = the records this campaign
// added, total = n_raw} (Ruling E-33a; a campaign that adds no record is refused);
// `research_cycle.py ledger-campaign --campaign OUTPUT` rebuilds it from campaign.json, checks
// the registry bytes against the head, and appends it chained) and registry_head.txt.
//
// Budget (pre-registration rule 10, Ruling E-32a; review MINE-4): --budget N is mandatory and
// fixed before the search. It must cover the configuration's trial capacity
// (mine_trial_capacity: the templates plus the stage-2 population times its generations), so
// no search can exceed it; the mined-v1 hurdle is computed from N, never from the realised or
// the registry's count, so neither a fresh registry nor a small search lowers it. campaign.json
// and the ledger line carry N.
//
// Pool (Ruling E-32a; review MINE-7): --pool is mandatory and its manifest names at least one
// regressor and one member, checked from the manifest before any payload. Without the book the
// marginal t is the raw IC t and the rho check meets no member, so mined-v1 would admit a copy of
// the book (the case Ruling E-32 exists for): no pool, no campaign.
//
// Label overlap (Ruling E-32a; review MINE-6): both mined-v1 reads are taken as t /
// kMinedOverlapFactor (strategy_mine_rule.hpp), a factor derived on discover windows of at least
// kMinedMinDiscoverRows and confirm windows of at least kMinedMinConfirmRows mature h 21 label
// rows; a shorter window is refused before any search. The factor is in the recipe and in
// campaign.json's hurdle.
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_research_role.hpp"

namespace atx::impl::strategy {

struct MineConfig {
  ResearchRoleSpec role;            // role.fields: the fields mined (base or pinned extras)
  std::string pool_path, pool_sha256; // required atx.mine-pool/v1 manifest (Ruling E-32a)
  // YYYY-MM-DD, [begin, end): chronological, non-overlapping, inside TRAIN.
  std::string discover_begin, discover_end, confirm_begin, confirm_end;
  std::string registry_path;      // the campaign's trial registry (created when absent)
  std::string registry_head_path; // required when the registry exists: its exported chain head
  std::string campaign_id;        // [a-z0-9_-], at most 64 characters
  std::string output_directory;   // must not exist
  atx::u64 budget{}; // --budget: the campaign's trial budget N (required; >= the capacity)
  atx::u64 seed{1};
  atx::usize workers{1};
  atx::usize stage2_seeds{12};       // stage-1 front members that seed stage 2 (0: no stage 2)
  atx::usize stage2_population{24};
  atx::usize stage2_generations{4};
  std::vector<atx::u32> race_strides{4}; // instrument strides of the racing rungs (empty: none)
  atx::f64 race_keep{1.0 / 3.0};         // promoted fraction per rung
  atx::usize min_names{50};
  atx::usize min_dates{128};
  atx::usize max_promotions{16}; // shortlist cap (each costs one signal in memory)
  atx::u64 max_working_bytes{2048ULL << 20};
};

// The stage-1 templates of `fields`, in field order.
[[nodiscard]] std::vector<std::string> mine_templates(std::span<const std::string> fields);

// The most distinct trials `cfg` can evaluate: its templates (stage 1, one generation) plus, when
// stage 2 runs, its population times its generations (each generation evaluates at most one
// population of new candidates; no immigrants, no grammar fill).
[[nodiscard]] atx::u64 mine_trial_capacity(const MineConfig &cfg);
inline constexpr atx::u64 kMineMaxBudget = 10'000'000ULL;

// A campaign's shape, known before any payload.
struct MineFootprint {
  atx::usize dates{}, names{}, extras{}, regressors{}, members{}, workers{1}, rungs{};
  atx::usize shortlist{};
};
// Conservative peak bytes: the role (research_role_bytes), the pool, the discover and confirm IC
// caches, per worker one VM engine (8 slots per cell assumed) and one signal set per racing rung
// and the full panel, the strided rung panels and caches, the shortlist's signals and rank rows.
[[nodiscard]] atx::core::Result<atx::u64> mine_working_bytes(const MineFootprint &footprint);

[[nodiscard]] atx::core::Status run_mine(const MineConfig &cfg, std::ostream &progress);
// argv[0] is the program (or verb) name.
[[nodiscard]] int dispatch_mine(int argc, char **argv, std::ostream &out, std::ostream &err);

} // namespace atx::impl::strategy
