#include "strategy_runner.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
    std::string path, std::string sha, std::string role) {
  ATX_TRY(auto j, read_pinned(path, sha));
  const auto d = j.at("dates").get<u64>(), n = j.at("instruments").get<u64>();
  const auto begin = j.at("score_begin").get<u64>(), end = j.at("score_end").get<u64>();
  if (!d || d > 4096 || !n || n > 20000 || begin < lib.lookback || begin <= cfg.liquidity_window ||
      end != d || begin >= end || end - begin < 4)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: role geometry/warmup/maturity");
  const auto cells = d * n, decisions = end - begin - 2;
  Budget b{cfg.max_working_bytes, 0};
  // Panel/support=26, guard=4, owned VM mask=1, aggregate=8, output=8,
  // execution owned price/support=14, execution positions=8. Additional 32
  // bytes/cell conservatively cover VM scratch; slots charge twice the compiled peak to cover grow-before-release.
  // 128/name/snapshot exceeds CostSurfaceRow payload; strings/slack separately.
  if (!b.add(1, 32ULL << 20) || !b.add(cells, 101 + 16 * lib.max_slots) ||
      !b.add(decisions * n, 128) || !b.add(decisions, 8192) || !b.add(d, 256) || !b.add(n, 1024))
    return co::Err(co::ErrorCode::Unavailable, "strategy: combined role/VM/surface/context/scratch budget; use a smaller declared role");
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
    std::span<const cost::CostSurface> snapshots, std::span<const u32> guard) {
  en::WeightPolicy policy; policy.transform = en::Transform::Rank;
  policy.winsorize_limit = 0; policy.dollar_neutral = true; policy.gross_leverage = 1;
  ex::ExecutionObjectiveConfig c; c.rule = ex::ExecutionObjectiveRule::DelayedSurfaceV2;
  c.window_begin = role.score_begin; c.window_end = role.score_end; c.maturity_end = role.score_end;
  c.min_names = cfg.min_names; c.initial_nav = cfg.initial_nav; c.max_working_bytes = cfg.max_working_bytes;
  c.rebalance_sessions = variant.cadence; c.trade_fraction = variant.fraction;
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
co::Result<Json> summarize(const al::AlphaStreams& s, const en::data::StrategyRoleData& role,
    const std::filesystem::path& series_path = {}) {
  Moments gross, net; f64 turnover = 0, execution = 0, borrow = 0, max_weight = 0;
  f64 peak_nav = 0, max_drawdown = 0;
  usize held_sum = 0, held_min = std::numeric_limits<usize>::max(), active = 0, capped = 0;
  const auto begin = s.first_realization_, end = s.realization_end_, n = role.panel.instruments();
  if (begin >= end) return co::Err(co::ErrorCode::Unavailable, "strategy: no mature execution interval");
  peak_nav = s.pretrade_nav_flat[begin];
  std::ofstream csv;
  if (!series_path.empty()) {
    csv.open(series_path, std::ios::binary); csv.imbue(std::locale::classic()); csv << std::setprecision(17);
    if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: series output");
    csv << "session_ns,gross_return,net_return,one_way_turnover,execution_cost,borrow_cost,pretrade_nav,end_nav,held_names,capped_names\n";
  }
  for (usize t = begin; t < end; ++t) {
    if (!s.valid_flat[t] || !std::isfinite(s.pnl_flat[t]) || !std::isfinite(s.gross_flat[t]))
      return co::Err(co::ErrorCode::Unavailable, "strategy: noncontiguous/nonfinite mature execution interval");
    gross.add(s.gross_flat[t]); net.add(s.pnl_flat[t]); turnover += s.turnover_flat[t];
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
        << s.pretrade_nav_flat[t] << ',' << s.end_nav_flat[t] << ',' << held << ',' << s.capped_names_flat[t] << '\n';
  }
  if (csv.is_open()) {
    csv.close();
    if (!csv) return co::Err(co::ErrorCode::IoError, "strategy: series flush/close");
  }
  if (net.n < 2) return co::Err(co::ErrorCode::Unavailable, "strategy: too few mature observations");
  const auto inference = en::eval::hac::mean_inference(std::span<const f64>{s.pnl_flat}.subspan(begin, end - begin),
      en::eval::hac::Kernel::BartlettV1, 5, true, true);
  Json j{{"observations", net.n}, {"realized_begin", begin}, {"realized_end", end},
      {"gross_sharpe", gross.sharpe()}, {"net_sharpe", net.sharpe()}, {"mean_net_return", net.mean},
      {"hac_lag_requested", 5}, {"hac_lag", inference.lag}, {"hac_defined", inference.defined != 0}, {"hac_t", inference.defined ? Json(inference.t) : Json(nullptr)},
      {"total_one_way_turnover", turnover}, {"mean_daily_one_way_turnover", turnover / static_cast<f64>(net.n)},
      {"monthly21_one_way_turnover", turnover * 21 / static_cast<f64>(net.n)},
      {"summed_execution_cost_returns", execution}, {"summed_borrow_cost_returns", borrow},
      {"final_nav", s.end_nav_flat[end - 1]}, {"total_net_return", s.end_nav_flat[end - 1] / s.pretrade_nav_flat[begin] - 1}, {"mean_held_names", static_cast<f64>(held_sum) / static_cast<f64>(net.n)},
      {"minimum_held_names", held_min}, {"active_interval_fraction", static_cast<f64>(active) / static_cast<f64>(net.n)},
      {"maximum_abs_interval_entry_weight", max_weight}, {"capped_name_fills", capped},
      {"maximum_net_nav_drawdown", max_drawdown},
      {"execution_context_sha256", s.execution_context_sha256}};
  return co::Ok(std::move(j));
}
struct ContributionCoverage { u64 eligible{}, finite{}, ranked{}; };
ContributionCoverage add_ranked(std::span<const f64> signal, f64 sign, f64 weight,
    const en::data::StrategyRoleData& role, std::vector<f64>& blend, std::vector<usize>& order) {
  const auto n = role.panel.instruments();
  const auto close = role.panel.field_all(*role.panel.field_id("close"));
  ContributionCoverage coverage;
  for (usize t = 0; t < role.panel.dates(); ++t) {
    order.clear();
    const bool reported = t >= role.score_begin && t < role.score_end - 2;
    for (usize i = 0; i < n; ++i) {
      const auto k = t * n + i;
      if (role.decision_member[k] && role.panel.in_universe(t, i) && std::isfinite(close[k]) && close[k] > 0) {
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
    std::vector<f64>& signs, const std::string& cost_recipe, std::ostream& progress) {
  progress << "loading " << spec.role << " admitted_bytes=" << spec.admitted_bytes << '\n' << std::flush;
  ATX_TRY(auto role, en::data::read_strategy_role(spec.path, cfg.max_working_bytes));
  if (role.manifest_sha256 != spec.sha)
    return co::Err(co::ErrorCode::InvalidArgument, "strategy: manifest changed after admission");
  ATX_TRY(auto guard, make_guard(role));
  ATX_TRY(auto snapshots, surfaces(role, cfg, guard, cost_recipe));
  al::Engine engine(role.panel); engine.set_eval_mode(al::EvalMode::ResearchFast);
  ATX_TRY_VOID(engine.set_cross_section_mask(role.decision_member));
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
    auto streams = ex::extract_execution_signal(signal, ctx, sign);
    if (!streams) {
      receipt["status"] = "failed"; receipt["error"] = streams.error().to_string();
      ledger << receipt.dump() << '\n' << std::flush;
      return co::Err(streams.error());
    }
    ATX_TRY(auto summary, summarize(*streams, role, csv));
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
    ATX_TRY(auto ctx, context(role, spec, cfg, lib.variants.front(), snapshots, guard));
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
      const auto coverage = add_ranked(signal, signs[k], candidate.weight, role, blend, order);
      auto& family = family_coverage[candidate.family];
      family.eligible += coverage.eligible; family.finite += coverage.finite; family.ranked += coverage.ranked;
      progress << spec.role << " candidate " << (k + 1) << '/' << lib.candidates.size() << ' ' << candidate.id << '\n' << std::flush;
    }
    ATX_TRY(auto summary, trial(blend, ctx, 1.0, lib.variants[0].id, "combined-strategy",
        std::filesystem::path(cfg.output_directory) / (spec.role + "_" + lib.variants[0].id + ".csv")));
    summary["variant"] = lib.variants[0].id; combined.push_back(std::move(summary));
  }
  {
    ATX_TRY(auto ctx, context(role, spec, cfg, lib.variants[1], snapshots, guard));
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
    if (cfg.output_directory.empty() || cfg.max_working_bytes < (32ULL << 20) || cfg.max_working_bytes > (16ULL << 30) ||
        cfg.initial_nav != 1'000'000'000.0 || cfg.min_names < 2 || cfg.liquidity_window < 2 || cfg.liquidity_window > 252 ||
        !std::isfinite(cfg.full_spread_bps) || cfg.full_spread_bps < 0 ||
        !std::isfinite(cfg.commission_bps) || cfg.commission_bps < 0 ||
        !std::isfinite(cfg.annual_borrow_bps) || cfg.annual_borrow_bps < 0 ||
        !std::isfinite(cfg.impact_y) || cfg.impact_y < 0 ||
        !std::isfinite(cfg.max_participation) || cfg.max_participation <= 0 || cfg.max_participation > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "strategy: invalid bounded $1bn scenario");
    ATX_TRY(auto lib, read_library(cfg));
    std::vector<RoleSpec> roles;
    ATX_TRY(auto train, admit_role(cfg, lib, cfg.train_manifest, cfg.train_sha256, "train")); roles.push_back(std::move(train));
    ATX_TRY(auto validation, admit_role(cfg, lib, cfg.validation_manifest, cfg.validation_sha256, "validation")); roles.push_back(std::move(validation));
    if (!cfg.holdout_manifest.empty()) {
      ATX_TRY(auto holdout, admit_role(cfg, lib, cfg.holdout_manifest, cfg.holdout_sha256, "holdout")); roles.push_back(std::move(holdout));
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
    for (const auto& role : roles) recipe["role_manifest_sha256"][role.role] = role.sha;
    ATX_TRY(auto recipe_sha, co::sha256_hex(recipe.dump()));
    std::error_code ec;
    if (!std::filesystem::create_directory(cfg.output_directory, ec))
      return co::Err(co::ErrorCode::AlreadyExists, "strategy: output directory must be new; " + ec.message());
    ATX_TRY_VOID(write_json(std::filesystem::path(cfg.output_directory) / "recipe.json", recipe));
    Json report{{"status", "running"}, {"recipe_sha256", recipe_sha}, {"train_hypotheses_planned", 2 * lib.candidates.size() + 2},
        {"validation_hypotheses_planned", 2}, {"holdout_requested", !cfg.holdout_manifest.empty()}, {"roles", Json::array()}};
    ATX_TRY_VOID(write_json(std::filesystem::path(cfg.output_directory) / "summary.json", report));
    std::vector<f64> signs; signs.reserve(lib.candidates.size());
    for (const auto& role : roles) {
      auto scored = score_role(cfg, lib, role, signs, recipe.dump(), progress);
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
