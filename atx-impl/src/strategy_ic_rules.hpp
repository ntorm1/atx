#pragma once
// The IC runner's composition rule registry and its theme table (P9 lane D1, DEC-8; contract
// K-P9-6). Internal to the strategy_ic_*.cpp translation units, like strategy_ic_detail.hpp.
//
// Rule table. A composition weights document (atx.dsl-composition-weights/v1|v2) names its rules
// by top-level blocks. Each block is one row of composition_rules() or, for theme_standardise, one
// of four rows chosen by the block's `rule`. parse_composition_rules reads a document's blocks in
// table order before any role payload (strategy_ic_admission.cpp composition_weights): each block
// key's shared parse runs once, its row's verify checks the pinned weights against the fit inputs
// the block records, and a pair of present rows whose ids list each other in `incompatible` is
// refused. The present rows whose stage runs (a theme_standardise row only with rerank true) are,
// in table order, the ordered stage list IcComposition::create_from_stages takes
// (composition_stages), the recipe's composition statement and keys (method_recipe) and the
// combined manifest's keys (strategy_ic_runner.cpp). A new rule is one row plus its parse and
// verify; `atx-equity-strategy-ic --list-rules --json` prints the table (list_rules_json).
//
// Theme table. The registered themes in registration order: the alpha registry's `themes` table
// (atx-impl/strategies/alphas/registry.json, atx.alpha-registry/v1) in document order, which the
// exe reads from --theme-registry FILE --theme-registry-sha256 SHA. theme-resid-v1 takes its
// registered order from it and two-speed-v1 its theme set. Without the option the exe uses
// builtin_theme_table() (the registry's thirteen themes at the P9 base, the order theme-resid-v1
// was registered on), so a run without it is byte-identical to the v8 runner; a theme registered
// later is honoured only through the option. test_composition_resid.py pins the built-in table as
// a prefix of the registry. It retires once every IC argv passes the registry (lane E2, K-P9-6).
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "strategy_ic_composition.hpp"
#include "strategy_ic_detail.hpp"

namespace atx::impl::strategy::ic_detail {
// ---- Theme table ---------------------------------------------------------------
struct ThemeTable {
  std::vector<std::string> names; // registration order; each [a-z0-9_]{1,64}, distinct
  std::string registry_sha256;    // the pinned registry file's SHA-256; empty: the built-in table
};
inline constexpr u64 theme_registry_max_bytes=4ULL<<20; // composition_rules.REGISTRY_LIMIT
inline constexpr usize max_registered_themes=256;
inline constexpr const char* theme_registry_schema="atx.alpha-registry/v1";
// The flag-absent table (see above): registry.json's themes at the P9 base, in their order.
inline constexpr std::array<std::string_view,13> builtin_registry_themes{
    "value",           "profitability_quality", "investment_issuance", "earnings_momentum",
    "price_momentum",  "low_risk",              "short_interest",      "reversal_seasonality",
    "options_implied", "ownership_flow",        "filing_events",       "price_volume",
    "merger_arbitrage"};
// The `themes` object of an alpha registry document, in document order: schema
// atx.alpha-registry/v1, 1..256 distinct names [a-z0-9_]{1,64}. Err (InvalidArgument) otherwise.
[[nodiscard]] co::Result<ThemeTable> theme_table_from_registry(const std::string& text,
                                                               std::string registry_sha256);
[[nodiscard]] ThemeTable builtin_theme_table();
// --theme-registry (pinned by --theme-registry-sha256, at most theme_registry_max_bytes), else the
// built-in table.
[[nodiscard]] co::Result<ThemeTable> theme_table(const IcRunnerConfig& cfg);
[[nodiscard]] bool registered_theme(const ThemeTable& table,std::string_view theme) noexcept;
// [a-z0-9_]{1,64}: a theme name as a weights block may spell it.
[[nodiscard]] bool theme_name(const std::string& s);

// ---- Rule table ----------------------------------------------------------------
// ew-theme-v6 (v4-prereg v6 revision V6-W): the only composition a theme_redistribution block
// names.
inline constexpr const char* theme_redistribution_composition="ew-theme-v6";
// What a rule's parse reads besides the document: the library and the theme table.
struct RuleInputs {
  const Library& lib;
  const ThemeTable& themes;
};
// Reads the rule's block of `doc` into `pinned` (absent: no change), with every check that
// precedes any role payload; the rows of one block key share it.
using RuleParse=co::Status(*)(const Json& doc,const RuleInputs& in,PinnedWeights& pinned);
// Checks the pinned weights (library order) against the fit inputs the block records.
using RuleVerify=co::Status(*)(const Json& block,const Library& lib,
                               const std::vector<f64>& weights);
struct CompositionRule {
  std::string_view id;             // the block's `rule` value (K-P9-6 id)
  std::string_view version;        // the row's version (K-P9-6)
  std::string_view block_key;      // the document's top-level key
  IcStageKind stage;               // the composition stage it runs as
  RuleParse parse;                 // the block key's shared parse
  RuleVerify verify;               // null: nothing recorded to check
  std::string_view recipe_key;     // the recipe and combined-manifest key that records the id
  std::string_view recipe_text;    // the recipe composition statement after the signs (empty: kept)
  std::string_view working_bytes;  // the composition envelope term (ic_composition_stage_bytes)
  std::string_view params_schema;  // JSON Schema of the block
  std::span<const std::string_view> incompatible;  // ids refused in one document with it
  std::span<const std::string_view> requires_any;  // one of these ids must be present (empty: none)
  // theme_standardise rows (finding R6B-C-5): whether the block may switch the re-rank off (the
  // R-1 identity device), the fitter rule that also writes the row's block (empty: none) and the
  // fitter rule whose files the rerank-off device grafts the block onto (empty: none).
  bool rerank_off;
  std::string_view also_written_by;
  std::string_view identity_source;
};
// Every row, in parse order; the rows of one block key are adjacent.
[[nodiscard]] std::span<const CompositionRule> composition_rules() noexcept;
// The row of `id` (null: none).
[[nodiscard]] const CompositionRule* find_rule(std::string_view id) noexcept;
// The theme_standardise row a block names (null: none, a block that is not an object or a block
// without a string rule).
[[nodiscard]] const CompositionRule* standardise_row(const Json& block);
// Every block of `doc` in table order, then the incompatible-pair refusal; fills pinned.rules with
// the rows present, in table order.
[[nodiscard]] co::Status parse_composition_rules(const Json& doc,const RuleInputs& in,
                                                 PinnedWeights& pinned);
// The present rows whose stage runs, in table order (a theme_standardise row only with rerank
// true).
[[nodiscard]] std::vector<const CompositionRule*> running_rules(const PinnedWeights& pinned);
[[nodiscard]] std::vector<IcStageKind> running_stage_kinds(const PinnedWeights& pinned);
// The ordered stage list of the pinned weights for a role whose sessions are `session_keys`: each
// theme-tsmom-v1 block starts at the first session at or after its from_session (a role after the
// last block keeps the last block's masses). Borrows `pinned` (weights and themes).
[[nodiscard]] co::Result<IcCompositionStages> composition_stages(const PinnedWeights& pinned,
                                                                 std::span<const i64> session_keys);
[[nodiscard]] std::string_view stage_name(IcStageKind kind) noexcept;
// What a row writes into a method recipe, canonical: {"composition": recipe_text, "key":
// recipe_key, "value": id}; recipe_sha256 is the SHA-256 of its compact dump.
[[nodiscard]] Json rule_recipe_json(const CompositionRule& rule);
// K-P9-6 (progress.md ruling P7): {"schema": "atx.composition-rules/v1", "capabilities": [...],
// "rules": [{id, version, block_key, stage, params_schema, incompatible, requires_any, recipe_key,
// recipe_sha256, verified, working_bytes}, ...]} in table order.
inline constexpr const char* composition_rules_schema="atx.composition-rules/v1";
[[nodiscard]] co::Result<Json> list_rules_json();

// ---- The block parses (each a RuleParse) and verifies (each a RuleVerify) -------
co::Status composition_themes(const Json& doc,const RuleInputs& in,PinnedWeights& pinned);
co::Status composition_standardise(const Json& doc,const RuleInputs& in,PinnedWeights& pinned);
co::Status composition_residualise(const Json& doc,const RuleInputs& in,PinnedWeights& pinned);
co::Status composition_schedule(const Json& doc,const RuleInputs& in,PinnedWeights& pinned);
co::Status composition_sleeves(const Json& doc,const RuleInputs& in,PinnedWeights& pinned);
co::Status verify_ic_shrink(const Json& block,const Library& lib,const std::vector<f64>& weights);
co::Status verify_ic_shrink_aim(const Json& block,const Library& lib,const std::vector<f64>& weights);
co::Status verify_theme_erc(const Json& block,const Library& lib,const std::vector<f64>& weights);
// The shapes of the two theme grouping blocks; composition_themes, composition_standardise and
// ic_weights_themes (the marginal verb's reader) check a block through these.
co::Status redistribution_block(const Json& block);
co::Status standardise_block(const Json& block);
} // namespace atx::impl::strategy::ic_detail
