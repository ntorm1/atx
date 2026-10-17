// P9 C3: NavSpec (`nav --spec`, book/nav_spec.hpp) and the K-P9-7 registries (book/nav_rules.hpp).
//
// Suites: NavSpec (typed spec <-> flags; a spec run publishes the flag run's bytes), NavRules (the
// one --list-rules table), CostLawRegistry, TradeRateRegistry, TargetRegistry, NavAnnMeanGross
// (the print-only --ann-mean-gross of Rulings PRE-7 / PRE-7a).
//
// Append-only (Ruling W3-9): the registry tests check C3's rows by lookup and as a prefix of their
// kind, so a lane that appends a row (AL-CLOCK, COV-MV) edits no assertion here.

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/book/nav_rules.hpp"
#include "../src/book/nav_spec.hpp"
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_risk_target.hpp"
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
namespace co = atx::core;
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
// A spec file as raw text (a Json object cannot hold a repeated key).
fs::path write_text(const fs::path& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary);
  out << text;
  return file;
}
std::vector<std::string> cells_of(const std::string& line) {
  std::vector<std::string> cells;
  std::istringstream in(line);
  for (std::string cell; std::getline(in, cell, ',');) cells.push_back(cell);
  return cells;
}
// summarize_nav's mean of one daily CSV column, recomputed from the published file: the rows with
// return_observation 1 except the first row, the running mean of Moments::add (bit for bit) and
// the plain mean; the CSV prints 17 digits, so every value reads back to its bits.
struct ColumnMean {
  size_t n{};
  f64 running{}, plain{};
};
ColumnMean return_row_mean(const fs::path& csv, const std::string& column) {
  std::ifstream in(csv, std::ios::binary);
  std::string line;
  std::getline(in, line);
  const auto header = cells_of(line);
  const auto index = [&](const std::string& name) {
    const auto at = std::find(header.begin(), header.end(), name);
    EXPECT_NE(at, header.end()) << name;
    return static_cast<size_t>(at - header.begin());
  };
  const size_t observed = index("return_observation"), value = index(column);
  ColumnMean m;
  f64 sum = 0;
  for (size_t row = 0; std::getline(in, line); ++row) {
    const auto cells = cells_of(line);
    if (row == 0 || cells.at(observed) != "1") continue;
    const f64 x = std::stod(cells.at(value));
    ++m.n;
    const f64 d = x - m.running;
    m.running += d / static_cast<f64>(m.n);
    sum += x;
  }
  m.plain = sum / static_cast<f64>(m.n);
  return m;
}
// `expected` is the prefix of `got` (C3's rows of a kind; a later lane appends after them).
bool starts_with(const std::vector<std::string>& got, const std::vector<std::string>& expected) {
  return got.size() >= expected.size() && std::equal(expected.begin(), expected.end(), got.begin());
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
                    "--order-basis", "delta", "--rate", "fixed", "--cost-law", "cost-sqrt-v2",
                    "--ann-mean-gross"}),
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
// around the run's aim leverage and without calibration or a construction grid, whose variants
// set their own aim leverage: review C3 R-2).
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
        spec({{"calibrate_gross", 1.2}}, {{"leverage_range", {1, 1.5}}}),
        spec({{"aim_leverage", 1.2}, {"construction_grid", "g.json"}},
             {{"leverage_range", {1, 1.5}}}),
        spec({{"ann_mean_gross", 1}})})
    EXPECT_FALSE(ns::to_args(bad)) << bad.dump();
  EXPECT_TRUE(ns::to_args(spec({{"aim_leverage", 1.2}}, {{"leverage_range", {1, 1.5}}})));
  EXPECT_TRUE(ns::to_args(spec(Json::object(), ns::constants_json())));
  // R-2's refusal is the narrower range's: a grid under the registry's whole range is a spec.
  EXPECT_TRUE(ns::to_args(spec({{"aim_leverage", 1.2}, {"construction_grid", "g.json"}},
                               {{"leverage_range", {1, 2}}})));
  const auto grid_narrow = ns::to_args(spec({{"construction_grid", "g.json"}},
                                            {{"leverage_range", {1, 1.5}}}));
  ASSERT_FALSE(grid_narrow);
  EXPECT_NE(grid_narrow.error().to_string().find("construction_grid"), std::string::npos);
  for (const auto& args :
       std::vector<std::vector<std::string>>{{"--list-rules"}, {"--help"}, {"--cadence"},
                                             {"--cadence", "+5"}, {"--aim-leverage", "nan"},
                                             {"--aim-leverage", "1.5x"}, {"--rate-clip", ".5"},
                                             {"--cadence", "1", "--cadence", "2"},
                                             {"--ann-mean-gross", "--ann-mean-gross"},
                                             {"--one-way-bps", "6"}})
    EXPECT_FALSE(ns::from_args(args)) << args.front();
}

// Review C3 R-1: a spec file is JSON with each key once per object. A key repeated at the top
// level, inside options or inside constants is refused by ns::read and by nav --spec (exit 2, the
// key named, no output directory), whether the values differ or not; the same key in two
// different objects is not a repetition (that file is refused for its unknown option instead).
TEST(NavSpec, RefusesADuplicateKey) {
  const Directory dir;
  const std::string head = R"({"schema": "atx.nav-spec/v1", )";
  const auto good = write_text(
      dir.path / "good.json",
      head + R"("options": {"aim_leverage": 1.2, "cadence": 5}, )" +
          R"("constants": {"leverage_range": [1, 1.5]}})");
  ASSERT_TRUE(ns::read(good.string())) << ns::read(good.string()).error().to_string();
  const std::vector<std::pair<std::string, std::string>> repeated{
      {"options", head + R"("options": {"aim_leverage": 1.2}, "options": {"aim_leverage": 1.9}})"},
      {"aim_leverage", head + R"("options": {"aim_leverage": 1.2, "aim_leverage": 1.9}})"},
      {"leverage_range", head + R"("options": {"aim_leverage": 1.2}, "constants": )" +
                             R"({"leverage_range": [1, 1.5], "leverage_range": [1, 2]}})"},
      {"schema", head + R"("schema": "atx.nav-spec/v1", "options": {}})"},
      {"cadence", head + R"("options": {"cadence": 5, "cadence": 5}})"}};
  for (size_t k = 0; k < repeated.size(); ++k) {
    const auto& [key, text] = repeated[k];
    const auto file = write_text(dir.path / ("dup" + std::to_string(k) + ".json"), text);
    const auto read = ns::read(file.string());
    ASSERT_FALSE(read) << key;
    EXPECT_NE(read.error().to_string().find("duplicate key \"" + key + "\""), std::string::npos)
        << read.error().to_string();
    const auto out_dir = dir.path / ("out" + std::to_string(k));
    std::ostringstream out, err;
    EXPECT_EQ(nav({"nav", "--spec", file.string(), "--output", out_dir.string()}, out, err), 2)
        << key;
    EXPECT_NE(err.str().find("duplicate key \"" + key + "\""), std::string::npos) << err.str();
    EXPECT_TRUE(out.str().empty()) << out.str(); // refused before the spec line is printed
    EXPECT_FALSE(fs::exists(out_dir)) << key;
  }
  // One key set per object: "schema" in options is no repetition of the top-level key.
  const auto nested = write_text(dir.path / "nested.json",
                                 head + R"("options": {"schema": "atx.nav-spec/v1"}})");
  const auto refused = ns::read(nested.string());
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().to_string().find("duplicate key"), std::string::npos);
  EXPECT_NE(refused.error().to_string().find("unknown option schema"), std::string::npos);
}

// Ruling C3-7 (review C3 S-9): the construction-grid file (a file a NavSpec names, options
// construction_grid) gets R-1's refusal. A key repeated inside one object (a flag twice in one
// variant's flags, "variants" twice) is refused before any output, exit 1 as every grid-file
// refusal, naming the key and the file: either file would otherwise run at the last value. The
// same grid without the repetition runs.
TEST(NavSpec, GridFileRefusesADuplicateKey) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const std::vector<std::string> v5{"--rule", "aim-partial-v5", "--cadence", "1",
                                    "--trade-fraction", ".25", "--aim-leverage", "1.2"};
  const auto run = [&](const fs::path& grid, const fs::path& output, std::ostream& out,
                       std::ostream& err) {
    std::vector<std::string> args{"nav"};
    const std::vector<std::string> grid_flag{"--construction-grid", grid.string()};
    for (const auto& t : joined(joined(pinned(in, output), v5), grid_flag)) args.push_back(t);
    return nav(args, out, err);
  };
  const std::string head = R"({"schema": "atx.nav-construction-grid/v1", "variants": )";
  const std::string one = R"([{"id": "a", "flags": {"--aim-leverage": "1.9"}}])";
  const std::vector<std::pair<std::string, std::string>> repeated{
      {"--aim-leverage",
       head + R"([{"id": "a", "flags": {"--aim-leverage": "1.1", "--aim-leverage": "1.9"}}]})"},
      {"variants", head + R"([], "variants": )" + one + "}"}};
  for (size_t k = 0; k < repeated.size(); ++k) {
    const auto& [key, text] = repeated[k];
    const auto grid = write_text(dir.path / ("dup" + std::to_string(k) + ".json"), text);
    const auto output = dir.path / ("out" + std::to_string(k));
    std::ostringstream out, err;
    EXPECT_EQ(run(grid, output, out, err), 1) << key;
    EXPECT_NE(err.str().find("nav grid: duplicate key \"" + key + "\" in " + grid.string()),
              std::string::npos)
        << err.str();
    EXPECT_FALSE(fs::exists(output)) << key;
  }
  const auto good = write_text(dir.path / "good.json", head + one + "}");
  std::ostringstream out, err;
  ASSERT_EQ(run(good, dir.path / "good", out, err), 0) << err.str();
  EXPECT_TRUE(fs::exists(dir.path / "good" / "a" / "summary.json"));
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
    std::ostringstream spec_out; // review C3 S-12: the console's first line names the bytes read
    ASSERT_EQ(nav({"nav", "--spec", file.string(), "--output", spec_dir.string()}, spec_out, err),
              0)
        << err.str();
    const auto digest = co::sha256_hex(bytes_of(file));
    ASSERT_TRUE(digest);
    const std::string line =
        "nav spec: " + std::string(ns::schema) + ' ' + file.string() + " sha256 " + *digest + '\n';
    EXPECT_EQ(spec_out.str().substr(0, line.size()), line) << spec_out.str();
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
// every property names its flag, x-flag "--flag VALUE" or null; ids unique; C3's rows lead their
// kind (a later lane appends after them, Ruling W3-9); text mode one "<kind> <id>" line per row
// in table order.
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
  std::map<std::string, std::vector<std::string>> by_kind;
  for (size_t k = 0; k < ids.size(); ++k) by_kind[kinds[k]].push_back(ids[k]);
  const std::map<std::string, std::vector<std::string>> leading{
      {"leverage", {"fixed-v1", "vol-target-v1", "risk-target-v1"}},
      {"trade-rate", {"aim-partial-v5", "per-name-v1", "two-speed-nav-v1"}},
      {"cost-law",
       {"flat-bps-v1", "sqrt-impact-v1", "ko-invariance-v1", "fim-live-v1", "cost-sqrt-v2"}},
      {"target", {}}};
  for (const auto& [kind, expected] : leading)
    EXPECT_TRUE(starts_with(by_kind[kind], expected)) << kind;
  EXPECT_EQ(by_kind.size(), leading.size()); // no other kind
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

// Ruling C3-3: the cost-law row cost-sqrt-v2 lists its legacy spelling, x-flag-aliases
// ["--cost-v2"] (a run flag of the nav verb, the switch whose run --cost-law cost-sqrt-v2 gives
// byte for byte: CostLawRegistry.CostSqrtV2IsTheCostV2RunByteForByte). The key is new and only on
// a row that has an alias: C1's leverage rows are risk_target's own rows byte for byte (the table
// prints risk_target::leverage_rules_json() unchanged), and C3's other rows keep their shape.
TEST(NavRules, LegacyAliasesOnTheirRowsOnly) {
  std::ostringstream out, err;
  ASSERT_EQ(nav({"nav", "--list-rules", "--json"}, out, err), 0) << err.str();
  const auto rules = Json::parse(out.str()).at("rules");
  const auto c1 = st::risk_target::leverage_rules_json();
  ASSERT_GE(rules.size(), c1.size());
  for (size_t k = 0; k < c1.size(); ++k) {
    EXPECT_EQ(rules.at(k).dump(), c1.at(k).dump()) << k; // C1's bytes
    EXPECT_FALSE(rules.at(k).contains("x-flag-aliases")) << k;
  }
  const std::vector<std::string> shape{"description", "id",           "incompatible",
                                       "kind",        "params_schema", "x-flag"};
  size_t aliased = 0;
  for (size_t k = c1.size(); k < rules.size(); ++k) {
    const auto& row = rules.at(k);
    const auto id = row.at("id").get<std::string>();
    std::vector<std::string> keys;
    for (auto it = row.begin(); it != row.end(); ++it)
      if (it.key() != "x-flag-aliases") keys.push_back(it.key());
    EXPECT_EQ(keys, shape) << id; // nlohmann orders keys: the part-A shape, plus the alias key
    if (!row.contains("x-flag-aliases")) continue;
    ++aliased;
    const auto& aliases = row.at("x-flag-aliases");
    ASSERT_TRUE(aliases.is_array() && !aliases.empty()) << id;
    ASSERT_TRUE(row.at("x-flag").is_string()) << id; // an alias spells a nameable rule
    for (const auto& alias : aliases) {
      const auto* key = ns::by_flag(alias.get<std::string>());
      ASSERT_NE(key, nullptr) << alias; // a run flag a wave can see in a command line
      EXPECT_NE(alias.get<std::string>(), row.at("x-flag").get<std::string>()) << id;
    }
  }
  const auto* v2 = nr::find(nr::Kind::CostLaw, nr::cost_sqrt_v2_id);
  ASSERT_NE(v2, nullptr);
  ASSERT_EQ(v2->x_flag_aliases.size(), 1U);
  EXPECT_EQ(v2->x_flag_aliases.front(), "--cost-v2");
  EXPECT_EQ(ns::by_flag("--cost-v2")->type, ns::Type::Switch);
  for (const auto& row : rules)
    if (row.at("id") == "cost-sqrt-v2")
      EXPECT_EQ(row.at("x-flag-aliases"), Json::array({"--cost-v2"}));
  EXPECT_GE(aliased, 1U);
  // C3's other rows have no legacy spelling (their x-flag, or none, is their only route).
  for (const auto& [kind, id] : std::vector<std::pair<nr::Kind, std::string>>{
           {nr::Kind::TradeRate, "aim-partial-v5"}, {nr::Kind::TradeRate, "per-name-v1"},
           {nr::Kind::TradeRate, "two-speed-nav-v1"}, {nr::Kind::CostLaw, "flat-bps-v1"},
           {nr::Kind::CostLaw, "sqrt-impact-v1"}, {nr::Kind::CostLaw, "ko-invariance-v1"},
           {nr::Kind::CostLaw, "fim-live-v1"}}) {
    const auto* row = nr::find(kind, id);
    ASSERT_NE(row, nullptr) << id;
    EXPECT_TRUE(row->x_flag_aliases.empty()) << id;
  }
}

// The cost-law registry: the laws of every scenario the replay can run, by their recipe labels
// (flat-bps-v1 / sqrt-impact-v1 the fixed S1 and S2 / S3 laws, ko-invariance-v1 / fim-live-v1 the
// v2 stress laws, not nameable alone) and the one nameable row, cost-sqrt-v2 = --cost-law
// cost-sqrt-v2, a closed schema with no parameter (a wave names it with params {}).
TEST(CostLawRegistry, RowsAreTheReplaysLawsAndOneNameableSet) {
  const auto all = nr::cost_law_rows();
  ASSERT_GE(all.size(), 5U);
  const auto rows = all.first(5); // C3's rows lead the table (append-only, Ruling W3-9)
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
  EXPECT_EQ(nr::flag_ids(nr::Kind::CostLaw).rfind("cost-sqrt-v2", 0), 0U);
  EXPECT_EQ(nr::find(nr::Kind::CostLaw, "aim-partial-v5"), nullptr);
}

// --cost-law cost-sqrt-v2 publishes byte for byte the --cost-v2 run; giving both, a fixed law
// (always on, no flag), a stress law alone and an unknown id are usage errors before any output.
// Every cost_rule a recipe records is a cost-law row id (review C3 S-5): the v2 run's recipe
// names the four laws of its books.
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
  std::set<std::string> recorded;
  const auto recipe = read_json_file(dir.path / "law" / "recipe.json");
  for (const auto& entry : recipe.at("scenarios")) {
    const auto rule = entry.at("cost_rule").get<std::string>();
    EXPECT_NE(nr::find(nr::Kind::CostLaw, rule), nullptr) << rule;
    recorded.insert(rule);
  }
  EXPECT_EQ(recorded, (std::set<std::string>{"flat-bps-v1", "sqrt-impact-v1", "ko-invariance-v1",
                                             "fim-live-v1"}));
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
  ASSERT_GE(rows.size(), 3U); // C3's three lead the table (append-only, Ruling W3-9)
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
  EXPECT_EQ(rows[2].id, "two-speed-nav-v1");
  EXPECT_EQ(nr::flag_ids(nr::Kind::TradeRate).rfind("aim-partial-v5|per-name-v1", 0), 0U);
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

// The target slot (K-P9-7 kind target; no row in this build, lane COV-MV adds mv-aim-v1 and its
// handler): --target-rule is a registry flag of the v7 seam and refuses an unregistered id before
// any output, naming the registered ids ("none registered" while the slot is empty). A synthetic
// id, so the test holds before and after COV-MV's row (Ruling W3-9).
TEST(TargetRegistry, UnregisteredIdIsRefusedBeforeOutput) {
  constexpr const char* unregistered = "zz-no-such-target-v0";
  EXPECT_EQ(nr::find(nr::Kind::Target, unregistered), nullptr);
  EXPECT_EQ(nr::kind_name(nr::Kind::Target), "target");
  for (const auto& row : nr::target_rows()) {
    EXPECT_EQ(row.kind, nr::Kind::Target);
    EXPECT_EQ(nr::find(nr::Kind::Target, row.id), &row);
  }
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  std::vector<std::string> args{"nav"};
  for (const auto& t : joined(pinned(in, dir.path / "out"),
                              {"--rule", "aim-partial-v5", "--target-rule", unregistered}))
    args.push_back(t);
  std::ostringstream out, err;
  EXPECT_EQ(nav(args, out, err), 2);
  const auto ids = nr::flag_ids(nr::Kind::Target);
  EXPECT_NE(err.str().find(std::string("unknown --target-rule ") + unregistered + " (" +
                           (ids.empty() ? std::string("none registered") : ids) + ")"),
            std::string::npos)
      << err.str();
  EXPECT_FALSE(fs::exists(dir.path / "out"));
}

// Rulings PRE-7 / PRE-7a: --ann-mean-gross (NavSpec key ann_mean_gross) is print-only. Without
// it every output byte is the run's as before (summary.json has no such key anywhere); with it
// every file but summary.json is byte-identical (the recipe, the CSVs, and under the v7 seam the
// capacity directory and the v7 files), and summary.json differs by that key alone: each
// scenario's ann_mean_gross = 252 x the running mean of the daily gross_return over the rows
// ann_mean averages, recomputed here from the published CSV bit for bit (the same recomputation
// of net_return gives ann_mean bit for bit, so they are the same rows), and above ann_mean (the
// costs and financing net out of it). A NavSpec carrying the key publishes the opt-in run's
// bytes; a construction grid's variant carries the key as its standalone run does.
TEST(NavAnnMeanGross, PrintOnlyOptInAddsOneSummaryKey) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const std::vector<std::string> v5{"--rule", "aim-partial-v5", "--cadence", "1",
                                    "--trade-fraction", ".25", "--dust-multiple", ".1",
                                    "--aim-leverage", "1.2", "--exit-rate", ".05"};
  const std::vector<std::vector<std::string>> shapes{v6_shape, v5};
  const auto args_of = [&](const fs::path& output, const std::vector<std::string>& shape,
                           bool opt_in) {
    std::vector<std::string> args{"nav"};
    for (const auto& t : joined(pinned(in, output), shape)) args.push_back(t);
    if (opt_in) args.push_back("--ann-mean-gross");
    return args;
  };
  for (size_t s = 0; s < shapes.size(); ++s) {
    const auto absent = dir.path / ("absent" + std::to_string(s));
    const auto present = dir.path / ("present" + std::to_string(s));
    std::ostringstream out, err;
    ASSERT_EQ(nav(args_of(absent, shapes[s], false), out, err), 0) << err.str();
    ASSERT_EQ(nav(args_of(present, shapes[s], true), out, err), 0) << err.str();
    // Every file but summary.json, byte for byte.
    const auto names = tree(absent);
    ASSERT_EQ(names, tree(present));
    for (const auto& name : names)
      if (name != "summary.json")
        EXPECT_TRUE(bytes_of(absent / name) == bytes_of(present / name)) << s << ' ' << name;
    if (s == 0) {
      EXPECT_TRUE(fs::exists(present / "capacity" / "summary.json"));
      EXPECT_TRUE(fs::exists(present / "v7_extras.json"));
    }
    // Absent: no such key anywhere in the summary.
    const auto absent_bytes = bytes_of(absent / "summary.json");
    EXPECT_EQ(absent_bytes.find("ann_mean_gross"), std::string::npos);
    // Present: summary.json minus the key is the absent run's file, byte for byte.
    auto summary = Json::parse(bytes_of(present / "summary.json"));
    const auto scenarios = summary.at("scenarios");
    ASSERT_GE(scenarios.size(), 3U);
    for (auto& entry : summary.at("scenarios")) {
      ASSERT_TRUE(entry.contains("ann_mean_gross")) << entry.at("scenario");
      entry.erase("ann_mean_gross");
    }
    EXPECT_EQ(summary.dump(2) + '\n', absent_bytes) << s;
    // The value, from the published daily CSV of each scenario.
    for (const auto& entry : scenarios) {
      const auto label = entry.at("scenario").get<std::string>();
      const auto daily = present / ("daily_" + label + ".csv");
      const auto gross = return_row_mean(daily, "gross_return");
      const auto net = return_row_mean(daily, "net_return");
      EXPECT_EQ(gross.n, entry.at("observations").get<size_t>()) << label;
      const f64 printed = entry.at("ann_mean_gross").get<f64>();
      EXPECT_EQ(std::bit_cast<u64>(printed), std::bit_cast<u64>(gross.running * 252)) << label;
      EXPECT_EQ(std::bit_cast<u64>(entry.at("ann_mean").get<f64>()),
                std::bit_cast<u64>(net.running * 252))
          << label;
      EXPECT_NEAR(printed, gross.plain * 252, 1e-12) << label;
      EXPECT_GT(printed, entry.at("ann_mean").get<f64>()) << label; // before costs and financing
    }
    // The NavSpec route: --print-spec carries the key only when named; the spec run publishes the
    // opt-in run's bytes.
    std::ostringstream printed, plain;
    auto print = args_of(dir.path / ("spec" + std::to_string(s)), shapes[s], true);
    print.push_back("--print-spec");
    ASSERT_EQ(nav(print, printed, err), 0) << err.str();
    const auto doc = Json::parse(printed.str());
    EXPECT_EQ(doc.at("options").at("ann_mean_gross"), true);
    auto print_absent = args_of(absent, shapes[s], false);
    print_absent.push_back("--print-spec");
    ASSERT_EQ(nav(print_absent, plain, err), 0) << err.str();
    EXPECT_FALSE(Json::parse(plain.str()).at("options").contains("ann_mean_gross"));
    const auto file = dir.path / ("spec" + std::to_string(s) + ".json");
    write_json_file(file, doc);
    ASSERT_EQ(nav({"nav", "--spec", file.string()}, out, err), 0) << err.str();
    EXPECT_GE(expect_same_tree(dir.path / ("spec" + std::to_string(s)), present), 4U);
  }
  // A grid variant with no override is its standalone run, the opt-in's key included.
  Json grid_doc{{"schema", "atx.nav-construction-grid/v1"},
                {"variants", Json::array({Json{{"id", "v"}, {"flags", Json::object()}}})}};
  const auto grid = dir.path / "grid.json";
  write_json_file(grid, grid_doc);
  auto grid_args = args_of(dir.path / "grid", v5, true);
  grid_args.push_back("--construction-grid");
  grid_args.push_back(grid.string());
  std::ostringstream out, err;
  ASSERT_EQ(nav(grid_args, out, err), 0) << err.str();
  EXPECT_GE(expect_same_tree(dir.path / "grid" / "v", dir.path / "present1"), 4U);
  // Repeated, it is a usage error like any switch.
  auto twice = args_of(dir.path / "twice", v5, true);
  twice.push_back("--ann-mean-gross");
  EXPECT_EQ(nav(twice, out, err), 2);
  EXPECT_FALSE(fs::exists(dir.path / "twice"));
}
