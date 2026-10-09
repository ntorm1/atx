#pragma once

// K-P9-7 (P9 C3): the NAV exe's rule registries, printed by `nav --list-rules [--json]` as ONE
// atx.nav-rules/v1 table {schema, capabilities, rules} and read by the flag parsers.
//
// Kinds and rows, in table order:
//   leverage    fixed-v1, vol-target-v1, risk-target-v1 (P9 C1, risk_target::leverage_rules_json,
//               printed unchanged)
//   trade-rate  aim-partial-v5 (--rule aim-partial-v5: theta, dust, exit rate), per-name-v1
//               (--rate per-name-v1 and its four parameters), two-speed-nav-v1 (the NAV half of
//               the composition rule two-speed-v1; no flag of its own); P9 AL-CLOCK appends
//               info-clock-rate-v1 (--rate info-clock-rate-v1, no parameter)
//   cost-law    flat-bps-v1 and sqrt-impact-v1 (the fixed scenarios S1 / S2, S3 of every run; no
//               flag), ko-invariance-v1 and fim-live-v1 (the v2 stress laws; no flag of their own),
//               cost-sqrt-v2 (--cost-law cost-sqrt-v2: both v2 stress books, the run --cost-v2
//               gives; x-flag-aliases ["--cost-v2"])
//   target      the desired-target rules (--target-rule ID): mv-aim-v1 (P9 COV-MV,
//               strategy_mv_aim.hpp; row in nav_rules.cpp, handler in strategy_nav_v7.cpp)
//
// Row shape (C1's, strategy_risk_target.cpp): {id, kind, x-flag, params_schema, incompatible};
// these registries add "description". x-flag is the flag that turns the rule on ("--flag VALUE",
// a constant) and each params_schema property names its own flag (x-flag), so a research wave
// derives a cell's NAV flags from the row alone (E2's wave_kinds.nav_flags); x-flag null: the
// rule cannot be named on its own (a fixed scenario law, or a law another row's flag turns on).
// params_schema is a closed JSON Schema object (additionalProperties false) whose defaults are
// the exe's own (TargetReplayConfig{}, nav_rate_*).
// x-flag-aliases (Ruling C3-3; only on a row that has one, so every other row keeps its shape and
// C1's leverage rows their bytes): the legacy spellings that turn the same rule on, byte for byte
// the x-flag's run (cost-sqrt-v2: ["--cost-v2"]); a research wave treats an alias as the owned
// flag it stands for (D3's E2-8 guard).
//
// Appending a rule (AL-CLOCK, COV-MV; Ruling W3-9): one Row at the end of the kind's table in
// nav_rules.cpp (the trade-rate and cost-law tables are size-deduced; the empty target slot
// becomes a table of its own), one parser entry where its flag is read (nav_spec.cpp keeps its
// NavSpec key). The registry tests check C3's rows by lookup and as a prefix of their kind.

#include <span>
#include <string>
#include <string_view>
#include <nlohmann/json_fwd.hpp>
#include "atx/core/types.hpp"

namespace atx::impl::strategy::nav_rules {

enum class Kind : atx::u8 { Leverage = 0, TradeRate = 1, CostLaw = 2, Target = 3 };
[[nodiscard]] std::string_view kind_name(Kind kind) noexcept;

struct Row {
  std::string_view id;
  Kind kind{Kind::TradeRate};
  std::string_view x_flag;            // empty: printed as null (not nameable on its own)
  nlohmann::json (*params)() = nullptr; // the params_schema properties (an object)
  std::span<const std::string_view> required;
  std::span<const std::string_view> incompatible;
  std::string_view description;
  std::span<const std::string_view> x_flag_aliases{}; // empty: the row prints no such key
};

// The registries C3 owns (the leverage rows are risk_target's).
[[nodiscard]] std::span<const Row> trade_rate_rows() noexcept;
[[nodiscard]] std::span<const Row> cost_law_rows() noexcept;
[[nodiscard]] std::span<const Row> target_rows() noexcept;
// The row of `id` in kind's registry (trade-rate, cost-law, target); nullptr otherwise.
[[nodiscard]] const Row* find(Kind kind, std::string_view id) noexcept;
// "a|b|c": the ids of kind's registry whose x-flag is set (the values its flag accepts).
[[nodiscard]] std::string flag_ids(Kind kind);

// The cost law --cost-law turns on, and the one that is the run --cost-v2 gives.
inline constexpr std::string_view cost_sqrt_v2_id = "cost-sqrt-v2";

// The rows, in table order: leverage, trade-rate, cost-law, target.
[[nodiscard]] nlohmann::json rules_json();
// The envelope {"schema": "atx.nav-rules/v1", "capabilities": [...], "rules": rules_json()}.
[[nodiscard]] nlohmann::json table_json();

} // namespace atx::impl::strategy::nav_rules
