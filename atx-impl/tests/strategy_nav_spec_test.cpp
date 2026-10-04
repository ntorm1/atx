// P9 C3: NavSpec (`nav --spec`, book/nav_spec.hpp) and the K-P9-7 registries (book/nav_rules.hpp).
//
// Suites: NavSpec (typed spec <-> flags; a spec run publishes the flag run's bytes), NavRules (the
// one --list-rules table), CostLawRegistry, TradeRateRegistry, TargetRegistry.

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "../src/book/nav_rules.hpp"
#include "../src/book/nav_spec.hpp"
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_target_replay.hpp"
#include "strategy_spo_cli_fixture.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
namespace ns = atx::impl::strategy::nav_spec;
namespace nr = atx::impl::strategy::nav_rules;
namespace v7 = atx::impl::strategy::v7;
using Json = nlohmann::json;
namespace fs = std::filesystem;

int nav(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err);
}
std::string bytes_of(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::vector<std::string> tree(const fs::path& root) {
  std::vector<std::string> names;
  for (const auto& e : fs::recursive_directory_iterator(root))
    if (e.is_regular_file()) names.push_back(fs::relative(e.path(), root).generic_string());
  std::sort(names.begin(), names.end());
  return names;
}
// Two run directories hold the same files (recursively), byte for byte; returns the file count.
size_t expect_same_tree(const fs::path& a, const fs::path& b) {
  const auto names = tree(a);
  EXPECT_EQ(names, tree(b)) << a.string() << " vs " << b.string();
  for (const auto& name : names)
    EXPECT_TRUE(bytes_of(a / name) == bytes_of(b / name)) << name;
  return names.size();
}
// The pinned inputs and output of a nav command line on `in`.
std::vector<std::string> pinned(const RunInputs& in, const fs::path& output) {
  return {"--combined", in.combined, "--combined-sha256", in.combined_sha256, "--role", in.role,
          "--role-sha256", in.role_sha256, "--output", output.string()};
}
std::vector<std::string> joined(std::vector<std::string> a, const std::vector<std::string>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
// W1-MIN M-1's shape: aim-partial-v6, the v2 cost laws, the capacity curve, adv-hold-v1.
const std::vector<std::string> v6_shape{
    "--rule", "aim-partial-v6", "--cadence", "1", "--trade-fraction", ".25", "--dust-multiple",
    ".1", "--aim-leverage", "1.5", "--exit-rate", ".05", "--cost-v2", "--capacity-curve",
    "--adv-hold-q", ".05", "--cost-shrink-kappa", "1", "--rate-clip", ".5,1.5"};

// One flag's value in a command line (nullptr when absent).
const std::string* value_of(const std::vector<std::string>& args, const std::string& flag) {
  for (size_t k = 0; k + 1 < args.size(); ++k)
    if (args[k] == flag) return &args[k + 1];
  return nullptr;
}
// The same value as the parsers read it: numbers bit for bit, a pair part by part, else the text.
bool same_value(const ns::Key& k, const std::string& a, const std::string& b) {
  const auto bits = [](const std::string& s) { return std::bit_cast<u64>(std::stod(s)); };
  switch (k.type) {
  case ns::Type::Real: return bits(a) == bits(b);
  case ns::Type::Count: return std::stoull(a) == std::stoull(b);
  case ns::Type::RealPair: {
    const auto ca = a.find(','), cb = b.find(',');
    return bits(a.substr(0, ca)) == bits(b.substr(0, cb)) &&
           bits(a.substr(ca + 1)) == bits(b.substr(cb + 1));
  }
  case ns::Type::Switch:
  case ns::Type::Text:
  case ns::Type::Choice: return a == b;
  }
  return false;
}
} // namespace

// The flag adapter and the spec agree: for command lines of every parser's flags (the replay's,
// the v7 seam's: aim-partial-v6, the cost laws, the capacity curve, vol-target, risk-target,
// spo-v3, calibration), flags -> NavSpec -> flags carries every flag with a value the parsers read
// to the same bits; the spec written to a file reads back equal; a hand-typed spec of the same
// run is the same NavSpec; and the v7 parser, given the flags and the spec's rendering, configures
// the same v7 options and passes the replay the same typed flags.
TEST(NavSpec, FlagsAndSpecSameConfig) {
  const Directory dir;
  const std::string sha(64, 'a');
  const auto base = std::vector<std::string>{
      "--combined", "c.json", "--combined-sha256", sha, "--role", "r.json", "--role-sha256", sha,
      "--output", "out"};
  const std::vector<std::vector<std::string>> lines{
      joined(base, joined(v6_shape, {"--book-workers", "4", "--locate-in-aim", "--neutralize",
                                     "price-risk-v1", "--fields", "f/manifest.json",
                                     "--fields-sha256", sha, "--liquidity-cache"})),
      joined(base, {"--rule", "aim-partial-v5", "--cadence", "5", "--aim-leverage", "1.1720",
                    "--vol-target", "vol-target-v1", "--risk-model", "risk", "--risk-model-sha256",
                    sha, "--calibrate-gross", "1.25", "--calibrate-tolerance", ".005",
                    "--two-speed", "two-speed-v1", "--trade-fraction", ".05",
                    "--warm-start-sessions", "21", "--max-bytes", "536870912"}),
      joined(base, {"--rule", "spo-v3", "--risk-model", "risk", "--risk-model-sha256", sha,
                    "--spo-books", "all", "--specific-ceiling-void", "on", "--risk-target",
                    ".05", "--risk-target-cadence", "21", "--hold-band", ".2", "--stage-timers",
                    "--order-basis", "delta", "--rate", "fixed", "--cost-law", "cost-sqrt-v2"}),
      joined(base, {"--rule", "aim-partial-v5", "--rate", "per-name-v1", "--rate-rra", "10",
                    "--rate-lambda", "2e-1", "--rate-min", ".01", "--rate-max", ".15",
                    "--daily-turnover-mean-max", ".2", "--daily-turnover-p95-max", ".3",
                    "--emit-holdings", "h", "--holdings-format", "csv"})};
  for (const auto& args : lines) {
    auto spec = ns::from_args(args);
    ASSERT_TRUE(spec) << spec.error().to_string();
    EXPECT_EQ(spec->at("schema"), "atx.nav-spec/v1");
    auto rendered = ns::to_args(*spec);
    ASSERT_TRUE(rendered) << rendered.error().to_string();
    // Every flag, once, with a value the parsers read to the same bits.
    size_t flags = 0;
    for (size_t k = 0; k < args.size(); ++k) {
      const auto* key = ns::by_flag(args[k]);
      ASSERT_NE(key, nullptr) << args[k];
      ++flags;
      EXPECT_EQ(std::count(rendered->begin(), rendered->end(), args[k]), 1) << args[k];
      if (key->type == ns::Type::Switch) continue;
      const auto* got = value_of(*rendered, args[k]);
      ASSERT_NE(got, nullptr) << args[k];
      EXPECT_TRUE(same_value(*key, args[k + 1], *got)) << args[k] << ' ' << *got;
      ++k;
    }
    size_t rendered_flags = 0;
    for (const auto& token : *rendered) rendered_flags += token.rfind("--", 0) == 0 ? 1U : 0U;
    EXPECT_EQ(rendered_flags, flags);
    // The rendering is the spec's own: flags -> spec -> flags -> spec is the same document.
    auto again = ns::from_args(*rendered);
    ASSERT_TRUE(again);
    EXPECT_EQ(*again, *spec);
    // A spec file reads back to the same NavSpec.
    const auto file = dir.path / "spec.json";
    write_json_file(file, *spec);
    auto read = ns::read(file.string());
    ASSERT_TRUE(read) << read.error().to_string();
    EXPECT_EQ(*read, *spec);
    // The v7 parser: the same options and the same typed replay flags from both forms.
    std::vector<std::string> a{"nav"}, b{"nav"};
    a.insert(a.end(), args.begin(), args.end());
    b.insert(b.end(), rendered->begin(), rendered->end());
    std::vector<char*> pa, pb;
    for (auto& t : a) pa.push_back(t.data());
    for (auto& t : b) pb.push_back(t.data());
    EXPECT_EQ(v7::claims_nav_args(static_cast<int>(pa.size()), pa.data()),
              v7::claims_nav_args(static_cast<int>(pb.size()), pb.data()));
    if (!v7::claims_nav_args(static_cast<int>(pa.size()), pa.data())) continue;
    auto ca = v7::parse_nav_v7_args(static_cast<int>(pa.size()), pa.data());
    auto cb = v7::parse_nav_v7_args(static_cast<int>(pb.size()), pb.data());
    if (!ca) { // a refused line is refused in both forms (vol-target + two-speed + calibrate)
      EXPECT_FALSE(cb);
      continue;
    }
    ASSERT_TRUE(cb) << cb.error().to_string();
    const auto& oa = ca->options;
    const auto& ob = cb->options;
    EXPECT_EQ(oa.stress, ob.stress);
    EXPECT_EQ(oa.capacity, ob.capacity);
    EXPECT_EQ(oa.aim_v6, ob.aim_v6);
    EXPECT_EQ(std::bit_cast<u64>(oa.v6.kappa), std::bit_cast<u64>(ob.v6.kappa));
    EXPECT_EQ(std::bit_cast<u64>(oa.v6.band_b), std::bit_cast<u64>(ob.v6.band_b));
    EXPECT_EQ(std::bit_cast<u64>(oa.v6.clip_lo), std::bit_cast<u64>(ob.v6.clip_lo));
    EXPECT_EQ(std::bit_cast<u64>(oa.v6.clip_hi), std::bit_cast<u64>(ob.v6.clip_hi));
    EXPECT_EQ(oa.spo_v1, ob.spo_v1);
    EXPECT_EQ(oa.spo_params.version, ob.spo_params.version);
    EXPECT_EQ(oa.spo_params.all_books, ob.spo_params.all_books);
    EXPECT_EQ(oa.spo_params.void_on_capped, ob.spo_params.void_on_capped);
    EXPECT_EQ(oa.risk_target.on, ob.risk_target.on);
    EXPECT_EQ(std::bit_cast<u64>(oa.risk_target.params.sigma_star),
              std::bit_cast<u64>(ob.risk_target.params.sigma_star));
    EXPECT_EQ(oa.risk_target.params.cadence, ob.risk_target.params.cadence);
    EXPECT_EQ(ca->output, cb->output);
    EXPECT_EQ(ca->risk_model, cb->risk_model);
    const std::vector<std::string> pass_a(ca->args.begin() + 1, ca->args.end());
    const std::vector<std::string> pass_b(cb->args.begin() + 1, cb->args.end());
    auto ta = ns::from_args(pass_a);
    auto tb = ns::from_args(pass_b);
    ASSERT_TRUE(ta && tb);
    EXPECT_EQ(*ta, *tb);
  }
  // The documented spelling: a hand-typed spec of the first line is the adapter's NavSpec.
  const Json typed{
      {"schema", "atx.nav-spec/v1"},
      {"options",
       {{"combined", "c.json"}, {"combined_sha256", sha}, {"role", "r.json"},
        {"role_sha256", sha}, {"output", "out"}, {"rule", "aim-partial-v6"}, {"cadence", 1},
        {"trade_fraction", 0.25}, {"dust_multiple", 0.1}, {"aim_leverage", 1.5},
        {"exit_rate", 0.05}, {"cost_v2", true}, {"capacity_curve", true}, {"adv_hold_q", 0.05},
        {"cost_shrink_kappa", 1}, {"rate_clip", {0.5, 1.5}}, {"book_workers", 4},
        {"locate_in_aim", true}, {"neutralize", "price-risk-v1"},
        {"fields", "f/manifest.json"}, {"fields_sha256", sha}, {"liquidity_cache", true}}}};
  auto from_typed = ns::to_args(typed);
  ASSERT_TRUE(from_typed) << from_typed.error().to_string();
  auto adapter = ns::from_args(lines.front());
  auto typed_again = ns::from_args(*from_typed);
  ASSERT_TRUE(adapter && typed_again);
  EXPECT_EQ(*typed_again, *adapter);
  // Every key is its flag's name (snake case), so a reader maps one to the other by eye.
  for (const auto& k : ns::keys()) {
    std::string flag = "--" + std::string(k.key);
    std::replace(flag.begin(), flag.end(), '_', '-');
    EXPECT_EQ(flag, k.flag);
    EXPECT_EQ(ns::by_key(k.key), &k);
  }
}

// A spec is typed and closed: unknown keys, wrong types, non-finite or partial numbers, a value
// outside a choice, a negative or fractional count, another schema and the flags it may not hold
// are refused; constants hold today's values (leverage_range only inside [1, 2], narrower only
// around the run's aim leverage and without calibration).
TEST(NavSpec, RefusesWhatItCannotType) {
  const auto spec = [](Json options, Json constants = nullptr) {
    Json doc{{"schema", "atx.nav-spec/v1"}, {"options", std::move(options)}};
    if (!constants.is_null()) doc["constants"] = std::move(constants);
    return doc;
  };
  EXPECT_TRUE(ns::to_args(spec(Json::object())));
  for (const auto& bad :
       {spec({{"no_such_key", 1}}), spec({{"cadence", 1.5}}), spec({{"cadence", -1}}),
        spec({{"cadence", "5"}}), spec({{"aim_leverage", "1.5"}}), spec({{"cost_v2", 1}}),
        spec({{"rule", "aim-partial-v9"}}), spec({{"rate_clip", {0.5}}}),
        spec({{"output", ""}}), Json{{"schema", "atx.nav-spec/v2"}, {"options", Json::object()}},
        Json{{"schema", "atx.nav-spec/v1"}, {"options", Json::object()}, {"flags", 1}},
        spec(Json::object(), {{"initial_nav", 2e9}}),
        spec(Json::object(), {{"liquidity_window", 21}}),
        spec(Json::object(), {{"capacity_multiples", {1, 2}}}),
        spec(Json::object(), {{"max_events", 1}}),
        spec(Json::object(), {{"leverage_range", {1, 3}}}),
        spec(Json::object(), {{"leverage_range", {0.5, 2}}}),
        spec({{"aim_leverage", 1.8}}, {{"leverage_range", {1, 1.5}}}),
        spec({{"calibrate_gross", 1.2}}, {{"leverage_range", {1, 1.5}}})})
    EXPECT_FALSE(ns::to_args(bad)) << bad.dump();
  EXPECT_TRUE(ns::to_args(spec({{"aim_leverage", 1.2}}, {{"leverage_range", {1, 1.5}}})));
  EXPECT_TRUE(ns::to_args(spec(Json::object(), ns::constants_json())));
  for (const auto& args :
       std::vector<std::vector<std::string>>{{"--list-rules"}, {"--help"}, {"--cadence"},
                                             {"--cadence", "+5"}, {"--aim-leverage", "nan"},
                                             {"--aim-leverage", "1.5x"}, {"--rate-clip", ".5"},
                                             {"--cadence", "1", "--cadence", "2"},
                                             {"--one-way-bps", "6"}})
    EXPECT_FALSE(ns::from_args(args)) << args.front();
}

// `nav --spec S.json` publishes byte for byte the directory of the flag run it renders, under
// the v7 seam (aim-partial-v6, the v2 cost laws, the capacity curve, adv-hold-v1) and a plain
// aim-partial-v5 run; `nav <flags> --print-spec` prints the NavSpec (with the constants block)
// and runs nothing; --spec takes no flag but --output (which replaces options.output).
TEST(NavSpec, SpecRunPublishesTheFlagRunBytes) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const std::vector<std::vector<std::string>> shapes{
      v6_shape, {"--rule", "aim-partial-v5", "--cadence", "1", "--trade-fraction", ".25",
                 "--dust-multiple", ".1", "--aim-leverage", "1.2", "--exit-rate", ".05"}};
  for (size_t s = 0; s < shapes.size(); ++s) {
    const auto flags_dir = dir.path / ("flags" + std::to_string(s));
    const auto spec_dir = dir.path / ("spec" + std::to_string(s));
    std::ostringstream out, err;
    std::vector<std::string> args{"nav"};
    for (const auto& t : joined(pinned(in, flags_dir), shapes[s])) args.push_back(t);
    ASSERT_EQ(nav(args, out, err), 0) << err.str();
    std::ostringstream printed;
    auto print = args;
    print.push_back("--print-spec");
    ASSERT_EQ(nav(print, printed, err), 0) << err.str();
    const auto doc = Json::parse(printed.str());
    EXPECT_EQ(doc.at("constants"), ns::constants_json());
    const auto file = dir.path / ("spec" + std::to_string(s) + ".json");
    write_json_file(file, doc);
    ASSERT_EQ(nav({"nav", "--spec", file.string(), "--output", spec_dir.string()}, out, err), 0)
        << err.str();
    EXPECT_GE(expect_same_tree(spec_dir, flags_dir), 4U);
  }
  EXPECT_TRUE(fs::exists(dir.path / "spec0" / "capacity" / "summary.json"));
  std::ostringstream out, err;
  EXPECT_EQ(nav({"nav", "--spec", (dir.path / "spec1.json").string(), "--cadence", "2"}, out, err),
            2);
  EXPECT_EQ(nav({"nav", "--spec", (dir.path / "missing.json").string()}, out, err), 2);
  EXPECT_EQ(nav({"nav", "--spec", (dir.path / "spec1.json").string(), "--print-spec"}, out, err),
            2);
}

// K-P9-7: one table, leverage rows first (C1's bytes), then trade-rate, cost-law, target; each
// row {id, kind, x-flag, params_schema, incompatible, description}, a closed params_schema whose
// every property names its flag, x-flag "--flag VALUE" or null; ids unique; text mode one
// "<kind> <id>" line per row in table order.
TEST(NavRules, OneTableEveryKindInOrder) {
  std::ostringstream out, err;
  ASSERT_EQ(nav({"nav", "--list-rules", "--json"}, out, err), 0) << err.str();
  const auto table = Json::parse(out.str());
  EXPECT_EQ(table, nr::table_json());
  const auto& caps = table.at("capabilities");
  for (const auto* cap : {"nav-spec-v1", "fields-seal-p13", "calibrate-gross-pm6-6"})
    EXPECT_NE(std::find(caps.begin(), caps.end(), Json(cap)), caps.end()) << cap;
  std::vector<std::string> ids, kinds;
  std::string text;
  for (const auto& row : table.at("rules")) {
    ids.push_back(row.at("id").get<std::string>());
    kinds.push_back(row.at("kind").get<std::string>());
    text += kinds.back() + ' ' + ids.back() + '\n';
    const auto& schema = row.at("params_schema");
    EXPECT_EQ(schema.at("type"), "object") << ids.back();
    EXPECT_FALSE(schema.at("additionalProperties").get<bool>()) << ids.back();
    for (const auto& property : schema.at("properties"))
      EXPECT_EQ(property.at("x-flag").get<std::string>().rfind("--", 0), 0U) << ids.back();
    EXPECT_TRUE(row.at("incompatible").is_array()) << ids.back();
    const auto& flag = row.at("x-flag");
    EXPECT_TRUE(flag.is_null() || flag.get<std::string>().rfind("--", 0) == 0) << ids.back();
    if (kinds.back() != "leverage") EXPECT_TRUE(row.at("description").is_string()) << ids.back();
  }
  EXPECT_EQ(ids, (std::vector<std::string>{"fixed-v1", "vol-target-v1", "risk-target-v1",
                                           "aim-partial-v5", "per-name-v1", "two-speed-nav-v1",
                                           "flat-bps-v1", "sqrt-impact-v1", "ko-invariance-v1",
                                           "fim-live-v1", "cost-sqrt-v2"}));
  auto sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());
  EXPECT_TRUE(std::is_sorted(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) {
    const std::map<std::string, int> order{
        {"leverage", 0}, {"trade-rate", 1}, {"cost-law", 2}, {"target", 3}};
    return order.at(a) < order.at(b);
  }));
  std::ostringstream lines;
  ASSERT_EQ(nav({"nav", "--list-rules"}, lines, err), 0);
  EXPECT_EQ(lines.str(), text);
}

// The cost-law registry: the laws of every scenario the replay can run, by their recipe labels
// (flat-bps-v1 / sqrt-impact-v1 the fixed S1 and S2 / S3 laws, ko-invariance-v1 / fim-live-v1 the
// v2 stress laws, not nameable alone) and the one nameable row, cost-sqrt-v2 = --cost-law
// cost-sqrt-v2, a closed schema with no parameter (a wave names it with params {}).
TEST(CostLawRegistry, RowsAreTheReplaysLawsAndOneNameableSet) {
  const auto rows = nr::cost_law_rows();
  ASSERT_EQ(rows.size(), 5U);
  EXPECT_EQ(rows[0].id, "flat-bps-v1");
  EXPECT_EQ(rows[1].id, "sqrt-impact-v1");
  EXPECT_EQ(rows[2].id, std::string_view(
                            st::cost_v2::reserved_cost_rule(st::cost_v2::ko_scenario_id)));
  EXPECT_EQ(rows[3].id, std::string_view(
                            st::cost_v2::reserved_cost_rule(st::cost_v2::fim_scenario_id)));
  for (const auto& row : rows) {
    EXPECT_EQ(row.kind, nr::Kind::CostLaw);
    EXPECT_EQ(nr::find(nr::Kind::CostLaw, row.id), &row);
    EXPECT_TRUE(row.params().empty()) << row.id;
    EXPECT_EQ(row.x_flag.empty(), row.id != nr::cost_sqrt_v2_id) << row.id;
  }
  EXPECT_EQ(nr::find(nr::Kind::CostLaw, "cost-sqrt-v2")->x_flag, "--cost-law cost-sqrt-v2");
  EXPECT_EQ(nr::flag_ids(nr::Kind::CostLaw), "cost-sqrt-v2");
  EXPECT_EQ(nr::find(nr::Kind::CostLaw, "aim-partial-v5"), nullptr);
}

// --cost-law cost-sqrt-v2 publishes byte for byte the --cost-v2 run; giving both, a fixed law
// (always on, no flag), a stress law alone and an unknown id are usage errors before any output.
TEST(CostLawRegistry, CostSqrtV2IsTheCostV2RunByteForByte) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const std::vector<std::string> v5{"--rule", "aim-partial-v5", "--cadence", "1",
                                    "--trade-fraction", ".25", "--dust-multiple", ".1",
                                    "--aim-leverage", "1.2"};
  const auto run = [&](const fs::path& output, const std::vector<std::string>& extra) {
    std::ostringstream out, err;
    std::vector<std::string> args{"nav"};
    for (const auto& t : joined(joined(pinned(in, output), v5), extra)) args.push_back(t);
    const int code = nav(args, out, err);
    EXPECT_TRUE(code == 0 || !err.str().empty());
    return code;
  };
  ASSERT_EQ(run(dir.path / "flag", {"--cost-v2"}), 0);
  ASSERT_EQ(run(dir.path / "law", {"--cost-law", "cost-sqrt-v2"}), 0);
  EXPECT_GE(expect_same_tree(dir.path / "law", dir.path / "flag"), 8U);
  EXPECT_TRUE(fs::exists(dir.path / "law" / "daily_modeled-1bn-ko-v1.csv"));
  for (const auto& bad : std::vector<std::vector<std::string>>{
           {"--cost-law", "cost-sqrt-v2", "--cost-v2"},
           {"--cost-v2", "--cost-law", "cost-sqrt-v2"},
           {"--cost-law", "flat-bps-v1"},
           {"--cost-law", "ko-invariance-v1"},
           {"--cost-law", "cost-sqrt-v9"},
           {"--cost-law", "cost-sqrt-v2", "--cost-law", "cost-sqrt-v2"}})
    EXPECT_EQ(run(dir.path / "bad", bad), 2) << bad.back();
  EXPECT_FALSE(fs::exists(dir.path / "bad"));
}

// The trade-rate registry: aim-partial-v5 (--rule aim-partial-v5; theta, dust, exit rate at the
// command line's defaults), per-name-v1 (--rate per-name-v1 and its four parameters at the
// nav_rate_* defaults; refused with the fixed-rate rules) and two-speed-nav-v1 (no flag of its
// own: the composition rule two-speed-v1 sets --two-speed on both exes). The params' flags are the
// parsers' own: each derived flag line is a NavSpec option.
TEST(TradeRateRegistry, RowsCarryTheParsersFlagsAndDefaults) {
  const auto rows = nr::trade_rate_rows();
  ASSERT_EQ(rows.size(), 3U);
  const st::TargetReplayConfig d{};
  const auto v5 = rows[0].params();
  EXPECT_EQ(rows[0].id, "aim-partial-v5");
  EXPECT_EQ(rows[0].x_flag, "--rule aim-partial-v5");
  EXPECT_EQ(v5.at("trade_fraction").at("default"), d.trade_fraction);
  EXPECT_EQ(v5.at("dust_multiple").at("default"), d.dust_multiple);
  EXPECT_EQ(v5.at("exit_rate").at("default"), d.exit_rate);
  const auto rate = rows[1].params();
  EXPECT_EQ(rows[1].x_flag, "--rate per-name-v1");
  EXPECT_EQ(rate.at("rate_rra").at("default"), st::nav_rate_rra);
  EXPECT_EQ(rate.at("rate_lambda").at("default"), st::nav_rate_lambda);
  EXPECT_EQ(rate.at("rate_min").at("default"), st::nav_rate_min);
  EXPECT_EQ(rate.at("rate_max").at("default"), st::nav_rate_max);
  EXPECT_NE(std::find(rows[1].incompatible.begin(), rows[1].incompatible.end(), "two-speed-v1"),
            rows[1].incompatible.end());
  EXPECT_TRUE(rows[2].x_flag.empty());
  EXPECT_EQ(nr::flag_ids(nr::Kind::TradeRate), "aim-partial-v5|per-name-v1");
  for (const auto& row : rows) {
    EXPECT_EQ(row.kind, nr::Kind::TradeRate);
    std::vector<std::string> line;
    std::istringstream tokens{std::string(row.x_flag)};
    for (std::string t; tokens >> t;) line.push_back(t);
    const auto params = row.params();
    for (const auto& property : params) {
      line.push_back(property.at("x-flag").get<std::string>());
      line.push_back("0.5");
    }
    EXPECT_TRUE(ns::from_args(line)) << row.id; // every derived flag is a typed run flag
  }
}

// The target slot (K-P9-7 kind target): no row in this build; --target-rule is a registry flag
// of the v7 seam and refuses every id before any output (lane COV-MV adds mv-aim-v1 and its
// handler).
TEST(TargetRegistry, SlotIsEmptyAndRefusesEveryId) {
  EXPECT_TRUE(nr::target_rows().empty());
  EXPECT_EQ(nr::find(nr::Kind::Target, "mv-aim-v1"), nullptr);
  EXPECT_EQ(nr::kind_name(nr::Kind::Target), "target");
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  std::vector<std::string> args{"nav"};
  for (const auto& t : joined(pinned(in, dir.path / "out"),
                              {"--rule", "aim-partial-v5", "--target-rule", "mv-aim-v1"}))
    args.push_back(t);
  std::ostringstream out, err;
  EXPECT_EQ(nav(args, out, err), 2);
  EXPECT_NE(err.str().find("unknown --target-rule mv-aim-v1 (none registered)"),
            std::string::npos)
      << err.str();
  EXPECT_FALSE(fs::exists(dir.path / "out"));
}
