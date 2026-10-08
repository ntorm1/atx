// mv-aim-v1 (P9 lane COV-MV, strategy_mv_aim.hpp): the NAV target rule through the v7 seam and
// its row in the K-P9-7 registry. Synthetic roles and stores only (strategy_spo_fixture.hpp Role,
// write_risk_model; strategy_cov_fixture.hpp add_container for the container store).
//
//   the --list-rules row (a closed target row, append-only); the refusals (no store, the rules
//   of incompatible[], an industry slot outside 0..49 / 255: Ruling COV-11 S11); the as-of rule
//   (Ruling COV-4: the target at d reads store row d only; deleting every session after t
//   changes nothing at or before t); the registered arithmetic (unpriced members pass through,
//   the priced block is the kernel's at the desired gross, dollar neutral); pass-through counts;
//   a decision counted once (a second pass forms it again); the container and directory stores
//   give the same book; --book-workers 1 and 4 publish the same bytes (W1-MIN M-1); the summary
//   block only with the rule; the rule-absent identity of the hook and of the NAV workspace
//   reserve (Ruling COVMV-1); --calibrate-gross counts each decision once; a failed eigen print
//   leaves every decision and NAV byte as they were and prints the figure as null (Ruling
//   COVMV-S S-2).
//
// Suite: MvAimRule

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/engine/book/mv_target.hpp"
#include "../src/book/nav_rules.hpp"
#include "../src/book/nav_spec.hpp"
#include "../src/strategy_mv_aim.hpp"
#include "../src/strategy_nav_calibrate.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "strategy_cov_fixture.hpp"
#include "strategy_spo_cli_fixture.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace nr = atx::impl::strategy::nav_rules;
namespace ns = atx::impl::strategy::nav_spec;
namespace ma = atx::impl::strategy::mv_aim;
namespace v7 = atx::impl::strategy::v7;
namespace eb = atx::engine::book;
namespace co = atx::core;
using Json = nlohmann::json;
namespace fs = std::filesystem;

// ---- helpers --------------------------------------------------------------------------------
bool same_bits(const std::vector<f64>& a, const std::vector<f64>& b) {
  if (a.size() != b.size()) return false;
  for (usize k = 0; k < a.size(); ++k)
    if (bits(a[k]) != bits(b[k])) return false;
  return true;
}
// The members' signal at d, demeaned over the members and scaled to gross 1 (nonmembers 0): the
// shape of the replay's desired target (strategy_vol_target_test.cpp).
std::vector<f64> desired_at(const Role& role, usize d) {
  const usize n = role.n;
  std::vector<f64> desired(n, 0.0);
  f64 sum = 0;
  usize members = 0;
  for (usize i = 0; i < n; ++i) {
    if (!role.member[d * n + i]) continue;
    desired[i] = role.signal[d * n + i];
    sum += desired[i];
    ++members;
  }
  f64 gross = 0;
  for (usize i = 0; i < n; ++i) {
    if (!role.member[d * n + i]) continue;
    desired[i] -= sum / static_cast<f64>(members);
    gross += std::abs(desired[i]);
  }
  for (f64& v : desired) v /= gross;
  return desired;
}
// A store of `role` in `dir` (created), opened against `role_sha`.
std::shared_ptr<const sp::RiskStore> store_of(
    const fs::path& dir, const Role& role, const std::vector<u8>& forecast,
    std::span<const std::pair<usize, f64>> overrides = {},
    const std::string& role_sha = "role-sha") {
  fs::create_directories(dir);
  const auto sha =
      write_risk_model(dir, role.sessions, role.n, forecast, role_sha, 3, overrides);
  auto store = sp::RiskStore::open(dir.string(), sha, role_sha);
  EXPECT_TRUE(store) << store.error().to_string();
  return store ? std::make_shared<const sp::RiskStore>(std::move(*store)) : nullptr;
}
// The rule's targets at decisions 0..through of `role` (each from desired_at), in order.
std::vector<std::vector<f64>> targets(ma::Rule& rule, const Role& role, usize through) {
  std::vector<std::vector<f64>> out;
  const auto x = role.target();
  for (usize d = 0; d <= through; ++d) {
    auto desired = desired_at(role, d);
    const auto status = rule.apply(x, d, {}, desired);
    EXPECT_TRUE(status) << d << ' ' << status.error().to_string();
    out.push_back(std::move(desired));
  }
  return out;
}
// The fixture's NAV replay (nav_config: aim-partial-v5, cadence 1) with the rule on `store`.
co::Result<st::NavReplayResult> replay_with(const Role& role,
                                            std::shared_ptr<const sp::RiskStore> store) {
  v7::NavV7Options o;
  o.mv_aim = true;
  o.spo_risk = std::move(store);
  const v7::ScopedNavExtension extension(o);
  return st::replay_nav(role.nav(), nav_config());
}
std::vector<u64> day_bits(const st::NavReplayDay& day) {
  return {static_cast<u64>(day.session_index), bits(day.pretrade_nav), bits(day.posttrade_nav),
          bits(day.net_return), bits(day.traded_dollars), bits(day.trade_cost_dollars),
          bits(day.planned_gross), bits(day.planned_net), static_cast<u64>(day.fills)};
}
// Instrument i's industry slot at row d of the store in `dir` set to `slot` (the manifest's file
// entry re-hashed); returns the new manifest SHA-256.
std::string set_slot(const fs::path& dir, usize names, usize d, usize i, u8 slot) {
  std::string raw;
  {
    std::ifstream in(dir / "industry_slot.u8", std::ios::binary);
    raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::vector<u8> slots(raw.size());
  for (usize k = 0; k < raw.size(); ++k) slots[k] = static_cast<u8>(raw[k]);
  slots.at(d * names + i) = slot;
  auto manifest = read_json_file(dir / "manifest.json");
  manifest["files"]["industry_slot.u8"] = write_payload(dir / "industry_slot.u8", slots);
  return write_json_file(dir / "manifest.json", manifest);
}

// ---- command-line helpers -------------------------------------------------------------------
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
size_t expect_same_tree(const fs::path& a, const fs::path& b) {
  const auto names = tree(a);
  EXPECT_EQ(names, tree(b)) << a.string() << " vs " << b.string();
  for (const auto& name : names)
    EXPECT_TRUE(bytes_of(a / name) == bytes_of(b / name)) << name;
  return names.size();
}
struct Pin {
  std::string dir, sha;
};
// The CLI store of `role` (pinned to the run's role manifest).
Pin cli_store(const fs::path& dir, const Role& role, const RunInputs& in) {
  fs::create_directories(dir);
  const std::vector<u8> forecast(role.d, u8{1});
  return {dir.string(), write_risk_model(dir, role.sessions, role.n, forecast, in.role_sha256, 3)};
}
// The fixture's nav command line (C1's CLI shape: cadence 1, theta .25, dust .1, L 1.2), then
// `extra`.
std::vector<std::string> nav_line(const RunInputs& in, const fs::path& output,
                                  const std::vector<std::string>& extra) {
  std::vector<std::string> args{"nav", "--combined", in.combined, "--combined-sha256",
                                in.combined_sha256, "--role", in.role, "--role-sha256",
                                in.role_sha256, "--output", output.string(), "--cadence", "1",
                                "--trade-fraction", ".25", "--dust-multiple", ".1", "--exit-rate",
                                ".05", "--aim-leverage", "1.2"};
  args.insert(args.end(), extra.begin(), extra.end());
  return args;
}
// --rule aim-partial-v5 with the rule on `pin`, then `extra`.
std::vector<std::string> rule_on(const Pin& pin, std::vector<std::string> extra = {}) {
  std::vector<std::string> args{"--rule", "aim-partial-v5", "--target-rule", "mv-aim-v1",
                                "--risk-model", pin.dir, "--risk-model-sha256", pin.sha};
  args.insert(args.end(), extra.begin(), extra.end());
  return args;
}

// Every row's F_{0,8} moved 1e-4 away from 0 (its sign kept), F_{8,0} left, in the store in
// `dir` (the manifest's file entry re-hashed); returns the new manifest SHA-256. Factor 8 is
// industry slot 7, which no fixture name has (write_risk_model: slot i % 5), so its column of F
// meets only exact zeros of G = X' D^-1 X and r = X' u in the kernel (each product the same
// signed zero) and the target reads the store bit for bit as before; F_d's forecast block is no
// longer symmetric, so the eigen print fails at every decision (Ruling COVMV-S S-2).
std::string plant_asymmetric_f(const fs::path& dir, usize dates) {
  constexpr usize k = sp::risk_factors, row = 0, column = 1 + 7;
  const auto raw = bytes_of(dir / "factor_covariance.f64");
  std::vector<f64> cov(dates * k * k);
  EXPECT_EQ(raw.size(), cov.size() * sizeof(f64));
  std::memcpy(cov.data(), raw.data(), std::min(raw.size(), cov.size() * sizeof(f64)));
  for (usize d = 0; d < dates; ++d) {
    f64& entry = cov[(d * k + row) * k + column];
    entry = std::copysign(std::abs(entry) + 1e-4, entry);
  }
  auto manifest = read_json_file(dir / "manifest.json");
  manifest["files"]["factor_covariance.f64"] = write_payload(dir / "factor_covariance.f64", cov);
  return write_json_file(dir / "manifest.json", manifest);
}

// ---- the registry row -----------------------------------------------------------------------

// K-P9-7: one target row, closed params_schema (the store pin, both required with their flags:
// Ruling PRE-9), x-flag "--target-rule mv-aim-v1", incompatible[] as registered; appended last,
// after every row of the other kinds (Ruling W3-9); its derived flags are typed NavSpec flags.
TEST(MvAimRule, ListRulesRowIsAClosedTargetRow) {
  std::ostringstream out, err;
  ASSERT_EQ(nav({"nav", "--list-rules", "--json"}, out, err), 0) << err.str();
  const auto table = Json::parse(out.str());
  std::vector<Json> rows;
  for (const auto& row : table.at("rules"))
    if (row.at("kind") == "target") rows.push_back(row);
  ASSERT_EQ(rows.size(), 1U);
  const auto& row = rows.front();
  EXPECT_EQ(table.at("rules").back(), row);
  EXPECT_EQ(row.at("id"), "mv-aim-v1");
  EXPECT_EQ(row.at("id"), std::string(ma::rule_id));
  EXPECT_EQ(row.at("x-flag"), "--target-rule mv-aim-v1");
  EXPECT_FALSE(row.contains("x-flag-aliases"));
  EXPECT_FALSE(row.at("description").get<std::string>().empty());
  const auto& schema = row.at("params_schema");
  EXPECT_EQ(schema.at("type"), "object");
  EXPECT_FALSE(schema.at("additionalProperties").get<bool>());
  EXPECT_EQ(schema.at("required"), Json::array({"risk_model", "risk_model_sha256"}));
  const auto& properties = schema.at("properties");
  ASSERT_EQ(properties.size(), 2U);
  EXPECT_EQ(properties.at("risk_model"), (Json{{"type", "string"}, {"x-flag", "--risk-model"}}));
  EXPECT_EQ(properties.at("risk_model_sha256"),
            (Json{{"type", "string"}, {"x-flag", "--risk-model-sha256"},
                  {"pattern", "^[0-9a-f]{64}$"}}));
  EXPECT_EQ(row.at("incompatible"),
            Json::array({"aim-partial-v6", "spo-v1", "spo-v2", "spo-v3", "two-speed-v1",
                         "adv-hold-v1", "hold-band-v1"}));
  ASSERT_EQ(nr::target_rows().size(), 1U);
  EXPECT_EQ(nr::find(nr::Kind::Target, "mv-aim-v1"), &nr::target_rows()[0]);
  EXPECT_EQ(nr::flag_ids(nr::Kind::Target), "mv-aim-v1");
  // The row's flags (x-flag, then each param's x-flag with a value) are typed run flags.
  const std::vector<std::string> line{"--target-rule", "mv-aim-v1", "--risk-model", "store",
                                      "--risk-model-sha256", std::string(64, 'a')};
  EXPECT_TRUE(ns::from_args(line));
  std::ostringstream text, quiet;
  ASSERT_EQ(nav({"nav", "--list-rules"}, text, quiet), 0);
  EXPECT_NE(text.str().find("target mv-aim-v1\n"), std::string::npos);
}

// ---- refusals -------------------------------------------------------------------------------

// No store: the parser refuses before any output (one store flag alone too); a store of another
// role is refused by RiskStore::open, before any replay work; the Rule itself refuses a null
// store.
TEST(MvAimRule, RefusesWithoutAStore) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const auto pin = cli_store(dir.path / "risk", role, in);
  const auto out_dir = dir.path / "out";
  const std::vector<std::vector<std::string>> lines{
      {"--rule", "aim-partial-v5", "--target-rule", "mv-aim-v1"},
      {"--rule", "aim-partial-v5", "--target-rule", "mv-aim-v1", "--risk-model", pin.dir},
      {"--rule", "aim-partial-v5", "--target-rule", "mv-aim-v1", "--risk-model-sha256", pin.sha}};
  for (const auto& extra : lines) {
    std::ostringstream out, err;
    EXPECT_EQ(nav(nav_line(in, out_dir, extra), out, err), 2) << extra.size();
    EXPECT_NE(err.str().find("mv-aim-v1 needs --risk-model and --risk-model-sha256"),
              std::string::npos)
        << err.str();
  }
  // A store fitted on another role.
  const auto other = write_risk_model(dir.path / "risk", role.sessions, role.n,
                                      std::vector<u8>(role.d, u8{1}), std::string(64, 'b'), 3);
  {
    std::ostringstream out, err;
    EXPECT_EQ(nav(nav_line(in, out_dir, rule_on(Pin{pin.dir, other})), out, err), 2);
    EXPECT_FALSE(err.str().empty());
  }
  EXPECT_FALSE(fs::exists(out_dir));
  ma::Rule rule(nullptr);
  auto desired = desired_at(role, 5);
  const auto refused = rule.apply(role.target(), 5, {}, desired);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
  EXPECT_NE(refused.error().message().find("no risk model"), std::string::npos);
}

// incompatible[] and the parser: the spo rules, aim-partial-v6, a rule other than aim-partial-v5
// (or none), two-speed-v1, adv-hold-v1, hold-band-v1 and a construction grid are refused before
// any output, each naming the rule.
TEST(MvAimRule, RefusesIncompatibleRules) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const auto pin = cli_store(dir.path / "risk", role, in);
  write_json_file(dir.path / "grid.json",
                  Json{{"schema", "atx.nav-construction-grid/v1"},
                       {"variants",
                        Json::array({{{"id", "a"}, {"flags", {{"--cadence", "1"}}}}})}});
  const std::vector<std::string> store{"--target-rule", "mv-aim-v1", "--risk-model", pin.dir,
                                       "--risk-model-sha256", pin.sha};
  const auto with = [&](std::vector<std::string> extra) {
    extra.insert(extra.end(), store.begin(), store.end());
    return extra;
  };
  const std::vector<std::pair<std::vector<std::string>, std::string>> cases{
      {with({"--rule", "spo-v1"}), "refused with --rule spo-v1|v2|v3"},
      {with({"--rule", "spo-v2"}), "refused with --rule spo-v1|v2|v3"},
      {with({"--rule", "spo-v3"}), "refused with --rule spo-v1|v2|v3"},
      {with({"--rule", "aim-partial-v6"}), "refused with --rule aim-partial-v6"},
      {with({"--rule", "baseline-v1"}), "needs --rule aim-partial-v5"},
      {with({}), "needs --rule aim-partial-v5"},
      {with({"--rule", "aim-partial-v5", "--two-speed", "two-speed-v1"}),
       "refused with --two-speed"},
      {with({"--rule", "aim-partial-v5", "--adv-hold-q", ".05"}), "refused with --adv-hold-q"},
      {with({"--rule", "aim-partial-v5", "--hold-band", ".2"}), "refused with --hold-band"},
      {with({"--rule", "aim-partial-v5", "--construction-grid",
             (dir.path / "grid.json").string()}),
       "refused with --construction-grid"}};
  for (const auto& [extra, message] : cases) {
    std::ostringstream out, err;
    EXPECT_EQ(nav(nav_line(in, dir.path / "out", extra), out, err), 2) << message;
    EXPECT_NE(err.str().find("--target-rule mv-aim-v1"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find(message), std::string::npos) << err.str();
  }
  EXPECT_FALSE(fs::exists(dir.path / "out"));
  // The rule given twice is a usage error too.
  std::ostringstream out, err;
  EXPECT_EQ(nav(nav_line(in, dir.path / "out", with({"--rule", "aim-partial-v5", "--target-rule",
                                                      "mv-aim-v1"})),
                out, err),
            2);
}

// Ruling COV-11 (S11): the container reader does not check the per-row industry slots, so the
// rule does: a slot of row d outside 0..49 and 255 refuses the decision (naming the row, the
// instrument and the slot) and the run before any output; 255 (no row) is accepted (unpriced).
TEST(MvAimRule, RefusesAnIndustrySlotOutsideTheTable) {
  const Directory dir;
  const Role role(40, 12, 53);
  fs::create_directories(dir.path / "s");
  const std::vector<u8> forecast(role.d, u8{1});
  (void)write_risk_model(dir.path / "s", role.sessions, role.n, forecast, "role-sha", 3);
  (void)set_slot(dir.path / "s", role.n, 26, 4, sp::risk_no_row);
  const auto sha = set_slot(dir.path / "s", role.n, 25, 3, 60);
  auto opened = sp::RiskStore::open((dir.path / "s").string(), sha, "role-sha");
  ASSERT_TRUE(opened) << opened.error().to_string();
  ma::Rule rule(std::make_shared<const sp::RiskStore>(std::move(*opened)));
  const auto x = role.target();
  auto ok = desired_at(role, 24);
  ASSERT_TRUE(rule.apply(x, 24, {}, ok));
  auto refused_target = desired_at(role, 25);
  const auto before = refused_target;
  const auto refused = rule.apply(x, 25, {}, refused_target);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
  EXPECT_NE(refused.error().message().find("instrument 103 the industry slot 60"),
            std::string::npos)
      << refused.error().message();
  EXPECT_TRUE(same_bits(refused_target, before));
  auto no_row = desired_at(role, 26);
  const auto unpriced = no_row[4];
  ASSERT_TRUE(rule.apply(x, 26, {}, no_row));
  EXPECT_EQ(bits(no_row[4]), bits(unpriced));
  // The command line: the replay stops at the decision, nothing is published.
  const auto in = write_run_inputs(dir.path, role);
  fs::create_directories(dir.path / "cli");
  (void)write_risk_model(dir.path / "cli", role.sessions, role.n, forecast, in.role_sha256, 3);
  const auto cli_sha = set_slot(dir.path / "cli", role.n, 25, 3, 60);
  std::ostringstream out, err;
  EXPECT_NE(nav(nav_line(in, dir.path / "out", rule_on(Pin{(dir.path / "cli").string(), cli_sha})),
                out, err),
            0);
  EXPECT_NE(err.str().find("the industry slot 60"), std::string::npos) << err.str();
  EXPECT_FALSE(fs::exists(dir.path / "out"));
}

// ---- the as-of rule (Ruling COV-4) ----------------------------------------------------------

// The target at d reads store row d and nothing later: every specific variance after row T
// replaced, every target at d <= T is bit for bit the same; replacing row T itself moves the
// target at T (teeth). In the NAV replay every daily row through T (its decision reads row T) is
// bit for bit the same and a later row moves.
TEST(MvAimRule, TargetAtDReadsStoreRowDOnly) {
  constexpr usize horizon = 40;
  const Directory dir;
  const Role role(70, 12, 53);
  const usize n = role.n;
  const std::vector<u8> forecast(role.d, u8{1});
  std::vector<std::pair<usize, f64>> future, at;
  for (usize d = horizon + 1; d < role.d; ++d)
    for (usize i = 0; i < n; ++i) future.emplace_back(d * n + i, 4e-4 + 1e-5 * static_cast<f64>(i));
  for (usize i = 0; i < n; ++i) at.emplace_back(horizon * n + i, 4e-4 + 1e-5 * static_cast<f64>(i));
  const auto now = store_of(dir.path / "now", role, forecast);
  const auto later = store_of(dir.path / "later", role, forecast, future);
  const auto moved = store_of(dir.path / "at", role, forecast, at);
  ma::Rule a(now), b(later), c(moved);
  const auto ta = targets(a, role, horizon);
  const auto tb = targets(b, role, horizon);
  const auto tc = targets(c, role, horizon);
  for (usize d = 0; d <= horizon; ++d) EXPECT_TRUE(same_bits(ta[d], tb[d])) << d;
  for (usize d = 0; d < horizon; ++d) EXPECT_TRUE(same_bits(ta[d], tc[d])) << d;
  EXPECT_FALSE(same_bits(ta[horizon], tc[horizon])); // row d itself is read at d
  EXPECT_FALSE(same_bits(ta[horizon], desired_at(role, horizon))); // the rule moved the target
  EXPECT_EQ(a.counts().applied, horizon + 1);
  const auto full = replay_with(role, now);
  const auto cut = replay_with(role, later);
  ASSERT_TRUE(full) << full.error().to_string();
  ASSERT_TRUE(cut) << cut.error().to_string();
  ASSERT_EQ(full->days.size(), cut->days.size());
  usize rows = 0;
  bool later_moved = false;
  for (usize k = 0; k < full->days.size(); ++k) {
    const auto& day = full->days[k];
    if (day.session_index <= horizon) {
      EXPECT_EQ(day_bits(day), day_bits(cut->days[k])) << k;
      ++rows;
    } else {
      later_moved = later_moved || day_bits(day) != day_bits(cut->days[k]);
    }
  }
  EXPECT_EQ(rows, horizon + 1);
  EXPECT_TRUE(later_moved);
}

// Every session after t deleted (the role and the store are the full ones' prefixes): every target
// at d <= t and every daily row the shorter replay can form identically (through t - 2, its last
// decision) are bit for bit the full run's.
TEST(MvAimRule, FutureReturnDoesNotChangePast) {
  constexpr usize horizon = 40;
  const Directory dir;
  const Role full(70, 12, 53);
  Role cut = full;
  const usize n = full.n, keep = (horizon + 1) * n;
  cut.d = horizon + 1;
  for (auto* v : {&cut.signal, &cut.close, &cut.raw, &cut.volume}) v->resize(keep);
  cut.member.resize(keep);
  cut.present.resize(keep);
  cut.sessions.resize(horizon + 1);
  const auto full_store = store_of(dir.path / "full", full, std::vector<u8>(full.d, u8{1}));
  const auto cut_store = store_of(dir.path / "cut", cut, std::vector<u8>(cut.d, u8{1}));
  ma::Rule a(full_store), b(cut_store);
  const auto ta = targets(a, full, horizon);
  const auto tb = targets(b, cut, horizon);
  for (usize d = 0; d <= horizon; ++d) EXPECT_TRUE(same_bits(ta[d], tb[d])) << d;
  const auto long_run = replay_with(full, full_store);
  const auto short_run = replay_with(cut, cut_store);
  ASSERT_TRUE(long_run) << long_run.error().to_string();
  ASSERT_TRUE(short_run) << short_run.error().to_string();
  usize rows = 0;
  for (usize k = 0; k < short_run->days.size(); ++k) {
    if (short_run->days[k].session_index + 2 > horizon) break;
    EXPECT_EQ(day_bits(short_run->days[k]), day_bits(long_run->days[k])) << k;
    ++rows;
  }
  EXPECT_EQ(rows, horizon - 1);
}

// ---- the registered arithmetic --------------------------------------------------------------

// Unpriced members (no specific variance, or 0) and nonmembers keep their desired weights bit for
// bit; the priced block is g x the kernel's target on the priced view of row d (g the desired
// gross over the priced names, Ruling W3-17), so it keeps that gross and is dollar neutral; the
// vector's net is the unpriced members' net.
TEST(MvAimRule, UnpricedMembersPassThrough) {
  const Directory dir;
  const Role role(70, 12, 53);
  const usize n = role.n;
  std::vector<std::pair<usize, f64>> overrides;
  for (usize d = 0; d < role.d; ++d) {
    overrides.emplace_back(d * n + 2, missing);
    overrides.emplace_back(d * n + 7, missing);
    overrides.emplace_back(d * n + 4, 0.0);
  }
  const auto store = store_of(dir.path / "s", role, std::vector<u8>(role.d, u8{1}), overrides);
  ma::Rule rule(store);
  const auto x = role.target();
  sp::RiskSlice slice;
  for (const usize d : {5U, 30U, 60U}) {
    const auto before = desired_at(role, d);
    auto after = before;
    ASSERT_TRUE(rule.apply(x, d, {}, after)) << d;
    ASSERT_TRUE(store->read(d, slice));
    std::vector<u32> group;
    std::vector<f64> exposures, specific, score, sigma;
    std::vector<usize> index;
    f64 g = 0, unpriced_net = 0;
    for (usize i = 0; i < n; ++i) {
      const bool member = role.member[d * n + i] != 0;
      const bool priced = member && slice.slot[i] < sp::risk_industry_slots &&
                          std::isfinite(slice.specific[i]) && slice.specific[i] > 0;
      if (!priced) {
        EXPECT_EQ(bits(after[i]), bits(before[i])) << d << ' ' << i;
        if (member) unpriced_net += before[i];
        continue;
      }
      group.push_back(1U + slice.slot[i]);
      for (usize c = 0; c < sp::risk_styles; ++c)
        exposures.push_back(slice.styles[i * sp::risk_styles + c]);
      specific.push_back(slice.specific[i]);
      score.push_back(before[i]);
      sigma.push_back(std::sqrt(slice.specific[i]));
      index.push_back(i);
      g += std::abs(before[i]);
    }
    ASSERT_GE(index.size(), 8U) << d;
    const eb::FactorRiskView view{sp::risk_industry_slots, sp::risk_styles, group, exposures,
                                  slice.covariance, specific};
    eb::MvAimScratch scratch;
    std::vector<f64> w(index.size());
    ASSERT_TRUE(eb::mv_aim(view, score, sigma, w, scratch));
    f64 priced_gross = 0, priced_net = 0, net = 0;
    for (usize k = 0; k < index.size(); ++k) {
      EXPECT_EQ(bits(after[index[k]]), bits(g * w[k])) << d << ' ' << index[k];
      priced_gross += std::abs(after[index[k]]);
      priced_net += after[index[k]];
    }
    for (const f64 v : after) net += v;
    EXPECT_NEAR(priced_gross, g, 1e-14) << d;
    EXPECT_NEAR(priced_net, 0.0, 1e-14) << d;
    EXPECT_NEAR(net, unpriced_net, 1e-14) << d;
    EXPECT_GT(std::abs(unpriced_net), 0.0) << d; // the book keeps the unpriced members' net
  }
  const auto& c = rule.counts();
  EXPECT_EQ(c.applied, 3U);
  EXPECT_EQ(c.unpriced.n, 3U);
  EXPECT_GE(c.unpriced.lo, 3.0);
}

// Pass-through, counted and never silent: no forecast at d, fewer than two priced names, a flat
// tilt (a zero desired target on the priced names) leave the target bit for bit.
TEST(MvAimRule, FewerThanTwoPricedPassesThrough) {
  const Directory dir;
  const Role role(70, 12, 53);
  const usize n = role.n;
  std::vector<u8> forecast(role.d, u8{1});
  forecast[10] = 0;
  std::vector<std::pair<usize, f64>> overrides;
  for (usize i = 1; i < n; ++i) overrides.emplace_back(20 * n + i, missing);
  const auto store = store_of(dir.path / "s", role, forecast, overrides);
  ma::Rule rule(store);
  const auto x = role.target();
  for (const usize d : {10U, 20U}) {
    const auto before = desired_at(role, d);
    auto after = before;
    ASSERT_TRUE(rule.apply(x, d, {}, after)) << d;
    EXPECT_TRUE(same_bits(after, before)) << d;
  }
  std::vector<f64> flat(n, 0.0);
  ASSERT_TRUE(rule.apply(x, 40, {}, flat));
  EXPECT_TRUE(same_bits(flat, std::vector<f64>(n, 0.0)));
  auto moved = desired_at(role, 50);
  ASSERT_TRUE(rule.apply(x, 50, {}, moved));
  EXPECT_FALSE(same_bits(moved, desired_at(role, 50)));
  const auto& c = rule.counts();
  EXPECT_EQ(c.decisions, 4U);
  EXPECT_EQ(c.applied, 1U);
  EXPECT_EQ(c.no_forecast, 1U);
  EXPECT_EQ(c.fewer_than_two_priced, 1U);
  EXPECT_EQ(c.flat, 1U);
  const auto block = rule.summary_json();
  EXPECT_EQ(block.at("decisions").at("pass_through"),
            (Json{{"no_forecast", 1}, {"fewer_than_two_priced", 1}, {"flat", 1}}));
}

// A decision is counted once, the first time its index is formed (a --calibrate-gross pass that
// forms the construction again repeats the same target and counts nothing); a warm-up decision
// is counted apart; under locate-in-aim the special-tier names the target puts short are counted.
TEST(MvAimRule, DecisionCountedOnceWarmUpAndLocateNegatives) {
  const Directory dir;
  const Role role(70, 12, 53);
  const usize n = role.n;
  const auto store = store_of(dir.path / "s", role, std::vector<u8>(role.d, u8{1}));
  ma::Rule rule(store);
  auto x = role.target();
  auto first = desired_at(role, 30);
  ASSERT_TRUE(rule.apply(x, 30, {}, first));
  for (const usize d : {30U, 29U}) { // a second pass reaching earlier and equal indexes again
    auto again = desired_at(role, d);
    ASSERT_TRUE(rule.apply(x, d, {}, again));
    if (d == 30U) EXPECT_TRUE(same_bits(again, first));
  }
  EXPECT_EQ(rule.counts().decisions, 1U);
  EXPECT_EQ(rule.counts().applied, 1U);
  EXPECT_EQ(rule.counts().priced.n, 1U);
  const auto& block_ratio = rule.counts().ratio;
  EXPECT_EQ(block_ratio.n, 1U);
  EXPECT_TRUE(std::isfinite(block_ratio.sum));
  EXPECT_EQ(rule.counts().share_mv.n, 1U);
  EXPECT_GE(rule.counts().share_mv.lo, 0.0);
  EXPECT_LE(rule.counts().share_mv.hi, 1.0 + 1e-12);
  rule.begin_run();
  EXPECT_EQ(rule.counts().decisions, 0U);
  x.decision_begin = 35;
  auto warm = desired_at(role, 31);
  ASSERT_TRUE(rule.apply(x, 31, {}, warm));
  EXPECT_EQ(rule.counts().decisions, 0U);
  EXPECT_EQ(rule.counts().warm_up, 1U);
  EXPECT_EQ(rule.counts().warm_up_applied, 1U);
  const std::vector<u8> special(n, u8{1});
  auto guarded = desired_at(role, 40);
  ASSERT_TRUE(rule.apply(x, 40, special, guarded));
  usize negative = 0;
  for (const f64 v : guarded) negative += v < 0 ? 1U : 0U;
  ASSERT_GT(negative, 0U);
  EXPECT_TRUE(rule.counts().locate_in_aim);
  EXPECT_EQ(rule.counts().negative_names, negative);
  EXPECT_EQ(rule.counts().negative_decisions, 1U);
}

// ---- the book ---------------------------------------------------------------------------------

// The container store (K-P9-12) and the directory store it was packed from give the same book:
// every file byte for byte except the recorded store pin (recipe.json and summary.json, whose
// recipe_sha256 binds the recipe), with the pin each run names.
TEST(MvAimRule, ContainerAndDirectoryStoresGiveTheSameBook) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const auto legacy = cli_store(dir.path / "legacy", role, in);
  fs::copy(dir.path / "legacy", dir.path / "packed", fs::copy_options::recursive);
  const Pin packed{(dir.path / "packed").string(),
                   atx_cov_test::add_container(dir.path / "packed")};
  std::ostringstream out, err;
  ASSERT_EQ(nav(nav_line(in, dir.path / "a", rule_on(legacy)), out, err), 0) << err.str();
  ASSERT_EQ(nav(nav_line(in, dir.path / "b", rule_on(packed)), out, err), 0) << err.str();
  const auto names = tree(dir.path / "a");
  ASSERT_EQ(names, tree(dir.path / "b"));
  ASSERT_GE(names.size(), 9U);
  for (const auto& name : names) {
    if (name != "recipe.json" && name != "summary.json") {
      EXPECT_TRUE(bytes_of(dir.path / "a" / name) == bytes_of(dir.path / "b" / name)) << name;
      continue;
    }
    auto a = read_json_file(dir.path / "a" / name);
    auto b = read_json_file(dir.path / "b" / name);
    EXPECT_EQ(a.at("mv_aim").at("risk_model"),
              (Json{{"directory", legacy.dir}, {"manifest_sha256", legacy.sha}}));
    EXPECT_EQ(b.at("mv_aim").at("risk_model"),
              (Json{{"directory", packed.dir}, {"manifest_sha256", packed.sha}}));
    for (auto* doc : {&a, &b}) {
      (*doc)["mv_aim"].erase("risk_model");
      doc->erase("recipe_sha256");
    }
    EXPECT_EQ(a, b) << name;
  }
}

// Ruling COVMV-S S-2: a print-only figure never stops a cell. On a store whose F the target reads
// bit for bit as the clean store's but whose forecast block is not symmetric (plant_asymmetric_f),
// the eigen print fails at every applied decision: every target is the clean store's bit for
// bit, the run publishes, every file but the two that record the store pin is byte-identical,
// summary.json's eigen_00_02_share is null and the failures are counted with the first error.
TEST(MvAimRule, PrintFailureLeavesTheBookAndWritesNull) {
  const Directory dir;
  const Role role(40, 12, 53);
  // The decisions, in process.
  const std::vector<u8> forecast(role.d, u8{1});
  const auto clean_store = store_of(dir.path / "clean", role, forecast);
  fs::copy(dir.path / "clean", dir.path / "planted", fs::copy_options::recursive);
  const auto planted_sha = plant_asymmetric_f(dir.path / "planted", role.d);
  auto opened = sp::RiskStore::open((dir.path / "planted").string(), planted_sha, "role-sha");
  ASSERT_TRUE(opened) << opened.error().to_string();
  ma::Rule clean(clean_store);
  ma::Rule planted(std::make_shared<const sp::RiskStore>(std::move(*opened)));
  constexpr usize through = 30;
  const auto tc = targets(clean, role, through);
  const auto tp = targets(planted, role, through);
  for (usize d = 0; d <= through; ++d) EXPECT_TRUE(same_bits(tc[d], tp[d])) << d;
  EXPECT_GT(clean.counts().applied, 0U);
  EXPECT_EQ(planted.counts().applied, clean.counts().applied);
  EXPECT_EQ(clean.counts().share_unavailable, 0U);
  EXPECT_GT(clean.counts().share_mv.n, 0U);
  EXPECT_EQ(planted.counts().share_unavailable, planted.counts().applied);
  EXPECT_EQ(planted.counts().share_mv.n, 0U);
  EXPECT_EQ(planted.counts().share_desired.n, 0U);
  EXPECT_NE(planted.counts().share_error.find("symmetric"), std::string::npos)
      << planted.counts().share_error;
  EXPECT_TRUE(clean.summary_json().at("eigen_00_02_share").is_object());
  EXPECT_TRUE(planted.summary_json().at("eigen_00_02_share").is_null());
  // The command line: the same NAV bytes, the figure null.
  const auto in = write_run_inputs(dir.path, role);
  const auto pin = cli_store(dir.path / "cli-clean", role, in);
  fs::copy(dir.path / "cli-clean", dir.path / "cli-planted", fs::copy_options::recursive);
  const Pin skewed{(dir.path / "cli-planted").string(),
                   plant_asymmetric_f(dir.path / "cli-planted", role.d)};
  std::ostringstream out, err;
  ASSERT_EQ(nav(nav_line(in, dir.path / "a", rule_on(pin)), out, err), 0) << err.str();
  ASSERT_EQ(nav(nav_line(in, dir.path / "b", rule_on(skewed)), out, err), 0) << err.str();
  const auto names = tree(dir.path / "a");
  ASSERT_EQ(names, tree(dir.path / "b"));
  ASSERT_GE(names.size(), 9U);
  for (const auto& name : names) {
    if (name != "recipe.json" && name != "summary.json") {
      EXPECT_TRUE(bytes_of(dir.path / "a" / name) == bytes_of(dir.path / "b" / name)) << name;
      continue;
    }
    auto a = read_json_file(dir.path / "a" / name);
    auto b = read_json_file(dir.path / "b" / name);
    EXPECT_EQ(b.at("mv_aim").at("risk_model").at("manifest_sha256"), skewed.sha) << name;
    for (auto* doc : {&a, &b}) {
      (*doc)["mv_aim"].erase("risk_model");
      doc->erase("recipe_sha256");
    }
    if (name == "summary.json") {
      const auto& block = b.at("mv_aim");
      EXPECT_TRUE(block.at("eigen_00_02_share").is_null());
      const auto& why = block.at("eigen_00_02_share_unavailable");
      EXPECT_GT(block.at("decisions").at("applied").get<usize>(), 0U);
      EXPECT_EQ(why.at("decisions"), block.at("decisions").at("applied"));
      EXPECT_NE(why.at("first_error").get<std::string>().find("symmetric"), std::string::npos);
      EXPECT_TRUE(a.at("mv_aim").at("eigen_00_02_share").is_object());
      EXPECT_FALSE(a.at("mv_aim").contains("eigen_00_02_share_unavailable"));
      for (auto* doc : {&a, &b}) {
        (*doc)["mv_aim"].erase("eigen_00_02_share");
        (*doc)["mv_aim"].erase("eigen_00_02_share_unavailable");
      }
    }
    EXPECT_EQ(a, b) << name;
  }
}

// W1-MIN M-1: with the rule on, --book-workers 4 publishes the serial run's bytes (the target is
// formed once per decision on the calling thread; the capacity books read the same target).
TEST(MvAimRule, BookWorkersParity) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const auto pin = cli_store(dir.path / "risk", role, in);
  std::ostringstream out, err;
  ASSERT_EQ(nav(nav_line(in, dir.path / "one", rule_on(pin, {"--capacity-curve"})), out, err), 0)
      << err.str();
  ASSERT_EQ(nav(nav_line(in, dir.path / "four",
                         rule_on(pin, {"--capacity-curve", "--book-workers", "4"})),
                out, err),
            0)
      << err.str();
  EXPECT_GE(expect_same_tree(dir.path / "one", dir.path / "four"), 9U);
}

// The "mv_aim" block and the rule id +mv-aim-v1 only with the rule: summary.json carries the
// run's counts and prints; recipe.json, the holdings manifest and the capacity directory carry
// the parameters only. Without the rule no byte of any file names it.
TEST(MvAimRule, SummaryBlockOnlyWhenOn) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const auto pin = cli_store(dir.path / "risk", role, in);
  std::ostringstream out, err;
  const auto on = dir.path / "on";
  ASSERT_EQ(nav(nav_line(in, on, rule_on(pin, {"--capacity-curve", "--emit-holdings",
                                               (dir.path / "holdings").string()})),
                out, err),
            0)
      << err.str();
  const auto summary = read_json_file(on / "summary.json");
  EXPECT_EQ(summary.at("rule"), "aim-partial-v5+mv-aim-v1");
  const auto& block = summary.at("mv_aim");
  EXPECT_EQ(block.at("rule"), "mv-aim-v1");
  EXPECT_EQ(block.at("risk_model").at("manifest_sha256"), pin.sha);
  const auto& decisions = block.at("decisions");
  EXPECT_GT(decisions.at("formed").get<usize>(), 30U);
  EXPECT_EQ(decisions.at("applied"), decisions.at("formed"));
  EXPECT_GT(block.at("priced_names").at("mean").get<f64>(), 10.0);
  const f64 ratio = block.at("specific_vol_ratio").at("pooled").get<f64>();
  EXPECT_TRUE(std::isfinite(ratio) && ratio > 0) << ratio;
  for (const char* which : {"mv", "desired"}) {
    const auto& share = block.at("eigen_00_02_share").at(which);
    EXPECT_GT(share.at("n").get<usize>(), 0U) << which;
    EXPECT_GE(share.at("min").get<f64>(), 0.0) << which;
    EXPECT_LE(share.at("max").get<f64>(), 1.0 + 1e-12) << which;
  }
  EXPECT_FALSE(block.at("special_tier_negative").at("locate_in_aim").get<bool>());
  const auto recipe = read_json_file(on / "recipe.json");
  EXPECT_EQ(recipe.at("mv_aim"), (Json{{"rule", "mv-aim-v1"},
                                       {"declaration", ma::declaration()},
                                       {"risk_model", block.at("risk_model")}}));
  EXPECT_EQ(recipe.at("rule"), "aim-partial-v5+mv-aim-v1");
  const auto capacity = read_json_file(on / "capacity" / "summary.json");
  EXPECT_EQ(capacity.at("mv_aim"), recipe.at("mv_aim"));
  const auto manifest = read_json_file(dir.path / "holdings" / "manifest.json");
  EXPECT_EQ(manifest.at("mv_aim"), recipe.at("mv_aim"));
  // Without the rule.
  const auto off = dir.path / "off";
  ASSERT_EQ(nav(nav_line(in, off, {"--rule", "aim-partial-v5", "--capacity-curve"}), out, err), 0)
      << err.str();
  for (const auto& name : tree(off)) {
    const auto text = bytes_of(off / name);
    EXPECT_EQ(text.find("mv_aim"), std::string::npos) << name;
    EXPECT_EQ(text.find("mv-aim-v1"), std::string::npos) << name;
  }
  EXPECT_EQ(read_json_file(off / "summary.json").at("rule"), "aim-partial-v5");
}

// ---- the rule-absent identity ---------------------------------------------------------------

// The hook is the identity without an extension and with an extension that does not carry the
// rule (another v7 rule on); the replay without the rule is bit for bit the replay without the
// extension's target rule, and the rule moves it (teeth).
TEST(MvAimRule, FlagAbsentIdentity) {
  const Directory dir;
  const Role role(70, 12, 53);
  const auto store = store_of(dir.path / "s", role, std::vector<u8>(role.d, u8{1}));
  const auto x = role.target();
  const auto desired = desired_at(role, 30);
  auto copy = desired;
  ASSERT_TRUE(v7::apply_target_rule(x, 30, {}, copy));
  EXPECT_TRUE(same_bits(copy, desired));
  EXPECT_EQ(v7::extension_workspace_bytes(role.n), 0U);
  {
    v7::NavV7Options o;
    o.stress = true;
    o.spo_risk = store;
    const v7::ScopedNavExtension extension(o);
    ASSERT_TRUE(v7::apply_target_rule(x, 30, {}, copy));
    EXPECT_TRUE(same_bits(copy, desired));
    EXPECT_EQ(extension.mv_aim_rule(), nullptr);
    EXPECT_EQ(v7::extension_workspace_bytes(role.n), 0U);
  }
  const auto plain = st::replay_nav(role.nav(), nav_config());
  ASSERT_TRUE(plain) << plain.error().to_string();
  std::vector<std::vector<u64>> without;
  for (const auto& day : plain->days) without.push_back(day_bits(day));
  {
    const v7::ScopedNavExtension extension(v7::NavV7Options{});
    const auto hooked = st::replay_nav(role.nav(), nav_config());
    ASSERT_TRUE(hooked) << hooked.error().to_string();
    std::vector<std::vector<u64>> rows;
    for (const auto& day : hooked->days) rows.push_back(day_bits(day));
    EXPECT_EQ(rows, without);
  }
  const auto ruled = replay_with(role, store);
  ASSERT_TRUE(ruled) << ruled.error().to_string();
  std::vector<std::vector<u64>> rows;
  for (const auto& day : ruled->days) rows.push_back(day_bits(day));
  ASSERT_EQ(rows.size(), without.size());
  EXPECT_NE(rows, without);
}

// Ruling COVMV-1: nav_workspace_reserve_bytes gains ONE term, the rule's workspace_bytes, only
// while the rule is on (no extension, or an extension without it: the reserve is unchanged), so
// the rule's scratch is admitted by --max-bytes; the term saturates.
TEST(MvAimRule, WorkspaceReserveTermOnlyWithTheRule) {
  const auto base = nav_config();
  const usize books = st::nav_scenario_matrix(false).size(), names = 5000, sessions = 1000;
  const auto reserve = [&] {
    return st::nav_workspace_reserve_bytes(base, books, false, names, sessions);
  };
  const u64 absent = reserve();
  {
    const v7::ScopedNavExtension extension(v7::NavV7Options{});
    EXPECT_EQ(reserve(), absent);
  }
  {
    v7::NavV7Options o;
    o.capacity = true;
    o.stress = true;
    const v7::ScopedNavExtension extension(o);
    EXPECT_EQ(reserve(), absent);
  }
  {
    v7::NavV7Options o;
    o.mv_aim = true;
    const v7::ScopedNavExtension extension(o);
    EXPECT_EQ(reserve(), absent + ma::workspace_bytes(names));
  }
  EXPECT_EQ(reserve(), absent);
  EXPECT_GT(ma::workspace_bytes(names), names * (sp::risk_styles * 2U * sizeof(f64)));
  EXPECT_GE(ma::workspace_bytes(names),
            eb::mv_aim_working_bytes(names, sp::risk_industry_slots, sp::risk_styles));
  EXPECT_EQ(ma::workspace_bytes(std::numeric_limits<usize>::max()),
            std::numeric_limits<u64>::max());
}

// The primary book's all-rows mean gross leverage of a published run (its daily CSV, the mean
// PM6-6 matches; strategy_nav_calibrate_test.cpp's reading).
f64 primary_gross(const fs::path& run) {
  const auto label = read_json_file(run / "summary.json").at("primary_scenario").get<std::string>();
  std::ifstream in(run / ("daily_" + label + ".csv"));
  std::string line;
  std::getline(in, line);
  std::vector<std::string> header;
  {
    std::stringstream row(line);
    for (std::string cell; std::getline(row, cell, ',');) header.push_back(cell);
  }
  const auto column = static_cast<usize>(
      std::find(header.begin(), header.end(), "gross_leverage") - header.begin());
  EXPECT_LT(column, header.size());
  std::vector<f64> gross;
  while (std::getline(in, line)) {
    std::stringstream row(line);
    std::string cell;
    for (usize k = 0; k <= column && std::getline(row, cell, ',');) ++k;
    gross.push_back(std::stod(cell));
  }
  return st::numpy_mean(gross);
}

// PM6-6 in one process (P9 C2): --calibrate-gross runs with the rule (pass 1 forms the
// constructions, the published pass restores them; the target is 1.1 x the plain run's gross, so
// the one correction moves L 1.2 to about 1.32), and the published summary counts every decision
// once: its mv_aim block equals the plain run's (the rule's target does not read L).
TEST(MvAimRule, CalibrateGrossCountsEachDecisionOnce) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto in = write_run_inputs(dir.path, role);
  const auto pin = cli_store(dir.path / "risk", role, in);
  std::ostringstream out, err;
  ASSERT_EQ(nav(nav_line(in, dir.path / "plain", rule_on(pin)), out, err), 0) << err.str();
  const f64 g = primary_gross(dir.path / "plain");
  ASSERT_TRUE(std::isfinite(g) && g > 0) << g;
  std::ostringstream target;
  target.precision(17);
  target << 1.1 * g;
  ASSERT_EQ(nav(nav_line(in, dir.path / "matched",
                         rule_on(pin, {"--calibrate-gross", target.str()})),
                out, err),
            0)
      << err.str();
  const auto plain = read_json_file(dir.path / "plain" / "summary.json");
  const auto matched = read_json_file(dir.path / "matched" / "summary.json");
  ASSERT_TRUE(matched.contains("calibration"));
  EXPECT_EQ(matched.at("mv_aim"), plain.at("mv_aim"));
  EXPECT_GT(plain.at("mv_aim").at("decisions").at("formed").get<usize>(), 30U);
}

} // namespace
