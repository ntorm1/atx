// The mining verb's trial log (platform v8 H-3): every distinct expression of a campaign, its
// status and its registry record. Contracts in strategy_mine_detail.hpp.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "strategy_mine_detail.hpp"

namespace atx::impl::strategy::mine_detail {
namespace {
namespace al = atx::engine::alpha;

u64 fnv1a64(std::string_view text) {
  u64 h = 1469598103934665603ULL;
  for (const char c : text) {
    h ^= static_cast<u8>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

const ex::ResearchIcTrial *find_read(const std::vector<ex::ResearchIcTrial> &reads, u64 hash) {
  const auto it = std::lower_bound(
      reads.begin(), reads.end(), hash,
      [](const ex::ResearchIcTrial &trial, u64 key) { return trial.canon_hash < key; });
  return it != reads.end() && it->canon_hash == hash ? &*it : nullptr;
}

MinedTrial classify_one(const StageRun &stage, usize stage_number, const ex::Genome &g) {
  MinedTrial t;
  t.canon_hash = g.canon_hash;
  t.stage = stage_number;
  t.genome = &g;
  const auto listed = [&g](const std::vector<u64> &sorted) {
    return std::binary_search(sorted.begin(), sorted.end(), g.canon_hash);
  };
  if (listed(stage.result.fidelity_rejected_hashes)) {
    t.status = TrialStatus::RacingRejected;
    t.reason = "racing-rejected";
  } else if (listed(stage.result.unscored_hashes)) {
    t.reason = "unscored";
  } else if (const ex::ResearchIcTrial *read = find_read(stage.trials, g.canon_hash)) {
    t.read = read;
    const bool scored = read->screen == ex::ResearchIcScreen::None &&
                        std::isfinite(ex::research_ic_f1(read->read)) &&
                        std::isfinite(ex::research_ic_f2(read->read));
    t.status = scored ? TrialStatus::Evaluated : TrialStatus::ScreenRejected;
    if (!scored)
      t.reason = read->screen == ex::ResearchIcScreen::None
                     ? std::string("non-finite-score")
                     : std::string(ex::research_ic_screen_name(read->screen));
  } else {
    t.reason = "no-read";
  }
  return t;
}

std::string csv_number(f64 v) {
  if (!std::isfinite(v)) return {};
  std::array<char, 40> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
  return std::string(buffer.data(), written.ptr);
}
} // namespace

co::Error fail(co::ErrorCode code, const std::string &message) {
  return co::Error{code, "mine: " + message};
}

std::string hex16(u64 value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (usize i = 0; i < 16U; ++i) out[15U - i] = digits[(value >> (4U * i)) & 15U];
  return out;
}

Json finite_or_null(f64 value) { return std::isfinite(value) ? Json(value) : Json(nullptr); }

std::string_view status_name(TrialStatus status) noexcept {
  switch (status) {
  case TrialStatus::Evaluated:
    return "evaluated";
  case TrialStatus::ScreenRejected:
    return "screen-rejected";
  case TrialStatus::RacingRejected:
    return "racing-rejected";
  case TrialStatus::Failed:
    return "failed";
  }
  return "failed";
}

void classify(const StageRun &stage, usize stage_number, std::vector<MinedTrial> &trials,
              std::unordered_map<u64, usize> &index) {
  for (const ex::Genome &g : stage.result.all_scored) {
    MinedTrial t = classify_one(stage, stage_number, g);
    const auto [at, inserted] = index.emplace(g.canon_hash, trials.size());
    if (inserted) {
      t.dsl = al::unparse(g.ast);
      trials.push_back(std::move(t));
      continue;
    }
    MinedTrial &seen = trials[at->second];
    if (static_cast<u8>(t.status) < static_cast<u8>(seen.status)) {
      seen.status = t.status;
      seen.reason = std::move(t.reason);
      seen.genome = t.genome;
      seen.read = t.read;
    }
  }
}

Counts count_statuses(const std::vector<MinedTrial> &trials) {
  Counts c;
  for (const MinedTrial &t : trials) {
    c.evaluated += t.status == TrialStatus::Evaluated ? 1U : 0U;
    c.screen_rejected += t.status == TrialStatus::ScreenRejected ? 1U : 0U;
    c.racing_rejected += t.status == TrialStatus::RacingRejected ? 1U : 0U;
    c.failed += t.status == TrialStatus::Failed ? 1U : 0U;
  }
  return c;
}

co::Result<std::optional<ev::TrialChainHead>> registry_anchor(const MineConfig &cfg) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(cfg.registry_path, ec);
  if (ec) return co::Err(fail(co::ErrorCode::IoError, "registry path: " + ec.message()));
  if (!exists) {
    if (!cfg.registry_head_path.empty())
      return co::Err(fail(co::ErrorCode::InvalidArgument,
                          "--registry-head names the anchor of an existing registry; " +
                              cfg.registry_path + " does not exist"));
    return co::Ok(std::optional<ev::TrialChainHead>{});
  }
  if (cfg.registry_head_path.empty())
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "the registry exists: pass --registry-head with its exported chain head"));
  ATX_TRY(const ev::TrialChainHead head, ev::read_chain_head(cfg.registry_head_path));
  return co::Ok(std::optional<ev::TrialChainHead>{head});
}

co::Result<ev::TrialRegistry> open_registry(const MineConfig &cfg, usize pnl_len,
                                            const std::optional<ev::TrialChainHead> &anchor) {
  ev::TrialRegistryConfig rc;
  rc.pnl_len = pnl_len;
  rc.format = ev::TrialLogFormat::V3;
  rc.keep_sketches = false;
  const std::filesystem::path path(cfg.registry_path);
  ATX_TRY(auto registry, anchor ? ev::TrialRegistry::open(path, rc, *anchor)
                                : ev::TrialRegistry::open(path, rc));
  if (registry.format() != ev::TrialLogFormat::V3)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "the registry log is not V3 (screened observations need V3)"));
  return co::Ok(std::move(registry));
}

co::Result<usize> record_trials(ev::TrialRegistry &registry, std::vector<MinedTrial> &trials,
                                const std::string &recipe_sha, const std::string &campaign_id,
                                usize rows) {
  ev::TrialMeta meta;
  meta.window_start = 0U;
  meta.window_end = rows - 1U;
  meta.sample = ev::TrialSample::InSample;
  meta.family_tag = ev::trial_tag(kMinedTheme);
  meta.theme_tag = ev::trial_tag(campaign_id);
  const u64 rule_tag = ev::trial_tag(std::string(kTrialRecipe) + "|screen|" + recipe_sha);
  std::vector<f64> pnl(rows);
  usize inserted = 0;
  for (MinedTrial &t : trials) {
    const u64 config = fnv1a64(std::string(kTrialRecipe) + "|" + recipe_sha + "|" +
                               hex16(t.canon_hash) + "|" + t.dsl);
    if (t.status == TrialStatus::Evaluated) {
      // The oriented daily h 21 rank IC over the discover label rows, undefined days as 0.
      const std::vector<f64> &daily = t.read->daily_rank_ic;
      if (daily.size() != rows)
        return co::Err(fail(co::ErrorCode::Internal, "daily IC rows differ from the labels"));
      const f64 sign = static_cast<f64>(t.read->read.sign);
      f64 sum = 0.0;
      for (usize k = 0; k < rows; ++k) {
        pnl[k] = std::isfinite(daily[k]) ? sign * daily[k] : 0.0;
        sum += pnl[k];
      }
      const f64 mean = sum / static_cast<f64>(rows);
      f64 ss = 0.0;
      for (const f64 v : pnl) ss += (v - mean) * (v - mean);
      const f64 sd = std::sqrt(ss / static_cast<f64>(rows - 1U));
      if (sd > 0.0 && std::isfinite(sd)) {
        auto recorded = registry.record(ev::TrialKind::MinerExpr, config, meta, pnl, mean / sd);
        if (recorded) {
          inserted += recorded->inserted ? 1U : 0U;
          continue;
        }
        if (recorded.error().code() != co::ErrorCode::InvalidArgument)
          return co::Err(recorded.error());
      }
      t.status = TrialStatus::ScreenRejected;
      t.reason = "degenerate-series";
    }
    ev::TrialMeta screened = meta;
    screened.fidelity = t.status == TrialStatus::RacingRejected ? u8{1} : u8{0};
    ATX_TRY(const auto recorded, registry.record_screened(ev::TrialKind::MinerExpr, config,
                                                          screened, rule_tag,
                                                          ev::trial_tag(t.reason)));
    inserted += recorded.inserted ? 1U : 0U;
  }
  return co::Ok(inserted);
}

co::Result<RegistryReceipt> record_campaign(const MineConfig &cfg, std::vector<MinedTrial> &trials,
                                            const std::string &recipe_sha, usize rows,
                                            const std::optional<ev::TrialChainHead> &anchor) {
  RegistryReceipt out;
  {
    ATX_TRY(auto registry, open_registry(cfg, rows, anchor));
    ATX_TRY(out.inserted, record_trials(registry, trials, recipe_sha, cfg.campaign_id, rows));
    out.chain = registry.chain_head();
    out.n_raw = registry.summary().n_raw;
  } // the log's handle closes here
  std::error_code ec;
  const auto size = std::filesystem::file_size(cfg.registry_path, ec);
  if (ec) return co::Err(fail(co::ErrorCode::IoError, "registry size: " + ec.message()));
  out.bytes = static_cast<u64>(size);
  ATX_TRY(out.sha256, co::sha256_file(cfg.registry_path));
  // Reopened against the head just recorded: a log that another writer extended since (before
  // or during the digest) holds more records than that head and is refused.
  ATX_TRY(const auto reopened,
          open_registry(cfg, rows, std::optional<ev::TrialChainHead>{out.chain}));
  if (!(reopened.chain_head() == out.chain))
    return co::Err(fail(co::ErrorCode::Unavailable,
                        "the registry changed while the campaign recorded (another writer "
                        "appended to " + cfg.registry_path + ")"));
  return co::Ok(std::move(out));
}

std::string trials_csv(const std::vector<MinedTrial> &trials) {
  std::string out = "canon_hash,stage,status,reason,ic_mean,ic_t,f1,marginal_mean,marginal_t,f2,"
                    "sign,dsl\n";
  for (const MinedTrial &t : trials) {
    const ex::ResearchIcRead r = t.read != nullptr ? t.read->read : ex::ResearchIcRead{};
    std::string dsl;
    for (const char c : t.dsl) dsl += c == '"' ? std::string("\"\"") : std::string(1, c);
    out += hex16(t.canon_hash) + ',' + std::to_string(t.stage) + ',' +
           std::string(status_name(t.status)) + ',' + t.reason + ',' + csv_number(r.ic_mean) +
           ',' + csv_number(r.ic_t) + ',' + csv_number(ex::research_ic_f1(r)) + ',' +
           csv_number(r.marginal_mean) + ',' + csv_number(r.marginal_t) + ',' +
           csv_number(ex::research_ic_f2(r)) + ',' +
           (t.read != nullptr ? std::to_string(r.sign) : std::string{}) + ",\"" + dsl + "\"\n";
  }
  return out;
}

} // namespace atx::impl::strategy::mine_detail
