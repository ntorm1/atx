// P9 lane AL-CLOCK: the trade-rate row info-clock-rate-v1 (Rulings W3-9, W3-16, ALCLOCK-2,
// ALCLOCK-3) and the print-only --info-clock-print (Ruling ALCLOCK-1).
//
// Suites: InfoClockRate (the kernel at its window boundary, the refusals, the closed-form step,
// causality, the capacity curve, book workers, the registry row, the reserve) and InfoClockPrint
// (flag absent = the bytes of before; the opt-in adds one summary block per scenario on any book;
// the rule alone prints no block; --spec equals argv). Synthetic roles only (the spo fixture).

#include <algorithm>
#include <bit>
#include <cmath>
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
#include "atx/core/sha256.hpp"
#include "atx/engine/book/two_speed.hpp"
#include "../src/book/nav_rules.hpp"
#include "../src/book/nav_spec.hpp"
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
namespace bk = atx::engine::book;
using Json = nlohmann::json;
namespace fs = std::filesystem;

const std::string clock_id = st::nav_info_clock_rate_id;
const std::string clock_field = st::nav_info_clock_field;

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
// Every file of `a` but summary.json equals `b`'s (the trees hold the same names).
void expect_same_but_summary(const fs::path& a, const fs::path& b) {
  const auto names = tree(a);
  ASSERT_EQ(names, tree(b));
  for (const auto& name : names)
    if (name != "summary.json")
      EXPECT_TRUE(bytes_of(a / name) == bytes_of(b / name)) << name;
}
std::vector<std::string> joined(std::vector<std::string> a, const std::vector<std::string>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
std::vector<std::string> cells_of(const std::string& line) {
  std::vector<std::string> cells;
  std::istringstream in(line);
  for (std::string cell; std::getline(in, cell, ',');) cells.push_back(cell);
  return cells;
}
// A CSV file: the header and the rows, split at commas (the replay's files quote nothing).
struct Csv {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;
  [[nodiscard]] size_t col(const std::string& name) const {
    const auto at = std::find(header.begin(), header.end(), name);
    EXPECT_NE(at, header.end()) << name;
    return static_cast<size_t>(at - header.begin());
  }
};
Csv read_csv(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  Csv csv;
  std::string line;
  if (std::getline(in, line)) csv.header = cells_of(line);
  while (std::getline(in, line)) csv.rows.push_back(cells_of(line));
  return csv;
}
// A printed number (17 digits read back to its bits; any spelling of NaN is NaN).
f64 number_of(const std::string& cell) {
  if (cell.find("nan") != std::string::npos || cell.find("NaN") != std::string::npos)
    return missing;
  return std::stod(cell);
}
// The file's text without one column (every line), for files that differ in that column only.
std::string without_column(const fs::path& path, const std::string& column) {
  const auto csv = read_csv(path);
  const size_t drop = csv.col(column);
  std::string out;
  const auto line = [&](const std::vector<std::string>& cells) {
    for (size_t k = 0; k < cells.size(); ++k) {
      if (k == drop) continue;
      out += cells[k];
      out += ',';
    }
    out += '\n';
  };
  line(csv.header);
  for (const auto& row : csv.rows) line(row);
  return out;
}
std::string replaced(std::string text, const std::string& from, const std::string& to) {
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}

// The fixture's information clock (ea_days_since, sessions since the reaction session): name 0
// never has a fresh announcement (NaN), name 1 always reacted today (0, fast), every other name
// cycles 0..44 sessions (fast in [0, 21]) with a NaN every 11th row.
std::vector<f64> fixture_clock(const Role& r) {
  std::vector<f64> clock(r.d * r.n);
  for (usize t = 0; t < r.d; ++t)
    for (usize i = 0; i < r.n; ++i) {
      f64& c = clock[t * r.n + i];
      if (i == 0 || (i > 1 && (t + i) % 11 == 0)) c = missing;
      else if (i == 1) c = 0.0;
      else c = static_cast<f64>((t + 7 * i) % 45);
    }
  return clock;
}
bool fast(f64 days) { return st::nav_info_clock_fast(days); }

// The NAV's atx.research-role-fields/v1 manifest (strategy_nav_replay_test.cpp write_fields):
// shares_out 1e8 and si_shares 1e6 in every cell and, when `clock` is not empty, the field
// ea_days_since (point in time unless `clock_pit` is false); pinned to the role manifest's own
// receipts. Returns {manifest path, its SHA-256, the clock payload's SHA-256 or empty}.
struct FieldsPin {
  std::string path, sha256, clock_sha256;
};
FieldsPin write_clock_fields(const fs::path& dir, const RunInputs& in, const Role& r,
                             const std::vector<f64>& clock, bool clock_pit = true) {
  if (!fs::create_directory(dir)) throw std::runtime_error("fields fixture directory");
  const auto role = read_json_file(in.role);
  const auto& receipts = role.at("files");
  const std::vector<f64> shares(r.d * r.n, 1e8), short_shares(r.d * r.n, 1e6);
  std::vector<std::pair<std::string, const std::vector<f64>*>> payloads{
      {"shares_out", &shares}, {"si_shares", &short_shares}};
  if (!clock.empty()) payloads.emplace_back(clock_field, &clock);
  Json files = Json::object(), entries = Json::array();
  for (const auto& [name, values] : payloads) {
    const std::string file = name + ".f64";
    files[file] = write_payload(dir / file, *values);
    entries.push_back({{"name", name}, {"file", file}, {"dtype", "<f8"},
                       {"layout", "date-major"}, {"shape", Json::array({r.d, r.n})},
                       {"sha256", files[file].at("sha256")},
                       {"point_in_time", name != clock_field || clock_pit},
                       {"non_pit_aspects", Json::array()}, {"clock", "fixture-" + name}});
  }
  const Json manifest{{"schema", "atx.research-role-fields/v1"}, {"status", "complete"},
      {"role", {{"path", "fixture"}, {"manifest_sha256", in.role_sha256},
                {"sessions_sha256", receipts.at("sessions.i64").at("sha256")},
                {"ids_sha256", receipts.at("ids.u64").at("sha256")},
                {"member_sha256", receipts.at("member.u8").at("sha256")},
                {"dates", r.d}, {"instruments", r.n}}},
      {"visibility_mark", "fixture: session-date 22:00 UTC"},
      {"fields", entries}, {"files", files}};
  FieldsPin pin;
  pin.path = (dir / "manifest.json").string();
  pin.sha256 = write_json_file(pin.path, manifest);
  if (!clock.empty())
    pin.clock_sha256 = files.at(clock_field + ".f64").at("sha256").get<std::string>();
  return pin;
}

// One synthetic world: the spo fixture's role (40 sessions, 12 names, name 11 a nonmember for
// rows 13..19), its pinned blend and role, the fixture clock in a fields manifest, and the same
// fields without the clock.
struct World {
  Directory dir;
  Role role{40, 12, 53};
  RunInputs in;
  std::vector<f64> clock;
  FieldsPin fields, plain;
  World()
      : in(write_run_inputs(dir.path, role)), clock(fixture_clock(role)),
        fields(write_clock_fields(dir.path / "fields", in, role, clock)),
        plain(write_clock_fields(dir.path / "plain", in, role, {})) {}
  [[nodiscard]] fs::path out(const std::string& name) const { return dir.path / name; }
  // `nav` with the pinned inputs, --output, the fields `f` (none when null) and `flags`.
  [[nodiscard]] std::vector<std::string> args(const fs::path& output, const FieldsPin* f,
                                              const std::vector<std::string>& flags) const {
    std::vector<std::string> a{"nav", "--combined", in.combined, "--combined-sha256",
                               in.combined_sha256, "--role", in.role, "--role-sha256",
                               in.role_sha256, "--output", output.string()};
    if (f) a = joined(a, {"--fields", f->path, "--fields-sha256", f->sha256});
    return joined(a, flags);
  }
};
// aim-partial-v5 at the rule's slow rate .05, and the rule on it.
const std::vector<std::string> v5_slow{"--rule", "aim-partial-v5", "--cadence", "1",
                                       "--trade-fraction", ".05", "--dust-multiple", ".1",
                                       "--aim-leverage", "1.2", "--exit-rate", ".05"};
const std::vector<std::string> rule_on = joined(v5_slow, {"--rate", "info-clock-rate-v1"});
// A book that does not run the rule (the reference shape) and the v7 seam's W1-MIN M-1 shape.
const std::vector<std::string> v5_fixed{"--rule", "aim-partial-v5", "--cadence", "1",
                                        "--trade-fraction", ".25", "--dust-multiple", ".1",
                                        "--aim-leverage", "1.2", "--exit-rate", ".05"};
const std::vector<std::string> v6_shape{
    "--rule", "aim-partial-v6", "--cadence", "1", "--trade-fraction", ".25", "--dust-multiple",
    ".1", "--aim-leverage", "1.5", "--exit-rate", ".05", "--cost-v2", "--capacity-curve"};

// The two reference shapes the print is shown on, by name.
std::vector<std::pair<std::string, std::vector<std::string>>> named_shapes() {
  return {{"v5", v5_fixed}, {"v6", v6_shape}};
}
// The summary minus every scenario's info_clock block (each present), as the file's text.
std::string summary_without_blocks(const fs::path& dir) {
  auto summary = Json::parse(bytes_of(dir / "summary.json"));
  for (auto& entry : summary.at("scenarios")) {
    EXPECT_TRUE(entry.contains("info_clock")) << entry.at("scenario");
    entry.erase("info_clock");
  }
  return summary.dump(2) + '\n';
}
} // namespace

// The kernel (strategy_nav_replay.hpp): the window [0, 21] closed at both ends (nextafter(21) and
// 22 are slow, -0 is fast), NaN slow, the fast rate two-speed-v1's 1 - 2^(-C/5) at any cadence
// (Ruling W3-16; .12945 is its cadence-1 value), a nonmember at the slow rate; every refused
// input writes no rate.
TEST(InfoClockRate, KernelAtTheWindowBoundary) {
  EXPECT_EQ(clock_id, "info-clock-rate-v1");
  EXPECT_LE(clock_id.size(), 18U); // W1-I1: an id an audit-exact cell can carry
  EXPECT_EQ(clock_field, "ea_days_since");
  EXPECT_EQ(st::nav_info_clock_window, 21.0);
  for (const usize c : {usize{1}, usize{2}, usize{5}, usize{21}})
    EXPECT_EQ(bits(st::nav_info_clock_fast_theta(c)),
              bits(bk::two_speed_fast_theta(static_cast<f64>(c))))
        << c;
  EXPECT_NEAR(st::nav_info_clock_fast_theta(1), .12945, 5e-6);
  EXPECT_EQ(st::nav_info_clock_fast_theta(5), 0.5);
  const f64 above = std::nextafter(21.0, inf);
  for (const f64 v : {0.0, -0.0, 1.0, 20.5, 21.0}) EXPECT_TRUE(fast(v)) << v;
  for (const f64 v : {above, 22.0, 1e9, missing, -1.0, inf}) EXPECT_FALSE(fast(v)) << v;
  const std::vector<u8> member{1, 1, 1, 1, 1, 1, 0, 1};
  const std::vector<f64> clock{0.0, 21.0, above, 22.0, missing, 3.5, 0.0, 1e9};
  for (const usize cadence : {usize{1}, usize{5}, usize{21}}) {
    const f64 f = st::nav_info_clock_fast_theta(cadence), s = bk::two_speed_slow_theta;
    std::vector<f64> rates(member.size(), -1.0);
    ASSERT_TRUE(st::nav_info_clock_rates(member, clock, cadence, s, rates));
    const std::vector<f64> expected{f, f, s, s, s, f, s, s};
    for (size_t k = 0; k < rates.size(); ++k)
      EXPECT_EQ(bits(rates[k]), bits(expected[k])) << cadence << ' ' << k;
  }
  // Refused: a span of another length, cadence 0, a slow rate outside (0, 1], a member flag that
  // is not 0/1, a clock outside its domain (negative, infinite). Nothing is written.
  const auto refused = [](const std::vector<u8>& m, const std::vector<f64>& c, usize cadence,
                          f64 slow, size_t size) {
    std::vector<f64> rates(size, -7.0);
    const bool ok = static_cast<bool>(st::nav_info_clock_rates(m, c, cadence, slow, rates));
    return !ok && std::all_of(rates.begin(), rates.end(), [](f64 v) { return v == -7.0; });
  };
  EXPECT_TRUE(refused(member, clock, 1, .05, member.size() - 1));
  EXPECT_TRUE(refused(member, {clock.begin(), clock.end() - 1}, 1, .05, member.size()));
  EXPECT_TRUE(refused(member, clock, 0, .05, member.size()));
  for (const f64 slow : {0.0, -0.05, 1.5, missing, inf})
    EXPECT_TRUE(refused(member, clock, 1, slow, member.size())) << slow;
  auto two = member;
  two[3] = 2;
  EXPECT_TRUE(refused(two, clock, 1, .05, member.size()));
  for (const f64 bad : {-1.0, -1e-300, inf, -inf}) {
    auto c = clock;
    c[6] = bad; // a nonmember's clock is checked too (the field's domain)
    EXPECT_TRUE(refused(member, c, 1, .05, member.size())) << bad;
  }
  EXPECT_FALSE(refused(member, clock, 1, 1.0, member.size())); // theta_s = 1 is in range
}

// validate_nav_config / validate_nav_input: the rule needs aim-partial-v5 at trade_fraction .05,
// no rate parameter, no two-speed, and the clock at the input's geometry with every cell NaN or
// finite >= 0; any of them missing refuses the replay. A NaN cell is valid (slow). The census
// (NavInfoClockStats) counts a main book's member-decisions of its scored effective rebalances
// whenever a clock is given, at any rate, and nothing without one.
TEST(InfoClockRate, ConfigAndInputRefusals) {
  const Role r(40, 12, 53);
  const auto clock = fixture_clock(r);
  auto cfg = nav_config();
  cfg.target.trade_fraction = bk::two_speed_slow_theta;
  cfg.rate = st::NavRateRule::InfoClockV1;
  st::NavReplayInput in = r.nav();
  in.info_clock = clock;
  const auto ok = st::replay_nav(in, cfg);
  ASSERT_TRUE(ok) << ok.error().to_string();
  // The census against the role: members of every effective rebalance row.
  st::NavInfoClockStats expected;
  for (const auto& day : ok->days) {
    if (!day.decision || !day.rebalance) continue;
    for (usize i = 0; i < r.n; ++i) {
      const usize k = day.session_index * r.n + i;
      if (!r.member[k]) continue;
      ++expected.name_days;
      expected.fast_name_days += fast(clock[k]) ? 1U : 0U;
      expected.missing_name_days += std::isnan(clock[k]) ? 1U : 0U;
    }
  }
  const auto& got = ok->construction.info_clock;
  EXPECT_GT(expected.fast_name_days, 0U);
  EXPECT_GT(expected.missing_name_days, 0U);
  EXPECT_GT(expected.name_days, expected.fast_name_days + expected.missing_name_days);
  EXPECT_EQ(got.name_days, expected.name_days);
  EXPECT_EQ(got.fast_name_days, expected.fast_name_days);
  EXPECT_EQ(got.missing_name_days, expected.missing_name_days);
  EXPECT_GT(got.planned_turnover, got.fast_planned_turnover);
  EXPECT_GT(got.fast_planned_turnover, 0.0);
  // The fixed rate with the clock counts the same name-days; without the clock nothing.
  auto fixed = cfg;
  fixed.rate = st::NavRateRule::Fixed;
  const auto printed = st::replay_nav(in, fixed);
  ASSERT_TRUE(printed) << printed.error().to_string();
  EXPECT_EQ(printed->construction.info_clock.name_days, expected.name_days);
  EXPECT_EQ(printed->construction.info_clock.fast_name_days, expected.fast_name_days);
  const auto bare = st::replay_nav(r.nav(), fixed);
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->construction.info_clock.name_days, 0U);
  EXPECT_EQ(bits(bare->construction.info_clock.planned_turnover), 0U);
  // An all-NaN clock is valid: every member-decision missing and slow.
  st::NavReplayInput nan_in = r.nav();
  const std::vector<f64> nan_clock(r.d * r.n, missing);
  nan_in.info_clock = nan_clock;
  const auto all_nan = st::replay_nav(nan_in, cfg);
  ASSERT_TRUE(all_nan) << all_nan.error().to_string();
  EXPECT_EQ(all_nan->construction.info_clock.missing_name_days, expected.name_days);
  EXPECT_EQ(all_nan->construction.info_clock.fast_name_days, 0U);
  // Refusals, each naming its cause.
  const auto refuses = [](const st::NavReplayInput& x, const st::NavReplayConfig& c,
                          const std::string& needle) {
    const auto res = st::replay_nav(x, c);
    if (res) return false;
    const auto text = res.error().to_string();
    EXPECT_NE(text.find(needle), std::string::npos) << text;
    return text.find(needle) != std::string::npos;
  };
  EXPECT_TRUE(refuses(r.nav(), cfg, "needs the info clock"));
  const std::vector<f64> short_clock(clock.begin(), clock.end() - 1);
  st::NavReplayInput short_in = r.nav();
  short_in.info_clock = short_clock;
  EXPECT_TRUE(refuses(short_in, cfg, "info clock (ea_days_since) geometry"));
  short_in.info_clock = short_clock;
  EXPECT_TRUE(refuses(short_in, fixed, "info clock (ea_days_since) geometry")); // the print too
  for (const f64 bad : {-1.0, inf, -inf}) {
    auto c = clock;
    c[5 * r.n + 3] = bad;
    st::NavReplayInput x = r.nav();
    x.info_clock = c;
    EXPECT_TRUE(refuses(x, cfg, "ea_days_since outside its domain (NaN or finite >= 0) at row 5, "
                                "name 3"))
        << bad;
  }
  const std::string config_needle =
      "rate info-clock-rate-v1 needs aim-partial-v5 at trade_fraction";
  auto other = cfg;
  other.target.trade_fraction = .25;
  EXPECT_TRUE(refuses(in, other, config_needle));
  other = cfg;
  other.target.rule = st::TargetReplayRule::BaselineTargetV1;
  EXPECT_TRUE(refuses(in, other, config_needle));
  for (const auto set : {+[](st::NavReplayConfig& c) { c.rate_rra = 11; },
                         +[](st::NavReplayConfig& c) { c.rate_lambda = .3; },
                         +[](st::NavReplayConfig& c) { c.rate_min = .02; },
                         +[](st::NavReplayConfig& c) { c.rate_max = .2; }}) {
    other = cfg;
    set(other);
    EXPECT_TRUE(refuses(in, other, config_needle));
  }
  other = cfg;
  other.target.two_speed = true;
  EXPECT_TRUE(refuses(in, other, "two-speed-v1 needs the fixed trading rate"));
}

// The rule's step, closed form (--emit-holdings csv, dust 0, the immediate exit): on every
// effective rebalance a member's rule_weight = held + theta_i (L desired - held), theta_i =
// 1 - 2^(-C/5) when its clock is in [0, 21], else .05, at cadence 1 and 5. The total of what it
// trades is not fixed: only the participation changes. A clock with no fresh announcement (all
// NaN) is the fixed-rate run at .05 byte for byte, but applied_fraction (the members' mean rate,
// the same value to rounding).
TEST(InfoClockRate, StepChangeClosedForm) {
  const World w;
  std::map<i64, usize> row_of;
  for (usize t = 0; t < w.role.d; ++t) row_of[w.role.sessions[t]] = t;
  for (const char* cadence : {"1", "5"}) {
    const auto out = w.out(std::string("step") + cadence);
    const auto holdings = w.out(std::string("holdings") + cadence);
    const std::vector<std::string> flags{
        "--rule", "aim-partial-v5", "--cadence", cadence, "--trade-fraction", ".05",
        "--dust-multiple", "0", "--aim-leverage", "1.2", "--rate", "info-clock-rate-v1",
        "--emit-holdings", holdings.string(), "--holdings-format", "csv"};
    std::ostringstream log, err;
    ASSERT_EQ(nav(w.args(out, &w.fields, flags), log, err), 0) << err.str();
    const f64 theta_f = st::nav_info_clock_fast_theta(static_cast<usize>(std::stoul(cadence)));
    const auto csv = read_csv(holdings / "holdings.csv");
    const size_t c_session = csv.col("session_ns"), c_id = csv.col("instrument_id"),
                 c_member = csv.col("member"), c_held = csv.col("held_weight"),
                 c_desired = csv.col("desired_weight"), c_rule = csv.col("rule_weight");
    usize fast_rows = 0, slow_rows = 0;
    for (const auto& row : csv.rows) {
      const f64 desired = number_of(row.at(c_desired));
      if (row.at(c_member) != "1" || std::isnan(desired)) continue; // a member's rebalance
      const usize t = row_of.at(std::stoll(row.at(c_session)));
      const usize i = static_cast<usize>(std::stoull(row.at(c_id)) - 100);
      const bool is_fast = fast(w.clock[t * w.role.n + i]);
      const f64 theta = is_fast ? theta_f : bk::two_speed_slow_theta;
      const f64 held = number_of(row.at(c_held));
      EXPECT_DOUBLE_EQ(number_of(row.at(c_rule)), held + theta * (1.2 * desired - held))
          << cadence << " t " << t << " name " << i;
      (is_fast ? fast_rows : slow_rows) += 1;
    }
    EXPECT_GT(fast_rows, 10U) << cadence;
    EXPECT_GT(slow_rows, 10U) << cadence;
    const auto recipe = Json::parse(bytes_of(out / "recipe.json"));
    EXPECT_EQ(recipe.at("rate"), clock_id);
    EXPECT_EQ(bits(recipe.at("rate_theta_fast").get<f64>()), bits(theta_f));
  }
  // No fresh announcement anywhere: the fixed-rate run at .05, except applied_fraction.
  const std::vector<f64> nan_clock(w.role.d * w.role.n, missing);
  const auto none = write_clock_fields(w.out("nan-fields"), w.in, w.role, nan_clock);
  std::ostringstream log, err;
  ASSERT_EQ(nav(w.args(w.out("all-nan"), &none, rule_on), log, err), 0) << err.str();
  ASSERT_EQ(nav(w.args(w.out("fixed"), &none, v5_slow), log, err), 0) << err.str();
  size_t compared = 0;
  for (const auto& name : tree(w.out("fixed"))) {
    if (name.rfind("events_", 0) == 0) {
      EXPECT_TRUE(bytes_of(w.out("all-nan") / name) == bytes_of(w.out("fixed") / name)) << name;
      ++compared;
    }
    if (name.rfind("daily_", 0) != 0) continue;
    EXPECT_EQ(without_column(w.out("all-nan") / name, "applied_fraction"),
              without_column(w.out("fixed") / name, "applied_fraction"))
        << name;
    const auto daily = read_csv(w.out("all-nan") / name);
    const size_t c_rebalance = daily.col("rebalance"), c_applied = daily.col("applied_fraction");
    for (const auto& row : daily.rows)
      if (row.at(c_rebalance) == "1")
        EXPECT_NEAR(number_of(row.at(c_applied)), .05, 1e-15) << name;
    ++compared;
  }
  EXPECT_GE(compared, 6U);
}

// Causal: decision d reads row d of the clock only. A clock changed on every row after d0 leaves
// every daily row up to d0 (its decision, its fills, its NAV) byte for byte, and changes a later
// one.
TEST(InfoClockRate, EventClockIsCausal) {
  const World w;
  constexpr usize d0 = 20;
  auto later = w.clock;
  for (usize t = d0 + 1; t < w.role.d; ++t)
    for (usize i = 0; i < w.role.n; ++i) {
      f64& c = later[t * w.role.n + i];
      c = fast(c) ? 40.0 : 0.0; // every later name-day flips speed
    }
  const auto changed = write_clock_fields(w.out("later-fields"), w.in, w.role, later);
  std::ostringstream log, err;
  ASSERT_EQ(nav(w.args(w.out("a"), &w.fields, rule_on), log, err), 0) << err.str();
  ASSERT_EQ(nav(w.args(w.out("b"), &changed, rule_on), log, err), 0) << err.str();
  size_t files = 0;
  for (const auto& name : tree(w.out("a"))) {
    if (name.rfind("daily_", 0) != 0) continue;
    const auto a = read_csv(w.out("a") / name), b = read_csv(w.out("b") / name);
    ASSERT_EQ(a.rows.size(), b.rows.size()) << name;
    const size_t c_index = a.col("session_index");
    bool later_differs = false;
    for (size_t k = 0; k < a.rows.size(); ++k) {
      if (std::stoull(a.rows[k].at(c_index)) <= d0) EXPECT_EQ(a.rows[k], b.rows[k]) << name << k;
      else later_differs = later_differs || a.rows[k] != b.rows[k];
    }
    EXPECT_TRUE(later_differs) << name;
    ++files;
  }
  EXPECT_GE(files, 3U);
}

// Command-line refusals, before any output (usage 2, run 1): --rate is aim-partial-v5's; its
// parameters are per-name-v1's; the rule needs trade_fraction .05, the fields and the field in
// them, point in time, every cell in the field's domain; not with two-speed-v1, aim-partial-v6 or
// an spo rule (the fixed rate); a grid variant keeps the base cadence. --info-clock-print needs
// the fields and the field, once.
TEST(InfoClockRate, CommandLineRefusalsWriteNothing) {
  const World w;
  auto bad_clock = w.clock;
  bad_clock[7 * w.role.n + 4] = -1.0;
  const auto negative = write_clock_fields(w.out("neg-fields"), w.in, w.role, bad_clock);
  const auto not_pit = write_clock_fields(w.out("pit-fields"), w.in, w.role, w.clock, false);
  const auto grid = w.out("grid.json");
  write_json_file(grid, Json{{"schema", "atx.nav-construction-grid/v1"},
                             {"variants", Json::array({Json{{"id", "a"}, {"flags", Json::object()}},
                                                       Json{{"id", "b"},
                                                            {"flags", {{"--cadence", "2"}}}}})}});
  struct Case {
    const FieldsPin* fields;
    std::vector<std::string> flags;
    int code;
    std::string needle;
  };
  const std::vector<std::string> baseline{"--rule", "baseline-v1", "--rate", "info-clock-rate-v1"};
  const std::vector<Case> cases{
      {&w.fields, baseline, 2, "--rate is aim-partial-v5 only"},
      {&w.fields,
       {"--rule", "aim-partial-v5", "--trade-fraction", ".25", "--rate", "info-clock-rate-v1"}, 1,
       "needs aim-partial-v5 at trade_fraction .05"},
      {&w.fields, joined(rule_on, {"--rate-rra", "10"}), 2, "--rate-rra needs --rate per-name-v1"},
      {nullptr, rule_on, 1, "need --fields"},
      {&w.plain, rule_on, 1, "fields lack ea_days_since"},
      {&not_pit, rule_on, 1, "not declared point in time: ea_days_since"},
      {&negative, rule_on, 1, "ea_days_since outside its domain (NaN or finite >= 0) at row 7"},
      // The v7 seam (claimed by --cost-v2) refuses it at parse; the replay's own refusal of the
      // pair is ConfigAndInputRefusals' (on this fixture its saved-sleeves check comes first).
      {&w.fields, joined(rule_on, {"--two-speed", "two-speed-v1", "--cost-v2"}), 2,
       "--two-speed two-speed-v1 needs aim-partial-v5 at the fixed rate"},
      {&w.fields,
       {"--rule", "aim-partial-v6", "--trade-fraction", ".05", "--rate", "info-clock-rate-v1"}, 2,
       "aim-partial-v6 and --capacity-curve need the fixed rate"},
      {&w.fields, {"--rule", "spo-v1", "--rate", "info-clock-rate-v1"}, 2, "fixed rate"},
      {&w.fields, joined(rule_on, {"--construction-grid", grid.string()}), 1,
       "with --rate info-clock-rate-v1 every variant has the base cadence"},
      {nullptr, joined(v5_fixed, {"--info-clock-print"}), 1, "need --fields"},
      {&w.plain, joined(v5_fixed, {"--info-clock-print"}), 1, "fields lack ea_days_since"},
      {&w.fields, joined(v5_fixed, {"--info-clock-print", "--info-clock-print"}), 2,
       "duplicate/missing flag"},
  };
  for (size_t k = 0; k < cases.size(); ++k) {
    const auto& c = cases[k];
    const auto out = w.out("refused" + std::to_string(k));
    std::ostringstream log, err;
    EXPECT_EQ(nav(w.args(out, c.fields, c.flags), log, err), c.code) << k << ' ' << err.str();
    EXPECT_NE(err.str().find(c.needle), std::string::npos) << k << ' ' << err.str();
    EXPECT_FALSE(fs::exists(out)) << k;
  }
  // The same grid without the rule runs (the cadence check is the rule's).
  std::ostringstream log, err;
  EXPECT_EQ(nav(w.args(w.out("grid-fixed"), &w.fields,
                       joined(v5_slow, {"--construction-grid", grid.string()})),
                log, err),
            0)
      << err.str();
}

// Ruling ALCLOCK-3: --capacity-curve accepts --rate info-clock-rate-v1 (the rate reads no NAV:
// each capacity book moves at the members' clock rates, as the main books do) and no other
// non-fixed rate: per-name-v1 is refused as before; aim-partial-v6 refuses every non-fixed rate,
// this one included.
TEST(InfoClockRate, CapacityCurveAcceptsThisRateOnly) {
  const auto parses = [](const std::vector<std::string>& flags) {
    std::vector<std::string> args{"nav", "--combined", "c.json", "--combined-sha256",
                                  std::string(64, 'a'), "--role", "r.json", "--role-sha256",
                                  std::string(64, 'a'), "--output", "out"};
    args = joined(args, flags);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    EXPECT_TRUE(v7::claims_nav_args(static_cast<int>(argv.size()), argv.data()));
    const auto parsed = v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
    if (!parsed)
      EXPECT_NE(parsed.error().to_string().find("need the fixed rate"), std::string::npos)
          << parsed.error().to_string();
    return static_cast<bool>(parsed);
  };
  const std::vector<std::string> capacity{"--rule", "aim-partial-v5", "--trade-fraction", ".05",
                                          "--capacity-curve"};
  const std::vector<std::string> v6{"--rule", "aim-partial-v6", "--trade-fraction", ".05"};
  EXPECT_TRUE(parses(capacity));
  EXPECT_TRUE(parses(joined(capacity, {"--rate", "fixed"})));
  EXPECT_TRUE(parses(joined(capacity, {"--rate", "info-clock-rate-v1"})));
  EXPECT_FALSE(parses(joined(capacity, {"--rate", "per-name-v1"})));
  EXPECT_TRUE(parses(joined(v6, {"--rate", "fixed"})));
  EXPECT_FALSE(parses(joined(v6, {"--rate", "info-clock-rate-v1"})));
  EXPECT_FALSE(parses(joined(v6, {"--rate", "per-name-v1"})));
  EXPECT_FALSE(parses(joined(joined(v6, {"--capacity-curve"}), {"--rate", "info-clock-rate-v1"})));
  // End to end: the rule with the capacity curve publishes both directories; per-name-v1 with it
  // is refused before any output.
  const World w;
  std::ostringstream log, err;
  ASSERT_EQ(nav(w.args(w.out("cap"), &w.fields, joined(rule_on, {"--capacity-curve"})), log, err),
            0)
      << err.str();
  EXPECT_TRUE(fs::exists(w.out("cap") / "capacity" / "summary.json"));
  EXPECT_EQ(Json::parse(bytes_of(w.out("cap") / "capacity" / "recipe.json")).at("rate"), clock_id);
  EXPECT_EQ(nav(w.args(w.out("cap-pn"), &w.fields,
                       joined(v5_slow, {"--rate", "per-name-v1", "--capacity-curve"})),
                log, err),
            2);
  EXPECT_FALSE(fs::exists(w.out("cap-pn")));
}

// W1-MIN M-1's pattern with the rule on: the rate vector is formed once per decision on the
// calling thread, so the cost-v2 books and the capacity books on a pool of 4 publish every byte
// of the one-worker run (the capacity directory, the v7 files and the info_clock blocks included).
TEST(InfoClockRate, BookWorkersOneAndFourSameBytes) {
  const World w;
  const auto flags = joined(rule_on, {"--cost-v2", "--capacity-curve", "--info-clock-print"});
  std::ostringstream log, err;
  ASSERT_EQ(nav(w.args(w.out("one"), &w.fields, flags), log, err), 0) << err.str();
  ASSERT_EQ(nav(w.args(w.out("four"), &w.fields, joined(flags, {"--book-workers", "4"})), log,
                err),
            0)
      << err.str();
  EXPECT_GE(expect_same_tree(w.out("four"), w.out("one")), 8U);
  EXPECT_TRUE(fs::exists(w.out("one") / "capacity" / "summary.json"));
  EXPECT_TRUE(fs::exists(w.out("one") / "v7_extras.json"));
  const auto summary = Json::parse(bytes_of(w.out("one") / "summary.json"));
  ASSERT_GE(summary.at("scenarios").size(), 5U); // S1..S3 + the two v2 stress books
  for (const auto& entry : summary.at("scenarios"))
    EXPECT_EQ(entry.at("info_clock").at("rate"), clock_id) << entry.at("scenario");
  EXPECT_EQ(bytes_of(w.out("one") / "capacity" / "summary.json").find("info_clock"),
            std::string::npos);
}

// K-P9-7 (append-only, Ruling W3-9): the row follows C3's three trade-rate rows, x-flag
// "--rate info-clock-rate-v1", no parameter (every constant is registered), incompatible with
// per-name-v1 and the rules that need the fixed rate; --list-rules prints it; its derived flag is
// a typed NavSpec option; the --rate parser takes the id.
TEST(InfoClockRate, RegistryRowAndParser) {
  const auto rows = nr::trade_rate_rows();
  ASSERT_GE(rows.size(), 4U);
  EXPECT_EQ(rows[0].id, "aim-partial-v5");
  EXPECT_EQ(rows[1].id, "per-name-v1");
  EXPECT_EQ(rows[2].id, "two-speed-nav-v1");
  const auto* row = nr::find(nr::Kind::TradeRate, clock_id);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row, &rows[3]);
  EXPECT_EQ(row->x_flag, "--rate info-clock-rate-v1");
  EXPECT_TRUE(row->params().empty());
  EXPECT_TRUE(row->required.empty());
  const std::vector<std::string> incompatible(row->incompatible.begin(), row->incompatible.end());
  EXPECT_EQ(incompatible, (std::vector<std::string>{"per-name-v1", "two-speed-v1",
                                                    "aim-partial-v6", "spo-v1", "spo-v2",
                                                    "spo-v3"}));
  EXPECT_NE(row->description.find("1 - 2^(-C/5)"), std::string::npos);
  EXPECT_NE(row->description.find("[0, 21]"), std::string::npos);
  EXPECT_NE(nr::flag_ids(nr::Kind::TradeRate).find(clock_id), std::string::npos);
  std::ostringstream out, err;
  ASSERT_EQ(nav({"nav", "--list-rules", "--json"}, out, err), 0) << err.str();
  const auto table = Json::parse(out.str());
  const auto listed = std::find_if(table.at("rules").begin(), table.at("rules").end(),
                                   [](const Json& r) { return r.at("id") == clock_id; });
  ASSERT_NE(listed, table.at("rules").end());
  EXPECT_EQ(listed->at("kind"), "trade-rate");
  EXPECT_EQ(listed->at("x-flag"), "--rate info-clock-rate-v1");
  EXPECT_TRUE(listed->at("params_schema").at("properties").empty());
  EXPECT_FALSE(listed->contains("x-flag-aliases"));
  const std::vector<std::string> line{"--rate", "info-clock-rate-v1", "--info-clock-print"};
  const auto spec = ns::from_args(line);
  ASSERT_TRUE(spec) << spec.error().to_string();
  EXPECT_EQ(spec->at("options").at("rate"), clock_id);
  EXPECT_EQ(spec->at("options").at("info_clock_print"), true);
  const auto* key = ns::by_key("info_clock_print");
  ASSERT_NE(key, nullptr);
  EXPECT_EQ(std::string(key->flag), "--info-clock-print");
  EXPECT_EQ(key->type, ns::Type::Switch);
  std::ostringstream lines;
  ASSERT_EQ(nav({"nav", "--list-rules"}, lines, err), 0);
  EXPECT_NE(lines.str().find("trade-rate info-clock-rate-v1\n"), std::string::npos);
}

// The admission reserve: the rule adds its shared rate vector (one f64 per name), nothing else;
// any other rate is unchanged by the lane.
TEST(InfoClockRate, WorkspaceReserveOneRatePerName) {
  auto base = nav_config();
  base.target.trade_fraction = bk::two_speed_slow_theta;
  const auto fixed = st::nav_workspace_reserve_bytes(base, 5, true, 300, 250);
  base.rate = st::NavRateRule::InfoClockV1;
  EXPECT_EQ(st::nav_workspace_reserve_bytes(base, 5, true, 300, 250), fixed + 300 * sizeof(f64));
  EXPECT_EQ(st::nav_workspace_reserve_bytes(base, 5, true, 300, 250, true),
            st::nav_workspace_reserve_bytes([&] {
              auto c = base;
              c.rate = st::NavRateRule::Fixed;
              return c;
            }(), 5, true, 300, 250, true) + 300 * sizeof(f64));
}

// Flag absent: the clock is never loaded. A fields manifest that also lists ea_days_since
// publishes the bytes of one that does not, after the substitution list root uses: the fields
// manifest's SHA-256 (the recipe pins it), each published file's own SHA-256 and each JSON file's
// compact-dump SHA-256 (summary.json's recipe_sha256). The CSVs (capacity directory included) are
// byte for byte, every JSON file after the substitution; no info_clock, rule key or field record.
TEST(InfoClockPrint, FlagAbsentLoadsNoClock) {
  const World w;
  for (const auto& [tag, shape] : named_shapes()) {
    const auto with = w.out("with-" + tag);
    const auto without = w.out("without-" + tag);
    std::ostringstream log, err;
    ASSERT_EQ(nav(w.args(with, &w.fields, shape), log, err), 0) << err.str();
    ASSERT_EQ(nav(w.args(without, &w.plain, shape), log, err), 0) << err.str();
    const auto names = tree(with);
    ASSERT_EQ(names, tree(without));
    std::vector<std::pair<std::string, std::string>> substitutions{
        {w.fields.sha256, w.plain.sha256}};
    for (const auto& name : names) {
      substitutions.emplace_back(atx::core::sha256_file((with / name).string()).value(),
                                 atx::core::sha256_file((without / name).string()).value());
      if (name.size() > 5 && name.substr(name.size() - 5) == ".json") // recipe_sha256: the dump's
        substitutions.emplace_back(
            atx::core::sha256_hex(Json::parse(bytes_of(with / name)).dump()).value(),
            atx::core::sha256_hex(Json::parse(bytes_of(without / name)).dump()).value());
    }
    size_t csvs = 0;
    for (const auto& name : names) {
      const auto a = bytes_of(with / name);
      if (name.size() > 4 && name.substr(name.size() - 4) == ".csv") {
        EXPECT_TRUE(a == bytes_of(without / name)) << name;
        ++csvs;
      }
      auto substituted = a;
      for (const auto& [from, to] : substitutions) substituted = replaced(substituted, from, to);
      EXPECT_EQ(substituted, bytes_of(without / name)) << name;
      for (const char* absent :
           {"info_clock", "info-clock", "ea_days_since", "rate_theta", "rate_window", "rate_field"})
        EXPECT_EQ(a.find(absent), std::string::npos) << name << ' ' << absent;
    }
    EXPECT_GE(csvs, 6U);
  }
}

// Ruling ALCLOCK-1: --info-clock-print on a book that does not run the rule (the reference)
// changes summary.json only, by one info_clock block per scenario (the recipe, the CSVs, the
// capacity directory and the v7 files byte for byte; summary.json minus the blocks is the absent
// run's file). The block is the book's census at the rate it runs: name-days of its scored
// effective rebalances, fast and missing ones from the fixture clock, planned turnover from the
// published holdings (sum of |planned_trade_weight| over member rows of rebalance sessions), the
// rule's constants at the run's cadence and the field's payload SHA-256. A grid variant carries
// it as its standalone run does.
TEST(InfoClockPrint, PrintOnlyOptInAddsOneBlockPerScenario) {
  const World w;
  for (const auto& [tag, shape] : named_shapes()) {
    const auto absent = w.out("absent-" + tag), present = w.out("present-" + tag);
    std::ostringstream log, err;
    ASSERT_EQ(nav(w.args(absent, &w.fields, shape), log, err), 0) << err.str();
    ASSERT_EQ(nav(w.args(present, &w.fields, joined(shape, {"--info-clock-print"})), log, err), 0)
        << err.str();
    expect_same_but_summary(present, absent);
    EXPECT_EQ(summary_without_blocks(present), bytes_of(absent / "summary.json"));
    EXPECT_EQ(bytes_of(absent / "summary.json").find("info_clock"), std::string::npos);
  }
  EXPECT_TRUE(fs::exists(w.out("present-v6") / "capacity" / "summary.json"));
  EXPECT_TRUE(fs::exists(w.out("present-v6") / "v7_extras.json"));
  // The values, against the primary book's holdings and the fixture clock.
  const auto holdings = w.out("holdings");
  std::ostringstream log, err;
  ASSERT_EQ(nav(w.args(w.out("values"), &w.fields,
                       joined(v5_fixed, {"--info-clock-print", "--emit-holdings",
                                         holdings.string(), "--holdings-format", "csv"})),
                log, err),
            0)
      << err.str();
  expect_same_tree(w.out("values"), w.out("present-v5"));
  const auto summary = Json::parse(bytes_of(w.out("values") / "summary.json"));
  const auto book = Json::parse(bytes_of(holdings / "manifest.json")).at("book");
  const auto primary = std::find_if(summary.at("scenarios").begin(), summary.at("scenarios").end(),
                                    [&](const Json& e) { return e.at("scenario") == book; });
  ASSERT_NE(primary, summary.at("scenarios").end());
  const auto daily = read_csv(w.out("values") / ("daily_" + book.get<std::string>() + ".csv"));
  std::map<i64, bool> rebalance;
  for (const auto& row : daily.rows)
    rebalance[std::stoll(row.at(daily.col("session_ns")))] =
        row.at(daily.col("decision")) == "1" && row.at(daily.col("rebalance")) == "1";
  usize name_days = 0, fast_days = 0, missing_days = 0;
  for (usize t = 0; t < w.role.d; ++t) {
    const auto found = rebalance.find(w.role.sessions[t]);
    if (found == rebalance.end() || !found->second) continue;
    for (usize i = 0; i < w.role.n; ++i) {
      const usize k = t * w.role.n + i;
      if (!w.role.member[k]) continue;
      ++name_days;
      fast_days += fast(w.clock[k]) ? 1U : 0U;
      missing_days += std::isnan(w.clock[k]) ? 1U : 0U;
    }
  }
  const auto csv = read_csv(holdings / "holdings.csv");
  std::map<i64, usize> row_of;
  for (usize t = 0; t < w.role.d; ++t) row_of[w.role.sessions[t]] = t;
  f64 turnover = 0, fast_turnover = 0;
  for (const auto& row : csv.rows) {
    const i64 session = std::stoll(row.at(csv.col("session_ns")));
    if (!rebalance[session] || row.at(csv.col("member")) != "1") continue;
    const usize i = static_cast<usize>(std::stoull(row.at(csv.col("instrument_id"))) - 100);
    const f64 trade = std::abs(number_of(row.at(csv.col("planned_trade_weight"))));
    turnover += trade;
    if (fast(w.clock[row_of.at(session) * w.role.n + i])) fast_turnover += trade;
  }
  const auto& block = primary->at("info_clock");
  EXPECT_EQ(block.at("rate"), "fixed");
  EXPECT_EQ(block.at("field"), (Json{{"name", clock_field}, {"sha256", w.fields.clock_sha256}}));
  EXPECT_EQ(block.at("window_sessions"), 21);
  EXPECT_EQ(bits(block.at("theta_fast").get<f64>()), bits(st::nav_info_clock_fast_theta(1)));
  EXPECT_EQ(block.at("theta_slow").get<f64>(), .05);
  EXPECT_EQ(block.at("name_days").get<usize>(), name_days);
  EXPECT_EQ(block.at("fast_name_days").get<usize>(), fast_days);
  EXPECT_EQ(block.at("missing_name_days").get<usize>(), missing_days);
  EXPECT_GT(fast_days, 0U);
  EXPECT_GT(missing_days, 0U);
  EXPECT_DOUBLE_EQ(block.at("fast_share").get<f64>(),
                   static_cast<f64>(fast_days) / static_cast<f64>(name_days));
  EXPECT_DOUBLE_EQ(block.at("planned_turnover").get<f64>(), turnover);
  EXPECT_DOUBLE_EQ(block.at("fast_planned_turnover").get<f64>(), fast_turnover);
  EXPECT_DOUBLE_EQ(block.at("turnover_share").get<f64>(), fast_turnover / turnover);
  EXPECT_GT(fast_turnover, 0.0);
  EXPECT_FALSE(block.at("definition").get<std::string>().empty());
  // Every book counts the same name-days (one calendar); a per-name-v1 book prints its rate.
  for (const auto& entry : summary.at("scenarios"))
    EXPECT_EQ(entry.at("info_clock").at("name_days").get<usize>(), name_days);
  ASSERT_EQ(nav(w.args(w.out("per-name"), &w.fields,
                       joined(v5_fixed, {"--rate", "per-name-v1", "--info-clock-print"})),
                log, err),
            0)
      << err.str();
  EXPECT_EQ(Json::parse(bytes_of(w.out("per-name") / "summary.json"))
                .at("scenarios")
                .at(0)
                .at("info_clock")
                .at("rate"),
            "per-name-v1");
  // A grid variant with no override is its standalone run, the blocks included.
  const auto grid = w.out("grid.json");
  write_json_file(grid, Json{{"schema", "atx.nav-construction-grid/v1"},
                             {"variants", Json::array({Json{{"id", "v"},
                                                            {"flags", Json::object()}}})}});
  ASSERT_EQ(nav(w.args(w.out("grid"), &w.fields,
                       joined(v5_fixed, {"--info-clock-print", "--construction-grid",
                                         grid.string()})),
                log, err),
            0)
      << err.str();
  EXPECT_GE(expect_same_tree(w.out("grid") / "v", w.out("present-v5")), 4U);
}

// Ruling ALCLOCK-1: the rule alone prints no block (it is a rule, not the print): its recipe
// records the rule (rate, the two rates at the cadence, the window, the field, which enters
// fields_used as an input of the run); naming the print too changes summary.json only, by the
// blocks, which then report the rule's rate.
TEST(InfoClockPrint, RuleOnWithoutFlagPrintsNoBlock) {
  const World w;
  std::ostringstream log, err;
  ASSERT_EQ(nav(w.args(w.out("rule"), &w.fields, rule_on), log, err), 0) << err.str();
  const auto summary = Json::parse(bytes_of(w.out("rule") / "summary.json"));
  for (const auto& entry : summary.at("scenarios"))
    EXPECT_FALSE(entry.contains("info_clock")) << entry.at("scenario");
  EXPECT_EQ(bytes_of(w.out("rule") / "summary.json").find("\"info_clock\""), std::string::npos);
  const auto recipe = Json::parse(bytes_of(w.out("rule") / "recipe.json"));
  EXPECT_EQ(recipe.at("rate"), clock_id);
  EXPECT_EQ(bits(recipe.at("rate_theta_fast").get<f64>()), bits(st::nav_info_clock_fast_theta(1)));
  EXPECT_EQ(recipe.at("rate_theta_slow").get<f64>(), .05);
  EXPECT_EQ(recipe.at("rate_window_sessions"), 21);
  EXPECT_EQ(recipe.at("rate_field"), clock_field);
  EXPECT_NE(recipe.at("aim_partial").get<std::string>().find("info-clock-rate-v1"),
            std::string::npos);
  const auto& used = recipe.at("financing_fields").at("fields_used");
  ASSERT_TRUE(used.contains(clock_field));
  EXPECT_EQ(used.at(clock_field).at("sha256"), w.fields.clock_sha256);
  ASSERT_EQ(nav(w.args(w.out("rule-print"), &w.fields, joined(rule_on, {"--info-clock-print"})),
                log, err),
            0)
      << err.str();
  expect_same_but_summary(w.out("rule-print"), w.out("rule"));
  EXPECT_EQ(summary_without_blocks(w.out("rule-print")), bytes_of(w.out("rule") / "summary.json"));
  const auto printed = Json::parse(bytes_of(w.out("rule-print") / "summary.json"));
  for (const auto& entry : printed.at("scenarios"))
    EXPECT_EQ(entry.at("info_clock").at("rate"), clock_id) << entry.at("scenario");
}

// --spec equals argv with the flag (and with the rule): --print-spec carries
// options.info_clock_print (true) and options.rate only when named, and the spec run publishes
// the argv run's bytes.
TEST(InfoClockPrint, SpecEqualsArgv) {
  const World w;
  const std::vector<std::vector<std::string>> shapes{
      joined(v5_fixed, {"--info-clock-print"}), joined(rule_on, {"--info-clock-print"}),
      joined(rule_on, {"--cost-v2", "--capacity-curve", "--info-clock-print"})};
  for (size_t s = 0; s < shapes.size(); ++s) {
    const auto argv_dir = w.out("argv" + std::to_string(s));
    const auto spec_dir = w.out("spec" + std::to_string(s));
    std::ostringstream log, err;
    auto args = w.args(argv_dir, &w.fields, shapes[s]);
    ASSERT_EQ(nav(args, log, err), 0) << err.str();
    std::ostringstream printed;
    args.push_back("--print-spec");
    ASSERT_EQ(nav(args, printed, err), 0) << err.str();
    const auto doc = Json::parse(printed.str());
    EXPECT_EQ(doc.at("options").at("info_clock_print"), true);
    EXPECT_EQ(doc.at("options").contains("rate"), s > 0);
    if (s > 0) EXPECT_EQ(doc.at("options").at("rate"), clock_id);
    const auto file = w.out("spec" + std::to_string(s) + ".json");
    write_json_file(file, doc);
    ASSERT_EQ(nav({"nav", "--spec", file.string(), "--output", spec_dir.string()}, log, err), 0)
        << err.str();
    EXPECT_GE(expect_same_tree(spec_dir, argv_dir), 4U);
  }
  std::ostringstream plain, err;
  auto absent = w.args(w.out("absent"), &w.fields, v5_fixed);
  absent.push_back("--print-spec");
  ASSERT_EQ(nav(absent, plain, err), 0) << err.str();
  EXPECT_FALSE(Json::parse(plain.str()).at("options").contains("info_clock_print"));
}
