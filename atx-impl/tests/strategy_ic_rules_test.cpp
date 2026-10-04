// P9 lane D1 (DEC-8, contract K-P9-6): the IC runner's composition rule table, its theme table,
// the `--list-rules --json` envelope and the ordered stage list IcComposition takes
// (strategy_ic_rules.hpp, strategy_ic_composition.hpp). The end-to-end runs (a registry-driven
// theme table, the over-cap plan) are in strategy_ic_runner_test.cpp.
#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/strategy_ic_composition.hpp"
#include "../src/strategy_ic_rules.hpp"
#include "../src/strategy_ic_runner.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
namespace st = atx::impl::strategy;
namespace icd = atx::impl::strategy::ic_detail;

std::filesystem::path tests_dir() { return std::filesystem::path(ATX_IMPL_TESTS_DIR); }
std::filesystem::path registry_path() {
  return tests_dir() / ".." / "strategies" / "alphas" / "registry.json";
}
std::string file_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
u64 bits(f64 x) { return std::bit_cast<u64>(x); }
// argv for dispatch_ic; returns its exit code, `out` its stdout.
int cli(std::vector<std::string> args, std::string& out) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  std::ostringstream stdout_log, stderr_log;
  const int code =
      st::dispatch_ic(static_cast<int>(argv.size()), argv.data(), stdout_log, stderr_log);
  out = stdout_log.str();
  return code;
}

// The v8 runner's recipe composition statements (strategy_ic_admission.cpp at the P9 base,
// method_recipe), spelled out here so a table row that drifts from them fails.
constexpr std::string_view v8_plain =
    "centered-tied-rank;missing-or-unoriented-neutral;no-redistribution";
constexpr std::string_view v8_redistribute =
    "centered-tied-rank;missing-or-unoriented-mass-stays-in-theme;within-theme-v1;"
    "theme-without-present-member-neutral";
constexpr std::string_view v8_standardise =
    "centered-tied-rank;theme-weighted-rank-sum-missing-neutral;"
    "theme-rerank-centered-tied-over-names-with-a-present-member;"
    "theme-weight-sum-of-member-weights";
std::string v8_signs(bool pinned_signs) {
  return pinned_signs ? "pinned-candidate-weights;pinned-candidate-signs;"
                      : "pinned-candidate-weights;TRAIN-orientation-signs;";
}
const icd::CompositionRule* row(std::string_view id) {
  const auto* rule = icd::find_rule(id);
  EXPECT_TRUE(rule != nullptr) << id;
  return rule;
}
// Pinned weights of two candidates in two standardised themes (a rerank-true block) carrying
// `rules`.
icd::PinnedWeights standardised(std::vector<const icd::CompositionRule*> rules) {
  icd::PinnedWeights pinned;
  pinned.values = {.5, .5};
  pinned.std_themes = {0, 1};
  pinned.std_theme_count = 2;
  pinned.rules = std::move(rules);
  return pinned;
}

// Every rule the v8 runner admitted is one row, in the v8 parse order, the rows of one block key
// adjacent; each row's metadata is complete and consistent (ids it names are rows, incompatible
// pairs list each other, its params schema is a JSON object schema naming its own id).
TEST(CompositionRules, TableCoversEveryRule) {
  const auto rules = icd::composition_rules();
  const std::vector<std::string_view> ids{"within-theme-v1", "ew-theme-std-v1", "ic-shrink-v1",
                                          "ic-shrink-aim-v1", "theme-erc-v1", "theme-resid-v1",
                                          "theme-tsmom-v1", "two-speed-v1"};
  const std::vector<std::string_view> stages{"redistribute", "standardise", "standardise",
                                             "standardise", "standardise", "residualise",
                                             "schedule", "sleeves"};
  const std::vector<std::string_view> keys{"composition_redistribution", "composition_standardise",
                                           "composition_standardise", "composition_standardise",
                                           "composition_standardise", "composition_residualise",
                                           "composition_schedule", "composition_sleeves"};
  ASSERT_EQ(rules.size(), ids.size());
  std::vector<std::string_view> blocks;
  for (usize r = 0; r < rules.size(); ++r) {
    const auto& rule = rules[r];
    SCOPED_TRACE(std::string(rule.id));
    EXPECT_EQ(rule.id, ids[r]);
    EXPECT_EQ(icd::stage_name(rule.stage), stages[r]);
    EXPECT_EQ(rule.recipe_key, keys[r]);
    EXPECT_EQ(rule.version, "v1");
    EXPECT_TRUE(rule.parse != nullptr);
    EXPECT_FALSE(rule.working_bytes.empty());
    EXPECT_TRUE(icd::find_rule(rule.id) == &rule);
    if (blocks.empty() || blocks.back() != rule.block_key) blocks.push_back(rule.block_key);
    const auto schema = Json::parse(rule.params_schema.begin(), rule.params_schema.end());
    EXPECT_EQ(schema.at("type"), "object");
    EXPECT_EQ(schema.at("properties").at("rule").at("const"), std::string(rule.id));
    const auto& required = schema.at("required");
    EXPECT_TRUE(std::find(required.begin(), required.end(), "rule") != required.end());
    for (const auto other : rule.incompatible) {
      const auto* o = icd::find_rule(other);
      ASSERT_TRUE(o != nullptr) << other;
      EXPECT_TRUE(std::find(o->incompatible.begin(), o->incompatible.end(), rule.id) !=
                  o->incompatible.end())
          << other;
    }
    for (const auto need : rule.requires_any) {
      const auto* n = icd::find_rule(need);
      ASSERT_TRUE(n != nullptr) << need;
      EXPECT_EQ(n->block_key, "theme_standardise") << need;
    }
  }
  EXPECT_EQ(blocks, (std::vector<std::string_view>{"theme_redistribution", "theme_standardise",
                                                   "theme_residualise", "theme_schedule",
                                                   "theme_sleeves"}));
  // A block key's rows share its parse; a row whose block records fit inputs verifies them.
  for (const auto& rule : rules)
    if (rule.block_key == "theme_standardise")
      EXPECT_TRUE(rule.parse == row("ew-theme-std-v1")->parse) << rule.id;
  EXPECT_TRUE(row("ic-shrink-v1")->verify != nullptr);
  EXPECT_TRUE(row("ic-shrink-aim-v1")->verify != nullptr);
  EXPECT_TRUE(row("theme-erc-v1")->verify != nullptr);
  EXPECT_TRUE(row("ew-theme-std-v1")->verify == nullptr);
  // The redistribution and every standardisation exclude each other; a rider needs a
  // standardisation and theme-resid-v1 excludes the two other riders.
  EXPECT_EQ(row("within-theme-v1")->incompatible.size(), 4U);
  EXPECT_EQ(row("theme-resid-v1")->incompatible.size(), 2U);
  for (const auto* id : {"theme-resid-v1", "theme-tsmom-v1", "two-speed-v1"})
    EXPECT_EQ(row(id)->requires_any.size(), 4U) << id;
  // Finding R6B-C-5: only ew-theme-std-v1 switches the re-rank off (R-1's identity device on
  // ew-theme-v1 weights), and ew-theme-std-aim-v1 (a fitter rule, not a row) writes its block.
  for (const auto& rule : rules) EXPECT_EQ(rule.rerank_off, rule.id == "ew-theme-std-v1");
  EXPECT_EQ(row("ew-theme-std-v1")->also_written_by, "ew-theme-std-aim-v1");
  EXPECT_EQ(row("ew-theme-std-v1")->identity_source, "ew-theme-v1");
  EXPECT_TRUE(icd::find_rule("ew-theme-std-aim-v1") == nullptr);
  EXPECT_TRUE(icd::find_rule("ew-theme-v6") == nullptr);
  // standardise_row reads only the theme_standardise rows.
  EXPECT_TRUE(icd::standardise_row(Json{{"rule", "ic-shrink-v1"}}) == row("ic-shrink-v1"));
  EXPECT_TRUE(icd::standardise_row(Json{{"rule", "theme-resid-v1"}}) == nullptr);
  EXPECT_TRUE(icd::standardise_row(Json{{"rule", 1}}) == nullptr);
  EXPECT_TRUE(icd::standardise_row(Json::array({1})) == nullptr);
}

// Each row writes into the method recipe what the v8 runner wrote for it, byte for byte: a
// grouping row replaces the composition statement, every running row records its id under its
// recipe key (theme-resid-v1 also its order), a rerank-false theme_standardise runs no stage and
// writes nothing, and without pinned weights the recipe is the default one.
TEST(CompositionRules, RecipeTextPinned) {
  st::IcRunnerConfig cfg;
  cfg.library_sha256 = std::string(64, 'a');
  cfg.composition_weights_sha256 = std::string(64, 'c');
  const auto expect_recipe = [&cfg](const icd::PinnedWeights& pinned, bool pinned_signs,
                                    std::string_view statement,
                                    const std::vector<std::pair<std::string, Json>>& keys) {
    auto want = icd::method_recipe(cfg, true, pinned_signs, nullptr);
    want["composition"] = v8_signs(pinned_signs) + std::string(statement);
    for (const auto& [key, value] : keys) want[key] = value;
    EXPECT_EQ(icd::method_recipe(cfg, true, pinned_signs, &pinned).dump(), want.dump());
  };
  icd::PinnedWeights within;
  within.values = {.5, .5};
  within.themes = {0, 0};
  within.theme_count = 1;
  within.rules = {row("within-theme-v1")};
  for (const bool pinned_signs : {false, true}) {
    SCOPED_TRACE(pinned_signs);
    // No row: the pinned statement only.
    const auto base = icd::method_recipe(cfg, true, pinned_signs, nullptr);
    EXPECT_EQ(base.at("composition"), v8_signs(pinned_signs) + std::string(v8_plain));
    const auto none = standardised({});
    EXPECT_EQ(icd::method_recipe(cfg, true, pinned_signs, &none).dump(), base.dump());
    // within-theme-v1 (ew-theme-v6).
    expect_recipe(within, pinned_signs, v8_redistribute,
                  {{"composition_redistribution", "within-theme-v1"}});
    // Every theme_standardise row with rerank true: the same statement, its own id.
    for (const auto* id : {"ew-theme-std-v1", "ic-shrink-v1", "ic-shrink-aim-v1", "theme-erc-v1"})
      expect_recipe(standardised({row(id)}), pinned_signs, v8_standardise,
                    {{"composition_standardise", id}});
    // Rerank false: no stage runs; only the weights pin differs from the pinned method.
    auto off = standardised({row("ew-theme-std-v1")});
    off.std_themes.clear();
    off.std_theme_count = 0;
    off.standardise = "ew-theme-std-v1;rerank-off";
    EXPECT_EQ(icd::method_recipe(cfg, true, pinned_signs, &off).dump(), base.dump());
    // The riders on a standardisation.
    auto resid = standardised({row("ew-theme-std-v1"), row("theme-resid-v1")});
    resid.residualise = true;
    resid.residualise_order = {"value", "price_momentum"};
    expect_recipe(resid, pinned_signs, v8_standardise,
                  {{"composition_standardise", "ew-theme-std-v1"},
                   {"composition_residualise", "theme-resid-v1"},
                   {"composition_residualise_order", Json::array({"value", "price_momentum"})}});
    expect_recipe(standardised({row("ic-shrink-v1"), row("theme-tsmom-v1")}), pinned_signs,
                  v8_standardise,
                  {{"composition_standardise", "ic-shrink-v1"},
                   {"composition_schedule", "theme-tsmom-v1"}});
    expect_recipe(
        standardised({row("ew-theme-std-v1"), row("theme-tsmom-v1"), row("two-speed-v1")}),
        pinned_signs, v8_standardise,
        {{"composition_standardise", "ew-theme-std-v1"},
         {"composition_schedule", "theme-tsmom-v1"},
         {"composition_sleeves", "two-speed-v1"}});
  }
  // Without pinned weights a row changes nothing (the flag-absent recipe).
  auto unpinned = cfg;
  unpinned.composition_weights_sha256.clear();
  EXPECT_EQ(icd::method_recipe(unpinned, true, false, &within).dump(),
            icd::method_recipe(unpinned).dump());
  // What each row records, canonical (its recipe_sha256 in --list-rules).
  EXPECT_EQ(icd::rule_recipe_json(*row("within-theme-v1")).dump(),
            R"({"composition":")" + std::string(v8_redistribute) +
                R"(","key":"composition_redistribution","value":"within-theme-v1"})");
  EXPECT_EQ(icd::rule_recipe_json(*row("theme-erc-v1")).dump(),
            R"({"composition":")" + std::string(v8_standardise) +
                R"(","key":"composition_standardise","value":"theme-erc-v1"})");
  EXPECT_EQ(icd::rule_recipe_json(*row("two-speed-v1")).dump(),
            R"({"composition":"","key":"composition_sleeves","value":"two-speed-v1"})");
}

// A document carrying two rows that list each other is refused with the v8 runner's message,
// after both blocks parsed (rerank true or false alike); either block alone passes and fills
// pinned.rules with its row. The riders' exclusions are refused by their parses first, with the
// v8 messages (the table's incompatible lists state the same pairs).
TEST(CompositionRules, IncompatiblePairRefused) {
  icd::Library lib;
  lib.candidates.push_back(icd::Candidate{"a", "fam", "sha-a", {}, {}});
  lib.candidates.push_back(icd::Candidate{"b", "fam", "sha-b", {}, {}});
  const auto themes = icd::builtin_theme_table();
  const icd::RuleInputs in{lib, themes};
  const Json grouping{{"a", "value"}, {"b", "price_momentum"}};
  const Json redistribution{
      {"rule", "within-theme-v1"}, {"composition", "ew-theme-v6"}, {"themes", grouping}};
  const auto parse = [&in](const Json& doc, icd::PinnedWeights& pinned) {
    pinned.values = {.5, .5};
    return icd::parse_composition_rules(doc, in, pinned);
  };
  for (const bool rerank : {true, false}) {
    SCOPED_TRACE(rerank);
    const Json standardise{{"rule", "ew-theme-std-v1"}, {"rerank", rerank}, {"themes", grouping}};
    Json doc{{"schema", "atx.dsl-composition-weights/v2"},
             {"theme_redistribution", redistribution},
             {"theme_standardise", standardise},
             {"provenance", {{"rule", "ew-theme-std-v1"}}}};
    icd::PinnedWeights both;
    const auto refused = parse(doc, both);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().message(),
              "IC runner: theme_redistribution and theme_standardise are exclusive");
    doc.erase("theme_redistribution");
    icd::PinnedWeights alone;
    ASSERT_TRUE(parse(doc, alone));
    ASSERT_EQ(alone.rules.size(), 1U);
    EXPECT_EQ(alone.rules.front()->id, "ew-theme-std-v1");
    EXPECT_EQ(icd::running_rules(alone).size(), rerank ? 1U : 0U);
  }
  const Json only{{"schema", "atx.dsl-composition-weights/v2"},
                  {"theme_redistribution", redistribution}};
  icd::PinnedWeights redistributed;
  ASSERT_TRUE(parse(only, redistributed));
  ASSERT_EQ(redistributed.rules.size(), 1U);
  EXPECT_EQ(redistributed.rules.front()->id, "within-theme-v1");
  // theme-resid-v1 beside theme-tsmom-v1 or two-speed-v1.
  const Json base{{"schema", "atx.dsl-composition-weights/v2"},
                  {"theme_standardise",
                   {{"rule", "ew-theme-std-v1"}, {"rerank", true}, {"themes", grouping}}},
                  {"theme_residualise",
                   {{"rule", "theme-resid-v1"},
                    {"order", Json::array({"value", "price_momentum"})}}},
                  {"provenance", {{"rule", "ew-theme-std-v1"}}}};
  icd::PinnedWeights resid;
  ASSERT_TRUE(parse(base, resid));
  ASSERT_EQ(resid.rules.size(), 2U);
  EXPECT_EQ(resid.rules.back()->id, "theme-resid-v1");
  // Parsed from text, as a weights file is: the registered constants are unsigned numbers.
  auto scheduled = base;
  scheduled["theme_schedule"] = Json::parse(
      R"({"rule":"theme-tsmom-v1","lookback":252,"lag":3,"step":21,)"
      R"("themes":["price_momentum","value"],"blocks":[{"from_session":1,"trailing":[1.0,1.0]}]})");
  icd::PinnedWeights tsmom;
  const auto with_schedule = parse(scheduled, tsmom);
  ASSERT_FALSE(with_schedule);
  EXPECT_NE(with_schedule.error().message().find("no theme_residualise"), std::string::npos)
      << with_schedule.error().message();
  auto sleeved = base;
  sleeved["theme_sleeves"] = {{"rule", "two-speed-v1"}};
  icd::PinnedWeights sleeves;
  const auto with_sleeves = parse(sleeved, sleeves);
  ASSERT_FALSE(with_sleeves);
  EXPECT_NE(with_sleeves.error().message().find("no theme_residualise"), std::string::npos)
      << with_sleeves.error().message();
}

// The theme table: the alpha registry's `themes` in document order (not sorted), refused unless
// it is an atx.alpha-registry/v1 document with 1..256 distinct well-formed names; the built-in
// table (the flag-absent device) is the registry's prefix at the P9 base, and theme_table(cfg)
// reads --theme-registry only through its SHA-256 pin.
TEST(CompositionRules, ThemeTableFromRegistry) {
  const std::vector<std::string> builtin(icd::builtin_registry_themes.begin(),
                                         icd::builtin_registry_themes.end());
  EXPECT_EQ(icd::builtin_theme_table().names, builtin);
  EXPECT_TRUE(icd::builtin_theme_table().registry_sha256.empty());
  ASSERT_EQ(builtin.size(), 13U);
  EXPECT_EQ(builtin.front(), "value");
  EXPECT_EQ(builtin.back(), "merger_arbitrage");
  // The real registry: the built-in table is its prefix (registration order), the pin is kept.
  const auto text = file_text(registry_path());
  ASSERT_FALSE(text.empty()) << registry_path().string();
  const auto real = icd::theme_table_from_registry(text, "pin");
  ASSERT_TRUE(real) << real.error().to_string();
  ASSERT_GE(real->names.size(), builtin.size());
  EXPECT_TRUE(std::equal(builtin.begin(), builtin.end(), real->names.begin()));
  EXPECT_EQ(real->registry_sha256, "pin");
  // Document order, other keys ignored.
  const auto ordered = icd::theme_table_from_registry(
      R"({"schema":"atx.alpha-registry/v1","alphas":{},"themes":{"zeta":{},"alpha":{"x":1},)"
      R"("mid_2":{}}})",
      "");
  ASSERT_TRUE(ordered) << ordered.error().to_string();
  EXPECT_EQ(ordered->names, (std::vector<std::string>{"zeta", "alpha", "mid_2"}));
  EXPECT_TRUE(icd::registered_theme(*ordered, "alpha"));
  EXPECT_FALSE(icd::registered_theme(*ordered, "value"));
  // Refusals.
  std::string many = R"({"schema":"atx.alpha-registry/v1","themes":{)";
  for (usize t = 0; t < 257; ++t) many += (t ? "," : "") + ("\"t" + std::to_string(t) + "\":{}");
  many += "}}";
  const std::vector<std::pair<std::string, std::string>> refused{
      {R"({"schema":"atx.alpha-registry/v1","themes":{"a":{},"a":{}}})", "duplicate key"},
      {R"({"schema":"atx.alpha-registry/v1","themes":{"a":{"k":1,"k":2}}})", "duplicate key"},
      {R"({"schema":"atx.alpha-registry/v1","themes":{"a":{})", "parse"},
      {R"({"schema":"atx.alpha-registry/v2","themes":{"a":{}}})", "atx.alpha-registry/v1"},
      {R"({"themes":{"a":{}}})", "atx.alpha-registry/v1"},
      {R"({"schema":"atx.alpha-registry/v1","themes":{}})", "1..256 themes"},
      {R"({"schema":"atx.alpha-registry/v1","themes":["a"]})", "1..256 themes"},
      {R"(["a"])", "atx.alpha-registry/v1"},
      {many, "1..256 themes"},
      {R"({"schema":"atx.alpha-registry/v1","themes":{"Value":{}}})",
       "theme registry theme name must match [a-z0-9_]{1,64}: Value"},
      {R"({"schema":"atx.alpha-registry/v1","themes":{"a-b":{}}})", "[a-z0-9_]{1,64}: a-b"}};
  for (const auto& [doc, reason] : refused) {
    const auto table = icd::theme_table_from_registry(doc, "");
    ASSERT_FALSE(table) << doc;
    EXPECT_NE(table.error().message().find(reason), std::string::npos)
        << doc << " -> " << table.error().message();
  }
  // theme_table(cfg): absent option = the built-in table; present = the pinned file.
  st::IcRunnerConfig cfg;
  const auto absent = icd::theme_table(cfg);
  ASSERT_TRUE(absent);
  EXPECT_EQ(absent->names, builtin);
  EXPECT_TRUE(absent->registry_sha256.empty());
  const auto sha = core::sha256_file(registry_path().string());
  ASSERT_TRUE(sha);
  cfg.theme_registry_path = registry_path().string();
  cfg.theme_registry_sha256 = *sha;
  const auto pinned = icd::theme_table(cfg);
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  EXPECT_EQ(pinned->names, real->names);
  EXPECT_EQ(pinned->registry_sha256, *sha);
  cfg.theme_registry_sha256 = std::string(64, '0');
  EXPECT_FALSE(icd::theme_table(cfg));
  // theme_name: what a weights block may spell.
  EXPECT_TRUE(icd::theme_name("price_volume"));
  EXPECT_TRUE(icd::theme_name(std::string(64, 'a')));
  EXPECT_FALSE(icd::theme_name(std::string(65, 'a')));
  EXPECT_FALSE(icd::theme_name(""));
  EXPECT_FALSE(icd::theme_name("Value"));
}

// K-P9-6: the envelope is the pinned fixture (regenerate it with the rule table; every
// recipe_sha256 is the SHA-256 of the row's canonical recipe record), and the CLI prints it on
// `--list-rules --json` alone (exit 0) and refuses any other argv with it (exit 2).
TEST(CompositionRules, ListRulesJsonPinned) {
  const auto listing = icd::list_rules_json();
  ASSERT_TRUE(listing) << listing.error().to_string();
  const auto fixture =
      Json::parse(file_text(tests_dir() / "fixtures" / "composition_rules_list.json"));
  EXPECT_EQ(*listing, fixture);
  EXPECT_EQ(listing->at("schema"), "atx.composition-rules/v1");
  const auto& caps = listing->at("capabilities");
  EXPECT_TRUE(std::is_sorted(caps.begin(), caps.end()));
  for (const auto* cap : {"list-rules", "marginal", "no-composition", "theme-registry"})
    EXPECT_TRUE(std::find(caps.begin(), caps.end(), cap) != caps.end()) << cap;
  const auto rules = icd::composition_rules();
  ASSERT_EQ(listing->at("rules").size(), rules.size());
  for (usize r = 0; r < rules.size(); ++r) {
    const auto& printed = listing->at("rules").at(r);
    SCOPED_TRACE(std::string(rules[r].id));
    EXPECT_EQ(printed.at("id"), std::string(rules[r].id));
    EXPECT_EQ(printed.at("block_key"), std::string(rules[r].block_key));
    const auto sha = core::sha256_hex(icd::rule_recipe_json(rules[r]).dump());
    ASSERT_TRUE(sha);
    EXPECT_EQ(printed.at("recipe_sha256"), *sha);
    EXPECT_EQ(printed.at("verified"), rules[r].verify != nullptr);
  }
  std::string printed;
  EXPECT_EQ(cli({"atx-equity-strategy-ic", "--list-rules", "--json"}, printed), 0);
  EXPECT_EQ(Json::parse(printed), fixture);
  EXPECT_EQ(cli({"atx-equity-strategy-ic", "--json", "--list-rules"}, printed), 0);
  EXPECT_EQ(Json::parse(printed), fixture);
  EXPECT_EQ(cli({"atx-equity-strategy-ic", "--list-rules"}, printed), 2);
  EXPECT_TRUE(printed.empty());
  EXPECT_EQ(cli({"atx-equity-strategy-ic", "--list-rules", "--yaml"}, printed), 2);
  EXPECT_EQ(cli({"atx-equity-strategy-ic", "--list-rules", "--json", "--plan-only"}, printed), 2);
  EXPECT_TRUE(printed.empty());
}

// The ordered stage list: create_from_stages is create under the grouping's rule followed by
// schedule_theme_masses and set_theme_sleeves, bit for bit, for every stage list the table runs;
// ic_composition_stage_bytes is the matching ic_composition_working_bytes; a malformed list is
// refused. composition_stages derives the list from pinned weights (a schedule block begins at
// the role's first session at or after its from_session).
struct StageFixture {
  static constexpr usize days = 4, width = 9;
  std::vector<st::IcCompositionCandidate> candidates{
      {"a1", "a"}, {"b1", "b"}, {"b2", "b"}, {"b3", "b"}};
  std::vector<usize> themes{0, 1, 1, 1};
  std::vector<f64> weights{.5, 1.0 / 6, 1.0 / 6, 1.0 / 6};
  std::vector<u8> member = std::vector<u8>(days * width, 1);
  std::vector<std::vector<f64>> signals =
      std::vector<std::vector<f64>>(4, std::vector<f64>(days * width));
  st::IcCompositionConfig cfg;
  StageFixture() {
    cfg.dates = days;
    cfg.instruments = width;
    cfg.decision_end = days;
    for (usize d = 0; d < days; ++d)
      for (usize i = 0; i < width; ++i) {
        const auto at = d * width + i, j = (i + d) % width;
        signals[0][at] = static_cast<f64>((5 * j) % width);
        signals[1][at] = static_cast<f64>(j);
        signals[2][at] = static_cast<f64>((j + 8) % width);
        signals[3][at] = static_cast<f64>((8 * j + 1) % width);
      }
    member[2 * width + 4] = 0;
  }
  std::optional<st::IcCompositionResult> finish(st::IcComposition& c) const {
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c.add(k, signals[k], 1)) return std::nullopt;
    auto out = c.finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  }
};
bool same_bits(const std::vector<f64>& a, const std::vector<f64>& b) {
  if (a.size() != b.size()) return false;
  for (usize k = 0; k < a.size(); ++k)
    if (bits(a[k]) != bits(b[k])) return false;
  return true;
}
bool same_result(const st::IcCompositionResult& a, const st::IcCompositionResult& b) {
  return same_bits(a.signal, b.signal) && same_bits(a.planned_turnover, b.planned_turnover) &&
         same_bits(a.planned_gross, b.planned_gross) && same_bits(a.planned_net, b.planned_net) &&
         same_bits(a.sleeve_fast, b.sleeve_fast) && same_bits(a.sleeve_slow, b.sleeve_slow) &&
         same_bits(a.sleeve_fast_share, b.sleeve_fast_share);
}
TEST(CompositionRules, StageListIsTheV8CallSequence) {
  using K = st::IcStageKind;
  const StageFixture f;
  const f64 w_a = .5, w_b = 0.0 + 1.0 / 6 + 1.0 / 6 + 1.0 / 6;
  const std::vector<st::IcThemeBlock> blocks{{2, {0.0, w_b}}, {3, {w_a, w_b}}};
  const std::vector<u8> fast{1, 0};
  const auto stage = [&](K kind) {
    return st::IcStage{kind, kind == K::schedule ? blocks : std::vector<st::IcThemeBlock>{},
                       kind == K::sleeves ? fast : std::vector<u8>{}};
  };
  const auto staged = [&](std::vector<K> kinds, bool grouped) {
    st::IcCompositionStages s{f.weights, grouped ? std::span<const usize>(f.themes)
                                                 : std::span<const usize>{},
                              {}};
    for (const auto kind : kinds) s.stages.push_back(stage(kind));
    auto c = st::IcComposition::create_from_stages(f.cfg, f.candidates, f.member, s);
    return c ? f.finish(*c) : std::nullopt;
  };
  // The v8 runner's call sequence: create, then schedule_theme_masses (non-empty `schedule`),
  // then set_theme_sleeves.
  const auto direct = [&](std::span<const usize> themes, st::IcThemeRule rule,
                          std::span<const st::IcThemeBlock> schedule,
                          bool sleeves) -> std::optional<st::IcCompositionResult> {
    auto c = st::IcComposition::create(f.cfg, f.candidates, f.member, f.weights, themes, rule);
    if (!c) return std::nullopt;
    if (!schedule.empty() && !c->schedule_theme_masses(schedule)) return std::nullopt;
    if (sleeves && !c->set_theme_sleeves(fast)) return std::nullopt;
    return f.finish(*c);
  };
  struct Case {
    std::vector<K> kinds;
    st::IcThemeRule rule;
    bool schedule, sleeves;
  };
  const auto std_rule = st::IcThemeRule::standardise;
  const std::vector<Case> cases{
      {{K::redistribute}, st::IcThemeRule::redistribute, false, false},
      {{K::standardise}, std_rule, false, false},
      {{K::standardise, K::residualise}, st::IcThemeRule::residualise, false, false},
      {{K::standardise, K::schedule}, std_rule, true, false},
      {{K::standardise, K::sleeves}, std_rule, false, true},
      {{K::standardise, K::schedule, K::sleeves}, std_rule, true, true}};
  for (usize c = 0; c < cases.size(); ++c) {
    SCOPED_TRACE(c);
    const auto got = staged(cases[c].kinds, true);
    const auto want = direct(f.themes, cases[c].rule,
                             cases[c].schedule ? std::span<const st::IcThemeBlock>(blocks)
                                               : std::span<const st::IcThemeBlock>{},
                             cases[c].sleeves);
    ASSERT_TRUE(got);
    ASSERT_TRUE(want);
    EXPECT_TRUE(same_result(*got, *want));
    const auto bytes = st::ic_composition_stage_bytes(f.days, f.width, 4, 2, cases[c].kinds);
    const auto envelope =
        st::ic_composition_working_bytes(f.days, f.width, 4, 2, cases[c].rule, cases[c].sleeves);
    ASSERT_TRUE(bytes);
    ASSERT_TRUE(envelope);
    EXPECT_EQ(*bytes, *envelope);
  }
  // No stage: the pinned-weights path (themes ignored by no stage are refused below).
  const auto plain = staged({}, false);
  const auto pinned = direct({}, st::IcThemeRule::redistribute, {}, false);
  ASSERT_TRUE(plain);
  ASSERT_TRUE(pinned);
  EXPECT_TRUE(same_result(*plain, *pinned));
  const auto none = st::ic_composition_stage_bytes(f.days, f.width, 4, 0, {});
  const auto default_bytes = st::ic_composition_working_bytes(f.days, f.width, 4);
  ASSERT_TRUE(none);
  ASSERT_TRUE(default_bytes);
  EXPECT_EQ(*none, *default_bytes);
  // Refusals: repeated or out-of-order kinds, riders without a standardisation, theme-resid-v1
  // with a rider, a redistribution with anything, and themes without a grouping (or the reverse).
  const std::vector<std::vector<K>> malformed{{K::standardise, K::standardise},
                                              {K::residualise, K::standardise},
                                              {K::residualise},
                                              {K::schedule},
                                              {K::redistribute, K::sleeves},
                                              {K::standardise, K::residualise, K::schedule},
                                              {K::standardise, K::residualise, K::sleeves}};
  for (const auto& kinds : malformed) {
    EXPECT_FALSE(staged(kinds, true)) << kinds.size();
    EXPECT_FALSE(st::ic_composition_stage_bytes(f.days, f.width, 4, 2, kinds)) << kinds.size();
  }
  EXPECT_FALSE(staged({K::standardise}, false));
  EXPECT_FALSE(staged({}, true));
  EXPECT_FALSE(st::ic_composition_stage_bytes(f.days, f.width, 4, 2, {}));

  // composition_stages: the running rows of pinned weights, in table order.
  icd::PinnedWeights p;
  p.values = f.weights;
  p.std_themes = f.themes;
  p.std_theme_count = 2;
  p.rules = {row("ew-theme-std-v1"), row("theme-tsmom-v1"), row("two-speed-v1")};
  p.schedule = "theme-tsmom-v1";
  p.schedule_from = {20, 35};
  p.schedule_mass = {{0.0, w_b}, {w_a, w_b}};
  p.sleeves = "two-speed-v1";
  p.sleeve_fast = fast;
  const std::vector<i64> sessions{10, 20, 30, 40};
  const auto list = icd::composition_stages(p, sessions);
  ASSERT_TRUE(list) << list.error().to_string();
  ASSERT_EQ(list->stages.size(), 3U);
  EXPECT_TRUE(list->stages[0].kind == K::standardise);
  EXPECT_TRUE(list->stages[1].kind == K::schedule);
  EXPECT_TRUE(list->stages[2].kind == K::sleeves);
  EXPECT_EQ(icd::running_stage_kinds(p),
            (std::vector<K>{K::standardise, K::schedule, K::sleeves}));
  ASSERT_EQ(list->stages[1].blocks.size(), 2U);
  EXPECT_EQ(list->stages[1].blocks[0].begin, 1U); // session 20
  EXPECT_EQ(list->stages[1].blocks[1].begin, 3U); // first session >= 35
  EXPECT_EQ(list->stages[1].blocks[1].mass, (std::vector<f64>{w_a, w_b}));
  EXPECT_EQ(list->stages[2].fast, fast);
  EXPECT_EQ(list->weights.data(), p.values.data());
  EXPECT_EQ(list->themes.data(), p.std_themes.data());
  // The list composes as the v8 runner's calls with the same blocks.
  auto from_pinned =
      st::IcComposition::create_from_stages(f.cfg, f.candidates, f.member, *list);
  ASSERT_TRUE(from_pinned);
  const auto got = f.finish(*from_pinned);
  const auto want = direct(f.themes, std_rule, list->stages[1].blocks, true);
  ASSERT_TRUE(got);
  ASSERT_TRUE(want);
  EXPECT_TRUE(same_result(*got, *want));
  // A role after every block starts both at date 0; one before every block at its end.
  const std::vector<i64> late{50, 60}, early{1, 2};
  const auto after = icd::composition_stages(p, late);
  const auto before = icd::composition_stages(p, early);
  ASSERT_TRUE(after);
  ASSERT_TRUE(before);
  EXPECT_EQ(after->stages[1].blocks[0].begin, 0U);
  EXPECT_EQ(after->stages[1].blocks[1].begin, 0U);
  EXPECT_EQ(before->stages[1].blocks[0].begin, 2U);
  EXPECT_EQ(before->stages[1].blocks[1].begin, 2U);
  p.schedule_mass.pop_back();
  EXPECT_FALSE(icd::composition_stages(p, sessions));
  // A rerank-false standardisation runs no stage.
  icd::PinnedWeights off;
  off.values = f.weights;
  off.rules = {row("ew-theme-std-v1")};
  const auto empty = icd::composition_stages(off, sessions);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->stages.empty());
  EXPECT_TRUE(empty->themes.empty());
}
} // namespace
