#include "book/nav_rules.hpp"

#include <array>
#include <initializer_list>
#include <utility>
#include <nlohmann/json.hpp>
#include "strategy_nav_replay.hpp"
#include "strategy_risk_target.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::nav_rules {
namespace {
using Json = nlohmann::json;

// One params_schema property: its type, its nav flag (x-flag) and the extra keywords (the shape
// of risk_target's rows).
Json property(const char* type, const char* flag, Json keywords) {
  keywords["type"] = type;
  keywords["x-flag"] = flag;
  return keywords;
}
Json no_params() { return Json::object(); }

// aim-partial-v5 (validate_target_config): theta in (0, 1], dust in [0, .5], exit rate in (0, 1];
// the defaults are TargetReplayConfig's, the command line's.
Json aim_partial_v5_params() {
  const TargetReplayConfig d{};
  Json theta{{"exclusiveMinimum", 0}, {"maximum", 1}, {"default", d.trade_fraction}};
  Json dust{{"minimum", 0}, {"maximum", 0.5}, {"default", d.dust_multiple}};
  Json exit_rate{{"exclusiveMinimum", 0}, {"maximum", 1}, {"default", d.exit_rate}};
  return Json{{"trade_fraction", property("number", "--trade-fraction", std::move(theta))},
              {"dust_multiple", property("number", "--dust-multiple", std::move(dust))},
              {"exit_rate", property("number", "--exit-rate", std::move(exit_rate))}};
}
// per-name-v1 (validate_nav_config): rra and lambda in (0, 1e6], 0 < min <= max <= 1.
Json per_name_v1_params() {
  const auto positive = [](f64 maximum, f64 fallback) {
    return Json{{"exclusiveMinimum", 0}, {"maximum", maximum}, {"default", fallback}};
  };
  return Json{{"rate_rra", property("number", "--rate-rra", positive(1e6, nav_rate_rra))},
              {"rate_lambda", property("number", "--rate-lambda", positive(1e6, nav_rate_lambda))},
              {"rate_min", property("number", "--rate-min", positive(1, nav_rate_min))},
              {"rate_max", property("number", "--rate-max", positive(1, nav_rate_max))}};
}

constexpr std::array<std::string_view, 0> none{};
constexpr std::array<std::string_view, 4> other_rules{"aim-partial-v6", "spo-v1", "spo-v2",
                                                      "spo-v3"};
// The fixed rate is required by aim-partial-v6, the spo rules and two-speed-v1 (v7 and replay
// refusals); per-name-v1 is refused with them.
constexpr std::array<std::string_view, 5> fixed_rate_rules{"aim-partial-v6", "spo-v1", "spo-v2",
                                                           "spo-v3", "two-speed-v1"};
constexpr std::array<std::string_view, 5> two_speed_refuses{"per-name-v1", "aim-partial-v6",
                                                            "spo-v1", "spo-v2", "spo-v3"};

constexpr std::array<Row, 3> trade_rates{{
    {"aim-partial-v5", Kind::TradeRate, "--rule aim-partial-v5", aim_partial_v5_params, none,
     other_rules,
     "aim-partial-v5: every member moves theta (--trade-fraction) of its gap toward aim = L x "
     "desired unless the gap is inside the dust band dust / N_d; nonmembers exit at exit_rate"},
    {"per-name-v1", Kind::TradeRate, "--rate per-name-v1", per_name_v1_params, none,
     fixed_rate_rules,
     "per-name-v1 (T36): aim-partial-v5's theta per name from its cost and risk (rra, lambda), "
     "clipped to [rate_min, rate_max]; needs --rule aim-partial-v5; one shared rate buffer, so "
     "--book-workers 1 only"},
    {"two-speed-nav-v1", Kind::TradeRate, "", no_params, none, two_speed_refuses,
     "the NAV half of the composition rule two-speed-v1 (named in rules as two-speed-v1, which "
     "sets --two-speed two-speed-v1 on both exes): a virtual fast sleeve at theta_f = 1 - "
     "2^(-C/5) at cadence C, the slow remainder at theta_s = .05, netted"},
}};

constexpr std::array<Row, 5> cost_laws{{
    {"flat-bps-v1", Kind::CostLaw, "", no_params, none, none,
     "S1 linear-6bps-stale5-v1: 6 bps per dollar traded (engine FlatBpsCost); a fixed scenario of "
     "every run, not a flag"},
    {"sqrt-impact-v1", Kind::CostLaw, "", no_params, none, none,
     "S2 modeled-1bn-stale5-v1 (PRIMARY) and S3 modeled-1bn-terminal-adverse-v1: half spread + "
     "Y sigma (|F| / ADV)^delta under a 1% ADV cap (engine SqrtImpactCost); fixed scenarios of "
     "every run, not a flag"},
    {"ko-invariance-v1", Kind::CostLaw, "", no_params, none, none,
     "Kyle-Obizhaeva invariance square-root law: the stress book modeled-1bn-ko-v1 beside S2; on "
     "with cost-sqrt-v2"},
    {"fim-live-v1", Kind::CostLaw, "", no_params, none, none,
     "Frazzini-Israel-Moskowitz live-trade law: the stress book modeled-1bn-fim-v1 beside S2; on "
     "with cost-sqrt-v2"},
    {cost_sqrt_v2_id, Kind::CostLaw, "--cost-law cost-sqrt-v2", no_params, none, none,
     "cost model v2: the ko-invariance-v1 and fim-live-v1 stress books beside S2 (S1, S2, S3 "
     "unchanged); byte for byte the run --cost-v2 gives (one of the two, never both)"},
}};

// The target slot (K-P9-7 kind target): empty in this build.
constexpr std::span<const Row> targets{};

Json row_json(const Row& r) {
  Json required = Json::array();
  for (const auto key : r.required) required.push_back(std::string(key));
  Json incompatible = Json::array();
  for (const auto id : r.incompatible) incompatible.push_back(std::string(id));
  return Json{{"id", std::string(r.id)},
              {"kind", std::string(kind_name(r.kind))},
              {"x-flag", r.x_flag.empty() ? Json(nullptr) : Json(std::string(r.x_flag))},
              {"params_schema", Json{{"type", "object"},
                                     {"additionalProperties", false},
                                     {"required", std::move(required)},
                                     {"properties", r.params()}}},
              {"incompatible", std::move(incompatible)},
              {"description", std::string(r.description)}};
}
} // namespace

std::string_view kind_name(Kind kind) noexcept {
  switch (kind) {
  case Kind::Leverage: return "leverage";
  case Kind::TradeRate: return "trade-rate";
  case Kind::CostLaw: return "cost-law";
  case Kind::Target: return "target";
  }
  return "unknown";
}

std::span<const Row> trade_rate_rows() noexcept { return trade_rates; }
std::span<const Row> cost_law_rows() noexcept { return cost_laws; }
std::span<const Row> target_rows() noexcept { return targets; }

const Row* find(Kind kind, std::string_view id) noexcept {
  std::span<const Row> rows;
  switch (kind) {
  case Kind::TradeRate: rows = trade_rates; break;
  case Kind::CostLaw: rows = cost_laws; break;
  case Kind::Target: rows = targets; break;
  case Kind::Leverage: return nullptr; // risk_target's rows (no lookup needed)
  }
  for (const auto& row : rows)
    if (row.id == id) return &row;
  return nullptr;
}

std::string flag_ids(Kind kind) {
  std::span<const Row> rows;
  switch (kind) {
  case Kind::TradeRate: rows = trade_rates; break;
  case Kind::CostLaw: rows = cost_laws; break;
  case Kind::Target: rows = targets; break;
  case Kind::Leverage: break;
  }
  std::string out;
  for (const auto& row : rows) {
    if (row.x_flag.empty()) continue;
    if (!out.empty()) out += '|';
    out += row.id;
  }
  return out;
}

Json rules_json() {
  Json rows = risk_target::leverage_rules_json();
  for (const auto table : {std::span<const Row>(trade_rates), std::span<const Row>(cost_laws),
                           targets})
    for (const auto& row : table) rows.push_back(row_json(row));
  return rows;
}

Json table_json() {
  return Json{{"schema", nav_rules_schema},
              {"capabilities", Json::array({"leverage-rule-per-book", "capacity-main-lockstep",
                                            "summary-binds-extras", "producer-identity",
                                            "sqrt-impact-exact", "calibrate-gross-pm6-6",
                                            "nav-spec-v1", "fields-seal-p13"})},
              {"rules", rules_json()}};
}

} // namespace atx::impl::strategy::nav_rules
