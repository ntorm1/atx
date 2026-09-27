#include "strategy_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <new>
#include <ostream>
#include <stdexcept>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "atx/engine/eval/hac.hpp"
#include "atx/engine/factory/execution_objective.hpp"
#include "atx/engine/factory/execution_cash_claim_streams.hpp"
#include "atx/engine/factory/execution_stock_transition_streams.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace en = atx::engine;
namespace al = en::alpha;
namespace ex = en::factory;
namespace cost = en::cost;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr u64 metadata_limit = 1ULL << 20;
struct Candidate { std::string id, family; al::Program program; f64 weight{}; };
struct Variant { std::string id; usize cadence{}; f64 fraction{}; };
struct Library {
  std::string sha, id;
  std::vector<Candidate> candidates;
  std::vector<Variant> variants;
  usize max_slots{}, lookback{};
};
struct RoleSpec { std::string path, sha, role; Json manifest; u64 admitted_bytes{}; };
struct CashClaims {
  std::string source_sha256;
  Json document;
  std::vector<ex::ExecutionCashClaimEvent> events;
};
struct StockTransitions {
  std::string source_sha256;
  Json document;
  std::vector<ex::ExecutionStockTransitionEvent> events;
};
struct Budget {
  u64 limit{}, used{};
  bool add(u64 n, u64 size) {
    if (size && n > (limit - used) / size) return false;
    used += n * size;
    return true;
  }
};
bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
co::Result<std::string> read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() <= 0 || static_cast<u64>(f.tellg()) > metadata_limit)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: metadata missing/over 1 MiB: " + path);
  std::string text(static_cast<usize>(f.tellg()), '\0');
  f.seekg(0); f.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f || f.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, "strategy: changed metadata extent");
  return co::Ok(std::move(text));
}
co::Result<Json> read_pinned(const std::string& path, const std::string& expected) {
  if (!hash_valid(expected)) return co::Err(co::ErrorCode::InvalidArgument, "strategy: external SHA256 pin required");
  ATX_TRY(auto text, read_text(path));
  ATX_TRY(auto actual, co::sha256_hex(text));
  if (actual != expected) return co::Err(co::ErrorCode::InvalidArgument, "strategy: external metadata SHA256 mismatch");
  return co::Ok(Json::parse(text));
}
co::Result<Library> read_library(const RunnerConfig& cfg) {
  ATX_TRY(auto j, read_pinned(cfg.library_path, cfg.library_sha256));
  if (j.at("schema") != "atx.dsl-strategy-library/v1" ||
      j.at("primary_variant") != "weekly_partial25")
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: unsupported fixed library recipe");
  Library out; out.sha = cfg.library_sha256; out.id = j.at("id").get<std::string>();
  std::set<std::string> fields, families, ids, expressions;
  for (const auto& f : j.at("fields")) fields.insert(f.at("name").get<std::string>());
  if (fields != std::set<std::string>{"close", "raw_close", "volume"})
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: unsupported field basis contract");
  for (const auto& f : j.at("families"))
    if (!families.insert(f.at("id").get<std::string>()).second)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: duplicate family");
  if (families.empty() || families.size() > 16 || j.at("candidates").empty() || j.at("candidates").size() > 64)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: bounded library size");
  std::map<std::string, usize> counts;
  const al::Library operators;
  for (const auto& row : j.at("candidates")) {
    Candidate c; c.id = row.at("id").get<std::string>(); c.family = row.at("family").get<std::string>();
    const auto dsl = row.at("dsl").get<std::string>();
    if (c.id.empty() || !ids.insert(c.id).second || !expressions.insert(dsl).second ||
        !families.contains(c.family) || row.at("sign_policy") != "train-net" || dsl.size() > 4096)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: malformed candidate/identity");
    ATX_TRY(auto ast, al::parse_expr(dsl, operators));
    ATX_TRY(auto analysis, al::analyze(ast));
    ATX_TRY(c.program, al::compile(ast, analysis));
    if (c.program.roots.size() != 1 || c.program.num_slots > 64)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: single bounded DSL root required");
    for (const auto& f : c.program.fields)
      if (!fields.contains(f)) return co::Err(co::ErrorCode::InvalidArgument, "strategy: undeclared DSL field");
    out.max_slots = std::max(out.max_slots, static_cast<usize>(c.program.num_slots));
    out.lookback = std::max(out.lookback, static_cast<usize>(c.program.required_lookback));
    ++counts[c.family]; out.candidates.push_back(std::move(c));
  }
  if (counts.size() != families.size()) return co::Err(co::ErrorCode::InvalidArgument, "strategy: empty family");
  for (auto& c : out.candidates)
    c.weight = 1.0 / static_cast<f64>(families.size()) / static_cast<f64>(counts.at(c.family));
  for (const auto& v : j.at("execution_variants")) {
    Variant x{v.at("id").get<std::string>(), v.at("rebalance_sessions").get<usize>(), v.at("trade_fraction").get<f64>()};
    if (x.cadence != 5 || v.at("signal_smoothing_sessions") != 0 ||
        !((x.id == "weekly_partial25" && x.fraction == .25) || (x.id == "weekly_full" && x.fraction == 1.0)))
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: unsupported predeclared variant");
    out.variants.push_back(std::move(x));
  }
  if (out.variants.size() != 2 || out.variants[0].id != "weekly_partial25" || out.variants[1].id != "weekly_full")
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: variant order/identity");
  return co::Ok(std::move(out));
}
co::Result<RoleSpec> admit_role(const RunnerConfig& cfg, const Library& lib,
    std::string path, std::string sha, std::string role, const CashClaims* claims = nullptr,
    const StockTransitions* stocks = nullptr) {
  ATX_TRY(auto j, read_pinned(path, sha));
  const auto d = j.at("dates").get<u64>(), n = j.at("instruments").get<u64>();
  const auto begin = j.at("score_begin").get<u64>(), end = j.at("score_end").get<u64>();
  if (!d || d > 4096 || !n || n > 20000 || begin < lib.lookback || begin <= cfg.liquidity_window ||
      end != d || begin >= end || end - begin < 4)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: role geometry/warmup/maturity");
  if (claims && j.at("source_sha256") != claims->source_sha256)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: cash-claim archive pin differs from role source");
  if (stocks && j.at("source_sha256") != stocks->source_sha256)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: stock-transition archive pin differs from role source");
  const auto cells = d * n, decisions = end - begin - 2;
  Budget b{cfg.max_working_bytes, 0};
  // Panel/support=26, guard=4, owned VM mask=1, aggregate=8, output=8,
  // execution owned price/support=14, execution positions=8. Additional 32
  // bytes/cell conservatively cover VM scratch; slots charge twice the compiled peak to cover grow-before-release.
  // 128/name/snapshot exceeds CostSurfaceRow payload; strings/slack separately.
  if (!b.add(1, 32ULL << 20) || !b.add(cells, 101 + 16 * lib.max_slots) ||
      !b.add(decisions * n, 128) || !b.add(decisions, 8192) || !b.add(d, 256) || !b.add(n, 1024))
    return co::Err(co::ErrorCode::Unavailable, "strategy: combined role/VM/surface/context/scratch budget; use a smaller declared role");
  const u64 event_count = (claims ? static_cast<u64>(claims->events.size()) : 0) +
                          (stocks ? static_cast<u64>(stocks->events.size()) : 0);
  if ((claims || stocks) && (!b.add(cells, 1) || !b.add(d, 64) || !b.add(event_count, 8192) || !b.add(1, 2 * metadata_limit)))
    return co::Err(co::ErrorCode::Unavailable, "strategy: additional cash-claim context/diagnostic budget");
  if (stocks && (!b.add(d, 32) || !b.add(1, 2 * metadata_limit)))
    return co::Err(co::ErrorCode::Unavailable, "strategy: additional stock-transition diagnostic budget");
  if (claims || stocks) {
    // TRAIN keeps both orientations, while every declared role keeps two
    // combined summaries. Charge the complete run even while loading one role:
    // earlier role summaries stay in the root report. Two further slots cover
    // live trial/receipt scratch. Bounded event IDs and the fixed recognition /
    // event-use schema fit an 8 KiB envelope per event, plus 8 KiB fixed summary
    // overhead. Four representations conservatively cover retained JSON,
    // role/receipt copies and pretty-printed serialization during publication.
    // C<=64, each event document<=256 and roles<=3 precede this arithmetic.
    const u64 roles = cfg.holdout_manifest.empty() ? 2 : 3;
    const u64 summary_slots = 2 * static_cast<u64>(lib.candidates.size()) + 2 * roles + 2;
    const u64 bytes_per_slot = 8192 * (1 + event_count);
    if (!b.add(summary_slots, 4 * bytes_per_slot))
      return co::Err(co::ErrorCode::Unavailable, "strategy: retained cash-claim summary/copy/serialization budget");
  }
  return co::Ok(RoleSpec{std::move(path), std::move(sha), std::move(role), std::move(j), b.used});
}
co::Result<std::vector<u32>> make_guard(const en::data::StrategyRoleData& role) {
  const auto& p = role.panel; const auto d = p.dates(), n = p.instruments();
  ATX_TRY(auto cid, p.field_id("close")); ATX_TRY(auto rid, p.field_id("raw_close"));
  const auto close = p.field_all(cid), raw = p.field_all(rid);
  std::vector<u32> out(d * n, 0);
  for (usize t = 1; t < d; ++t) for (usize i = 0; i < n; ++i) {
    const auto a = (t - 1) * n + i, b = t * n + i;
    bool bad = false;
    if (std::isfinite(close[a]) && std::isfinite(close[b]) && close[a] > 0 && close[b] > 0) {
      const auto r = std::log(close[b]) - std::log(close[a]);
      bad = std::abs(r) > 1.5;
      if (std::isfinite(raw[a]) && std::isfinite(raw[b]) && raw[a] > 0 && raw[b] > 0)
        bad = bad || std::abs(r) > std::abs(std::log(raw[b]) - std::log(raw[a])) + .10;
    }
    out[b] = out[a] + static_cast<u32>(bad);
  }
  return co::Ok(std::move(out));
}
co::Result<std::vector<cost::CostSurface>> surfaces(const en::data::StrategyRoleData& role,
    const RunnerConfig& cfg, std::span<const u32> guard, const std::string& cost_recipe) {
  const auto& p = role.panel; const auto n = p.instruments(), w = cfg.liquidity_window;
  ATX_TRY(auto cid, p.field_id("close")); ATX_TRY(auto rid, p.field_id("raw_close"));
  ATX_TRY(auto vid, p.field_id("volume"));
  const auto close = p.field_all(cid), raw = p.field_all(rid), vol = p.field_all(vid);
  cost::CostSurfaceRecipe recipe; recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
  recipe.impact_y = cfg.impact_y; recipe.commission_bps = cfg.commission_bps;
  recipe.max_participation = cfg.max_participation;
  std::vector<cost::CostSurface> out; out.reserve(role.score_end - role.score_begin - 2);
  std::vector<cost::CostSurfaceRow> rows(n);
  for (usize t = role.score_begin; t < role.score_end - 2; ++t) {
    for (usize i = 0; i < n; ++i) {
      auto& row = rows[i]; row = {}; row.instrument_id = role.instrument_ids[i];
      row.available_at_ns = role.mark_times_ns[t - 1];
      // Borrow is an explicit ex-ante constant scenario, independent of trailing
      // liquidity availability. No locate/observed fee claim is made.
      row.borrow_state = cost::CostInputState::Available;
      row.borrow_available_at_ns = role.mark_times_ns[t - 1];
      row.borrow_annual_fraction = cfg.annual_borrow_bps * 1e-4;
      f64 dv = 0, mean = 0, m2 = 0; usize count = 0; bool complete = true;
      for (usize k = t - w; k < t; ++k) {
        const auto b = k * n + i, a = (k - 1) * n + i;
        if (!p.in_universe(k, i) || !p.in_universe(k - 1, i) ||
            !std::isfinite(raw[b]) || raw[b] <= 0 || !std::isfinite(vol[b]) || vol[b] < 0 ||
            !std::isfinite(close[a]) || !std::isfinite(close[b]) || close[a] <= 0 || close[b] <= 0 || guard[a] != guard[b]) {
          complete = false; break;
        }
        dv += raw[b] * vol[b];
        const auto r = close[b] / close[a] - 1;
        ++count; const auto delta = r - mean; mean += delta / static_cast<f64>(count); m2 += delta * (r - mean);
      }
      if (complete && count == w && std::isfinite(dv) && dv > 0 && std::isfinite(m2) && m2 >= 0) {
        row.state = cost::CostInputState::Available;
        row.adv_dollars = dv / static_cast<f64>(w);
        row.daily_vol = std::sqrt(m2 / static_cast<f64>(w - 1));
        row.full_spread = cfg.full_spread_bps * 1e-4;
      }
    }
    cost::CostSurfaceIdentity identity{role.decision_times_ns[t], role.manifest_sha256,
        "prior-complete-raw-dollar-adv-and-adjusted-simple-return-sample-sd-v1;window=" + std::to_string(w), cost_recipe};
    ATX_TRY(auto surface, cost::CostSurface::create(recipe, identity, rows, cfg.max_working_bytes));
    out.push_back(std::move(surface));
  }
  return co::Ok(std::move(out));
}
co::Result<ex::ExecutionObjectiveContext> context(const en::data::StrategyRoleData& role,
    const RoleSpec& spec, const RunnerConfig& cfg, const Variant& variant,
    std::span<const cost::CostSurface> snapshots, std::span<const u32> guard,
    std::span<const ex::ExecutionCashClaimEvent> events = {},
    std::span<const ex::ExecutionStockTransitionEvent> stock_events = {}) {
  en::WeightPolicy policy; policy.transform = en::Transform::Rank;
  policy.winsorize_limit = 0; policy.dollar_neutral = true; policy.gross_leverage = 1;
  ex::ExecutionObjectiveConfig c; c.rule = ex::ExecutionObjectiveRule::DelayedSurfaceV2;
  c.window_begin = role.score_begin; c.window_end = role.score_end; c.maturity_end = role.score_end;
  c.min_names = cfg.min_names; c.initial_nav = cfg.initial_nav; c.max_working_bytes = cfg.max_working_bytes;
  c.rebalance_sessions = variant.cadence; c.trade_fraction = variant.fraction;
  if (!stock_events.empty())
    return ex::prepare_execution_objective_transitions(role.panel, policy, c, snapshots,
        role.mark_times_ns, role.decision_times_ns, role.instrument_ids,
        {role.manifest_sha256, spec.role, "vendor-return-proxy;" + role.clock_recipe}, events, stock_events,
        role.decision_member, guard);
  if (!events.empty())
    return ex::prepare_execution_objective_claims(role.panel, policy, c, snapshots,
        role.mark_times_ns, role.decision_times_ns, role.instrument_ids,
        {role.manifest_sha256, spec.role, "vendor-return-proxy;" + role.clock_recipe}, events,
        role.decision_member, guard);
  return ex::prepare_execution_objective(role.panel, policy, c, snapshots,
      role.mark_times_ns, role.decision_times_ns, role.instrument_ids,
      {role.manifest_sha256, spec.role, "vendor-return-proxy;" + role.clock_recipe},
      role.decision_member, guard);
}
struct Moments {
  usize n{}; f64 mean{}, m2{};
  void add(f64 x) { ++n; const auto d = x - mean; mean += d / static_cast<f64>(n); m2 += d * (x - mean); }
  f64 sharpe() const { return n > 1 && m2 > 0 ? mean / std::sqrt(m2 / static_cast<f64>(n - 1)) * std::sqrt(252.0) : nan; }
};
struct CalendarTurnover {
  usize intervals{}, traded_intervals{};
  f64 turnover{}, filled_dollars{};
};
struct CalendarReturn {
  usize observations{};
  f64 first_pretrade_nav{}, last_end_nav{};
};
std::pair<int, unsigned> calendar_month(i64 session_ns) {
  constexpr i64 day_ns = 86'400'000'000'000LL;
  const std::chrono::year_month_day date{
      std::chrono::sys_days{std::chrono::days{session_ns / day_ns}}};
  return {static_cast<int>(date.year()), static_cast<unsigned>(date.month())};
}
bool exact_keys(const Json& value, std::initializer_list<std::string_view> keys) {
  if (!value.is_object() || value.size() != keys.size()) return false;
  return std::all_of(keys.begin(), keys.end(), [&](auto key) { return value.contains(std::string(key)); });
}
co::Result<CashClaims> read_cash_claims(const RunnerConfig& cfg) {
  ATX_TRY(auto text, read_text(cfg.cash_claims_path));
  ATX_TRY(auto actual, co::sha256_hex(text));
  if (actual != cfg.cash_claims_sha256)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: cash-claim external SHA256 mismatch");
  bool duplicate = false;
  std::vector<std::set<std::string>> keys;
  const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (depth > 8) throw std::invalid_argument("cash-claim document nesting exceeds bound");
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    else if (event == Json::parse_event_t::key) duplicate |= !keys.back().insert(parsed.get<std::string>()).second;
    else if (event == Json::parse_event_t::object_end) keys.pop_back();
    return true;
  };
  auto j = Json::parse(text, callback);
  if (duplicate || !exact_keys(j, {"schema", "source_snapshot_sha256", "currency", "publication_evidence", "settlement_status", "events"}) ||
      j.at("schema") != "atx.strategy-cash-claims/v1" || j.at("currency") != "USD" ||
      j.at("publication_evidence") != "reconstructed-source-publication-research-v1" ||
      j.at("settlement_status") != "unknown" || !j.at("events").is_array() ||
      j.at("events").empty() || j.at("events").size() > 256)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: unsupported bounded cash-claim document");
  CashClaims out; out.source_sha256 = j.at("source_snapshot_sha256").get<std::string>();
  const auto evidence_pin = [](std::string_view value) {
    return hash_valid(value) && value.find_first_not_of('0') != std::string_view::npos;
  };
  if (!evidence_pin(out.source_sha256))
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: invalid cash-claim source snapshot pin");
  std::set<std::string> event_ids;
  std::set<u64> instruments;
  const auto positive_integer = [](const Json& value) -> u64 {
    if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<i64>() <= 0))
      throw std::invalid_argument("cash-claim positive integer required");
    const auto n = value.get<u64>();
    if (!n || n > static_cast<u64>(std::numeric_limits<i64>::max()))
      throw std::invalid_argument("cash-claim integer out of range");
    return n;
  };
  for (const auto& row : j.at("events")) {
    if (!exact_keys(row, {"event_id", "revision", "instrument_id", "security_id_namespace", "historical_identity",
        "identity_evidence_sha256", "completion_evidence_sha256", "basis_evidence_sha256", "evidence_urls",
        "reference_mark_ns", "reference_raw_close", "reference_adjusted_close", "effective_after_ns", "effective_by_ns",
        "available_at_ns", "recognition_mark_ns", "cash_usd_per_raw_share", "cash_excluded_from_adjusted_close"}))
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: unknown/missing cash-claim event field");
    ex::ExecutionCashClaimEvent event;
    event.event_id = row.at("event_id").get<std::string>();
    event.revision = positive_integer(row.at("revision"));
    event.instrument_id = positive_integer(row.at("instrument_id"));
    event.security_id_namespace = row.at("security_id_namespace").get<std::string>();
    event.historical_identity = row.at("historical_identity").get<std::string>();
    event.identity_evidence_sha256 = row.at("identity_evidence_sha256").get<std::string>();
    event.completion_evidence_sha256 = row.at("completion_evidence_sha256").get<std::string>();
    event.basis_evidence_sha256 = row.at("basis_evidence_sha256").get<std::string>();
    event.reference_mark_ns = static_cast<i64>(positive_integer(row.at("reference_mark_ns")));
    event.effective_after_ns = static_cast<i64>(positive_integer(row.at("effective_after_ns")));
    event.effective_by_ns = static_cast<i64>(positive_integer(row.at("effective_by_ns")));
    event.available_at_ns = static_cast<i64>(positive_integer(row.at("available_at_ns")));
    event.recognition_mark_ns = static_cast<i64>(positive_integer(row.at("recognition_mark_ns")));
    event.reference_raw_close = row.at("reference_raw_close").get<f64>();
    event.reference_adjusted_close = row.at("reference_adjusted_close").get<f64>();
    event.cash_usd_per_raw_share = row.at("cash_usd_per_raw_share").get<f64>();
    event.cash_excluded_from_adjusted_close = row.at("cash_excluded_from_adjusted_close").get<bool>();
    event.evidence = ex::ExecutionCashClaimEvidence::ReconstructedPublicationV1;
    if (event.event_id.empty() || event.event_id.size() > 128 ||
        !std::all_of(event.event_id.begin(), event.event_id.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == ':';
        }) || !event_ids.insert(event.event_id).second || !instruments.insert(event.instrument_id).second ||
        event.security_id_namespace != "spiderrock.securityID" || event.historical_identity.empty() ||
        event.historical_identity.size() > 256 || !evidence_pin(event.identity_evidence_sha256) ||
        !evidence_pin(event.completion_evidence_sha256) || !evidence_pin(event.basis_evidence_sha256) ||
        !event.cash_excluded_from_adjusted_close || !std::isfinite(event.reference_raw_close) || event.reference_raw_close <= 0 ||
        !std::isfinite(event.reference_adjusted_close) || event.reference_adjusted_close <= 0 ||
        !std::isfinite(event.cash_usd_per_raw_share) || event.cash_usd_per_raw_share <= 0 ||
        event.reference_mark_ns > event.effective_after_ns || event.effective_after_ns >= event.effective_by_ns ||
        event.available_at_ns < event.effective_by_ns || event.effective_by_ns >= event.recognition_mark_ns ||
        event.available_at_ns >= event.recognition_mark_ns ||
        !row.at("evidence_urls").is_array() || row.at("evidence_urls").empty() || row.at("evidence_urls").size() > 8)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: invalid/duplicate cash-claim event or evidence clock");
    for (const auto& url : row.at("evidence_urls")) {
      const auto value = url.get<std::string>();
      if (!value.starts_with("https://") || value.size() > 4096)
        return co::Err(co::ErrorCode::InvalidArgument, "strategy: bounded HTTPS evidence URL required");
    }
    // Only the immutable archive pin is asserted here. The exact role manifest
    // is bound later, after comparing its source snapshot with this document.
    out.events.push_back(std::move(event));
  }
  out.document = std::move(j);
  return co::Ok(std::move(out));
}
co::Result<StockTransitions> read_stock_transitions(const RunnerConfig& cfg) {
  ATX_TRY(auto text, read_text(cfg.stock_transitions_path));
  ATX_TRY(auto actual, co::sha256_hex(text));
  if (actual != cfg.stock_transitions_sha256)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: stock-transition external SHA256 mismatch");
  bool duplicate = false;
  std::vector<std::set<std::string>> keys;
  const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (depth > 8) throw std::invalid_argument("stock-transition document nesting exceeds bound");
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    else if (event == Json::parse_event_t::key) duplicate |= !keys.back().insert(parsed.get<std::string>()).second;
    else if (event == Json::parse_event_t::object_end) keys.pop_back();
    return true;
  };
  auto j = Json::parse(text, callback);
  if (duplicate || !exact_keys(j, {"schema", "source_snapshot_sha256", "currency", "publication_evidence", "settlement_status", "units_policy", "events"}) ||
      j.at("schema") != "atx.strategy-stock-transitions/v1" || j.at("currency") != "USD" ||
      j.at("publication_evidence") != "reconstructed-source-publication-research-v1" ||
      j.at("settlement_status") != "unknown" || j.at("units_policy") != "continuous-research-share-equivalents-v1" ||
      !j.at("events").is_array() || j.at("events").empty() || j.at("events").size() > 256)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: unsupported bounded stock-transition document");
  const auto evidence_pin = [](std::string_view value) {
    return hash_valid(value) && value.find_first_not_of('0') != std::string_view::npos;
  };
  StockTransitions out; out.source_sha256 = j.at("source_snapshot_sha256").get<std::string>();
  if (!evidence_pin(out.source_sha256))
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: invalid stock-transition source snapshot pin");
  const auto positive_integer = [](const Json& value) -> u64 {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<i64>() <= 0))
      throw std::invalid_argument("stock-transition positive integer required");
    const auto n = value.get<u64>();
    if (!n || n > static_cast<u64>(std::numeric_limits<i64>::max()))
      throw std::invalid_argument("stock-transition integer out of range");
    return n;
  };
  std::set<std::string> event_ids;
  std::set<u64> predecessors;
  for (const auto& row : j.at("events")) {
    if (!exact_keys(row, {"event_id", "revision", "predecessor_instrument_id", "successor_instrument_id", "security_id_namespace",
        "predecessor_historical_identity", "successor_historical_identity", "predecessor_identity_evidence_sha256",
        "successor_identity_evidence_sha256", "completion_evidence_sha256", "basis_evidence_sha256", "evidence_urls",
        "reference_mark_ns", "reference_raw_close", "reference_adjusted_close", "effective_after_ns", "effective_by_ns",
        "available_at_ns", "recognition_mark_ns", "stock_ratio_numerator", "stock_ratio_denominator",
        "successor_recognition_raw_close", "successor_recognition_adjusted_close", "fixed_cash_usd_per_predecessor_raw_share",
        "stock_and_cash_excluded_from_adjusted_close"}))
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: unknown/missing stock-transition event field");
    ex::ExecutionStockTransitionEvent event;
    event.event_id = row.at("event_id").get<std::string>();
    event.revision = positive_integer(row.at("revision"));
    event.predecessor_id = positive_integer(row.at("predecessor_instrument_id"));
    event.successor_id = positive_integer(row.at("successor_instrument_id"));
    event.security_id_namespace = row.at("security_id_namespace").get<std::string>();
    event.predecessor_identity = row.at("predecessor_historical_identity").get<std::string>();
    event.successor_identity = row.at("successor_historical_identity").get<std::string>();
    event.predecessor_identity_evidence_sha256 = row.at("predecessor_identity_evidence_sha256").get<std::string>();
    event.successor_identity_evidence_sha256 = row.at("successor_identity_evidence_sha256").get<std::string>();
    event.completion_evidence_sha256 = row.at("completion_evidence_sha256").get<std::string>();
    event.basis_evidence_sha256 = row.at("basis_evidence_sha256").get<std::string>();
    event.reference_mark_ns = static_cast<i64>(positive_integer(row.at("reference_mark_ns")));
    event.effective_after_ns = static_cast<i64>(positive_integer(row.at("effective_after_ns")));
    event.effective_by_ns = static_cast<i64>(positive_integer(row.at("effective_by_ns")));
    event.available_at_ns = static_cast<i64>(positive_integer(row.at("available_at_ns")));
    event.recognition_mark_ns = static_cast<i64>(positive_integer(row.at("recognition_mark_ns")));
    event.reference_raw_close = row.at("reference_raw_close").get<f64>();
    event.reference_adjusted_close = row.at("reference_adjusted_close").get<f64>();
    event.stock_ratio_numerator = positive_integer(row.at("stock_ratio_numerator"));
    event.stock_ratio_denominator = positive_integer(row.at("stock_ratio_denominator"));
    event.successor_raw_close = row.at("successor_recognition_raw_close").get<f64>();
    event.successor_adjusted_close = row.at("successor_recognition_adjusted_close").get<f64>();
    event.fixed_cash_usd_per_raw_share = row.at("fixed_cash_usd_per_predecessor_raw_share").get<f64>();
    event.stock_and_cash_excluded_from_adjusted_close = row.at("stock_and_cash_excluded_from_adjusted_close").get<bool>();
    if (event.event_id.empty() || event.event_id.size() > 128 ||
        !std::all_of(event.event_id.begin(), event.event_id.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == ':';
        }) || !event_ids.insert(event.event_id).second || !predecessors.insert(event.predecessor_id).second ||
        event.predecessor_id == event.successor_id || event.security_id_namespace != "spiderrock.securityID" ||
        event.predecessor_identity.empty() || event.predecessor_identity.size() > 256 ||
        event.successor_identity.empty() || event.successor_identity.size() > 256 ||
        !evidence_pin(event.predecessor_identity_evidence_sha256) || !evidence_pin(event.successor_identity_evidence_sha256) ||
        !evidence_pin(event.completion_evidence_sha256) || !evidence_pin(event.basis_evidence_sha256) ||
        !event.stock_and_cash_excluded_from_adjusted_close ||
        !std::isfinite(event.reference_raw_close) || event.reference_raw_close <= 0 ||
        !std::isfinite(event.reference_adjusted_close) || event.reference_adjusted_close <= 0 ||
        !std::isfinite(event.successor_raw_close) || event.successor_raw_close <= 0 ||
        !std::isfinite(event.successor_adjusted_close) || event.successor_adjusted_close <= 0 ||
        !std::isfinite(event.fixed_cash_usd_per_raw_share) || event.fixed_cash_usd_per_raw_share < 0 ||
        event.reference_mark_ns > event.effective_after_ns || event.effective_after_ns >= event.effective_by_ns ||
        event.available_at_ns < event.effective_by_ns || event.available_at_ns >= event.recognition_mark_ns ||
        !row.at("evidence_urls").is_array() || row.at("evidence_urls").empty() || row.at("evidence_urls").size() > 8)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: invalid/duplicate stock transition, basis or clock");
    for (const auto& url : row.at("evidence_urls")) {
      const auto value = url.get<std::string>();
      if (!value.starts_with("https://") || value.size() > 4096)
        return co::Err(co::ErrorCode::InvalidArgument, "strategy: bounded HTTPS stock evidence URL required");
    }
    out.events.push_back(std::move(event));
  }
  out.document = std::move(j);
  return co::Ok(std::move(out));
}
co::Result<Json> summarize(const al::AlphaStreams& s, const en::data::StrategyRoleData& role,
    const std::filesystem::path& series_path = {}) {
  Moments gross, net; f64 turnover = 0, execution = 0, borrow = 0, max_weight = 0;
  f64 peak_nav = 0, max_drawdown = 0;
  usize held_sum = 0, held_min = std::numeric_limits<usize>::max(), active = 0, capped = 0;
  const auto begin = s.first_realization_, end = s.realization_end_, n = role.panel.instruments();
  if (begin == 0 || begin >= end || end > role.session_keys.size())
    return co::Err(co::ErrorCode::Unavailable, "strategy: no valid mature execution calendar");
  std::map<std::pair<int, unsigned>, CalendarTurnover> months;
  std::map<int, CalendarReturn> years;
  bool deployed = false;
  i64 deployment_session = 0;
  f64 deployment_turnover = 0, deployment_dollars = 0;
  peak_nav = s.pretrade_nav_flat[begin];
  std::ofstream csv;
  if (!series_path.empty()) {
    csv.open(series_path, std::ios::binary); csv.imbue(std::locale::classic()); csv << std::setprecision(17);
    if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: series output");
    csv << "session_ns,gross_return,net_return,one_way_turnover,execution_cost,borrow_cost,pretrade_nav,end_nav,held_names,capped_names,execution_session_ns\n";
  }
  for (usize t = begin; t < end; ++t) {
    if (!s.valid_flat[t] || !std::isfinite(s.pnl_flat[t]) || !std::isfinite(s.gross_flat[t]))
      return co::Err(co::ErrorCode::Unavailable, "strategy: noncontiguous/nonfinite mature execution interval");
    gross.add(s.gross_flat[t]); net.add(s.pnl_flat[t]); turnover += s.turnover_flat[t];
    // An endpoint row closes the interval entered at the preceding actual
    // session, which may be in a different month/year or across a weekend.
    const auto execution_session = role.session_keys[t - 1];
    auto& month = months[calendar_month(execution_session)];
    ++month.intervals;
    month.traded_intervals += s.turnover_flat[t] > 0;
    month.turnover += s.turnover_flat[t];
    const auto filled_dollars = s.turnover_flat[t] * s.pretrade_nav_flat[t];
    month.filled_dollars += filled_dollars;
    if (!deployed && s.turnover_flat[t] > 0) {
      deployed = true; deployment_session = execution_session;
      deployment_turnover = s.turnover_flat[t]; deployment_dollars = filled_dollars;
    }
    auto& year = years[calendar_month(role.session_keys[t]).first];
    if (year.observations++ == 0) year.first_pretrade_nav = s.pretrade_nav_flat[t];
    year.last_end_nav = s.end_nav_flat[t];
    peak_nav = std::max(peak_nav, s.end_nav_flat[t]);
    max_drawdown = std::max(max_drawdown, 1.0 - s.end_nav_flat[t] / peak_nav);
    execution += s.execution_cost_flat[t]; borrow += s.borrow_cost_flat[t]; capped += s.capped_names_flat[t];
    usize held = 0;
    for (usize i = 0; i < n; ++i) {
      const auto w = s.pos_flat[t * n + i];
      if (!std::isfinite(w)) return co::Err(co::ErrorCode::Unavailable, "strategy: missing realized position");
      held += w != 0; max_weight = std::max(max_weight, std::abs(w));
    }
    held_sum += held; held_min = std::min(held_min, held); active += held != 0;
    if (csv) csv << role.session_keys[t] << ',' << s.gross_flat[t] << ',' << s.pnl_flat[t] << ','
        << s.turnover_flat[t] << ',' << s.execution_cost_flat[t] << ',' << s.borrow_cost_flat[t] << ','
        << s.pretrade_nav_flat[t] << ',' << s.end_nav_flat[t] << ',' << held << ',' << s.capped_names_flat[t]
        << ',' << execution_session << '\n';
  }
  if (csv.is_open()) {
    csv.close();
    if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: series flush/close");
  }
  if (net.n < 2) return co::Err(co::ErrorCode::Unavailable, "strategy: too few mature observations");
  const auto inference = en::eval::hac::mean_inference(std::span<const f64>{s.pnl_flat}.subspan(begin, end - begin),
      en::eval::hac::Kernel::BartlettV1, 5, true, true);
  Json monthly = Json::array(), annual = Json::array();
  f64 monthly_total = 0, maximum_monthly = 0;
  for (const auto& [date, values] : months) {
    const auto month = std::to_string(date.first) + (date.second < 10 ? "-0" : "-") + std::to_string(date.second);
    monthly.push_back({{"month", month}, {"execution_intervals", values.intervals},
        {"traded_intervals", values.traded_intervals}, {"one_way_turnover", values.turnover},
        {"filled_dollars_from_turnover", values.filled_dollars}});
    monthly_total += values.turnover; maximum_monthly = std::max(maximum_monthly, values.turnover);
  }
  for (const auto& [year, values] : years)
    annual.push_back({{"year", year}, {"observations", values.observations},
        {"net_compounded_return", values.last_end_nav / values.first_pretrade_nav - 1}});
  Json j{{"observations", net.n}, {"realized_begin", begin}, {"realized_end", end},
      {"gross_sharpe", gross.sharpe()}, {"net_sharpe", net.sharpe()}, {"mean_net_return", net.mean},
      {"hac_lag_requested", 5}, {"hac_lag", inference.lag}, {"hac_defined", inference.defined != 0}, {"hac_t", inference.defined ? Json(inference.t) : Json(nullptr)},
      {"total_one_way_turnover", turnover}, {"mean_daily_one_way_turnover", turnover / static_cast<f64>(net.n)},
      {"monthly21_one_way_turnover", turnover * 21 / static_cast<f64>(net.n)},
      {"monthly21_is_approximation", true}, {"calendar_month_turnover", monthly},
      {"maximum_calendar_month_one_way_turnover", maximum_monthly},
      {"calendar_month_turnover_sum", monthly_total},
      {"calendar_month_turnover_reconciliation_error", monthly_total - turnover},
      {"calendar_month_turnover_basis", "execution session at endpoint index minus one; initial deployment included"},
      {"initial_deployment", {{"occurred", deployed},
          {"execution_session_ns", deployed ? Json(deployment_session) : Json(nullptr)},
          {"one_way_turnover", deployment_turnover}, {"filled_dollars_from_turnover", deployment_dollars},
          {"share_of_total_one_way_turnover", turnover > 0 ? Json(deployment_turnover / turnover) : Json(nullptr)},
          {"included_in_totals", true},
          {"basis", "first nonzero actual-fill interval; dollar amount reconstructed as turnover times entry NAV"}}},
      {"calendar_year_net_returns", annual},
      {"calendar_year_net_return_basis", "realized endpoint session year; last end NAV / first interval pretrade NAV minus one"},
      {"summed_execution_cost_returns", execution}, {"summed_borrow_cost_returns", borrow},
      {"final_nav", s.end_nav_flat[end - 1]}, {"total_net_return", s.end_nav_flat[end - 1] / s.pretrade_nav_flat[begin] - 1}, {"mean_held_names", static_cast<f64>(held_sum) / static_cast<f64>(net.n)},
      {"minimum_held_names", held_min}, {"active_interval_fraction", static_cast<f64>(active) / static_cast<f64>(net.n)},
      {"maximum_abs_interval_entry_weight", max_weight}, {"capped_name_fills", capped},
      {"maximum_net_nav_drawdown", max_drawdown},
      {"execution_context_sha256", s.execution_context_sha256}};
  return co::Ok(std::move(j));
}
co::Result<ex::ExecutionStockTransitionStreams> execute_trial(std::span<const f64> signal,
    const ex::ExecutionObjectiveContext& ctx, f64 sign, bool claims, bool stocks) {
  if (stocks) return ex::extract_execution_signal_transitions(signal, ctx, sign);
  ex::ExecutionStockTransitionStreams out;
  if (claims) {
    ATX_TRY(out.cash, ex::extract_execution_signal_claims(signal, ctx, sign));
  } else {
    ATX_TRY(out.cash.streams, ex::extract_execution_signal(signal, ctx, sign));
  }
  return co::Ok(std::move(out));
}
co::Result<Json> summarize_claims(const ex::ExecutionCashClaimStreams& out,
    const en::data::StrategyRoleData& role, std::span<const ex::ExecutionCashClaimEvent> events,
    const std::filesystem::path& series_path) {
  const auto dates = role.panel.dates(), begin = out.streams.first_realization_, end = out.streams.realization_end_;
  if (begin >= end || end > dates || out.signed_claim_dollars.size() != dates || out.receivable_dollars.size() != dates ||
      out.payable_dollars.size() != dates || out.recognition_pnl_dollars.size() != dates ||
      out.claim_borrow_dollars.size() != dates || out.settled_cash_dollars.size() != dates || !out.payment_dates_unknown)
    return co::Err(co::ErrorCode::Internal, "strategy: invalid cash-claim diagnostic geometry/policy");
  std::ofstream csv;
  if (!series_path.empty()) {
    const auto path = series_path.parent_path() / (series_path.stem().string() + "_cash_claims.csv");
    csv.open(path, std::ios::binary); csv.imbue(std::locale::classic()); csv << std::setprecision(17);
    if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: cash-claim diagnostic output");
    csv << "session_ns,signed_unsettled_claim_dollars,receivable_dollars,payable_dollars,recognition_pnl_dollars,claim_borrow_dollars,settled_cash_dollars,nav_including_claims\n";
  }
  f64 recognition_total = 0, borrow_total = 0;
  for (usize t = begin; t < end; ++t) {
    if (!std::isfinite(out.signed_claim_dollars[t]) || !std::isfinite(out.receivable_dollars[t]) ||
        !std::isfinite(out.payable_dollars[t]) || !std::isfinite(out.recognition_pnl_dollars[t]) ||
        !std::isfinite(out.claim_borrow_dollars[t]) || !std::isfinite(out.settled_cash_dollars[t]))
      return co::Err(co::ErrorCode::Unavailable, "strategy: nonfinite cash-claim diagnostics");
    recognition_total += out.recognition_pnl_dollars[t]; borrow_total += out.claim_borrow_dollars[t];
    if (!std::isfinite(recognition_total) || !std::isfinite(borrow_total))
      return co::Err(co::ErrorCode::OutOfRange, "strategy: accumulated cash-claim diagnostics overflow");
    if (csv) csv << role.session_keys[t] << ',' << out.signed_claim_dollars[t] << ',' << out.receivable_dollars[t] << ','
        << out.payable_dollars[t] << ',' << out.recognition_pnl_dollars[t] << ',' << out.claim_borrow_dollars[t] << ','
        << out.settled_cash_dollars[t] << ',' << out.streams.end_nav_flat[t] << '\n';
  }
  if (csv.is_open()) { csv.close(); if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: cash-claim CSV flush/close"); }
  Json recognized = Json::array(), uses = Json::array();
  for (const auto& r : out.recognitions) {
    if (r.event_index >= events.size() || r.period >= dates || r.instrument_index >= role.panel.instruments() ||
        role.instrument_ids[r.instrument_index] != r.instrument_id)
      return co::Err(co::ErrorCode::Internal, "strategy: cash-claim recognition identity mismatch");
    recognized.push_back({{"event_id", events[r.event_index].event_id}, {"revision", events[r.event_index].revision},
        {"instrument_id", r.instrument_id}, {"period", r.period}, {"recognition_mark_ns", r.recognition_mark_ns},
        {"removed_equity_dollars", r.removed_equity_dollars}, {"research_share_equivalents", r.research_share_equivalents},
        {"signed_claim_dollars", r.signed_claim_dollars}, {"recognition_pnl_dollars", r.recognition_pnl_dollars},
        {"continued_annual_borrow_rate", r.continued_annual_borrow_rate}});
  }
  for (const auto& use : out.event_uses) {
    if (use.event_index >= events.size())
      return co::Err(co::ErrorCode::Internal, "strategy: cash-claim event-use identity mismatch");
    std::string label;
    switch (use.use) {
    case ex::ExecutionCashClaimUse::InRole: label = "in-role"; break;
    case ex::ExecutionCashClaimUse::PreRoleRetired: label = "pre-role-retired-no-opening-claim"; break;
    case ex::ExecutionCashClaimUse::OutsideAxis: label = "outside-axis-retained-in-identity"; break;
    case ex::ExecutionCashClaimUse::AfterRole: label = "after-role-retained-in-identity"; break;
    default: return co::Err(co::ErrorCode::Internal, "strategy: unknown cash-claim event-use policy");
    }
    uses.push_back({{"event_id", events[use.event_index].event_id}, {"instrument_id", events[use.event_index].instrument_id}, {"use", label}});
  }
  return co::Ok(Json{{"publication_evidence", "reconstructed-source-publication-research-v1"},
      {"historical_delivery_verified", false}, {"settlement_status", "unknown-no-payment-modeled"},
      {"final_signed_unsettled_claim_dollars", out.signed_claim_dollars[end - 1]},
      {"final_receivable_dollars", out.receivable_dollars[end - 1]}, {"final_payable_dollars", out.payable_dollars[end - 1]},
      {"final_settled_cash_dollars", out.settled_cash_dollars[end - 1]},
      {"final_nav_including_unsettled_claims", out.streams.end_nav_flat[end - 1]},
      {"summed_recognition_pnl_dollars", recognition_total}, {"summed_claim_borrow_dollars", borrow_total},
      {"recognitions", recognized}, {"event_uses", uses},
      {"policy", "fixed USD face; no cash receipt; receivables excluded from target NAV; payables reserve settled cash and continue last modeled short rate; no claim conversion turnover"}});
}
co::Result<Json> summarize_stocks(const ex::ExecutionStockTransitionStreams& out,
    const en::data::StrategyRoleData& role, std::span<const ex::ExecutionStockTransitionEvent> events,
    const std::filesystem::path& series_path) {
  const auto dates = role.panel.dates(), begin = out.cash.streams.first_realization_, end = out.cash.streams.realization_end_;
  if (begin >= end || end > dates || out.signed_delivered_dollars.size() != dates ||
      out.recognition_pnl_dollars.size() != dates || out.fixed_cash_component_dollars.size() != dates ||
      !out.physical_delivery_and_fraction_cash_unresolved)
    return co::Err(co::ErrorCode::Internal, "strategy: invalid stock-transition diagnostic geometry/policy");
  std::ofstream csv;
  if (!series_path.empty()) {
    const auto path = series_path.parent_path() / (series_path.stem().string() + "_stock_transitions.csv");
    csv.open(path, std::ios::binary); csv.imbue(std::locale::classic()); csv << std::setprecision(17);
    if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: stock-transition diagnostic output");
    csv << "session_ns,signed_delivered_dollars,recognition_pnl_dollars,fixed_cash_component_dollars,nav_including_claims\n";
  }
  f64 delivered_total = 0, bridge_total = 0, fixed_total = 0;
  for (usize t = begin; t < end; ++t) {
    if (!std::isfinite(out.signed_delivered_dollars[t]) || !std::isfinite(out.recognition_pnl_dollars[t]) ||
        !std::isfinite(out.fixed_cash_component_dollars[t]))
      return co::Err(co::ErrorCode::Unavailable, "strategy: nonfinite stock-transition diagnostics");
    delivered_total += out.signed_delivered_dollars[t]; bridge_total += out.recognition_pnl_dollars[t];
    fixed_total += out.fixed_cash_component_dollars[t];
    if (!std::isfinite(delivered_total) || !std::isfinite(bridge_total) || !std::isfinite(fixed_total))
      return co::Err(co::ErrorCode::OutOfRange, "strategy: accumulated stock-transition diagnostics overflow");
    if (csv) csv << role.session_keys[t] << ',' << out.signed_delivered_dollars[t] << ',' << out.recognition_pnl_dollars[t] << ','
        << out.fixed_cash_component_dollars[t] << ',' << out.cash.streams.end_nav_flat[t] << '\n';
  }
  if (csv.is_open()) { csv.close(); if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: stock-transition CSV flush/close"); }
  Json recognized = Json::array(), uses = Json::array();
  for (const auto& r : out.stock_recognitions) {
    if (r.event_index >= events.size() || r.period >= dates || r.predecessor_index >= role.panel.instruments() ||
        r.successor_index >= role.panel.instruments() || role.instrument_ids[r.predecessor_index] != r.predecessor_id ||
        role.instrument_ids[r.successor_index] != r.successor_id)
      return co::Err(co::ErrorCode::Internal, "strategy: stock-transition recognition identity mismatch");
    recognized.push_back({{"event_id", events[r.event_index].event_id}, {"revision", events[r.event_index].revision},
        {"predecessor_id", r.predecessor_id}, {"successor_id", r.successor_id}, {"period", r.period},
        {"recognition_mark_ns", r.recognition_mark_ns}, {"removed_equity_dollars", r.removed_equity_dollars},
        {"predecessor_share_equivalents", r.predecessor_share_equivalents},
        {"delivered_successor_share_equivalents", r.delivered_successor_share_equivalents},
        {"delivered_successor_dollars", r.delivered_successor_dollars}, {"fixed_cash_claim_dollars", r.fixed_cash_claim_dollars},
        {"recognition_pnl_dollars", r.recognition_pnl_dollars}, {"continued_cash_annual_borrow_rate", r.continued_cash_annual_borrow_rate}});
  }
  for (const auto& use : out.stock_event_uses) {
    if (use.event_index >= events.size())
      return co::Err(co::ErrorCode::Internal, "strategy: stock-transition event-use identity mismatch");
    std::string label;
    switch (use.use) {
    case ex::ExecutionCashClaimUse::InRole: label = "in-role"; break;
    case ex::ExecutionCashClaimUse::PreRoleRetired: label = "pre-role-retired-no-opening-delivery"; break;
    case ex::ExecutionCashClaimUse::OutsideAxis: label = "outside-axis-retained-in-identity"; break;
    case ex::ExecutionCashClaimUse::AfterRole: label = "after-role-retained-in-identity"; break;
    default: return co::Err(co::ErrorCode::Internal, "strategy: unknown stock-transition event-use policy");
    }
    uses.push_back({{"event_id", events[use.event_index].event_id}, {"predecessor_id", events[use.event_index].predecessor_id},
        {"successor_id", events[use.event_index].successor_id}, {"use", label}});
  }
  return co::Ok(Json{{"publication_evidence", "reconstructed-source-publication-research-v1"},
      {"historical_delivery_verified", false}, {"units_policy", "continuous-research-share-equivalents-v1"},
      {"physical_delivery_and_fraction_cash_unresolved", true}, {"stock_loan_discharge_assumed", false},
      {"summed_signed_delivered_dollars", delivered_total}, {"summed_recognition_pnl_dollars", bridge_total},
      {"summed_fixed_cash_component_dollars", fixed_total}, {"recognitions", recognized}, {"event_uses", uses},
      {"policy", "signed delivery adds to actual successor holding; bridge has no execution turnover; queued successor targets net against actual delivery; optional fixed cash remains an unsettled claim; no invented fractional cash"}});
}
struct ContributionCoverage { u64 eligible{}, finite{}, ranked{}; };
ContributionCoverage add_ranked(std::span<const f64> signal, f64 sign, f64 weight,
    const en::data::StrategyRoleData& role, std::span<const u8> signal_member,
    std::vector<f64>& blend, std::vector<usize>& order) {
  const auto n = role.panel.instruments();
  const auto close = role.panel.field_all(*role.panel.field_id("close"));
  ContributionCoverage coverage;
  for (usize t = 0; t < role.panel.dates(); ++t) {
    order.clear();
    const bool reported = t >= role.score_begin && t < role.score_end - 2;
    for (usize i = 0; i < n; ++i) {
      const auto k = t * n + i;
      if (signal_member[k] && role.panel.in_universe(t, i) && std::isfinite(close[k]) && close[k] > 0) {
        if (reported) ++coverage.eligible;
        if (std::isfinite(signal[k])) order.push_back(i);
      }
    }
    if (reported) coverage.finite += order.size();
    std::sort(order.begin(), order.end(), [&](usize a, usize b) {
      const auto x = sign * signal[t * n + a], y = sign * signal[t * n + b]; return x != y ? x < y : a < b;
    });
    if (order.size() < 2) continue; // fixed neutral contribution, no denominator change
    if (reported) coverage.ranked += order.size();
    for (usize a = 0; a < order.size();) {
      usize b = a + 1;
      while (b < order.size() && signal[t * n + order[a]] == signal[t * n + order[b]]) ++b;
      const auto rank = (static_cast<f64>(a) + static_cast<f64>(b - 1)) * .5 /
                        static_cast<f64>(order.size() - 1) - .5;
      for (usize k = a; k < b; ++k) blend[t * n + order[k]] += weight * rank;
      a = b;
    }
  }
  return coverage;
}
co::Result<Json> score_role(const RunnerConfig& cfg, const Library& lib, const RoleSpec& spec,
    std::vector<f64>& signs, const std::string& cost_recipe, std::ostream& progress, const CashClaims* claims = nullptr,
    const StockTransitions* stocks = nullptr) {
  progress << "loading " << spec.role << " admitted_bytes=" << spec.admitted_bytes << '\n' << std::flush;
  ATX_TRY(auto role, en::data::read_strategy_role(spec.path, cfg.max_working_bytes));
  if (role.manifest_sha256 != spec.sha)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: manifest changed after admission");
  std::vector<ex::ExecutionCashClaimEvent> events;
  if (claims) {
    if (role.source_sha256 != claims->source_sha256)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: cash-claim source changed after admission");
    events = claims->events;
    for (auto& event : events) event.panel_source_sha256 = role.manifest_sha256;
  }
  std::vector<ex::ExecutionStockTransitionEvent> stock_events;
  if (stocks) {
    if (role.source_sha256 != stocks->source_sha256)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: stock-transition source changed after admission");
    stock_events = stocks->events;
    for (auto& event : stock_events) event.panel_source_sha256 = role.manifest_sha256;
  }
  ATX_TRY(auto guard, make_guard(role));
  ATX_TRY(auto snapshots, surfaces(role, cfg, guard, cost_recipe));
  std::vector<u8> claim_member;
  std::span<const u8> signal_member = role.decision_member;
  if (claims || stocks) {
    claim_member = role.decision_member;
    const auto retire = [&](u64 instrument_id, i64 effective_by, i64 available) {
      const auto found = std::lower_bound(role.instrument_ids.begin(), role.instrument_ids.end(), instrument_id);
      if (found == role.instrument_ids.end() || *found != instrument_id) return;
      const auto i = static_cast<usize>(found - role.instrument_ids.begin());
      for (usize t = 0; t < role.panel.dates(); ++t)
        // Public completion changes decision eligibility immediately; the
        // context separately validates the first mark for claim valuation.
        if (effective_by < role.decision_times_ns[t] && available < role.decision_times_ns[t])
          claim_member[t * role.panel.instruments() + i] = 0;
    };
    for (const auto& event : events) retire(event.instrument_id, event.effective_by_ns, event.available_at_ns);
    for (const auto& event : stock_events) retire(event.predecessor_id, event.effective_by_ns, event.available_at_ns);
    signal_member = claim_member;
  }
  al::Engine engine(role.panel); engine.set_eval_mode(al::EvalMode::ResearchFast);
  ATX_TRY_VOID(engine.set_cross_section_mask(std::vector<u8>(signal_member.begin(), signal_member.end())));
  std::vector<f64> blend(role.panel.cells(), 0.0); std::vector<usize> order; order.reserve(role.panel.instruments());
  Json orientation = Json::array(), combined = Json::array();
  std::map<std::string, ContributionCoverage> family_coverage;
  const bool train = spec.role == "train";
  std::ofstream ledger(std::filesystem::path(cfg.output_directory) / (spec.role + "_trials.jsonl"), std::ios::binary);
  if (!ledger) return co::Err(co::ErrorCode::IoError, "strategy: trial receipt output");
  usize started = 0, completed = 0;
  const auto trial = [&](std::span<const f64> signal, const ex::ExecutionObjectiveContext& ctx,
                         f64 sign, const std::string& id, const std::string& kind,
                         const std::filesystem::path& csv = {}) -> co::Result<Json> {
    const auto number = ++started;
    Json receipt{{"trial", number}, {"kind", kind}, {"id", id}, {"sign", sign},
        {"context_sha256", ctx.identity_sha256()}, {"status", "started"}};
    ledger << receipt.dump() << '\n' << std::flush;
    if (!ledger) return co::Err(co::ErrorCode::IoError, "strategy: trial-start receipt");
    auto streams = execute_trial(signal, ctx, sign, claims != nullptr, stocks != nullptr);
    if (!streams) {
      receipt["status"] = "failed"; receipt["error"] = streams.error().to_string();
      ledger << receipt.dump() << '\n' << std::flush;
      return co::Err(streams.error());
    }
    ATX_TRY(auto summary, summarize(streams->cash.streams, role, csv));
    if (claims || stocks) {
      ATX_TRY(auto diagnostics, summarize_claims(streams->cash, role, events, csv));
      summary["cash_claims"] = std::move(diagnostics);
    }
    if (stocks) {
      ATX_TRY(auto diagnostics, summarize_stocks(*streams, role, stock_events, csv));
      summary["stock_transitions"] = std::move(diagnostics);
    }
    receipt["status"] = "complete"; receipt["summary"] = summary;
    ledger << receipt.dump() << '\n' << std::flush;
    if (!ledger) return co::Err(co::ErrorCode::IoError, "strategy: completed-trial receipt");
    ++completed;
    return co::Ok(std::move(summary));
  };
  if (!train && signs.size() != lib.candidates.size())
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: missing frozen TRAIN signs");
  // This scope releases the primary context before preparing the diagnostic.
  {
    ATX_TRY(auto ctx, context(role, spec, cfg, lib.variants.front(), snapshots, guard, events, stock_events));
    for (usize k = 0; k < lib.candidates.size(); ++k) {
      const auto& candidate = lib.candidates[k]; engine.reset();
      ATX_TRY(auto evaluated, engine.evaluate(candidate.program));
      if (evaluated.alphas.size() != 1) return co::Err(co::ErrorCode::Internal, "strategy: missing VM root");
      const auto& signal = evaluated.alphas.front().values;
      if (train) {
        Json two = Json::array();
        for (auto sign : {1.0, -1.0}) {
          ATX_TRY(auto summary, trial(signal, ctx, sign, candidate.id, "candidate-orientation"));
          two.push_back(std::move(summary));
        }
        if (!two[0]["net_sharpe"].is_number() || !two[1]["net_sharpe"].is_number())
          return co::Err(co::ErrorCode::Unavailable, "strategy: unscorable TRAIN orientation: " + candidate.id);
        const auto plus = two[0]["net_sharpe"].get<f64>(), minus = two[1]["net_sharpe"].get<f64>();
        if (!std::isfinite(plus) || !std::isfinite(minus))
          return co::Err(co::ErrorCode::Unavailable, "strategy: nonfinite TRAIN orientation: " + candidate.id);
        signs.push_back(minus > plus ? -1.0 : 1.0); // exact finite tie pins +1
        orientation.push_back({{"id", candidate.id}, {"family", candidate.family}, {"weight", candidate.weight},
            {"sign", signs.back()}, {"plus", two[0]}, {"minus", two[1]}});
      }
      const auto coverage = add_ranked(signal, signs[k], candidate.weight, role, signal_member, blend, order);
      auto& family = family_coverage[candidate.family];
      family.eligible += coverage.eligible; family.finite += coverage.finite; family.ranked += coverage.ranked;
      progress << spec.role << " candidate " << (k + 1) << '/' << lib.candidates.size() << ' ' << candidate.id << '\n' << std::flush;
    }
    ATX_TRY(auto summary, trial(blend, ctx, 1.0, lib.variants[0].id, "combined-strategy",
        std::filesystem::path(cfg.output_directory) / (spec.role + "_" + lib.variants[0].id + ".csv")));
    summary["variant"] = lib.variants[0].id; combined.push_back(std::move(summary));
  }
  {
    ATX_TRY(auto ctx, context(role, spec, cfg, lib.variants[1], snapshots, guard, events, stock_events));
    ATX_TRY(auto summary, trial(blend, ctx, 1.0, lib.variants[1].id, "combined-strategy",
        std::filesystem::path(cfg.output_directory) / (spec.role + "_" + lib.variants[1].id + ".csv")));
    summary["variant"] = lib.variants[1].id; combined.push_back(std::move(summary));
  }
  Json coverage_report = Json::array();
  for (const auto& [family, coverage] : family_coverage)
    coverage_report.push_back({{"family", family}, {"candidate_member_cells", coverage.eligible},
        {"finite_candidate_cells", coverage.finite}, {"ranked_contribution_cells", coverage.ranked},
        {"fixed_denominator_contribution_fraction", coverage.eligible ? Json(static_cast<f64>(coverage.ranked) / static_cast<f64>(coverage.eligible)) : Json(nullptr)},
        {"basis", "sum across fixed family candidates and mature decision dates; member/source-present/positive current close; rank needs >=2;no renormalization"}});
  ledger.close();
  if (!ledger) return co::Err(co::ErrorCode::IoError, "strategy: trial ledger flush/close");
  return co::Ok(Json{{"role", spec.role}, {"manifest_sha256", role.manifest_sha256},
      {"source_sha256", role.source_sha256}, {"membership_recipe", role.membership_recipe},
      {"clock_recipe", role.clock_recipe}, {"admitted_working_bytes", spec.admitted_bytes},
      {"trial_attempts", started}, {"completed_trials", completed},
      {"family_contribution_coverage", coverage_report},
      {"dates", role.panel.dates()}, {"instruments", role.panel.instruments()},
      {"score_begin", role.score_begin}, {"score_end", role.score_end}, {"orientations", orientation}, {"combined", combined}});
}
co::Status write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream out(path, std::ios::binary); if (!out) return co::Err(co::ErrorCode::IoError, "strategy: output JSON");
  out << j.dump(2) << '\n'; out.close();
  return out ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "strategy: failed output JSON"));
}
} // namespace

co::Status run(const RunnerConfig& cfg, std::ostream& progress) {
  try {
    const bool claims_enabled = !cfg.cash_claims_path.empty();
    if (claims_enabled != !cfg.cash_claims_sha256.empty() ||
        (claims_enabled && !hash_valid(cfg.cash_claims_sha256)))
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: cash-claims path and lowercase SHA256 must be paired");
    const bool stocks_enabled = !cfg.stock_transitions_path.empty();
    if (stocks_enabled != !cfg.stock_transitions_sha256.empty() ||
        (stocks_enabled && !hash_valid(cfg.stock_transitions_sha256)))
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: stock-transitions path and lowercase SHA256 must be paired");
    if (cfg.output_directory.empty() || cfg.max_working_bytes < (32ULL << 20) || cfg.max_working_bytes > (16ULL << 30) ||
        cfg.initial_nav != 1'000'000'000.0 || cfg.min_names < 2 || cfg.liquidity_window < 2 || cfg.liquidity_window > 252 ||
        !std::isfinite(cfg.full_spread_bps) || cfg.full_spread_bps < 0 ||
        !std::isfinite(cfg.commission_bps) || cfg.commission_bps < 0 ||
        !std::isfinite(cfg.annual_borrow_bps) || cfg.annual_borrow_bps < 0 ||
        !std::isfinite(cfg.impact_y) || cfg.impact_y < 0 ||
        !std::isfinite(cfg.max_participation) || cfg.max_participation <= 0 || cfg.max_participation > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: invalid bounded $1bn scenario");
    CashClaims claim_document;
    if (claims_enabled) {
      ATX_TRY(auto admitted_claims, read_cash_claims(cfg));
      claim_document = std::move(admitted_claims);
    }
    const auto* claims = claims_enabled ? &claim_document : nullptr;
    StockTransitions stock_document;
    if (stocks_enabled) {
      ATX_TRY(auto admitted_stocks, read_stock_transitions(cfg));
      stock_document = std::move(admitted_stocks);
    }
    const auto* stocks = stocks_enabled ? &stock_document : nullptr;
    ATX_TRY(auto lib, read_library(cfg));
    std::vector<RoleSpec> roles;
    ATX_TRY(auto train, admit_role(cfg, lib, cfg.train_manifest, cfg.train_sha256, "train", claims, stocks)); roles.push_back(std::move(train));
    ATX_TRY(auto validation, admit_role(cfg, lib, cfg.validation_manifest, cfg.validation_sha256, "validation", claims, stocks)); roles.push_back(std::move(validation));
    if (!cfg.holdout_manifest.empty()) {
      ATX_TRY(auto holdout, admit_role(cfg, lib, cfg.holdout_manifest, cfg.holdout_sha256, "holdout", claims, stocks)); roles.push_back(std::move(holdout));
    }
    for (usize i = 1; i < roles.size(); ++i)
      if (roles[i - 1].manifest.at("score_end_ns").get<i64>() > roles[i].manifest.at("score_start_ns").get<i64>())
        return co::Err(co::ErrorCode::InvalidArgument, "strategy: overlapping/nonchronological score roles");
    Json recipe{{"schema", "atx.dsl-combined-execution/v1"}, {"library_sha256", lib.sha}, {"seed", cfg.seed},
        {"initial_nav", cfg.initial_nav}, {"primary_variant", "weekly_partial25"}, {"min_names", cfg.min_names},
        {"liquidity_window", cfg.liquidity_window}, {"full_spread_bps", cfg.full_spread_bps},
        {"commission_bps", cfg.commission_bps}, {"annual_borrow_bps", cfg.annual_borrow_bps},
        {"impact_y", cfg.impact_y}, {"max_participation", cfg.max_participation},
        {"max_working_bytes", cfg.max_working_bytes}, {"cost_input_status", "declared-unfitted-scenario-not-observed-no-locate-proof"},
        {"vm", "ResearchFast;corrected-kernels;full-asof-CS-member-mask"},
        {"blend", "equal-family/equal-within-family;centered-tied-rank;missing-zero;fixed-denominators;final-rank-neutral-gross1"},
        {"return_guard", "abs-log<=1.5;adjusted-abs-log<=raw-abs-log+.10"},
        {"timing", "decision-fixed-dollar;delay1;endpoint-return;mature-contiguous-prefix"},
        {"turnover", "sum absolute actual filled dollars / interval-entry pretrade NAV;initial deployment included;monthly=21*mean"},
        {"terminal", "marked holdings;no automatic liquidation"},
        {"participation", "one-percent default of prior ADV per execution;target dollars may remain partially unfilled"},
        {"membership_exit", "partial target unwind on cadence;not immediate flatten"},
        {"statistical_claim", "TRAIN orientation fit;no selection-adjusted significance or guaranteed net Sharpe"}};
    if (claims) {
      recipe["schema"] = "atx.dsl-combined-execution/cash-claims-v2";
      recipe["cash_claims"] = {{"schema", "atx.strategy-cash-claims/v1"}, {"sha256", cfg.cash_claims_sha256},
          {"source_snapshot_sha256", claims->source_sha256}, {"event_count", claims->events.size()},
          {"publication_evidence", "reconstructed-source-publication-research-v1"},
          {"historical_delivery_verified", false}, {"settlement_status", "unknown-no-payment-modeled"},
          {"signal_support_policy", "causal-decision-retirement-v1: VM ranks and fixed blend exclude when effective-by and public availability are strictly before decision; context validates first eligible valuation mark; source payload retained"}};
    }
    if (stocks) {
      recipe["schema"] = "atx.dsl-combined-execution/stock-transitions-v3";
      recipe["stock_transitions"] = {{"schema", "atx.strategy-stock-transitions/v1"}, {"sha256", cfg.stock_transitions_sha256},
          {"source_snapshot_sha256", stocks->source_sha256}, {"event_count", stocks->events.size()},
          {"publication_evidence", "reconstructed-source-publication-research-v1"}, {"historical_delivery_verified", false},
          {"units_policy", "continuous-research-share-equivalents-v1"}, {"settlement_status", "unknown-no-payment-modeled"},
          {"fraction_cash", "unresolved-no-physical-rounding-or-invented-cash"},
          {"borrow_policy", "modeled-successor-no-loan-discharge-v1"},
          {"signal_support_policy", "causal-decision-retirement-v1: predecessor excluded when effective-by and public availability strictly precede decision; successor membership unchanged; source payload retained"}};
    }
    for (const auto& role : roles) recipe["role_manifest_sha256"][role.role] = role.sha;
    ATX_TRY(auto recipe_sha, co::sha256_hex(recipe.dump()));
    std::error_code ec;
    if (!std::filesystem::create_directory(cfg.output_directory, ec))
      return co::Err(co::ErrorCode::AlreadyExists, "strategy: output directory must be new; " + ec.message());
    ATX_TRY_VOID(write_json(std::filesystem::path(cfg.output_directory) / "recipe.json", recipe));
    Json report{{"status", "running"}, {"recipe_sha256", recipe_sha}, {"train_hypotheses_planned", 2 * lib.candidates.size() + 2},
        {"validation_hypotheses_planned", 2}, {"holdout_requested", !cfg.holdout_manifest.empty()}, {"roles", Json::array()}};
    if (claims) report["cash_claims_evidence"] = claims->document;
    if (stocks) report["stock_transitions_evidence"] = stocks->document;
    ATX_TRY_VOID(write_json(std::filesystem::path(cfg.output_directory) / "summary.json", report));
    std::vector<f64> signs; signs.reserve(lib.candidates.size());
    for (const auto& role : roles) {
      auto scored = score_role(cfg, lib, role, signs, recipe.dump(), progress, claims, stocks);
      if (!scored) {
        report["status"] = "failed"; report["error"] = scored.error().to_string();
        ATX_TRY_VOID(write_json(std::filesystem::path(cfg.output_directory) / "summary.json", report));
        return co::Err(scored.error());
      }
      report["roles"].push_back(std::move(*scored));
      if (role.role == "train") {
        report["frozen_signs"] = signs;
        ATX_TRY(auto fitted_sha, co::sha256_hex(Json{{"recipe_sha256", recipe_sha}, {"signs", signs},
            {"training_manifest_sha256", role.sha}}.dump())); report["fitted_strategy_sha256"] = fitted_sha;
      }
      ATX_TRY_VOID(write_json(std::filesystem::path(cfg.output_directory) / "summary.json", report));
    }
    report["status"] = "complete";
    return write_json(std::filesystem::path(cfg.output_directory) / "summary.json", report);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::Unavailable, "strategy: allocation failed within admitted payload/workspace");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("strategy: ") + e.what());
  }
}

int dispatch(int argc, char** argv, std::ostream& out, std::ostream& err) {
  RunnerConfig cfg;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "equity-strategy --library JSON --library-sha256 SHA --train MANIFEST --train-sha256 SHA "
               "--validation MANIFEST --validation-sha256 SHA --output NEWDIR [--holdout MANIFEST --holdout-sha256 SHA] "
               "[--cash-claims JSON --cash-claims-sha256 SHA] "
               "[--stock-transitions JSON --stock-transitions-sha256 SHA] "
               "[--max-memory-mib N --min-names N --seed N --spread-bps X --commission-bps X --borrow-bps X --impact-y X --max-participation X]\n";
        return 0;
      }
      if (++i >= argc) throw std::invalid_argument("missing value for " + key);
      const std::string v = argv[i];
      const auto integer = [&]() -> u64 { usize used{}; if (v.empty() || v.front() == '-') throw std::invalid_argument("unsigned integer required");
        auto x = std::stoull(v, &used); if (used != v.size()) throw std::invalid_argument("invalid integer"); return x; };
      const auto real = [&]() { usize used{}; auto x = std::stod(v, &used); if (used != v.size()) throw std::invalid_argument("invalid real"); return x; };
      if (key == "--library") cfg.library_path = v;
      else if (key == "--library-sha256") cfg.library_sha256 = v;
      else if (key == "--train") cfg.train_manifest = v;
      else if (key == "--train-sha256") cfg.train_sha256 = v;
      else if (key == "--validation") cfg.validation_manifest = v;
      else if (key == "--validation-sha256") cfg.validation_sha256 = v;
      else if (key == "--holdout") cfg.holdout_manifest = v;
      else if (key == "--holdout-sha256") cfg.holdout_sha256 = v;
      else if (key == "--cash-claims") cfg.cash_claims_path = v;
      else if (key == "--cash-claims-sha256") cfg.cash_claims_sha256 = v;
      else if (key == "--stock-transitions") cfg.stock_transitions_path = v;
      else if (key == "--stock-transitions-sha256") cfg.stock_transitions_sha256 = v;
      else if (key == "--output") cfg.output_directory = v;
      else if (key == "--max-memory-mib") { auto x = integer(); if (x > 16384) throw std::invalid_argument("memory limit"); cfg.max_working_bytes = x << 20; }
      else if (key == "--min-names") cfg.min_names = static_cast<usize>(integer());
      else if (key == "--seed") cfg.seed = integer();
      else if (key == "--spread-bps") cfg.full_spread_bps = real();
      else if (key == "--commission-bps") cfg.commission_bps = real();
      else if (key == "--borrow-bps") cfg.annual_borrow_bps = real();
      else if (key == "--impact-y") cfg.impact_y = real();
      else if (key == "--max-participation") cfg.max_participation = real();
      else throw std::invalid_argument("unknown option " + key);
    }
    auto result = run(cfg, out);
    if (!result) { err << result.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
