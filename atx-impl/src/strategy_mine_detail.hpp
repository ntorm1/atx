#pragma once
// Internal to the mining verb translation units (strategy_mine*.cpp); callers use
// strategy_mine.hpp. Platform v8 H-3.
//   strategy_mine.cpp          configuration, windows, the two search stages, outputs, CLI
//   strategy_mine_trials.cpp   the campaign's distinct trials and their registry records
//   strategy_mine_promote.cpp  mined-v1 applied to the evaluated trials (strategy_mine_rule.hpp)
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/trial_registry.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/research_ic_fitness.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "strategy_mine.hpp"
#include "strategy_mine_pool.hpp"
#include "strategy_mine_rule.hpp"
#include "strategy_research_role.hpp"

namespace atx::impl::strategy::mine_detail {
namespace co = atx::core;
namespace ev = atx::engine::eval;
namespace ex = atx::engine::factory;
using Json = nlohmann::json;

// The recipe tag every trial's configuration hash starts with.
inline constexpr std::string_view kTrialRecipe = "atx.mine-trial/v1";

[[nodiscard]] co::Error fail(co::ErrorCode code, const std::string &message);
[[nodiscard]] std::string hex16(u64 value);
[[nodiscard]] Json finite_or_null(f64 value);

// [begin, end) as dates and, once a role is loaded, as its decision rows.
struct MineWindow {
  std::string begin_date, end_date;
  i64 begin_ns{}, end_ns{};
  usize begin{}, end{};
};

struct StageRun {
  ex::SearchResult result;
  std::vector<ex::ResearchIcTrial> trials; // the stage's full-pass reads, by canonical hash
  f64 seconds{};
};

// ---- strategy_mine_trials.cpp ------------------------------------------------------------------
// Ordered best first: an expression seen in both stages keeps its best status. Review MINE-16:
// RungFailed is a racing rejection whose rung read failed (compile, VM or functor error;
// SearchResult::rung_failed_hashes) rather than one scored and lost.
enum class TrialStatus : u8 { Evaluated, ScreenRejected, RacingRejected, RungFailed, Failed };
[[nodiscard]] std::string_view status_name(TrialStatus status) noexcept;

// One distinct expression of the campaign. The pointers borrow the stage results.
struct MinedTrial {
  u64 canon_hash{};
  usize stage{}; // the stage that first saw it (1 or 2)
  TrialStatus status{TrialStatus::Failed};
  // screen name, racing-rejected, rung-failed, unscored, slot-bound, degenerate-series (empty:
  // none)
  std::string reason;
  std::string dsl;
  const ex::Genome *genome{};
  const ex::ResearchIcTrial *read{}; // the full-pass read (evaluated or screened), else null
};

// The registry identity (review MINE-16): n_raw = evaluated + screen_rejected + racing_rejected
// + rung_failed + failed.
struct Counts {
  usize evaluated{}, screen_rejected{}, racing_rejected{}, rung_failed{}, failed{};
};

// Adds the stage's distinct expressions in all_scored order (worker-invariant), first seen first.
void classify(const StageRun &stage, usize stage_number, std::vector<MinedTrial> &trials,
              std::unordered_map<u64, usize> &index);
[[nodiscard]] Counts count_statuses(const std::vector<MinedTrial> &trials);
// The exported chain head an existing registry is reopened against (nullopt: a new registry).
[[nodiscard]] co::Result<std::optional<ev::TrialChainHead>> registry_anchor(const MineConfig &cfg);
[[nodiscard]] co::Result<ev::TrialRegistry>
open_registry(const MineConfig &cfg, usize pnl_len,
              const std::optional<ev::TrialChainHead> &anchor);
// How many of `trials` the registry already holds under `recipe_sha` (0 for a new registry,
// which is not created). Review MINE-3: the recipe binds the confirm window, so a held trial is
// an expression whose confirm window was read already under the same identity.
[[nodiscard]] co::Result<usize> registered_trials(const MineConfig &cfg,
                                                  const std::vector<MinedTrial> &trials,
                                                  const std::string &recipe_sha, usize rows,
                                                  const std::optional<ev::TrialChainHead> &anchor);
// Records every trial once, in order; returns how many records were new. An evaluated trial
// whose oriented daily IC is degenerate becomes screen-rejected.
[[nodiscard]] co::Result<usize> record_trials(ev::TrialRegistry &registry,
                                              std::vector<MinedTrial> &trials,
                                              const std::string &recipe_sha,
                                              const std::string &campaign_id, usize rows);
// What the campaign left in its registry (review MINE-1). `sha256` is the SHA-256 of the log's
// first `bytes` bytes, the whole append-only log after this campaign's records: the ledger's
// chain head. `chain` is the registry's own tamper-evident head (the --registry-head anchor).
struct RegistryReceipt {
  ev::TrialChainHead chain{};
  u64 n_raw{};
  usize inserted{};
  std::string sha256;
  u64 bytes{};
};
// Opens the registry (against `anchor` when it exists), records the trials (record_trials),
// closes it, digests the log and reopens it against the new chain head: Err when the log no
// longer ends at that head (another writer appended), since the digest would not be this
// campaign's.
[[nodiscard]] co::Result<RegistryReceipt>
record_campaign(const MineConfig &cfg, std::vector<MinedTrial> &trials,
                const std::string &recipe_sha, usize rows,
                const std::optional<ev::TrialChainHead> &anchor);
[[nodiscard]] std::string trials_csv(const std::vector<MinedTrial> &trials);

// ---- strategy_mine_promote.cpp -----------------------------------------------------------------
struct Promotion {
  usize trial{}; // index into the campaign's trials
  MinedRho rho{};
  bool confirm_read{};
  bool confirm_defined{}; // the read covers its full window (mined_confirm_defined)
  usize confirm_rows{};   // the confirm window's mature label rows
  ex::ResearchIcRead confirm{};
  MinedConfirm decision{};
  bool admitted{};
};

// What mined-v1 reads besides the trials. Everything is borrowed.
struct PromotionContext {
  const ResearchRole *role{};
  const MinePool *pool{};
  std::span<const std::span<const f64>> regressors{};
  const MineWindow *discover{};
  const MineWindow *confirm{};
  usize min_names{};
  usize min_dates{};
  usize max_promotions{};
  u64 max_cache_bytes{};
};

// Shortlist at `hurdle`, the greedy rho check, one confirm read each, Benjamini-Yekutieli.
[[nodiscard]] co::Result<std::vector<Promotion>>
promote(const std::vector<MinedTrial> &trials, f64 hurdle, const PromotionContext &context);
// campaign.json promotions (every shortlisted trial) and mined_members.json members (admitted).
[[nodiscard]] Json promotions_json(const std::vector<MinedTrial> &trials,
                                   const std::vector<Promotion> &promotions, const MinePool &pool);
[[nodiscard]] Json members_json(const std::vector<MinedTrial> &trials,
                                const std::vector<Promotion> &promotions);

} // namespace atx::impl::strategy::mine_detail
