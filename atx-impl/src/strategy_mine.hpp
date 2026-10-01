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
// membership as the cross-section mask and race on instrument strides only; a program that needs
// more than kMineMaxProgramSlots VM slots is refused before any evaluation (review MINE-10).
// Registry: every distinct expression of the campaign is one trial of a V3 TrialRegistry
// (--registry; an existing log is reopened only against --registry-head), in first-seen order:
// evaluated trials with their oriented daily h 21 rank IC over the discover label rows (NaN as
// 0), screen-rejected, racing-rejected, rung-failed (review MINE-16: rejected at a racing rung
// because the read failed there, not scored and lost) and failed ones as screened observations,
// so the registry's count is evaluated + screen-rejected + racing-rejected + rung-failed +
// failed (campaign.json trials). A trial's
// identity is (recipe, canonical expression hash; review MINE-17); the recipe (campaign.json recipe, recipe_sha256) binds the
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
// and the ledger line carry N. Ruling PM4-13: N is at most kMinedMaxBudget (1,000; the budget
// the overlap factor is validated to), refused before any payload; the ceiling is in the recipe
// and in campaign.json's hurdle, and campaign_line refuses a line above it.
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
//
// Memory (review MINE-10, lane MINE-MEM): mine_working_bytes, the peak of mine_memory's phases,
// is checked against --max-memory-mib before any payload. The search holds no pool member (each
// is checked before it and loaded after it, before the registry is written), the discover
// fitness is released before the promotion, and with racing on the race and the full pass never
// hold each other's engines. None of this changes an evaluation or its order.
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

// Review MINE-10: the most VM slots a mined program may claim (SearchConfig::max_program_slots).
// A template needs two or three; a larger program is refused before any evaluation and filed as
// a failed trial (reason slot-bound), so no engine's slot pool exceeds this many panel columns.
inline constexpr atx::u32 kMineMaxProgramSlots = 8;

// A campaign's shape, known before any payload.
struct MineFootprint {
  atx::usize dates{}, names{}, extras{}, regressors{}, members{}, workers{1}, rungs{};
  atx::usize shortlist{};
  atx::u64 trials{};        // the most full-pass reads: mine_trial_capacity
  atx::u64 prior_records{}; // the records a reopened registry holds (its anchor)
};

// The campaign's memory term by term (review MINE-10), grouped by the phase that holds it (lane
// MINE-MEM). Each term is the bytes of one group of allocations of the verb as coded, or a stated
// upper bound of them. C = dates x names panel cells, H = dates x ceil(names / 2) cells of a racing
// rung (instrument stride >= 2), F = 3 + extras panel fields, S = kMineMaxProgramSlots, W workers,
// R rungs, T trials, P prior records, G regressors, M members, K the shortlist (max_promotions).
// A workspace is 128 B per name (IC rows, marginal kernel, rank row and sort buffer) and 80 B
// per date (calendar and daily series); a label row count is at most dates.
struct MineMemory {
  // Resident: from the role load to the end of the campaign.
  atx::u64 metadata{};    // 64 MiB: library, catalogue, populations, genomes, I/O buffers, JSON
  atx::u64 role{};        // research_role_bytes(dates, names, extras)
  atx::u64 regressors{};  // G x C x 8: the pool's regressor payloads (the fitness borrows them)
  atx::u64 trial_reads{}; // T x (8 dates + 16 KiB): each full-pass read's daily h 21 IC, its
                          // genome and log rows
  atx::u64 registry{};    // 512 KiB + 128 x (P + T): the registry's Gram and record index
  // Search: one stage at a time (stage 1's driver is gone before stage 2's). The discover
  // fitness lives through both stages and is released before the promotion.
  atx::u64 discover_cache{};      // 49 C + 32 dates: IC labels and ranks (3 horizons x 2 x 8 B),
                                  // member rows (1 B) and per-row counts (4 x 8 B)
  atx::u64 discover_workspaces{}; // W workspaces of names
  atx::u64 rung_caches{};         // R x (48 H + 32 dates): the rung scorers' IC caches
  atx::u64 rung_workspaces{};     // W x R workspaces of ceil(names / 2)
  atx::u64 bind_transient{};      // H x (8 F + 6) when R > 0: one rung's strided panel, member
                                  // and guard while the fitness builds its cache (no engine yet)
  atx::u64 race_panels{};         // R x H x (8 F + 1): the driver's strided rung panels
  atx::u64 race_engines{};        // W x R x H x (8 S + 1): rung engines' slot pools, mask copies
  atx::u64 race_signals{};        // W x H x 8 when R > 0: one rung signal per worker
  atx::u64 full_engines{};        // W x C x (8 S + 1): full-pass engines' slot pools, mask copies
  atx::u64 full_signals{};        // W x C x 8: one full-pass signal per worker
  // Promotion: after the search, the fitness released.
  atx::u64 members{};          // M x C x 8: loaded after the search
  atx::u64 shortlist{};        // K x C x 8: the shortlist's signals
  atx::u64 promotion_engine{}; // C x (8 S + 1): while the shortlist is evaluated
  atx::u64 rho_rows{};         // (M + K) x names x 8 + (M + K)^2 x 16 + 16 names: rank rows,
                               // pair sums and counts, sort buffer
  atx::u64 confirm_cache{};    // 49 C + 32 dates + one workspace: the confirm read

  [[nodiscard]] atx::u64 resident() const noexcept;
  // The fitness plus the largest of the bind, race (race_*) and full-pass (full_*) sub-phases:
  // with racing on, the race and the full pass never hold each other's engines or panels.
  [[nodiscard]] atx::u64 search() const noexcept;
  // The members and the shortlist plus the largest of the promotion engine, the rho step and
  // the confirm read (strategy_mine_promote.cpp holds them one after another).
  [[nodiscard]] atx::u64 promotion() const noexcept;
  // The campaign's peak: resident() + max(search(), promotion()).
  [[nodiscard]] atx::u64 peak() const noexcept;
};
// Err on a geometry outside the configuration bounds.
[[nodiscard]] atx::core::Result<MineMemory> mine_memory(const MineFootprint &footprint);
// The memory admission: mine_memory(footprint)->peak(), compared with --max-memory-mib before
// any payload.
[[nodiscard]] atx::core::Result<atx::u64> mine_working_bytes(const MineFootprint &footprint);

[[nodiscard]] atx::core::Status run_mine(const MineConfig &cfg, std::ostream &progress);
// argv[0] is the program (or verb) name.
[[nodiscard]] int dispatch_mine(int argc, char **argv, std::ostream &out, std::ostream &err);

} // namespace atx::impl::strategy
