#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_holdings.hpp"
#include "../src/strategy_live.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_replay_detail.hpp"
#include "../src/strategy_orders.hpp"
#include "../src/strategy_reconcile.hpp"
#include "../src/strategy_target_replay.hpp"

// v7 lane L3: --emit-holdings, the decide verb and the atx.book-deploy/v1 manifest.
// v7 lane W4: the f64 holdings layout, share orders, reconcile, freshness and the TC band.
// Synthetic roles only (no real data).
namespace {
using namespace atx;
namespace st = atx::impl::strategy;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();

u64 bits(f64 x) { return std::bit_cast<u64>(x); }
std::vector<i64> weekdays(usize count) {
  std::vector<i64> out;
  auto date = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2};
  while (out.size() < count) {
    const std::chrono::weekday wd{date};
    if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
      out.push_back(static_cast<i64>(date.time_since_epoch().count()) * day_ns);
    date += std::chrono::days{1};
  }
  return out;
}
std::string date_of(i64 session) {
  const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{session / day_ns}}};
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year()) << '-'
      << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-' << std::setw(2)
      << static_cast<unsigned>(ymd.day());
  return out.str();
}
struct Lcg {
  u64 state;
  f64 next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * 0x1.0p-53;
  }
};
// Date-major synthetic role on weekdays from 2020-01-02 (member => present, NaN
// nonmember signals, absent cells NaN prices/volume).
struct Panel {
  usize d{}, n{}, begin{}, end{};
  std::vector<f64> signal, close, raw, volume;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Panel(usize dates, usize names)
      : d(dates), n(names), end(dates), signal(dates * names, 0.0), close(dates * names, 50.0),
        raw(dates * names, 50.0), volume(dates * names, 1e6), member(dates * names, 1),
        present(dates * names, 1), sessions(weekdays(dates)), ids(names) {
    for (usize i = 0; i < n; ++i) ids[i] = 1000 + 7 * i;
  }
  [[nodiscard]] usize k(usize t, usize i) const { return t * n + i; }
  void absent(usize t, usize i) {
    const auto c = k(t, i);
    present[c] = 0; member[c] = 0; signal[c] = missing;
    close[c] = missing; raw[c] = missing; volume[c] = missing;
  }
  void nonmember(usize t, usize i) { member[k(t, i)] = 0; signal[k(t, i)] = missing; }
  [[nodiscard]] st::TargetReplayInput target() const {
    return {d, n, begin, end, signal, member, sessions, ids, close, raw, present, volume};
  }
};
// One common factor (loadings .5-1.7) plus idiosyncratic noise, share volume `shares`
// x U(.5, 1.5), random signals; one short absence run (name 3), a name that leaves the
// universe (name 5, from 60% of the sessions on: the decaying exit) and a two-session
// nonmember gap (name 7).
Panel market_panel(usize dates, usize names, u64 seed, f64 shares) {
  Panel p(dates, names);
  Lcg g{seed};
  std::vector<f64> level(names);
  for (auto& v : level) v = 20 + 60 * g.next();
  for (usize t = 0; t < dates; ++t) {
    const f64 common = 0.02 * (g.next() - 0.5);
    for (usize i = 0; i < names; ++i) {
      const f64 loading = 0.5 + 0.2 * static_cast<f64>(i % 7);
      if (t) level[i] *= 1 + loading * common + 0.012 * (g.next() - 0.5);
      p.close[p.k(t, i)] = level[i]; p.raw[p.k(t, i)] = level[i];
      p.volume[p.k(t, i)] = shares * (0.5 + g.next());
      p.signal[p.k(t, i)] = g.next() - 0.5;
    }
  }
  const usize mid = dates / 2, late = dates * 3 / 5;
  for (usize t = mid; t < mid + 3; ++t) p.absent(t, 3);
  for (usize t = late; t < dates; ++t) p.nonmember(t, 5);
  for (usize t = mid + 5; t < mid + 7; ++t) p.nonmember(t, 7);
  return p;
}
// Shares out 1e6..1e9 and SI ratio 0..0.25 (5% missing): mixed GC / warm / special tiers.
struct Fields {
  std::vector<f64> shares_out, si_shares;
  Fields(const Panel& p, u64 seed) : shares_out(p.d * p.n), si_shares(p.d * p.n) {
    Lcg g{seed};
    for (usize k = 0; k < shares_out.size(); ++k) {
      shares_out[k] = std::pow(10.0, 6 + 3 * g.next());
      si_shares[k] = shares_out[k] * 0.25 * g.next();
      if (g.next() < 0.05) si_shares[k] = missing;
    }
  }
  [[nodiscard]] st::NavFinancingFields view() const { return {shares_out, si_shares}; }
};

// ---- pinned artifacts (the NAV fixtures' layouts) ----
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_live_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!std::filesystem::create_directory(path))
      throw std::runtime_error("unique fixture directory");
  }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};
template<class T>
Json write_payload(const std::filesystem::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
std::string write_json(const std::filesystem::path& path, const Json& value) {
  std::ofstream out(path, std::ios::binary); out << value.dump(2) << '\n'; out.close();
  if (!out) throw std::runtime_error("fixture JSON write");
  return co::sha256_file(path.string()).value();
}
Json read_json(const std::filesystem::path& path) {
  std::ifstream in(path); Json j; in >> j; return j;
}
std::string file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::vector<std::string> lines(const std::filesystem::path& path) {
  std::ifstream in(path); std::vector<std::string> out; std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}
const std::string pin(64, 'a');
const std::string code_pin(64, 'c');
constexpr const char* member_semantics =
    "decision-member-and-source-present-and-finite-positive-close;"
    "independent-of-component-coverage";
struct Artifact {
  st::TargetReplayRunConfig cfg;
  Json role;
  st::NavFieldsPin fields;
};
// The combined blend, the price role (with volume) and the role fields
// (atx.research-role-fields/v1 with code_sha256), every provenance pin `pin`. score_begin
// (default 0) is the role's and the blend's (a warm start needs pre-score history).
Artifact write_artifact(const std::filesystem::path& dir, const Panel& p, const Fields& f,
                        usize score_begin = 0) {
  std::vector<u8> finite(p.signal.size()); u64 finite_count = 0, members = 0;
  for (usize k = 0; k < finite.size(); ++k) {
    finite[k] = static_cast<u8>(std::isfinite(p.signal[k]));
    finite_count += finite[k]; members += p.member[k];
  }
  Json files;
  files["train_combined.f64"] = write_payload(dir / "train_combined.f64", p.signal);
  files["train_combined_member.u8"] = write_payload(dir / "train_combined_member.u8", p.member);
  files["train_combined_finite.u8"] = write_payload(dir / "train_combined_finite.u8", finite);
  files["train_combined_sessions.i64"] =
      write_payload(dir / "train_combined_sessions.i64", p.sessions);
  files["train_combined_ids.u64"] = write_payload(dir / "train_combined_ids.u64", p.ids);
  Json role_files;
  role_files["sessions.i64"] = write_payload(dir / "sessions.i64", p.sessions);
  role_files["ids.u64"] = write_payload(dir / "ids.u64", p.ids);
  role_files["close.f64"] = write_payload(dir / "close.f64", p.close);
  role_files["raw_close.f64"] = write_payload(dir / "raw_close.f64", p.raw);
  role_files["present.u8"] = write_payload(dir / "present.u8", p.present);
  role_files["member.u8"] = write_payload(dir / "member.u8", p.member);
  role_files["volume.f64"] = write_payload(dir / "volume.f64", p.volume);
  Artifact a;
  a.role = Json{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"source_sha256", pin}, {"instrument_namespace", "spiderrock.securityID"},
      {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
      {"volume_basis", "raw-share-volume"},
      {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
      {"common_stock_verified", false}, {"historical_vintage_verified", false},
      {"dates", p.d}, {"instruments", p.n}, {"score_begin", score_begin}, {"score_end", p.d},
      {"files", role_files}};
  a.cfg.role_path = (dir / "role.json").string();
  a.cfg.role_sha256 = write_json(a.cfg.role_path, a.role);
  const Json manifest{{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"},
      {"role", "train"}, {"layout", "date-major-little-endian"}, {"dates", p.d},
      {"instruments", p.n}, {"score_begin", score_begin}, {"score_end", p.d},
      {"role_manifest_sha256", a.cfg.role_sha256}, {"source_sha256", pin},
      {"library_sha256", pin}, {"train_manifest_sha256", pin}, {"run_recipe_sha256", pin},
      {"orientation_candidates_sha256", pin}, {"orientations_artifact_sha256", nullptr},
      {"role_window_required", true},
      {"signal_semantics", "exact-pre-target-composition;equal-family/equal-within;"
                           "missing-or-unoriented-neutral-fixed-denominator"},
      {"member_semantics", member_semantics},
      {"finite_semantics",
       "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal"},
      {"actual_trades_or_returns", false}, {"finite_cells", finite_count},
      {"member_cells", members}, {"files", files}};
  a.cfg.combined_path = (dir / "train_combined.json").string();
  a.cfg.combined_sha256 = write_json(a.cfg.combined_path, manifest);
  const auto root = dir / "fields";
  if (!std::filesystem::create_directory(root)) throw std::runtime_error("fields directory");
  Json field_files, entries = Json::array();
  for (const auto& [name, values] : {std::pair{"shares_out", &f.shares_out},
                                      std::pair{"si_shares", &f.si_shares}}) {
    const std::string file = std::string(name) + ".f64";
    field_files[file] = write_payload(root / file, *values);
    entries.push_back({{"name", name}, {"file", file}, {"dtype", "<f8"},
        {"layout", "date-major"}, {"shape", Json::array({p.d, p.n})},
        {"sha256", field_files[file].at("sha256")}, {"point_in_time", true},
        {"clock", std::string("fixture-") + name}});
  }
  const auto& receipts = a.role.at("files");
  const Json fields{{"schema", "atx.research-role-fields/v1"}, {"status", "complete"},
      {"code_sha256", code_pin},
      {"role", {{"manifest_sha256", a.cfg.role_sha256},
                {"sessions_sha256", receipts.at("sessions.i64").at("sha256")},
                {"ids_sha256", receipts.at("ids.u64").at("sha256")},
                {"member_sha256", receipts.at("member.u8").at("sha256")}}},
      {"fields", entries}, {"files", field_files}};
  a.fields.manifest_path = (root / "manifest.json").string();
  a.fields.manifest_sha256 = write_json(a.fields.manifest_path, fields);
  return a;
}
int nav_cli(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  return st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err);
}
int decide_cli(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  return st::dispatch_decide(static_cast<int>(argv.size()), argv.data(), out, err);
}
// The v6.1 cell's construction and execution flags (theta .3 instead of .05, so a short
// synthetic book moves) under the CLI's default price-risk-v1 windows.
const std::vector<std::string> book_flags{
    "--rule", "aim-partial-v5", "--cadence", "1", "--trade-fraction", ".3",
    "--dust-multiple", ".1", "--aim-leverage", "1.247", "--neutralize", "price-risk-v1",
    "--order-basis", "delta", "--exit-rate", ".05", "--locate-in-aim", "--liquidity-cache",
    "--daily-turnover-mean-max", ".20", "--daily-turnover-p95-max", ".30",
    "--max-bytes", "1073741824"};
std::vector<std::string> nav_args(const Artifact& a, const std::filesystem::path& out,
                                  std::vector<std::string> extra = {}) {
  std::vector<std::string> all{"nav", "--combined", a.cfg.combined_path, "--combined-sha256",
      a.cfg.combined_sha256, "--role", a.cfg.role_path, "--role-sha256", a.cfg.role_sha256,
      "--fields", a.fields.manifest_path, "--fields-sha256", a.fields.manifest_sha256,
      "--output", out.string()};
  all.insert(all.end(), book_flags.begin(), book_flags.end());
  all.insert(all.end(), extra.begin(), extra.end());
  return all;
}
const std::string exe_pin(64, 'e');
Json deploy_manifest(const Artifact& a, const std::string& recipe_sha) {
  return Json{{"schema", "atx.book-deploy/v1"}, {"book", "synthetic-v61-s2"},
      {"library", {{"sha256", pin}}}, {"recipe", {{"sha256", pin}}},
      {"orientations", {{"sha256", pin}}},
      {"composition", {{"scheme", "equal-family"}, {"weights_sha256", nullptr}}},
      {"combined", {{"path", a.cfg.combined_path}, {"sha256", a.cfg.combined_sha256}}},
      {"role", {{"path", a.cfg.role_path}, {"sha256", a.cfg.role_sha256}, {"class", "train"}}},
      {"fields", {{"path", a.fields.manifest_path}, {"sha256", a.fields.manifest_sha256},
                  {"code_sha256", code_pin},
                  {"names", Json::array({"shares_out", "si_shares"})}}},
      {"data_source_sha256", pin}, {"universe", {{"member_semantics", member_semantics}}},
      {"nav", {{"rule", "aim-partial-v5"}, {"cadence", 1}, {"trade_fraction", 0.3},
               {"dust_multiple", 0.1}, {"aim_leverage", 1.247}, {"neutralize", "price-risk-v1"},
               {"band_multiple", 0.0}, {"monthly_budget", 0.3}, {"exit_rate", 0.05},
               {"order_basis", "delta"}, {"locate_in_aim", true}, {"liquidity_cache", true},
               {"rate", "fixed"}, {"daily_turnover_mean_max", 0.2},
               {"daily_turnover_p95_max", 0.3}, {"max_working_bytes", 1073741824},
               {"recipe_sha256", recipe_sha}}},
      {"executables", {{"atx-equity-strategy-targets", exe_pin},
                       {"atx-equity-strategy-ic", pin}}},
      {"source", {{"git_sha", std::string(40, 'b')}}},
      {"seal", {{"policy", st::research_seal_policy},
                {"exclusive_session", st::research_seal_session}}},
      {"owner_gate", nullptr},
      {"health", {{"gross_leverage", Json::array({0.0, 3.0})}, {"abs_net_leverage_max", 0.5},
                  {"planned_turnover_max", 3.0}, {"names_without_locate_max", 1000}}}};
}
// 60 names x 200 sessions (price-risk-v1's default 252/126 windows and 50 names neutralize
// from about session 127), the v6.1 flags, NAV 1e9 with ample volume (capped fills in the
// first sessions), a NAV run with --emit-holdings and the deploy manifest pinned to it.
struct Fixture {
  Panel panel;
  Fields fields;
  Artifact artifact;
  std::filesystem::path nav_dir, holdings_dir, deploy_path;
  Json deploy;
};
// `extra`: further nav flags (the deploy manifest's nav block then needs the matching keys).
Fixture make_fixture(const std::filesystem::path& dir, std::vector<std::string> extra = {}) {
  auto panel = market_panel(200, 60, 17, 3e7);
  Fields fields(panel, 23);
  auto artifact = write_artifact(dir, panel, fields);
  Fixture fx{std::move(panel), std::move(fields), std::move(artifact), dir / "nav",
             dir / "holdings", dir / "deploy.json", Json{}};
  std::ostringstream out, err;
  extra.emplace_back("--emit-holdings");
  extra.push_back(fx.holdings_dir.string());
  if (nav_cli(nav_args(fx.artifact, fx.nav_dir, std::move(extra)), out, err) != 0)
    throw std::runtime_error("fixture nav run: " + err.str());
  const auto recipe = read_json(fx.nav_dir / "summary.json").at("recipe_sha256");
  fx.deploy = deploy_manifest(fx.artifact, recipe.get<std::string>());
  write_json(fx.deploy_path, fx.deploy);
  return fx;
}
st::DecideConfig decide_config(const Fixture& fx, usize d, const std::filesystem::path& out) {
  st::DecideConfig cfg;
  cfg.deploy_path = fx.deploy_path.string();
  cfg.asof = date_of(fx.panel.sessions[d]);
  cfg.positions_path = fx.holdings_dir.string(); // the f64 layout (the default emit)
  cfg.output_directory = out.string();
  cfg.executable_sha256 = exe_pin;
  cfg.build_source_sha = std::string(40, 'b');
  cfg.allow_stale = true; // TRAIN-window decisions are not the role's last session
  return cfg;
}
// A deploy manifest edit refused by run_decide with a message containing `expected`.
template<class Edit>
void expect_refused(const Fixture& fx, const std::filesystem::path& dir, const std::string& label,
                    Edit edit, const std::string& expected, usize d = 150) {
  auto m = fx.deploy;
  edit(m);
  const auto path = dir / ("deploy-" + label + ".json");
  write_json(path, m);
  auto cfg = decide_config(fx, d, dir / ("refused-" + label));
  cfg.deploy_path = path.string();
  std::ostringstream progress;
  const auto outcome = st::run_decide(cfg, progress);
  ASSERT_FALSE(outcome) << label;
  EXPECT_NE(outcome.error().to_string().find(expected), std::string::npos)
      << label << ": " << outcome.error().to_string();
  EXPECT_FALSE(std::filesystem::exists(cfg.output_directory)) << label;
}

// ---- library-level fixtures: the v6.1 construction on short price-risk windows ----
st::NavReplayConfig v61_book(f64 nav) {
  st::NavReplayConfig c;
  c.target.rule = st::TargetReplayRule::AimPartialV5; c.target.cadence = 1;
  c.target.trade_fraction = 0.3; c.target.dust_multiple = 0.1; c.target.aim_leverage = 1.247;
  c.target.exit_rate = 0.05;
  c.target.neutralize = st::TargetNeutralize::PriceRiskV1;
  c.target.price_risk.beta_window = 40; c.target.price_risk.vol_window = 20;
  c.target.price_risk.adv_window = 10; c.target.price_risk.min_return_pairs = 20;
  c.target.price_risk.min_names = 5;
  c.order_basis = st::NavOrderBasis::Delta; c.locate_in_aim = true; c.liquidity_cache = true;
  c.scenario = st::nav_scenario_matrix(true)[st::nav_primary_scenario_index];
  c.initial_nav = nav;
  return c;
}
// Every session the observed book reported, by role row.
struct Recorder final : st::NavHoldingsSink {
  std::map<usize, std::pair<st::NavReplayDay, std::vector<st::NavHolding>>> sessions;
  co::Status session(const st::NavReplayDay& day, std::span<const st::NavHolding> names) override {
    sessions[day.session_index] = {day, std::vector<st::NavHolding>(names.begin(), names.end())};
    return co::Ok();
  }
};
// The replay's end-of-session holding per name at row d (+0 when not reported).
std::vector<f64> held_at(const Recorder& r, usize d, usize names) {
  std::vector<f64> held(names, 0.0);
  for (const auto& h : r.sessions.at(d).second) held[h.index] = h.held_dollars;
  return held;
}
} // namespace

// (1) Flag off is the old path and the observer changes nothing: a primary book observed
// by a sink is bit-identical to the unobserved replay, and the reported rows reconcile to
// the book's own day row bit for bit (same summation order: longs, shorts, fills).
TEST(StrategyLive, HoldingsObserverIsReadOnlyAndReconcilesToTheDayRow) {
  const auto p = market_panel(70, 12, 5, 2e5);
  const Fields f(p, 9);
  const st::NavReplayInput in{p.target(), p.volume, f.view()};
  const auto cfg = v61_book(1e7);
  const std::array<st::NavScenario, 1> one{cfg.scenario};
  Recorder recorder;
  const auto plain = st::replay_nav_scenarios(in, cfg, one);
  const auto seen = st::replay_nav_scenarios(in, cfg, one, recorder, 0);
  ASSERT_TRUE(plain) << plain.error().to_string();
  ASSERT_TRUE(seen) << seen.error().to_string();
  const auto& a = plain->front().days; const auto& b = seen->front().days;
  ASSERT_EQ(a.size(), b.size());
  usize capped = 0;
  for (usize t = 0; t < a.size(); ++t) {
    EXPECT_EQ(bits(a[t].pretrade_nav), bits(b[t].pretrade_nav)) << t;
    EXPECT_EQ(bits(a[t].posttrade_nav), bits(b[t].posttrade_nav)) << t;
    EXPECT_EQ(bits(a[t].traded_dollars), bits(b[t].traded_dollars)) << t;
    EXPECT_EQ(bits(a[t].planned_gross), bits(b[t].planned_gross)) << t;
    EXPECT_EQ(bits(a[t].blocked_short_dollars), bits(b[t].blocked_short_dollars)) << t;
    EXPECT_EQ(a[t].fills, b[t].fills) << t;
    const auto& day = b[t];
    if (!day.decision && !day.executed) {
      EXPECT_FALSE(recorder.sessions.count(day.session_index)) << t;
      continue;
    }
    const auto& rows = recorder.sessions.at(day.session_index).second;
    f64 longs = 0, shorts = 0, traded = 0;
    usize fills = 0, placed = 0, blocked = 0;
    for (const auto& h : rows) {
      if (h.held_dollars > 0) longs += h.held_dollars;
      if (h.held_dollars < 0) shorts += -h.held_dollars;
      const bool filled = h.fill == st::NavFillStatus::Complete ||
                          h.fill == st::NavFillStatus::Capped;
      if (filled) { traded += std::abs(h.filled_dollars); ++fills; }
      capped += h.fill == st::NavFillStatus::Capped ? 1U : 0U;
      blocked += h.locate_blocked ? 1U : 0U;
      placed += h.order_placed ? 1U : 0U;
      EXPECT_EQ(bits(h.held_weight), bits(h.held_dollars / day.posttrade_nav));
      if (!day.decision) {
        EXPECT_TRUE(std::isnan(h.target_weight));
      }
    }
    EXPECT_EQ(bits(longs), bits(day.long_dollars)) << t;
    EXPECT_EQ(bits(shorts), bits(day.short_dollars)) << t;
    EXPECT_EQ(bits(traded), bits(day.traded_dollars)) << t;
    EXPECT_EQ(fills, day.fills) << t;
    EXPECT_LE(placed, rows.size()) << t;
    // A plan block that lands on +0 with nothing held leaves no row, so rows <= count.
    EXPECT_LE(blocked, day.blocked_short_names) << t;
  }
  EXPECT_GT(capped, 0U); // the participation cap binds in the fixture
}

// (2) B2 parity at the library level: at three rebalance decisions nav_decide, fed the
// replay's end-of-session holdings and post-trade NAV of that row, returns the replay's
// rule plan and planned weights bit for bit (every name, an unreported name is +0), with
// the same construction record and tiers.
TEST(StrategyLive, NavDecideReproducesReplayPlannedWeightsBitForBit) {
  const auto p = market_panel(70, 12, 5, 2e5);
  const Fields f(p, 9);
  const st::NavReplayInput in{p.target(), p.volume, f.view()};
  const auto cfg = v61_book(1e7);
  const std::array<st::NavScenario, 1> one{cfg.scenario};
  Recorder recorder;
  ASSERT_TRUE(st::replay_nav_scenarios(in, cfg, one, recorder, 0));
  for (const usize d : {usize{30}, usize{45}, usize{60}}) {
    const auto& [day, rows] = recorder.sessions.at(d);
    ASSERT_TRUE(day.decision && day.rebalance) << d;
    const auto held = held_at(recorder, d, p.n);
    ASSERT_TRUE(std::any_of(held.begin(), held.end(), [](f64 h) { return h != 0; })) << d;
    const auto dec = st::detail::nav_decide(in, cfg, d, held, day.posttrade_nav);
    ASSERT_TRUE(dec) << dec.error().to_string();
    std::vector<f64> target(p.n, 0.0), rule(p.n, 0.0);
    for (const auto& h : rows) { target[h.index] = h.target_weight; rule[h.index] = h.rule_weight; }
    for (usize i = 0; i < p.n; ++i) {
      EXPECT_EQ(bits(dec->target[i]), bits(target[i])) << d << " name " << i;
      EXPECT_EQ(bits(dec->rule[i]), bits(rule[i])) << d << " name " << i;
    }
    for (const auto& h : rows) {
      EXPECT_EQ(bits(dec->desired[h.index]), bits(h.desired)) << d << " name " << h.index;
      EXPECT_EQ(dec->tier[h.index], h.tier) << d;
    }
    EXPECT_TRUE(dec->rebalance);
    EXPECT_EQ(dec->construction.neutralize, day.construction.neutralize);
    EXPECT_EQ(dec->construction.locate_zeroed, day.construction.locate_zeroed);
    EXPECT_EQ(dec->construction.banded_names, day.construction.banded_names);
    EXPECT_EQ(bits(dec->plan.turnover), bits(day.planned_turnover));
    EXPECT_EQ(bits(dec->plan.gross), bits(day.planned_gross));
    EXPECT_EQ(dec->blocked_short_names, day.blocked_short_names);
    EXPECT_EQ(bits(dec->blocked_short_dollars), bits(day.blocked_short_dollars));
  }
}

// B2 on the last rows (the replay never decides on its final two) and the book-state
// refusals: nav_decide decides at end-1; rate per-name-v1 and monthly-budget-v2 are
// refused; a no-locate name cannot open or grow a short (aim mask + post-rule block).
TEST(StrategyLive, NavDecideLastRowRefusalsAndLocateMask) {
  const auto p = market_panel(70, 12, 5, 2e5);
  const Fields f(p, 9);
  const st::NavReplayInput in{p.target(), p.volume, f.view()};
  const auto cfg = v61_book(1e7);
  const std::array<st::NavScenario, 1> one{cfg.scenario};
  Recorder recorder;
  ASSERT_TRUE(st::replay_nav_scenarios(in, cfg, one, recorder, 0));
  const usize last = p.d - 1, d = 60;
  const auto& executed = recorder.sessions.at(p.d - 2).first; // execution-only session
  const auto final_row = st::detail::nav_decide(in, cfg, last, held_at(recorder, p.d - 2, p.n),
                                                executed.posttrade_nav);
  ASSERT_TRUE(final_row) << final_row.error().to_string();
  EXPECT_TRUE(final_row->cadence);
  const auto held = held_at(recorder, d, p.n);
  const f64 nav = recorder.sessions.at(d).first.posttrade_nav;
  auto per_name = cfg; per_name.rate = st::NavRateRule::PerNameV1;
  EXPECT_FALSE(st::detail::nav_decide(in, per_name, d, held, nav));
  auto budget = cfg; budget.target.rule = st::TargetReplayRule::MonthlyTargetBudgetV2;
  budget.target.dust_multiple = 0; budget.target.aim_leverage = 1; budget.target.exit_rate = 1;
  EXPECT_FALSE(st::detail::nav_decide(in, budget, d, held, nav));
  EXPECT_FALSE(st::detail::nav_decide(in, cfg, d, held, -1.0));
  EXPECT_FALSE(st::detail::nav_decide(in, cfg, d, held, nav, std::vector<u8>(p.n - 1)));
  // A decision where a non-special name's plan grows a short (target < min(current, 0));
  // without a locate it may not: the block holds it at the floor.
  bool tested = false;
  for (usize row = 25; row + 2 < p.d && !tested; ++row) {
    const auto at = held_at(recorder, row, p.n);
    const f64 post = recorder.sessions.at(row).first.posttrade_nav;
    const auto base = st::detail::nav_decide(in, cfg, row, at, post);
    ASSERT_TRUE(base) << base.error().to_string();
    for (usize j = 0; j < p.n && !tested; ++j) {
      if (base->tier[j] == 3 || !(base->target[j] < std::min(base->current[j], 0.0))) continue;
      std::vector<u8> no_locate(p.n, 0);
      no_locate[j] = 1;
      const auto masked = st::detail::nav_decide(in, cfg, row, at, post, no_locate);
      ASSERT_TRUE(masked) << masked.error().to_string();
      EXPECT_EQ(masked->no_short[j], 1U);
      EXPECT_GE(masked->target[j], std::min(masked->current[j], 0.0)) << row << " name " << j;
      EXPECT_NE(bits(masked->target[j]), bits(base->target[j]));
      tested = true;
    }
  }
  EXPECT_TRUE(tested);
}

// (1) at the CLI: --emit-holdings leaves every NAV output byte-identical (run twice,
// compare bytes), and publishes the f64 layout (holdings.f64, holdings_index.json) and
// holdings_days.csv with manifest.json (v2) last binding their SHAs and the NAV recipe SHA.
TEST(StrategyLive, EmitHoldingsLeavesNavOutputsByteIdentical) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  std::ostringstream out, err;
  ASSERT_EQ(nav_cli(nav_args(fx.artifact, dir.path / "plain"), out, err), 0) << err.str();
  std::vector<std::string> names;
  for (const auto& e : std::filesystem::directory_iterator(dir.path / "plain"))
    names.push_back(e.path().filename().string());
  ASSERT_FALSE(names.empty());
  for (const auto& name : names)
    EXPECT_TRUE(file_bytes(dir.path / "plain" / name) == file_bytes(fx.nav_dir / name)) << name;
  EXPECT_EQ(names.size(), static_cast<usize>(std::distance(
                              std::filesystem::directory_iterator(fx.nav_dir),
                              std::filesystem::directory_iterator{})));
  const auto manifest = read_json(fx.holdings_dir / "manifest.json");
  EXPECT_EQ(manifest.at("schema"), "atx.nav-holdings/v2");
  EXPECT_EQ(manifest.at("status"), "complete");
  EXPECT_EQ(manifest.at("format").at("id"), "f64");
  EXPECT_EQ(manifest.at("book"), "modeled-1bn-stale5-v1+swap-fin-v1");
  EXPECT_EQ(manifest.at("nav_recipe_sha256"),
            read_json(fx.nav_dir / "summary.json").at("recipe_sha256"));
  for (const auto* file : {"holdings.f64", "holdings_index.json", "holdings_days.csv"})
    EXPECT_EQ(manifest.at("files").at(file),
              co::sha256_file((fx.holdings_dir / file).string()).value()) << file;
  EXPECT_FALSE(std::filesystem::exists(fx.holdings_dir / "holdings.csv"));
  const auto index = read_json(fx.holdings_dir / "holdings_index.json");
  const auto days = lines(fx.holdings_dir / "holdings_days.csv");
  EXPECT_EQ(index.at("schema"), "atx.nav-holdings-f64/v1");
  const auto rows = index.at("data").at("rows").get<u64>();
  ASSERT_GT(rows, 0U);
  EXPECT_EQ(index.at("data").at("sha256"), manifest.at("files").at("holdings.f64"));
  EXPECT_EQ(std::filesystem::file_size(fx.holdings_dir / "holdings.f64"),
            rows * st::holdings::row_width * sizeof(f64));
  EXPECT_EQ(index.at("instrument_ids").size(), fx.panel.n);
  EXPECT_EQ(manifest.at("rows").get<u64>(), rows);
  EXPECT_EQ(manifest.at("sessions").get<usize>(), days.size() - 1);
  EXPECT_EQ(index.at("sessions").size(), days.size() - 1);
  EXPECT_EQ(days.size() - 1, fx.panel.d - 1); // sessions [begin, end-2]
  // Before price-risk-v1 has 126 return pairs every cadence rebalance is skipped: the
  // skips are counted (B9) instead of passing silently.
  EXPECT_GT(manifest.at("neutralize_skipped_rebalances").get<usize>(), 0U);
  std::ostringstream out2, err2;
  EXPECT_NE(nav_cli(nav_args(fx.artifact, dir.path / "again",
                             {"--emit-holdings", fx.holdings_dir.string()}), out2, err2), 0);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "again"));
}

// (2) B2 root acceptance at the file level: decide --positions <the replay's holdings.csv>
// --check-replay at three TRAIN-window decisions reproduces the replay's target weights
// bit for bit on every name, through the full deploy-manifest path (all pins verified,
// the NAV recipe recomputed), and writes targets.csv, orders.csv and decision.json last.
TEST(StrategyLive, DecideFromEmittedHoldingsEqualsReplayTargetsAtThreeDates) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  for (const usize d : {usize{150}, usize{170}, usize{190}}) {
    auto cfg = decide_config(fx, d, dir.path / ("decide-" + std::to_string(d)));
    cfg.check_replay = true;
    std::ostringstream progress;
    const auto outcome = st::run_decide(cfg, progress);
    ASSERT_TRUE(outcome) << outcome.error().to_string();
    EXPECT_TRUE(outcome->parity_checked);
    EXPECT_EQ(outcome->parity_names, fx.panel.n);
    EXPECT_EQ(outcome->parity_mismatches, 0U) << d;
    EXPECT_NE(outcome->health, st::DecideHealth::Error) << d;
    const auto out = std::filesystem::path(cfg.output_directory);
    const auto summary = read_json(out / "decision.json");
    EXPECT_EQ(summary.at("schema"), "atx.book-decision/v1");
    EXPECT_EQ(summary.at("replay_parity").at("mismatches"), 0);
    EXPECT_EQ(summary.at("decision").at("rebalance"), true);
    EXPECT_EQ(summary.at("decision").at("neutralize"), "applied");
    EXPECT_GT(summary.at("positions").at("rows").get<usize>(), 0U);
    EXPECT_GT(summary.at("orders").at("count").get<usize>(), 0U);
    EXPECT_GT(summary.at("transfer_coefficient").at("signal_names").get<usize>(), 2U);
    for (const auto* key : {"transfer_coefficient", "desired_over_sigma_target",
                            "signal_over_variance_target"}) {
      const auto tc = summary.at("transfer_coefficient").at(key);
      ASSERT_TRUE(tc.is_number()) << key;
      EXPECT_LE(std::abs(tc.get<f64>()), 1.0) << key;
    }
    EXPECT_EQ(summary.at("pins_verified").at("nav_recipe_sha256"),
              fx.deploy.at("nav").at("recipe_sha256"));
    EXPECT_EQ(summary.at("files").at("orders.csv"),
              co::sha256_file((out / "orders.csv").string()).value());
    EXPECT_EQ(lines(out / "orders.csv").size(),
              summary.at("orders").at("count").get<usize>() + 1);
  }
  // The live use: decide on the final row (the replay never decides there; the role's
  // last session, so fresh without --allow-stale) from a plain positions file and an
  // explicit NAV.
  const usize held_row = fx.panel.d - 2;
  const auto held = st::holdings::read_session(fx.holdings_dir.string(),
                                               fx.panel.sessions[held_row]);
  ASSERT_TRUE(held) << held.error().to_string();
  ASSERT_FALSE(held->names.empty());
  std::ofstream positions(dir.path / "positions.csv", std::ios::binary);
  positions.imbue(std::locale::classic()); positions << std::setprecision(17);
  positions << "instrument_id,held_dollars\n";
  for (const auto& h : held->names) positions << h.instrument_id << ',' << h.held_dollars << '\n';
  positions.close();
  auto live = decide_config(fx, fx.panel.d - 1, dir.path / "decide-last");
  live.positions_path = (dir.path / "positions.csv").string();
  live.nav = held->session.nav_post;
  live.allow_stale = false;
  std::ostringstream progress;
  const auto last = st::run_decide(live, progress);
  ASSERT_TRUE(last) << last.error().to_string();
  EXPECT_FALSE(last->parity_checked);
  const auto summary = read_json(dir.path / "decide-last" / "decision.json");
  bool fresh = false;
  for (const auto& c : summary.at("health").at("checks")) {
    if (c.at("check") != "data_freshness") continue;
    fresh = true;
    EXPECT_EQ(c.at("status"), "ok");
    EXPECT_EQ(c.at("sessions_behind"), 0);
  }
  EXPECT_TRUE(fresh);
}

// B1: every required pin, removed, is refused by name before anything is written.
TEST(StrategyLive, DeployManifestRefusesEveryMissingPin) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  const std::vector<std::pair<std::string, std::string>> pins{
      {"", "book"}, {"library", "sha256"}, {"recipe", "sha256"}, {"orientations", "sha256"},
      {"composition", "scheme"}, {"composition", "weights_sha256"}, {"combined", "path"},
      {"combined", "sha256"}, {"role", "path"}, {"role", "sha256"}, {"role", "class"},
      {"fields", "path"}, {"fields", "sha256"}, {"fields", "code_sha256"},
      {"fields", "names"}, {"", "data_source_sha256"}, {"universe", "member_semantics"},
      {"nav", "recipe_sha256"}, {"executables", "atx-equity-strategy-targets"},
      {"executables", "atx-equity-strategy-ic"}, {"source", "git_sha"}, {"seal", "policy"},
      {"seal", "exclusive_session"}, {"", "owner_gate"}};
  usize k = 0;
  for (const auto& entry : pins) {
    const std::string object = entry.first, key = entry.second;
    const auto name = object.empty() ? key : object + "." + key;
    expect_refused(fx, dir.path, "missing-" + std::to_string(k++),
                   [object, key](Json& m) { (object.empty() ? m : m[object]).erase(key); },
                   "missing pin " + name);
  }
  expect_refused(fx, dir.path, "missing-library-object", [](Json& m) { m.erase("library"); },
                 "missing pin library.sha256");
  for (const auto* key : {"rule", "cadence", "trade_fraction", "dust_multiple", "aim_leverage",
                          "neutralize", "exit_rate", "order_basis", "locate_in_aim", "rate",
                          "max_working_bytes"})
    expect_refused(fx, dir.path, std::string("missing-nav-") + key,
                   [key](Json& m) { m["nav"].erase(key); }, std::string("nav.") + key);
  expect_refused(fx, dir.path, "missing-health", [](Json& m) { m.erase("health"); },
                 "health bands");
  expect_refused(fx, dir.path, "malformed-sha",
                 [](Json& m) { m["library"]["sha256"] = "ABC"; }, "malformed pin library.sha256");
  expect_refused(fx, dir.path, "malformed-git",
                 [](Json& m) { m["source"]["git_sha"] = std::string(64, 'b'); },
                 "malformed pin source.git_sha");
  expect_refused(fx, dir.path, "schema", [](Json& m) { m["schema"] = "atx.book-deploy/v0"; },
                 "schema");
}

// B1: every verifiable pin, changed, is refused by name (the combined manifest's own
// bindings, the pinned files, the fields code and names, the recomputed NAV recipe, the
// running executable and the seal policy); a validation role class is refused (TRAIN-only).
TEST(StrategyLive, DeployManifestRefusesEveryMismatchedPin) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  const std::string other(64, 'f');
  const std::vector<std::pair<std::pair<std::string, std::string>, std::string>> changes{
      {{"library", "sha256"}, "library.sha256"},
      {{"recipe", "sha256"}, "recipe.sha256"},
      {{"orientations", "sha256"}, "orientations.sha256"},
      {{"composition", "weights_sha256"}, "composition.weights_sha256"},
      {{"combined", "sha256"}, "combined.sha256"},
      {{"role", "sha256"}, "role.sha256"},
      {{"fields", "sha256"}, "fields.sha256"},
      {{"fields", "code_sha256"}, "fields.code_sha256"},
      {{"", "data_source_sha256"}, "data_source_sha256"},
      {{"nav", "recipe_sha256"}, "nav.recipe_sha256"},
      {{"executables", "atx-equity-strategy-targets"}, "executables.atx-equity-strategy-targets"}};
  usize k = 0;
  for (const auto& change : changes) {
    const std::string object = change.first.first, key = change.first.second;
    expect_refused(fx, dir.path, "mismatch-" + std::to_string(k++),
                   [object, key, other](Json& m) { (object.empty() ? m : m[object])[key] = other; },
                   "pin mismatch " + change.second);
  }
  expect_refused(fx, dir.path, "mismatch-universe",
                 [](Json& m) { m["universe"]["member_semantics"] = "decision-member"; },
                 "pin mismatch universe.member_semantics");
  expect_refused(fx, dir.path, "mismatch-names",
                 [](Json& m) { m["fields"]["names"].push_back("no_such_field"); },
                 "pin mismatch fields.names");
  // A construction flag that no longer reproduces the qualified NAV run's recipe.
  const std::string recipe_mismatch = "pin mismatch nav.recipe_sha256";
  expect_refused(fx, dir.path, "mismatch-leverage",
                 [](Json& m) { m["nav"]["aim_leverage"] = 1.25; }, recipe_mismatch);
  expect_refused(fx, dir.path, "mismatch-basis",
                 [](Json& m) { m["nav"]["order_basis"] = "target"; }, recipe_mismatch);
  expect_refused(fx, dir.path, "mismatch-seal",
                 [](Json& m) { m["seal"]["exclusive_session"] = "2026-01-01"; },
                 "pin mismatch seal");
  expect_refused(fx, dir.path, "mismatch-policy",
                 [](Json& m) { m["seal"]["policy"] = "research-seal-v0"; }, "pin mismatch seal");
  expect_refused(fx, dir.path, "validation-class",
                 [](Json& m) { m["role"]["class"] = "validation"; }, "TRAIN-only");
  expect_refused(fx, dir.path, "per-name-rate", [](Json& m) { m["nav"]["rate"] = "per-name-v1"; },
                 "nav.rate must be fixed");
  // The CLI hashes its own executable (here the test binary): refused by name, exit 1.
  std::ostringstream out, err;
  EXPECT_EQ(decide_cli({"decide", "--deploy", fx.deploy_path.string(), "--asof",
                        date_of(fx.panel.sessions[150]), "--positions",
                        fx.holdings_dir.string(), "--allow-stale", "--output",
                        (dir.path / "cli").string()}, out, err), 1);
  EXPECT_NE(err.str().find("pin mismatch executables.atx-equity-strategy-targets"),
            std::string::npos) << err.str();
  EXPECT_EQ(decide_cli({"decide", "--deploy", "x", "--bogus", "y"}, out, err), 2);
}

// B1 seal: a session past the research seal is refused without an owner_gate, and still
// refused with one (live sessions are not enabled in this build; the seal is unchanged); a
// malformed owner_gate is refused. Nothing is written. The seal is research_window.hpp's.
TEST(StrategyLive, SessionPastTheSealIsRefusedWithOrWithoutOwnerGate) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  constexpr i64 seal = atx::engine::data::kSealBeginNs;
  EXPECT_EQ(st::research_seal_exclusive_ns, seal);
  EXPECT_EQ(std::string(st::research_seal_session), date_of(seal));
  EXPECT_EQ(std::string(st::research_seal_policy), "research-window-v2");
  EXPECT_FALSE(st::live_sessions_enabled);
  const auto refused = [&](const Json& m, const std::string& asof, const std::string& label) {
    const auto path = dir.path / ("deploy-" + label + ".json");
    write_json(path, m);
    auto cfg = decide_config(fx, 150, dir.path / ("out-" + label));
    cfg.deploy_path = path.string(); cfg.asof = asof;
    std::ostringstream progress;
    const auto outcome = st::run_decide(cfg, progress);
    EXPECT_FALSE(std::filesystem::exists(cfg.output_directory)) << label;
    return outcome ? std::string("decided") : outcome.error().to_string();
  };
  const std::string after_seal = date_of(seal + day_ns);
  EXPECT_NE(refused(fx.deploy, after_seal, "sealed").find("no owner_gate"), std::string::npos);
  EXPECT_NE(refused(fx.deploy, date_of(seal), "boundary").find("research seal"),
            std::string::npos);
  auto gated = fx.deploy;
  gated["owner_gate"] = {{"owner", "owner"}, {"ruling", "U-live-1"}, {"date", "2026-09-28"}};
  EXPECT_NE(refused(gated, after_seal, "gated").find("not enabled in this build"),
            std::string::npos);
  // A deploy manifest written under the superseded research-seal-v1 is refused by name.
  auto superseded = fx.deploy;
  superseded["seal"] = {{"policy", "research-seal-v1"}, {"exclusive_session", "2025-01-01"}};
  EXPECT_NE(refused(superseded, date_of(fx.panel.sessions[150]), "v1").find("pin mismatch seal"),
            std::string::npos);
  auto malformed = fx.deploy;
  malformed["owner_gate"] = {{"owner", "owner"}};
  EXPECT_NE(refused(malformed, "2021-06-01", "malformed").find("malformed owner_gate"),
            std::string::npos);
  EXPECT_NE(refused(fx.deploy, "2021-13-01", "date").find("calendar date"), std::string::npos);
}

// B9 health: a neutralization skip at the as-of is an ERROR (the replay would silently
// keep the book); leverage/turnover outside the manifest bands and short targets without
// a locate are warnings; a --locates file blocks unlocated short growth.
TEST(StrategyLive, HealthFlagsSkipAsErrorAndBandsLocatesAsWarnings) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  std::ostringstream progress;
  // Session 60: price-risk-v1 has too few return pairs, the book is still flat (declared:
  // an empty selection is refused otherwise, review R1 M-1).
  auto early = decide_config(fx, 60, dir.path / "early");
  early.flat_book = true;
  const auto skipped = st::run_decide(early, progress);
  ASSERT_TRUE(skipped) << skipped.error().to_string();
  EXPECT_EQ(skipped->health, st::DecideHealth::Error);
  const auto early_json = read_json(dir.path / "early" / "decision.json");
  EXPECT_EQ(early_json.at("health").at("status"), "error");
  bool found = false;
  for (const auto& c : early_json.at("health").at("checks")) {
    if (c.at("check") != "neutralization") continue;
    found = true;
    EXPECT_EQ(c.at("status"), "error");
    EXPECT_EQ(c.at("value"), "skipped-too-few-names");
  }
  EXPECT_TRUE(found);
  EXPECT_EQ(early_json.at("decision").at("rebalance"), false);
  // Tight bands and an empty locate list at a normal decision: warnings only.
  auto tight = fx.deploy;
  tight["health"] = {{"gross_leverage", Json::array({0.0, 0.001})}, {"abs_net_leverage_max", 0.0},
                     {"planned_turnover_max", 0.0}, {"names_without_locate_max", 0}};
  write_json(dir.path / "tight.json", tight);
  std::ofstream locates(dir.path / "locates.csv", std::ios::binary);
  locates << "instrument_id,locate\n";
  locates.close();
  auto warned = decide_config(fx, 170, dir.path / "warned");
  warned.deploy_path = (dir.path / "tight.json").string();
  warned.locates_path = (dir.path / "locates.csv").string();
  const auto outcome = st::run_decide(warned, progress);
  ASSERT_TRUE(outcome) << outcome.error().to_string();
  EXPECT_EQ(outcome->health, st::DecideHealth::Warn);
  const auto summary = read_json(dir.path / "warned" / "decision.json");
  std::map<std::string, std::string> status;
  for (const auto& c : summary.at("health").at("checks"))
    status[c.at("check").get<std::string>()] = c.at("status").get<std::string>();
  EXPECT_EQ(status.at("gross_leverage"), "warn");
  EXPECT_EQ(status.at("planned_turnover"), "warn");
  EXPECT_EQ(status.at("neutralization"), "ok");
  EXPECT_EQ(status.at("source_git_sha"), "ok");
  EXPECT_EQ(summary.at("locates").at("supplied"), true);
  EXPECT_EQ(summary.at("locates").at("names_without_locate"), fx.panel.n);
  // No locate anywhere: no short may open or grow, so every target >= min(current, 0).
  for (const auto& row : lines(dir.path / "warned" / "targets.csv")) {
    if (row.rfind("instrument_id", 0) == 0) continue;
    std::vector<std::string> cells;
    std::stringstream line(row); std::string cell;
    while (std::getline(line, cell, ',')) cells.push_back(cell);
    const f64 current = std::stod(cells[7]), target = std::stod(cells[11]);
    EXPECT_GE(target, std::min(current, 0.0)) << row;
  }
  // Positions refusals: an unknown instrument, a NAV that differs from nav_post. Each is
  // refused for its own reason before its (fresh) output directory is created; the fixture
  // already owns dir/"nav" (its NAV run), so these outputs must not reuse that name.
  std::ofstream unknown(dir.path / "unknown.csv", std::ios::binary);
  unknown << "instrument_id,held_dollars\n999999,1000\n";
  unknown.close();
  auto stray = decide_config(fx, 170, dir.path / "decide-stray");
  stray.positions_path = (dir.path / "unknown.csv").string(); stray.nav = 1e9;
  const auto stray_outcome = st::run_decide(stray, progress);
  ASSERT_FALSE(stray_outcome);
  EXPECT_NE(stray_outcome.error().to_string().find("outside the role"), std::string::npos)
      << stray_outcome.error().to_string();
  auto differs = decide_config(fx, 170, dir.path / "decide-nav-differs");
  differs.nav = 123.0;
  const auto differs_outcome = st::run_decide(differs, progress);
  ASSERT_FALSE(differs_outcome);
  EXPECT_NE(differs_outcome.error().to_string().find("--nav differs"), std::string::npos)
      << differs_outcome.error().to_string();
  EXPECT_FALSE(std::filesystem::exists(dir.path / "decide-stray"));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "decide-nav-differs"));
}

// ==== v7 lane W4 ====
namespace {
// The CSV the v1 writer produces for one session's rows (its expressions, verbatim).
std::string v1_rows(const st::holdings::SessionEntry& s, const std::vector<st::NavHolding>& names) {
  std::ostringstream f;
  f.imbue(std::locale::classic()); f << std::setprecision(17);
  const auto value = [&f](f64 v) { if (std::isnan(v)) f << "nan"; else f << v; };
  for (const auto& h : names) {
    f << s.session_ns << ',' << h.instrument_id << ',' << h.member << ',' << h.stale << ','
      << st::detail::borrow_tier_label(h.tier) << ',' << unsigned{h.tier_missing} << ','
      << (h.held_dollars > 0 ? "long" : h.held_dollars < 0 ? "short" : "flat") << ','
      << h.held_dollars << ',' << h.held_weight << ',' << s.nav_post << ','
      << st::detail::fill_status_label(h.fill) << ',' << h.filled_dollars << ','
      << h.fill_cost_dollars << ',' << h.unfilled_dollars << ',';
    value(h.desired); f << ',';
    value(h.rule_weight); f << ',';
    value(h.target_weight); f << ',';
    value(s.decision ? h.target_weight - h.held_weight : missing); f << ',';
    f << (h.order_placed ? h.order_dollars - h.held_dollars : 0.0) << ',' << h.order_placed
      << ',' << h.locate_blocked << ',' << h.order_working << ',';
    value(h.order_dollars); f << '\n';
  }
  return f.str();
}
// The fixture's NAV run again with --holdings-format csv (the v1 holdings.csv).
std::filesystem::path emit_csv(const Fixture& fx, const std::filesystem::path& dir) {
  std::ostringstream out, err;
  const auto holdings = dir / "holdings-csv";
  if (nav_cli(nav_args(fx.artifact, dir / "nav-csv",
                       {"--emit-holdings", holdings.string(), "--holdings-format", "csv"}),
              out, err) != 0)
    throw std::runtime_error("csv nav run: " + err.str());
  return holdings;
}
std::vector<std::string> cells_of(const std::string& line) {
  std::vector<std::string> cells;
  std::stringstream in(line); std::string cell;
  while (std::getline(in, cell, ',')) cells.push_back(cell);
  if (!line.empty() && line.back() == ',') cells.emplace_back();
  return cells;
}
// A CSV as header -> column index and data rows.
struct CsvFile {
  std::map<std::string, usize> column;
  std::vector<std::vector<std::string>> rows;
};
CsvFile read_csv(const std::filesystem::path& path) {
  const auto all = lines(path);
  CsvFile out;
  const auto header = cells_of(all.at(0));
  for (usize k = 0; k < header.size(); ++k) out.column[header[k]] = k;
  for (usize r = 1; r < all.size(); ++r) out.rows.push_back(cells_of(all[r]));
  return out;
}
bool parse_i64(const std::string& s, i64& out) {
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size();
}
std::string fmt17(f64 v) {
  std::ostringstream out;
  out.imbue(std::locale::classic()); out << std::setprecision(17) << v;
  return out.str();
}
int reconcile_cli(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  return st::dispatch_reconcile(static_cast<int>(argv.size()), argv.data(), out, err);
}
const Json* health_check(const Json& summary, const std::string& name) {
  for (const auto& c : summary.at("health").at("checks"))
    if (c.at("check") == name) return &c;
  return nullptr;
}
// The deploy's primary book config, as the decide path parses it from deploy_manifest().
st::NavReplayConfig deployed_book() {
  st::NavReplayConfig c;
  c.target.rule = st::TargetReplayRule::AimPartialV5; c.target.cadence = 1;
  c.target.trade_fraction = 0.3; c.target.dust_multiple = 0.1; c.target.aim_leverage = 1.247;
  c.target.band_multiple = 0; c.target.monthly_budget = 0.3; c.target.exit_rate = 0.05;
  c.target.neutralize = st::TargetNeutralize::PriceRiskV1;
  c.target.max_working_bytes = 1073741824;
  c.order_basis = st::NavOrderBasis::Delta; c.locate_in_aim = true; c.liquidity_cache = true;
  c.scenario = st::nav_scenario_matrix(true)[st::nav_primary_scenario_index];
  return c;
}
} // namespace

// Writer identity: the f64 layout carries every v1 holdings.csv column bit for bit (the CSV
// the v1 writer emits, rebuilt from the f64 reader, equals --holdings-format csv's bytes),
// the NAV outputs and holdings_days.csv are byte-identical across the formats, and the csv
// format keeps the v1 manifest.
TEST(StrategyLive, HoldingsF64CarriesEveryV1CsvColumnBitForBit) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  const auto csv = emit_csv(fx, dir.path);
  usize nav_files = 0;
  for (const auto& e : std::filesystem::directory_iterator(fx.nav_dir)) {
    ++nav_files;
    EXPECT_TRUE(file_bytes(e.path()) == file_bytes(dir.path / "nav-csv" / e.path().filename()))
        << e.path().filename();
  }
  EXPECT_GT(nav_files, 0U);
  EXPECT_TRUE(file_bytes(fx.holdings_dir / "holdings_days.csv") ==
              file_bytes(csv / "holdings_days.csv"));
  const auto v1 = read_json(csv / "manifest.json");
  const auto v2 = read_json(fx.holdings_dir / "manifest.json");
  EXPECT_EQ(v1.at("schema"), "atx.nav-holdings/v1");
  EXPECT_FALSE(v1.contains("format"));
  EXPECT_EQ(v1.at("files").size(), 2U);
  EXPECT_EQ(v1.at("rows"), v2.at("rows"));
  EXPECT_EQ(v1.at("columns"), v2.at("columns"));
  const auto index = read_json(fx.holdings_dir / "holdings_index.json");
  std::string rebuilt = std::string(st::detail::holdings_csv_columns()) + "\n";
  usize sessions = 0;
  for (const auto& entry : index.at("sessions")) {
    const auto read = st::holdings::read_session(fx.holdings_dir.string(), entry[1].get<i64>());
    ASSERT_TRUE(read) << read.error().to_string();
    EXPECT_EQ(read->session.session_index, entry[0].get<usize>());
    EXPECT_EQ(read->names.size(), entry[5].get<usize>());
    rebuilt += v1_rows(read->session, read->names);
    ++sessions;
  }
  EXPECT_EQ(sessions, fx.panel.d - 1);
  EXPECT_TRUE(rebuilt == file_bytes(csv / "holdings.csv"));
  // The index path is the same reader; a session the replay did not report is refused.
  const auto by_index = st::holdings::read_session(
      (fx.holdings_dir / "holdings_index.json").string(), fx.panel.sessions[150]);
  ASSERT_TRUE(by_index) << by_index.error().to_string();
  EXPECT_FALSE(st::holdings::read_session(fx.holdings_dir.string(),
                                          fx.panel.sessions[fx.panel.d - 1]));
  // Unknown format: usage error.
  std::ostringstream out, err;
  EXPECT_EQ(nav_cli(nav_args(fx.artifact, dir.path / "nav-bad",
                             {"--emit-holdings", (dir.path / "h-bad").string(),
                              "--holdings-format", "parquet"}), out, err), 2);
}

// decide accepts the f64 layout (directory or index) and the v1 CSV with the same bytes out,
// and refuses a tampered holdings.f64 (its SHA-256 no longer matches the index).
TEST(StrategyLive, DecideReadsF64AndV1CsvIdenticallyAndRefusesTamperedHoldings) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  const auto csv = emit_csv(fx, dir.path);
  const usize d = 170;
  const std::vector<std::pair<std::string, std::string>> sources{
      {"f64", fx.holdings_dir.string()},
      {"index", (fx.holdings_dir / "holdings_index.json").string()},
      {"csv", (csv / "holdings.csv").string()}};
  for (const auto& [label, path] : sources) {
    auto cfg = decide_config(fx, d, dir.path / ("decide-" + label));
    cfg.positions_path = path; cfg.check_replay = true;
    std::ostringstream progress;
    const auto outcome = st::run_decide(cfg, progress);
    ASSERT_TRUE(outcome) << label << ": " << outcome.error().to_string();
    EXPECT_EQ(outcome->parity_mismatches, 0U) << label;
    const auto summary = read_json(dir.path / ("decide-" + label) / "decision.json");
    EXPECT_EQ(summary.at("positions").at("layout"), label == "csv" ? "csv" : "f64") << label;
  }
  for (const auto* file : {"targets.csv", "orders.csv", "orders_shares.csv",
                           "expected_holdings.csv"}) {
    const auto reference = file_bytes(dir.path / "decide-f64" / file);
    EXPECT_FALSE(reference.empty()) << file;
    EXPECT_TRUE(reference == file_bytes(dir.path / "decide-index" / file)) << file;
    EXPECT_TRUE(reference == file_bytes(dir.path / "decide-csv" / file)) << file;
  }
  const auto tampered = dir.path / "tampered";
  std::filesystem::copy(fx.holdings_dir, tampered, std::filesystem::copy_options::recursive);
  {
    std::fstream f(tampered / "holdings.f64", std::ios::binary | std::ios::in | std::ios::out);
    f.seekg(100); char byte = 0; f.read(&byte, 1);
    f.seekp(100); byte = static_cast<char>(byte ^ 1); f.write(&byte, 1);
  }
  auto cfg = decide_config(fx, d, dir.path / "decide-tampered");
  cfg.positions_path = tampered.string();
  std::ostringstream progress;
  const auto refused = st::run_decide(cfg, progress);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("SHA-256"), std::string::npos)
      << refused.error().to_string();
  EXPECT_FALSE(std::filesystem::exists(dir.path / "decide-tampered"));
}

// B7 rounding rule, exact binary64 fixtures (nav = nav_dollars = 2^20, price 8, lot 100,
// min notional 1000): half away from zero, residuals, min-notional drop, exits kept,
// rounding to zero, no close, participation cap, shares column; refusals.
TEST(StrategyLive, ShareOrdersRoundToLotsAndDropBelowMinNotional) {
  const f64 nav = 1048576.0, unit = 0x1.0p-17; // raw shares = weight x 2^20 / 8 = weight / unit
  const f64 hold = 0.001;
  const std::vector<f64> target{150 * unit, -150 * unit, 149 * unit, 40 * unit, 0.0,
                                1000 * unit, hold};
  const std::vector<f64> current{0, 0, 0, 0, 800.0 / nav, 0, hold};
  const std::vector<f64> held{0, 0, 0, 0, 800.0, 0, hold * nav};
  const std::vector<f64> price{8, 8, 8, 8, 8, missing, 8};
  const std::vector<f64> mark = price;
  const std::vector<f64> adv(7, 1e5);
  st::orders::Inputs in{target, current, held, {}, price, mark, adv, nav, nav, 1000.0, 0.01, 100};
  const auto book = st::orders::build(in);
  ASSERT_TRUE(book) << book.error().to_string();
  const auto& s = book->summary;
  ASSERT_EQ(book->orders.size(), 3U);
  EXPECT_EQ(book->orders[0].index, 0U);
  EXPECT_EQ(book->orders[0].shares, 200);  // raw 150, lots 1.5 -> 2 (half away from zero)
  EXPECT_EQ(book->orders[0].residual_shares, -50.0);
  EXPECT_EQ(book->orders[0].notional, 1600.0);
  EXPECT_EQ(book->orders[0].participation, 0.016);
  EXPECT_EQ(book->orders[1].shares, -200); // raw -150 -> -2 lots
  EXPECT_TRUE(book->orders[1].short_sale);
  EXPECT_EQ(book->orders[2].index, 4U);    // the exit: 100 held shares, $800 < 1000, kept
  EXPECT_EQ(book->orders[2].shares, -100);
  EXPECT_TRUE(book->orders[2].exit);
  EXPECT_TRUE(book->orders[2].below_min_kept);
  EXPECT_FALSE(book->orders[2].short_sale);
  EXPECT_EQ(s.orders, 3U); EXPECT_EQ(s.buys, 1U); EXPECT_EQ(s.sells, 2U);
  EXPECT_EQ(s.short_sales, 1U); EXPECT_EQ(s.exits, 1U); EXPECT_EQ(s.exits_below_min_kept, 1U);
  EXPECT_EQ(s.dropped_min_notional, 1U); EXPECT_EQ(s.dropped_notional, 800.0); // raw 149 -> 100
  EXPECT_EQ(s.rounded_to_zero, 1U); EXPECT_EQ(s.rounded_to_zero_notional, 320.0); // raw 40
  EXPECT_EQ(s.refused_no_close, 1U);
  EXPECT_EQ(s.buy_notional, 1600.0); EXPECT_EQ(s.sell_notional, 2400.0);
  EXPECT_EQ(s.residual_notional_net, 0.0); EXPECT_EQ(s.residual_notional_abs, 800.0);
  EXPECT_EQ(s.residual_notional_max_abs, 400.0);
  EXPECT_EQ(s.above_participation_cap, 2U); EXPECT_EQ(s.participation_max, 0.016);
  // Expected book: the two opened names, the exit (0) and the held name that did not trade.
  ASSERT_EQ(book->expected.size(), 4U);
  EXPECT_EQ(book->expected[0].shares, 200.0);
  EXPECT_EQ(book->expected[1].shares, -200.0);
  EXPECT_EQ(book->expected[2].index, 4U); EXPECT_EQ(book->expected[2].shares, 0.0);
  EXPECT_EQ(book->expected[3].index, 6U);
  EXPECT_EQ(book->expected[3].shares, hold * nav / 8); EXPECT_EQ(book->expected[3].order_shares, 0);
  // A broker shares column and nav_dollars = 2 x nav: current shares = shares x 2.
  const std::vector<f64> shares{0, 0, 0, 0, 100, 0, 131};
  in.shares = shares; in.nav_dollars = 2 * nav;
  const auto scaled = st::orders::build(in);
  ASSERT_TRUE(scaled) << scaled.error().to_string();
  EXPECT_EQ(scaled->orders.front().shares, 300); // raw 300
  const auto exit_order = std::find_if(scaled->orders.begin(), scaled->orders.end(),
                                       [](const auto& o) { return o.index == 4; });
  ASSERT_NE(exit_order, scaled->orders.end());
  EXPECT_EQ(exit_order->shares, -200);
  EXPECT_EQ(scaled->expected.back().shares, 262.0); // 131 x 2, no order
  // round_to_lots and the refusals.
  EXPECT_EQ(st::orders::round_to_lots(2.5, 1).value(), 3);
  EXPECT_EQ(st::orders::round_to_lots(-2.5, 1).value(), -3);
  EXPECT_EQ(st::orders::round_to_lots(249.0, 100).value(), 200);
  EXPECT_FALSE(st::orders::round_to_lots(missing, 1));
  EXPECT_FALSE(st::orders::round_to_lots(1e300, 1));
  EXPECT_FALSE(st::orders::round_to_lots(1.0, 0));
  auto bad = in; bad.lot_size = 0;
  EXPECT_FALSE(st::orders::build(bad));
  bad = in; bad.nav = 0;
  EXPECT_FALSE(st::orders::build(bad));
  bad = in; bad.min_notional = -1;
  EXPECT_FALSE(st::orders::build(bad));
  const std::vector<f64> short_price(6, 8.0);
  bad = in; bad.price = short_price;
  EXPECT_FALSE(st::orders::build(bad));
}

// B7 at the decide: orders_shares.csv and expected_holdings.csv (lot 1, min notional $500,
// nav_dollars 1e7), every formed order in exactly one bucket, notional = shares x price,
// the files and the inputs pinned (R1 M-2), both TC definitions reported (R1 M-3), and
// targets.csv / orders.csv unchanged by the share-order flags.
TEST(StrategyLive, DecideWritesShareOrdersExpectedHoldingsAndInputPins) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  const usize d = 170;
  auto cfg = decide_config(fx, d, dir.path / "shares");
  cfg.nav_dollars = 1e7; cfg.min_notional = 500; cfg.lot_size = 1;
  std::ostringstream progress;
  const auto outcome = st::run_decide(cfg, progress);
  ASSERT_TRUE(outcome) << outcome.error().to_string();
  const auto out = dir.path / "shares";
  const auto summary = read_json(out / "decision.json");
  const auto& so = summary.at("orders_shares");
  EXPECT_EQ(so.at("nav_dollars"), 1e7);
  EXPECT_EQ(so.at("nav_dollars_source"), "--nav-dollars");
  EXPECT_EQ(so.at("price_source"), "close");
  const usize sent = so.at("orders").get<usize>();
  EXPECT_GT(sent, 0U);
  EXPECT_EQ(sent + so.at("dropped_min_notional").at("count").get<usize>() +
                so.at("rounded_to_zero").at("count").get<usize>() +
                so.at("refused_no_close").get<usize>(),
            summary.at("orders").at("count").get<usize>());
  const auto orders = read_csv(out / "orders_shares.csv");
  ASSERT_EQ(orders.rows.size(), sent);
  const auto col = [&orders](const char* name) { return orders.column.at(name); };
  for (const auto& r : orders.rows) {
    i64 shares = 0;
    ASSERT_TRUE(parse_i64(r[col("shares")], shares)) << r[col("shares")];
    EXPECT_NE(shares, 0);
    EXPECT_EQ(r[col("side")], shares > 0 ? "buy" : "sell");
    const f64 price = std::stod(r[col("reference_price")]);
    const f64 notional = std::stod(r[col("notional")]);
    EXPECT_EQ(bits(notional), bits(static_cast<f64>(shares) * price));
    if (r[col("exit")] == "0") {
      EXPECT_GE(std::abs(notional), 500.0);
    }
    EXPECT_EQ(r[col("tag")], "moc");
  }
  const auto expected = read_csv(out / "expected_holdings.csv");
  EXPECT_EQ(expected.rows.size(), so.at("expected_holdings_rows").get<usize>());
  for (const auto* file : {"orders_shares.csv", "expected_holdings.csv"})
    EXPECT_EQ(summary.at("files").at(file), co::sha256_file((out / file).string()).value());
  // R1 M-2: the positions (an f64 directory: its manifest.json) and the NAV source.
  const auto& pins = summary.at("pins_verified");
  EXPECT_EQ(pins.at("positions").at("layout"), "f64");
  EXPECT_EQ(pins.at("positions").at("sha256_of"), "manifest.json");
  EXPECT_EQ(pins.at("positions").at("sha256"),
            co::sha256_file((fx.holdings_dir / "manifest.json").string()).value());
  EXPECT_TRUE(pins.at("locates").is_null());
  EXPECT_EQ(pins.at("nav").at("source"), "positions nav_post");
  // R1 M-3: transfer_coefficient is the replay's own definition, recomputed here.
  const st::NavReplayInput in{fx.panel.target(), fx.panel.volume, fx.fields.view()};
  const auto held = st::holdings::read_session(fx.holdings_dir.string(), fx.panel.sessions[d]);
  ASSERT_TRUE(held) << held.error().to_string();
  std::vector<f64> dollars(fx.panel.n, 0.0);
  for (const auto& h : held->names) dollars[h.index] = h.held_dollars;
  const auto dec = st::detail::nav_decide(in, deployed_book(), d, dollars, held->session.nav_post);
  ASSERT_TRUE(dec) << dec.error().to_string();
  const auto member = std::span<const u8>(fx.panel.member).subspan(d * fx.panel.n, fx.panel.n);
  const auto& tc = summary.at("transfer_coefficient");
  EXPECT_EQ(bits(tc.at("transfer_coefficient").get<f64>()),
            bits(st::cost_v2::transfer_coefficient(member, dec->desired, dec->sigma, dec->rule)));
  EXPECT_EQ(bits(tc.at("desired_over_sigma_target").get<f64>()),
            bits(st::cost_v2::transfer_coefficient(member, dec->desired, dec->sigma,
                                                   dec->target)));
  EXPECT_TRUE(tc.at("definitions").contains("signal_over_variance"));
  // The share-order flags change no other output.
  auto plain = decide_config(fx, d, dir.path / "plain");
  ASSERT_TRUE(st::run_decide(plain, progress));
  for (const auto* file : {"targets.csv", "orders.csv"})
    EXPECT_TRUE(file_bytes(out / file) == file_bytes(dir.path / "plain" / file)) << file;
  EXPECT_EQ(read_json(dir.path / "plain" / "decision.json").at("orders_shares")
                .at("nav_dollars_source"), "the book NAV");
  // Invalid share-order arguments are refused before anything is written.
  auto bad = decide_config(fx, d, dir.path / "bad");
  bad.price_source = "vwap";
  EXPECT_FALSE(st::run_decide(bad, progress));
  bad.price_source = "close"; bad.lot_size = 0;
  EXPECT_FALSE(st::run_decide(bad, progress));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "bad"));
}

// B8: reconcile on the decide's own expected book finds no break; a mutated broker copy
// finds exactly the planted missing / extra / quantity breaks, and a 2:1 split the
// corporate actions explain; ticker and CIK brokers map through the identity bridge.
TEST(StrategyLive, ReconcileFindsBreaksAndExplainsASplitByCorporateActions) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  const usize d = 170;
  auto cfg = decide_config(fx, d, dir.path / "decide");
  std::ostringstream progress;
  ASSERT_TRUE(st::run_decide(cfg, progress));
  const auto decided = dir.path / "decide";
  const auto broker_asof = date_of(fx.panel.sessions[d + 1]);
  const auto args = [&](const std::filesystem::path& broker, std::vector<std::string> extra) {
    std::vector<std::string> all{"reconcile", "--deploy", fx.deploy_path.string(), "--expected",
                                 decided.string(), "--broker", broker.string(), "--asof",
                                 broker_asof};
    all.insert(all.end(), extra.begin(), extra.end());
    return all;
  };
  std::ostringstream out, err;
  ASSERT_EQ(reconcile_cli(args(decided / "expected_holdings.csv", {"--output",
                                   (dir.path / "self").string()}), out, err), 0) << err.str();
  const auto self = read_json(dir.path / "self" / "reconcile.json");
  EXPECT_EQ(self.at("schema"), "atx.book-reconcile/v1");
  EXPECT_EQ(self.at("counts").at("unexplained_breaks"), 0);
  EXPECT_GT(self.at("counts").at("ok").get<usize>(), 3U);
  // Plant the breaks.
  const auto expected = read_csv(decided / "expected_holdings.csv");
  const usize id_col = expected.column.at("instrument_id"), sh_col = expected.column.at("shares");
  std::vector<std::pair<std::string, f64>> rows;
  std::set<std::string> held_ids;
  for (const auto& r : expected.rows) {
    rows.emplace_back(r[id_col], std::stod(r[sh_col]));
    held_ids.insert(r[id_col]);
  }
  std::vector<usize> big;
  for (usize k = 0; k < rows.size(); ++k)
    if (std::abs(rows[k].second) > 10) big.push_back(k);
  ASSERT_GE(big.size(), 3U);
  const usize missing_row = big[0], quantity_row = big[1], split_row = big[2];
  const std::string extra_id = "999999"; // an instrument the book never expected
  ASSERT_FALSE(held_ids.count(extra_id));
  {
    std::ofstream broker(dir.path / "broker.csv", std::ios::binary);
    broker << "instrument_id,shares\n";
    for (usize k = 0; k < rows.size(); ++k) {
      if (k == missing_row) continue;
      f64 shares = rows[k].second;
      if (k == quantity_row) shares += 10;
      if (k == split_row) shares *= 2;
      broker << rows[k].first << ',' << fmt17(shares) << '\n';
    }
    broker << extra_id << ",100\n";
    std::ofstream actions(dir.path / "actions.csv", std::ios::binary);
    actions << "instrument_id,date,ratio\n"
            << rows[split_row].first << ',' << broker_asof << ",2\n"
            // dated at the decision's own session: before the window, never applied
            << rows[quantity_row].first << ',' << date_of(fx.panel.sessions[d]) << ",3\n";
  }
  const auto actions = (dir.path / "actions.csv").string();
  st::ReconcileConfig rc;
  rc.deploy_path = fx.deploy_path.string(); rc.expected_path = decided.string();
  rc.broker_path = (dir.path / "broker.csv").string(); rc.asof = broker_asof;
  rc.corporate_actions_path = actions;
  std::ostringstream report;
  const auto broken = st::run_reconcile(rc, report);
  ASSERT_TRUE(broken) << broken.error().to_string();
  EXPECT_EQ(broken->missing, 1U);
  EXPECT_EQ(broken->extra, 1U);
  EXPECT_EQ(broken->quantity, 1U);
  EXPECT_EQ(broken->explained, 1U);
  EXPECT_EQ(broken->unmapped, 0U);
  EXPECT_EQ(broken->breaks(), 3U);
  EXPECT_NE(report.str().find("break missing " + rows[missing_row].first), std::string::npos)
      << report.str();
  // Without the corporate actions the split is a quantity break too; the CLI exits 5.
  rc.corporate_actions_path.clear();
  const auto unexplained = st::run_reconcile(rc, report);
  ASSERT_TRUE(unexplained);
  EXPECT_EQ(unexplained->quantity, 2U);
  EXPECT_EQ(unexplained->explained, 0U);
  EXPECT_EQ(reconcile_cli(args(dir.path / "broker.csv",
                               {"--corporate-actions", actions, "--output",
                                (dir.path / "broken").string()}), out, err), 5);
  const auto listed = read_csv(dir.path / "broken" / "reconciliation.csv");
  std::map<std::string, std::string> status;
  for (const auto& r : listed.rows)
    status[r[listed.column.at("instrument_id")]] = r[listed.column.at("status")];
  EXPECT_EQ(status.at(rows[split_row].first), "explained");
  EXPECT_EQ(status.at(rows[missing_row].first), "missing");
  EXPECT_EQ(status.at(extra_id), "extra");
  // Tickers and CIKs through the identity bridge (a CIK with a P and a J line: P wins).
  {
    std::ofstream identity(dir.path / "identity.csv", std::ios::binary);
    identity << "sr_id,ticker,cik,start,end_incl,primary\n";
    for (const auto& [id, shares] : rows)
      identity << id << ",T" << id << ',' << id << ",2019-01-01,2030-12-31,P\n";
    identity << extra_id << ",OTHER," << rows[0].first << ",2019-01-01,2030-12-31,J\n";
    std::ofstream tickers(dir.path / "tickers.csv", std::ios::binary);
    std::ofstream ciks(dir.path / "ciks.csv", std::ios::binary);
    tickers << "ticker,shares\n";
    ciks << "cik,shares\n";
    for (const auto& [id, shares] : rows) {
      tickers << 't' << id << ',' << fmt17(shares) << '\n';
      ciks << id << ',' << fmt17(shares) << '\n';
    }
    tickers << "ZZZZ,5\n";
  }
  const auto identity = (dir.path / "identity.csv").string();
  EXPECT_EQ(reconcile_cli(args(dir.path / "ciks.csv", {"--identity", identity}), out, err), 0)
      << err.str();
  rc.broker_path = (dir.path / "tickers.csv").string(); rc.identity_path = identity;
  const auto by_ticker = st::run_reconcile(rc, report);
  ASSERT_TRUE(by_ticker) << by_ticker.error().to_string();
  EXPECT_EQ(by_ticker->unmapped, 1U); // ZZZZ
  EXPECT_EQ(by_ticker->breaks(), 1U);
  rc.identity_path.clear();
  EXPECT_FALSE(st::run_reconcile(rc, report)); // tickers without a bridge: refused
  // Bindings: another deploy manifest, a tampered expected book, a broker before the decision.
  auto other = fx.deploy;
  other["book"] = "another-book";
  write_json(dir.path / "other.json", other);
  rc.broker_path = (decided / "expected_holdings.csv").string();
  rc.deploy_path = (dir.path / "other.json").string();
  EXPECT_FALSE(st::run_reconcile(rc, report));
  rc.deploy_path = fx.deploy_path.string();
  rc.asof = date_of(fx.panel.sessions[d - 1]);
  EXPECT_FALSE(st::run_reconcile(rc, report));
  rc.asof = broker_asof;
  ASSERT_TRUE(st::run_reconcile(rc, report));
  { std::ofstream tamper(decided / "expected_holdings.csv", std::ios::binary | std::ios::app);
    tamper << extra_id << ",1\n"; }
  const auto tampered = st::run_reconcile(rc, report);
  ASSERT_FALSE(tampered);
  EXPECT_NE(tampered.error().to_string().find("SHA-256"), std::string::npos);
  EXPECT_EQ(reconcile_cli({"reconcile", "--bogus", "x"}, out, err), 2);
}

// B9 freshness: an as-of that is not the role's last session is refused (nothing written)
// unless --allow-stale, which decides and warns with the lag.
TEST(StrategyLive, FreshnessRefusesAStaleAsOfUnlessAllowed) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  auto cfg = decide_config(fx, 170, dir.path / "stale");
  cfg.allow_stale = false;
  std::ostringstream progress;
  const auto refused = st::run_decide(cfg, progress);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("stale data"), std::string::npos)
      << refused.error().to_string();
  EXPECT_NE(refused.error().to_string().find(date_of(fx.panel.sessions[fx.panel.d - 1])),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "stale"));
  cfg.allow_stale = true;
  const auto allowed = st::run_decide(cfg, progress);
  ASSERT_TRUE(allowed) << allowed.error().to_string();
  EXPECT_NE(allowed->health, st::DecideHealth::Ok);
  const auto stale = read_json(dir.path / "stale" / "decision.json");
  const auto* check = health_check(stale, "data_freshness");
  ASSERT_NE(check, nullptr);
  EXPECT_EQ(check->at("status"), "warn");
  EXPECT_EQ(check->at("sessions_behind").get<usize>(), fx.panel.d - 1 - 170);
  EXPECT_EQ(check->at("allow_stale"), true);
}

// B9 TC band: the run rule, then the prior decisions read from --prior-decisions (this
// book's, strictly before the as-of; other books, later sessions and other schemas are
// ignored; two records of one session must agree).
TEST(StrategyLive, TransferCoefficientBandWarnsOnFiveLowDecisions) {
  const std::vector<f64> low{0.2, 0.3, 0.4, 0.1, 0.1, 0.1};
  EXPECT_EQ(st::tc_band_run(0.1, std::span(low).first(4)), 5U);
  EXPECT_EQ(st::tc_band_run(0.1, low), 5U); // capped
  EXPECT_EQ(st::tc_band_run(0.1, std::span(low).first(3)), 4U);
  const std::vector<f64> broken{0.2, missing, 0.1};
  EXPECT_EQ(st::tc_band_run(0.1, broken), 2U);
  const std::vector<f64> high{0.2, 0.7, 0.1};
  EXPECT_EQ(st::tc_band_run(0.1, high), 2U);
  EXPECT_EQ(st::tc_band_run(0.6, low), 0U);
  EXPECT_EQ(st::tc_band_run(missing, low), 0U);
  EXPECT_EQ(st::tc_band_run(0.5, low), 0U); // the band is strict: < .5
  Directory dir;
  const auto fx = make_fixture(dir.path);
  const usize d = 170;
  std::ostringstream progress;
  ASSERT_TRUE(st::run_decide(decide_config(fx, d, dir.path / "alone"), progress));
  const auto alone = read_json(dir.path / "alone" / "decision.json");
  const auto* unsupplied = health_check(alone, "transfer_coefficient_band");
  ASSERT_NE(unsupplied, nullptr);
  EXPECT_EQ(unsupplied->at("prior_decisions"), "not-supplied");
  EXPECT_EQ(unsupplied->at("status"), "warn");
  const auto tc0 = alone.at("transfer_coefficient").at("desired_over_sigma_target");
  const bool low_now = tc0.is_number() && tc0.get<f64>() < st::tc_band_min;
  const auto prior = [&](const std::filesystem::path& root, const std::string& name, usize row,
                         const std::string& book, f64 tc) {
    std::filesystem::create_directories(root / name);
    write_json(root / name / "decision.json",
               Json{{"schema", "atx.book-decision/v1"}, {"status", "complete"}, {"book", book},
                    {"asof_session_ns", fx.panel.sessions[row]},
                    {"transfer_coefficient", {{"desired_over_sigma_target", tc}}}});
  };
  const auto priors = dir.path / "priors";
  for (usize k = 1; k <= 4; ++k)
    prior(priors, "p" + std::to_string(k), d - k, "synthetic-v61-s2", 0.1);
  prior(priors, "other-book", d - 5, "another-book", 0.1);
  prior(priors, "later", d + 3, "synthetic-v61-s2", 0.1);
  write_json(priors / "deploy.json", fx.deploy);
  auto cfg = decide_config(fx, d, dir.path / "banded");
  cfg.prior_decisions_directory = priors.string();
  ASSERT_TRUE(st::run_decide(cfg, progress));
  const auto banded = read_json(dir.path / "banded" / "decision.json");
  const auto* band = health_check(banded, "transfer_coefficient_band");
  ASSERT_NE(band, nullptr);
  EXPECT_EQ(band->at("prior_decisions"), "supplied");
  EXPECT_EQ(band->at("prior_files_read"), 7);
  EXPECT_EQ(band->at("prior_decisions_used"), 4);
  EXPECT_EQ(band->at("consecutive_below"), low_now ? 5 : 0);
  EXPECT_EQ(band->at("status"), low_now ? "warn" : "ok");
  std::filesystem::remove_all(priors / "p4");
  cfg.output_directory = (dir.path / "banded-3").string();
  ASSERT_TRUE(st::run_decide(cfg, progress));
  const auto banded3 = read_json(dir.path / "banded-3" / "decision.json");
  const auto* three = health_check(banded3, "transfer_coefficient_band");
  ASSERT_NE(three, nullptr);
  EXPECT_EQ(three->at("consecutive_below"), low_now ? 4 : 0);
  EXPECT_EQ(three->at("status"), "ok");
  prior(priors, "p1-again", d - 1, "synthetic-v61-s2", 0.2); // disagrees with p1
  cfg.output_directory = (dir.path / "disagree").string();
  const auto refused = st::run_decide(cfg, progress);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("disagree"), std::string::npos);
}

// Review R1 M-1: a positions file with a session column and no row at the as-of is refused
// even with --nav (never read as a flat book) unless --flat-book declares it.
TEST(StrategyLive, PositionsWithoutAnAsOfRowAreRefusedUnlessFlatBook) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  {
    std::ofstream positions(dir.path / "dated.csv", std::ios::binary);
    positions << "session_ns,instrument_id,held_dollars\n"
              << fx.panel.sessions[100] << ',' << fx.panel.ids[0] << ",1000\n";
  }
  auto cfg = decide_config(fx, 170, dir.path / "no-row");
  cfg.positions_path = (dir.path / "dated.csv").string(); cfg.nav = 1e9;
  std::ostringstream progress;
  const auto refused = st::run_decide(cfg, progress);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("select no row"), std::string::npos)
      << refused.error().to_string();
  EXPECT_FALSE(std::filesystem::exists(dir.path / "no-row"));
  cfg.flat_book = true;
  ASSERT_TRUE(st::run_decide(cfg, progress));
  const auto summary = read_json(dir.path / "no-row" / "decision.json");
  EXPECT_EQ(summary.at("positions").at("flat_book"), true);
  EXPECT_EQ(summary.at("positions").at("rows"), 0);
  EXPECT_EQ(summary.at("pins_verified").at("positions").at("sha256"),
            co::sha256_file((dir.path / "dated.csv").string()).value());
  EXPECT_EQ(summary.at("pins_verified").at("nav").at("source"), "--nav");
}

// Review R1 m-14: nav.cadence_anchor, when pinned, must be the role's decision_begin session.
TEST(StrategyLive, CadenceAnchorPinIsChecked) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  expect_refused(fx, dir.path, "anchor",
                 [](Json& m) { m["nav"]["cadence_anchor"] = "2020-01-03"; },
                 "pin mismatch nav.cadence_anchor");
  auto pinned = fx.deploy;
  pinned["nav"]["cadence_anchor"] = date_of(fx.panel.sessions[0]);
  write_json(dir.path / "anchored.json", pinned);
  auto cfg = decide_config(fx, 150, dir.path / "anchored");
  cfg.deploy_path = (dir.path / "anchored.json").string();
  std::ostringstream progress;
  const auto outcome = st::run_decide(cfg, progress);
  EXPECT_TRUE(outcome) << (outcome ? "" : outcome.error().to_string());
}

namespace {
// A 160 x 60 book with 20 sessions of pre-score history (a warm start needs them) and a flat
// positions file: every decide below is one verified_load (all pins and the recomputed NAV
// recipe) plus one decision at row 150 of a manifest pinned to one of its NAV runs.
struct PinBench {
  Directory dir;
  Panel panel = market_panel(160, 60, 17, 3e7);
  Fields fields{panel, 23};
  usize score_begin; // the role's first scored row (20 unless a test asks for another)
  Artifact artifact = write_artifact(dir.path, panel, fields, score_begin);
  usize runs = 0;
  explicit PinBench(usize begin = 20) : score_begin(begin) {
    std::ofstream positions(dir.path / "flat.csv", std::ios::binary);
    positions << "instrument_id,held_dollars\n";
  }
  // The recipe_sha256 of a NAV run with the book flags plus `extra`.
  std::string nav_recipe(const std::string& name, std::vector<std::string> extra) {
    std::ostringstream out, err;
    const auto status = nav_cli(nav_args(artifact, dir.path / name, std::move(extra)), out, err);
    EXPECT_EQ(status, 0) << name << ": " << err.str();
    return read_json(dir.path / name / "summary.json").at("recipe_sha256").get<std::string>();
  }
  // run_decide on the deploy manifest pinned to `recipe`, its nav block edited by `edit`.
  template<class Edit> co::Result<st::DecideOutcome> decide(const std::string& recipe, Edit edit) {
    auto m = deploy_manifest(artifact, recipe);
    edit(m.at("nav"));
    const auto tag = std::to_string(runs++);
    const auto path = dir.path / ("deploy-" + tag + ".json");
    write_json(path, m);
    st::DecideConfig cfg;
    cfg.deploy_path = path.string();
    cfg.asof = date_of(panel.sessions[150]);
    cfg.positions_path = (dir.path / "flat.csv").string();
    cfg.output_directory = (dir.path / ("decide-" + tag)).string();
    cfg.flat_book = true; cfg.nav = 1e9;
    cfg.executable_sha256 = exe_pin; cfg.build_source_sha = std::string(40, 'b');
    cfg.allow_stale = true;
    std::ostringstream progress;
    return st::run_decide(cfg, progress);
  }
};
std::string outcome_text(const co::Result<st::DecideOutcome>& r) {
  return r ? std::string("verified") : r.error().to_string();
}
bool recipe_mismatch(const co::Result<st::DecideOutcome>& r) {
  return !r && r.error().to_string().find("pin mismatch nav.recipe_sha256") != std::string::npos;
}
} // namespace

// PM (v8): the deploy manifest's optional nav keys (warm_start_sessions, D-0; hold_band, R-4).
// Absent, or at the off value, the recomputed NAV recipe hashes exactly as before, so a
// manifest pinned to a NAV run without the flag still verifies; on, the key is part of the pin
// both ways (a run with the flag needs the key, a run without it refuses the key); a malformed
// key is refused by name. The role scores from row 140: the book's price-risk exposures (126
// return pairs) exist on the 5 warm-up rows, so the warm start builds a book (review A-3 refuses
// one that does not; from row 20 every warm-up rebalance would be skipped).
TEST(StrategyLive, RecipePinBackwardCompatibleWithNewConstructionFields) {
  PinBench bench(140);
  const auto plain = bench.nav_recipe("nav-plain", {});
  const auto warm = bench.nav_recipe("nav-warm", {"--warm-start-sessions", "5"});
  EXPECT_NE(plain, warm);
  const auto none = [](Json&) {};
  EXPECT_EQ(outcome_text(bench.decide(plain, none)), "verified");
  EXPECT_EQ(outcome_text(bench.decide(plain, [](Json& nav) { nav["warm_start_sessions"] = 0; })),
            "verified");
  EXPECT_EQ(outcome_text(bench.decide(warm, [](Json& nav) { nav["warm_start_sessions"] = 5; })),
            "verified");
  EXPECT_TRUE(recipe_mismatch(bench.decide(warm, none)));
  EXPECT_TRUE(recipe_mismatch(bench.decide(plain, [](Json& nav) {
    nav["warm_start_sessions"] = 5;
  })));
  const auto expect_malformed = [&bench](const std::string& recipe, const char* key,
                                         const Json& bad) {
    const auto refused = bench.decide(recipe, [&](Json& nav) { nav[key] = bad; });
    ASSERT_FALSE(refused) << key << ' ' << bad.dump();
    EXPECT_NE(refused.error().to_string().find(std::string("nav.") + key), std::string::npos)
        << refused.error().to_string();
  };
  for (const Json& bad : {Json("5"), Json(-1), Json(5000), Json(2.5)})
    expect_malformed(warm, "warm_start_sessions", bad);
  // v8 R-4 hold_band: B = 0 is the identity and hashes as absent.
  const auto hold = bench.nav_recipe("nav-hold", {"--hold-band", ".1"});
  EXPECT_NE(hold, plain);
  EXPECT_EQ(bench.nav_recipe("nav-hold0", {"--hold-band", "0"}), plain);
  EXPECT_EQ(outcome_text(bench.decide(hold, [](Json& nav) { nav["hold_band"] = 0.1; })),
            "verified");
  EXPECT_EQ(outcome_text(bench.decide(plain, [](Json& nav) { nav["hold_band"] = 0.0; })),
            "verified");
  EXPECT_TRUE(recipe_mismatch(bench.decide(hold, none)));
  EXPECT_TRUE(recipe_mismatch(bench.decide(plain, [](Json& nav) { nav["hold_band"] = 0.1; })));
  expect_malformed(hold, "hold_band", Json("0.1"));
}

// v8 R-4, the identity cell's shape (the accepted construction's flags with the borrow fields):
// --hold-band 0 runs the kernel at every decision, yet every NAV output file is byte-identical
// to the run without the flag.
TEST(HoldBand, ZeroBandNavRunIsByteIdentical) {
  PinBench bench;
  EXPECT_EQ(bench.nav_recipe("plain", {}), bench.nav_recipe("zero", {"--hold-band", "0"}));
  const auto root = bench.dir.path;
  std::vector<std::string> names;
  for (const auto& e : std::filesystem::directory_iterator(root / "plain"))
    names.push_back(e.path().filename().string());
  ASSERT_GE(names.size(), 5U);
  for (const auto& name : names)
    EXPECT_TRUE(file_bytes(root / "plain" / name) == file_bytes(root / "zero" / name)) << name;
  EXPECT_EQ(names.size(), static_cast<usize>(std::distance(
                              std::filesystem::directory_iterator(root / "zero"),
                              std::filesystem::directory_iterator{})));
}

// v8 R-4 at the library level: nav_decide chained over every decision row from the first,
// each fed the replay's holdings and the previous decision's hold state, is the replay's
// decision bit for bit (target and rule weights, desired, the band's counts); the same row
// decided without the carried state re-sets every member instead.
TEST(HoldBand, NavDecideChainReproducesReplay) {
  const auto p = market_panel(70, 12, 5, 2e5);
  const Fields f(p, 9);
  const st::NavReplayInput in{p.target(), p.volume, f.view()};
  auto cfg = v61_book(1e7);
  cfg.target.hold_band = 0.1;
  const std::array<st::NavScenario, 1> one{cfg.scenario};
  Recorder recorder;
  ASSERT_TRUE(st::replay_nav_scenarios(in, cfg, one, recorder, 0));
  atx::engine::book::HoldBandState state; // every name unset before the first decision
  usize kept = 0, compared = 0;
  for (usize d = 0; d <= 60; ++d) {
    const auto& [day, rows] = recorder.sessions.at(d);
    const auto dec = st::detail::nav_decide(in, cfg, d, held_at(recorder, d, p.n),
                                            day.posttrade_nav, {}, &state);
    ASSERT_TRUE(dec) << d << ": " << dec.error().to_string();
    EXPECT_EQ(dec->construction.hold_moved, day.construction.hold_moved) << d;
    EXPECT_EQ(dec->construction.hold_kept, day.construction.hold_kept) << d;
    EXPECT_EQ(dec->construction.hold_first_set, day.construction.hold_first_set) << d;
    kept += dec->construction.hold_kept;
    if (d == 30 || d == 45 || d == 60) {
      ASSERT_TRUE(day.rebalance) << d;
      std::vector<f64> target(p.n, 0.0), rule(p.n, 0.0);
      for (const auto& h : rows) {
        target[h.index] = h.target_weight; rule[h.index] = h.rule_weight;
        EXPECT_EQ(bits(dec->desired[h.index]), bits(h.desired)) << d << " name " << h.index;
      }
      for (usize i = 0; i < p.n; ++i) {
        EXPECT_EQ(bits(dec->target[i]), bits(target[i])) << d << " name " << i;
        EXPECT_EQ(bits(dec->rule[i]), bits(rule[i])) << d << " name " << i;
      }
      ++compared;
    }
    state = dec->hold;
  }
  EXPECT_EQ(compared, 3U);
  EXPECT_GT(kept, 0U);
  const auto& day45 = recorder.sessions.at(45).first;
  const auto fresh = st::detail::nav_decide(in, cfg, 45, held_at(recorder, 45, p.n),
                                            day45.posttrade_nav);
  ASSERT_TRUE(fresh) << fresh.error().to_string();
  EXPECT_EQ(fresh->construction.hold_kept, 0U);
  EXPECT_EQ(fresh->construction.hold_first_set, fresh->members);
}

// v8 R-4 at the CLI: the decide verb reads the hold-band state from the positions file's
// rank_set/desired_prev columns and writes the state after the decision into targets.csv.
// A positions file without the columns (a broker file: the replay's holdings of 150 as
// instrument_id,held_dollars) loads every name unset, so the first decision sets every member;
// fed back as the next session's positions columns the state carries: nothing is re-set, and
// every member either kept its state or moved its rank by more than the band. (Since E-16 the
// hold-band run's own export carries the state: HoldBand.CheckReplayMatchesEmittedHoldings.)
TEST(HoldBand, DecideVerbCarriesState) {
  Directory dir;
  auto fx = make_fixture(dir.path, {"--hold-band", ".1"});
  fx.deploy["nav"]["hold_band"] = 0.1;
  write_json(fx.deploy_path, fx.deploy);
  std::ostringstream progress;
  const auto held150 = st::holdings::read_session(fx.holdings_dir.string(),
                                                  fx.panel.sessions[150]);
  ASSERT_TRUE(held150) << held150.error().to_string();
  {
    std::ofstream positions(dir.path / "positions-150.csv", std::ios::binary);
    positions.imbue(std::locale::classic()); positions << std::setprecision(17);
    positions << "instrument_id,held_dollars\n";
    for (const auto& h : held150->names)
      positions << h.instrument_id << ',' << h.held_dollars << '\n';
  }
  auto first = decide_config(fx, 150, dir.path / "decide-150");
  first.positions_path = (dir.path / "positions-150.csv").string();
  first.nav = held150->session.nav_post;
  ASSERT_TRUE(st::run_decide(first, progress));
  const auto s150 = read_json(dir.path / "decide-150" / "decision.json");
  const usize members = s150.at("decision").at("members").get<usize>();
  EXPECT_EQ(s150.at("rule"), "aim-partial-v5+neutral-price-risk-v1+hold-band-0.1");
  EXPECT_EQ(s150.at("hold_band").at("state_in"), "none (no state columns): every name unset");
  EXPECT_EQ(s150.at("hold_band").at("first_set"), members);
  EXPECT_EQ(s150.at("hold_band").at("names_set_out"), members);
  const auto t150 = read_csv(dir.path / "decide-150" / "targets.csv");
  ASSERT_TRUE(t150.column.count("rank_set") && t150.column.count("desired_prev"));
  // The next session's positions: the replay's holdings at 151 plus the state columns of 150.
  const usize d = 151;
  const auto held = st::holdings::read_session(fx.holdings_dir.string(), fx.panel.sessions[d]);
  ASSERT_TRUE(held) << held.error().to_string();
  std::map<u64, std::array<std::string, 3>> book; // held_dollars, rank_set, desired_prev
  for (const auto& h : held->names) book[h.instrument_id] = {fmt17(h.held_dollars), "nan", "nan"};
  std::map<u64, std::pair<f64, f64>> before;
  for (const auto& row : t150.rows) {
    const u64 id = std::stoull(row.at(t150.column.at("instrument_id")));
    const auto& rank = row.at(t150.column.at("rank_set"));
    const auto& prev = row.at(t150.column.at("desired_prev"));
    auto& entry = book[id];
    if (entry[0].empty()) entry[0] = "0";
    entry[1] = rank; entry[2] = prev;
    if (rank != "nan") before[id] = {std::stod(rank), std::stod(prev)};
  }
  EXPECT_EQ(before.size(), members);
  {
    std::ofstream positions(dir.path / "positions-151.csv", std::ios::binary);
    positions << "instrument_id,held_dollars,rank_set,desired_prev\n";
    for (const auto& [id, cells] : book)
      positions << id << ',' << cells[0] << ',' << cells[1] << ',' << cells[2] << '\n';
  }
  auto next = decide_config(fx, d, dir.path / "decide-151");
  next.positions_path = (dir.path / "positions-151.csv").string();
  next.nav = held->session.nav_post;
  ASSERT_TRUE(st::run_decide(next, progress));
  const auto s151 = read_json(dir.path / "decide-151" / "decision.json");
  const auto& hold = s151.at("hold_band");
  EXPECT_EQ(hold.at("state_in"), "positions rank_set,desired_prev columns");
  EXPECT_EQ(hold.at("names_set_in"), members);
  EXPECT_EQ(hold.at("first_set"), 0);
  EXPECT_EQ(hold.at("moved").get<usize>() + hold.at("kept").get<usize>(), members);
  EXPECT_GT(hold.at("kept").get<usize>(), 0U);
  const auto t151 = read_csv(dir.path / "decide-151" / "targets.csv");
  usize checked = 0;
  for (const auto& row : t151.rows) {
    const u64 id = std::stoull(row.at(t151.column.at("instrument_id")));
    const auto it = before.find(id);
    if (it == before.end() || row.at(t151.column.at("member")) != "1") continue;
    const f64 rank = std::stod(row.at(t151.column.at("rank_set")));
    const f64 prev = std::stod(row.at(t151.column.at("desired_prev")));
    if (bits(rank) == bits(it->second.first)) {
      EXPECT_EQ(bits(prev), bits(it->second.second)) << id; // kept: the whole state carried
    } else {
      EXPECT_GT(std::abs(rank - it->second.first), 0.1) << id; // moved: left the band
    }
    ++checked;
  }
  EXPECT_EQ(checked, members);
  // One state column without the other is refused.
  {
    std::ofstream half(dir.path / "half.csv", std::ios::binary);
    half << "instrument_id,held_dollars,rank_set\n" << fx.panel.ids[0] << ",0,0.1\n";
  }
  auto refused = decide_config(fx, d, dir.path / "decide-half");
  refused.positions_path = (dir.path / "half.csv").string();
  refused.nav = held->session.nav_post;
  const auto half = st::run_decide(refused, progress);
  ASSERT_FALSE(half);
  EXPECT_NE(half.error().to_string().find("go together"), std::string::npos);
}

// v8 R-4/R-5 in a construction grid (lane D's lockstep): the hold band and the ADV cap shape the
// shared desired target, so every variant carries the base's; the band's state advances on the
// shared cadence decisions (the union of the variants' cadences), so a hold-band grid has one
// cadence. A grid that keeps both is each variant's standalone replay bit for bit.
TEST(HoldBand, GridSharesTheBandAndOneCadence) {
  const auto p = market_panel(70, 12, 5, 2e5);
  const Fields f(p, 9);
  const st::NavReplayInput in{p.target(), p.volume, f.view()};
  auto base = v61_book(1e7);
  base.target.hold_band = 0.1;
  const std::array<st::NavScenario, 1> one{base.scenario};
  const auto refused = [&](const std::vector<st::NavReplayConfig>& grid) {
    const auto r = st::replay_nav_grid(in, grid, one);
    return !r && r.error().code() == co::ErrorCode::InvalidArgument;
  };
  auto slower = base;
  slower.target.cadence = 2;
  EXPECT_TRUE(refused({base, slower}));
  auto unbanded = base;
  unbanded.target.hold_band.reset();
  EXPECT_TRUE(refused({base, unbanded}));
  auto capped = base;
  capped.target.adv_hold_q = 0.1;
  EXPECT_TRUE(refused({base, capped}));
  auto faster = base;
  faster.target.trade_fraction = 0.5;
  const std::vector<st::NavReplayConfig> grid{base, faster};
  const auto run = st::replay_nav_grid(in, grid, one);
  ASSERT_TRUE(run) << run.error().to_string();
  ASSERT_EQ(run->size(), grid.size());
  usize kept = 0;
  for (usize v = 0; v < grid.size(); ++v) {
    const auto alone = st::replay_nav_scenarios(in, grid[v], one);
    ASSERT_TRUE(alone) << alone.error().to_string();
    const auto& a = (*run)[v].front().days;
    const auto& b = alone->front().days;
    ASSERT_EQ(a.size(), b.size());
    for (usize t = 0; t < a.size(); ++t) {
      EXPECT_EQ(bits(a[t].pretrade_nav), bits(b[t].pretrade_nav)) << v << ' ' << t;
      EXPECT_EQ(bits(a[t].planned_gross), bits(b[t].planned_gross)) << v << ' ' << t;
      EXPECT_EQ(a[t].construction.hold_kept, b[t].construction.hold_kept) << v << ' ' << t;
      kept += a[t].construction.hold_kept;
    }
  }
  EXPECT_GT(kept, 0U); // the band acts in the grid
}

// Ruling E-16: the holdings export of a hold-band NAV run (b > 0 declared) carries the band's
// state DECIDE read at each session (rank_set, desired_prev: the state entering the session's
// construction) in both layouts, and a name holding a set rank has a row; decide reads it, so
// decide --check-replay from the export reproduces the replay's targets bit for bit at every
// session tried, from either layout. Emitting it changes no NAV output. Without the state
// columns the same session re-sets every member and no longer matches.
TEST(HoldBand, CheckReplayMatchesEmittedHoldings) {
  Directory dir;
  auto fx = make_fixture(dir.path, {"--hold-band", ".1"});
  fx.deploy["nav"]["hold_band"] = 0.1;
  write_json(fx.deploy_path, fx.deploy);
  const auto csv = dir.path / "holdings-csv";
  {
    std::ostringstream out, err;
    ASSERT_EQ(nav_cli(nav_args(fx.artifact, dir.path / "nav-csv",
                               {"--hold-band", ".1", "--emit-holdings", csv.string(),
                                "--holdings-format", "csv"}), out, err), 0) << err.str();
    ASSERT_EQ(nav_cli(nav_args(fx.artifact, dir.path / "nav-plain", {"--hold-band", ".1"}), out,
                      err), 0) << err.str();
  }
  usize nav_files = 0;
  for (const auto& e : std::filesystem::directory_iterator(fx.nav_dir)) {
    ++nav_files;
    EXPECT_TRUE(file_bytes(e.path()) ==
                file_bytes(dir.path / "nav-plain" / e.path().filename())) << e.path().filename();
  }
  EXPECT_GT(nav_files, 0U);
  // The layouts name the state columns; both manifests declare them.
  const auto data = read_json(fx.holdings_dir / "holdings_index.json").at("data");
  EXPECT_EQ(data.at("row_width"), st::holdings::hold_row_width);
  EXPECT_EQ(data.at("columns").at(st::holdings::col_rank_set), "rank_set");
  EXPECT_EQ(data.at("columns").at(st::holdings::col_desired_prev), "desired_prev");
  EXPECT_EQ(std::filesystem::file_size(fx.holdings_dir / "holdings.f64"),
            data.at("rows").get<u64>() * st::holdings::hold_row_width * sizeof(f64));
  EXPECT_EQ(lines(csv / "holdings.csv").front(),
            std::string(st::detail::holdings_csv_columns()) + ",rank_set,desired_prev");
  for (const auto& emitted : {fx.holdings_dir, csv})
    EXPECT_TRUE(read_json(emitted / "manifest.json").contains("hold_band_state")) << emitted;
  const std::vector<std::pair<std::string, std::string>> sources{
      {"f64", fx.holdings_dir.string()}, {"csv", (csv / "holdings.csv").string()}};
  usize kept_at = 0; // a session where the replay kept some member's desired value
  for (const usize d : {usize{150}, usize{151}, usize{170}, usize{190}}) {
    for (const auto& [label, path] : sources) {
      auto cfg = decide_config(fx, d, dir.path / ("decide-" + label + "-" + std::to_string(d)));
      cfg.positions_path = path; cfg.check_replay = true;
      std::ostringstream progress;
      const auto outcome = st::run_decide(cfg, progress);
      ASSERT_TRUE(outcome) << label << ' ' << d << ": " << outcome.error().to_string();
      EXPECT_TRUE(outcome->parity_checked);
      EXPECT_EQ(outcome->parity_names, fx.panel.n);
      EXPECT_EQ(outcome->parity_mismatches, 0U) << label << ' ' << d;
      const auto hold =
          read_json(std::filesystem::path(cfg.output_directory) / "decision.json").at("hold_band");
      EXPECT_NE(hold.at("state_in").get<std::string>().find("rank_set,desired_prev"),
                std::string::npos) << label << ' ' << d;
      EXPECT_GT(hold.at("names_set_in").get<usize>(), 0U) << label << ' ' << d;
      if (!kept_at && hold.at("kept").get<usize>() > 0) kept_at = d;
    }
  }
  ASSERT_GT(kept_at, 0U); // the band held some member at a session tried
  // The same session from the export's positions and targets without the state columns: every
  // member re-set, so the band's kept members move and the replay's targets are not reproduced.
  const auto held = st::holdings::read_session(fx.holdings_dir.string(),
                                               fx.panel.sessions[kept_at]);
  ASSERT_TRUE(held) << held.error().to_string();
  {
    std::ofstream positions(dir.path / "stateless.csv", std::ios::binary);
    positions.imbue(std::locale::classic()); positions << std::setprecision(17);
    positions << "instrument_id,held_dollars,nav_post,target_weight\n";
    for (const auto& h : held->names)
      positions << h.instrument_id << ',' << h.held_dollars << ',' << held->session.nav_post
                << ',' << h.target_weight << '\n';
  }
  auto stateless = decide_config(fx, kept_at, dir.path / "decide-stateless");
  stateless.positions_path = (dir.path / "stateless.csv").string();
  stateless.check_replay = true;
  std::ostringstream progress;
  const auto unmatched = st::run_decide(stateless, progress);
  ASSERT_TRUE(unmatched) << unmatched.error().to_string();
  EXPECT_GT(unmatched->parity_mismatches, 0U);
}

// E-16 identity: without a declared band (flag off, or --hold-band 0: b = 0 runs the kernel but
// declares nothing) the holdings export is the pre-E-16 layout byte for byte: in both formats
// the same files with the same bytes, 11-value f64 rows under the v1 column names, the v1
// holdings.csv header and no hold_band_state key.
TEST(HoldBand, EmittedHoldingsUnchangedWithoutADeclaredBand) {
  PinBench bench;
  const auto root = bench.dir.path;
  Json v1_columns = Json::array();
  for (const char* column : st::holdings::column_names) v1_columns.push_back(column);
  for (const char* layout : {"f64", "csv"}) {
    const std::string format = layout;
    const auto off = root / ("holdings-off-" + format);
    const auto zero = root / ("holdings-zero-" + format);
    bench.nav_recipe("nav-off-" + format,
                     {"--emit-holdings", off.string(), "--holdings-format", format});
    bench.nav_recipe("nav-zero-" + format, {"--hold-band", "0", "--emit-holdings", zero.string(),
                                            "--holdings-format", format});
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(off))
      names.push_back(e.path().filename().string());
    EXPECT_EQ(names.size(), format == "f64" ? 4U : 3U) << format;
    EXPECT_EQ(names.size(), static_cast<usize>(std::distance(
                                std::filesystem::directory_iterator(zero),
                                std::filesystem::directory_iterator{}))) << format;
    for (const auto& name : names)
      EXPECT_TRUE(file_bytes(off / name) == file_bytes(zero / name)) << format << ' ' << name;
    EXPECT_FALSE(read_json(off / "manifest.json").contains("hold_band_state")) << format;
    if (format == "f64") {
      const auto index = read_json(off / "holdings_index.json");
      const auto& data = index.at("data");
      EXPECT_EQ(data.at("row_width"), st::holdings::row_width);
      EXPECT_EQ(data.at("columns"), v1_columns);
      EXPECT_FALSE(index.contains("hold_state"));
      EXPECT_EQ(std::filesystem::file_size(off / "holdings.f64"),
                data.at("rows").get<u64>() * st::holdings::row_width * sizeof(f64));
    } else {
      EXPECT_EQ(lines(off / "holdings.csv").front(), st::detail::holdings_csv_columns());
    }
  }
}

// ---- v8 R-5: adv-hold-v1 (ADV holding cap) ----
namespace {
// One decision of a flat book (the desired target does not depend on the holdings) and the caps
// adv-hold-v1 applies there: Q x the execution ADV of the fill session d + 1 / (L x NAV), the
// replay's own expression (+inf with the cap off).
struct CapCheck {
  st::detail::NavDecision dec;
  std::vector<f64> caps;
};
CapCheck capped_decision(const st::NavReplayInput& in, const st::NavReplayConfig& cfg, usize d) {
  const usize n = in.target.instruments;
  CapCheck out{{}, std::vector<f64>(n, std::numeric_limits<f64>::infinity())};
  auto dec = st::detail::nav_decide(in, cfg, d, std::vector<f64>(n, 0.0), cfg.initial_nav);
  EXPECT_TRUE(dec) << d << ": " << (dec ? "" : dec.error().to_string());
  const auto adv = st::detail::execution_adv(in, cfg, d + 1);
  EXPECT_TRUE(adv) << (adv ? "" : adv.error().to_string());
  if (!dec || !adv) return out;
  out.dec = std::move(*dec);
  if (cfg.target.adv_hold_q > 0)
    for (usize i = 0; i < n; ++i)
      out.caps[i] = cfg.target.adv_hold_q * (*adv)[i] /
                    (cfg.target.aim_leverage * cfg.initial_nav);
  return out;
}
// A 12-name book whose ADV (about $1e7) is near its NAV ($1e7): at Q = .1 the cap binds on the
// larger rank weights and the pro rata spreading lifts some names above their own cap.
struct CapBench {
  Panel panel = market_panel(70, 12, 5, 2e5);
  Fields fields{panel, 9};
  st::NavReplayInput input() const { return {panel.target(), panel.volume, fields.view()}; }
};
} // namespace

// After the one pass every weighted name is at or below its cap, except the residual breach the
// decision reports: its names, summed excess and largest excess are exactly what is left above
// the caps (recomputed from the execution ADV).
TEST(AdvHold, NoNameAboveCapAfterOnePassOrReported) {
  const CapBench bench;
  const auto in = bench.input();
  auto cfg = v61_book(1e7);
  cfg.target.adv_hold_q = 0.1;
  usize rows = 0, clipped = 0;
  for (usize d = 25; d <= 65; d += 5) {
    const auto c = capped_decision(in, cfg, d);
    if (!c.dec.rebalance) continue;
    ++rows;
    usize over = 0;
    f64 mass = 0, worst = 0;
    for (usize i = 0; i < bench.panel.n; ++i) {
      if (c.dec.desired[i] == 0) continue;
      const f64 excess = std::abs(c.dec.desired[i]) - c.caps[i];
      if (!(excess > 0)) continue;
      ++over; mass += excess; worst = std::max(worst, excess);
    }
    const auto& k = c.dec.construction;
    EXPECT_EQ(over, k.adv_residual_names) << d;
    EXPECT_NEAR(mass, k.adv_residual_mass, 1e-15) << d;
    EXPECT_EQ(bits(worst), bits(k.adv_residual_max)) << d;
    clipped += k.adv_clipped;
  }
  EXPECT_GT(rows, 3U);
  EXPECT_GT(clipped, 0U); // the cap binds on this book
}

// The cap acts on the projected target: clipped names sit exactly at their cap, every unclipped
// name of a side is scaled by one common factor, signs and zeros are kept, and each side's
// gross (hence the net) is the projection's.
TEST(AdvHold, SideGrossPreserved) {
  const CapBench bench;
  const auto in = bench.input();
  const auto off = v61_book(1e7);
  auto on = off;
  on.target.adv_hold_q = 0.1;
  usize compared = 0, clipped = 0;
  for (usize d = 25; d <= 65; d += 5) {
    const auto a = capped_decision(in, off, d);
    const auto b = capped_decision(in, on, d);
    if (!a.dec.rebalance) continue;
    ASSERT_TRUE(b.dec.rebalance) << d;
    const f64 unplaced = b.dec.construction.adv_unplaced_mass;
    std::array<f64, 2> before{}, after{}, factor{0.0, 0.0};
    for (usize i = 0; i < bench.panel.n; ++i) {
      const f64 w0 = a.dec.desired[i], w1 = b.dec.desired[i];
      if (w0 == 0) {
        EXPECT_EQ(bits(w1), bits(w0)) << d << ' ' << i;
        continue;
      }
      EXPECT_GT(w0 * w1, 0) << d << ' ' << i;
      const usize side = w0 > 0 ? 0U : 1U;
      before[side] += std::abs(w0); after[side] += std::abs(w1);
      const f64 cap = b.caps[i];
      if (std::abs(w0) > cap) {
        EXPECT_EQ(bits(std::abs(w1)), bits(cap)) << d << ' ' << i;
        ++clipped;
      } else if (factor[side] == 0) {
        factor[side] = w1 / w0;
      } else {
        EXPECT_NEAR(w1 / w0, factor[side], 1e-14) << d << ' ' << i;
      }
    }
    // Only a side with every name clipped (unplaced mass) may shrink, by exactly that mass.
    EXPECT_NEAR(after[0] + after[1] + unplaced, before[0] + before[1], 1e-12) << d;
    if (unplaced == 0) {
      EXPECT_NEAR(after[0], before[0], 1e-12) << d;
      EXPECT_NEAR(after[1], before[1], 1e-12) << d;
    }
    ++compared;
  }
  EXPECT_GT(compared, 3U);
  EXPECT_GT(clipped, 0U);
}

// A Q no name reaches is the accepted construction byte for byte in every daily and events CSV
// (the recipe and summary add only the adv-hold keys, with no clip and no breach recorded).
TEST(AdvHold, LargeQIsByteIdentical) {
  PinBench bench;
  bench.nav_recipe("plain", {});
  bench.nav_recipe("large", {"--adv-hold-q", "1e9"});
  const auto root = bench.dir.path;
  usize compared = 0;
  for (const auto& e : std::filesystem::directory_iterator(root / "plain")) {
    const auto name = e.path().filename().string();
    if (name == "recipe.json" || name == "summary.json") continue;
    EXPECT_TRUE(file_bytes(root / "plain" / name) == file_bytes(root / "large" / name)) << name;
    ++compared;
  }
  EXPECT_GE(compared, 4U);
  const auto plain = read_json(root / "plain" / "recipe.json");
  auto large = read_json(root / "large" / "recipe.json");
  EXPECT_EQ(large.at("rule"), plain.at("rule").get<std::string>() + "+adv-hold-1e+09");
  EXPECT_EQ(large.at("adv_hold_q"), 1e9);
  EXPECT_TRUE(large.at("adv_hold_rule").is_string());
  large.erase("adv_hold_q"); large.erase("adv_hold_rule"); large["rule"] = plain.at("rule");
  EXPECT_EQ(large, plain);
  const auto summary = read_json(root / "large" / "summary.json");
  ASSERT_FALSE(summary.at("scenarios").empty());
  for (const auto& s : summary.at("scenarios")) {
    const auto& cap = s.at("construction").at("adv_hold");
    EXPECT_GT(cap.at("decisions").get<usize>(), 0U);
    EXPECT_EQ(cap.at("clipped_names_total"), 0);
    EXPECT_EQ(cap.at("residual_breach").at("names_total"), 0);
  }
}

// The deploy key nav.adv_hold_q: 0 hashes as absent, Q > 0 is part of the pin both ways, and the
// decision records its cap pass in decision.json (adv_hold).
TEST(AdvHold, DeployPinAndDecisionRecord) {
  PinBench bench;
  const auto plain = bench.nav_recipe("nav-plain", {});
  const auto capped = bench.nav_recipe("nav-adv", {"--adv-hold-q", ".1"});
  EXPECT_NE(plain, capped);
  EXPECT_EQ(bench.nav_recipe("nav-adv0", {"--adv-hold-q", "0"}), plain);
  EXPECT_EQ(outcome_text(bench.decide(plain, [](Json& nav) { nav["adv_hold_q"] = 0.0; })),
            "verified");
  EXPECT_TRUE(recipe_mismatch(bench.decide(capped, [](Json&) {})));
  EXPECT_TRUE(recipe_mismatch(bench.decide(plain, [](Json& nav) { nav["adv_hold_q"] = 0.1; })));
  EXPECT_EQ(outcome_text(bench.decide(capped, [](Json& nav) { nav["adv_hold_q"] = 0.1; })),
            "verified");
  const auto decision =
      read_json(bench.dir.path / ("decide-" + std::to_string(bench.runs - 1)) / "decision.json");
  EXPECT_EQ(decision.at("adv_hold").at("q"), 0.1);
  EXPECT_EQ(decision.at("adv_hold").at("nav"), 1e9);
  EXPECT_TRUE(decision.at("adv_hold").contains("residual_names"));
  const auto malformed = bench.decide(capped, [](Json& nav) { nav["adv_hold_q"] = "0.1"; });
  ASSERT_FALSE(malformed);
  EXPECT_NE(malformed.error().to_string().find("nav.adv_hold_q"), std::string::npos);
}

// adv-hold-v1 is a NAV replay option (it reads the execution ADV and the run's NAV): the target
// replay refuses Q > 0 (its CLI has no such flag), and the target replay, the NAV replay, decide
// and the nav CLI refuse a negative, NaN or infinite Q and Q > 0 under baseline-v1, before any
// output. The same configurations at Q = 0 run (the controls), so each refusal is the cap's.
TEST(AdvHold, RefusedOutsideTheNavPathAndWhenMalformed) {
  const CapBench bench;
  const auto in = bench.input();
  const std::array<st::NavScenario, 1> one{v61_book(1e7).scenario};
  const auto text = [](const auto& r) {
    return r ? std::string("accepted") : r.error().to_string();
  };
  const std::string malformed = "adv_hold_q must be finite >= 0";
  // The target replay: Q = 0 runs; Q > 0 under aim-partial-v5 is a NAV replay option.
  auto v5 = v61_book(1e7).target;
  EXPECT_EQ(text(st::replay_targets(in.target, v5)), "accepted");
  v5.adv_hold_q = 0.1;
  EXPECT_NE(text(st::replay_targets(in.target, v5)).find("is a NAV replay option"),
            std::string::npos);
  // baseline-v1: Q = 0 runs in both replays; Q > 0 is refused in both.
  st::TargetReplayConfig baseline;
  auto nav_baseline = v61_book(1e7);
  nav_baseline.target.rule = st::TargetReplayRule::BaselineTargetV1;
  nav_baseline.target.dust_multiple = 0;
  nav_baseline.target.aim_leverage = 1;
  nav_baseline.target.exit_rate = 1;
  EXPECT_EQ(text(st::replay_targets(in.target, baseline)), "accepted");
  EXPECT_EQ(text(st::replay_nav_scenarios(in, nav_baseline, one)), "accepted");
  baseline.adv_hold_q = 0.1;
  nav_baseline.target.adv_hold_q = 0.1;
  EXPECT_NE(text(st::replay_targets(in.target, baseline)).find(malformed), std::string::npos);
  EXPECT_NE(text(st::replay_nav_scenarios(in, nav_baseline, one)).find(malformed),
            std::string::npos);
  // A negative, NaN or infinite Q under aim-partial-v5: refused on every path.
  const std::vector<f64> flat(bench.panel.n, 0.0);
  for (const f64 q : {-0.1, missing, std::numeric_limits<f64>::infinity()}) {
    auto target = v61_book(1e7).target;
    target.adv_hold_q = q;
    EXPECT_NE(text(st::replay_targets(in.target, target)).find(malformed), std::string::npos)
        << q;
    auto nav = v61_book(1e7);
    nav.target.adv_hold_q = q;
    EXPECT_NE(text(st::replay_nav_scenarios(in, nav, one)).find(malformed), std::string::npos)
        << q;
    EXPECT_NE(text(st::detail::nav_decide(in, nav, 40, flat, 1e7)).find(malformed),
              std::string::npos) << q;
  }
  // The CLIs: the target replay does not know the flag (usage error); nav refuses a malformed
  // Q (exit 1) before its output exists, and a value that is not a number is a usage error.
  std::ostringstream out, err;
  std::vector<std::string> targets{"targets", "--adv-hold-q", ".1"};
  std::vector<char*> argv;
  for (auto& arg : targets) argv.push_back(arg.data());
  EXPECT_EQ(st::dispatch_target_replay(static_cast<int>(argv.size()), argv.data(), out, err), 2);
  EXPECT_NE(err.str().find("unknown flag: --adv-hold-q"), std::string::npos) << err.str();
  PinBench pins;
  for (const char* q : {"-1", "nan", "inf"}) {
    std::ostringstream nav_out, nav_err;
    const auto dir = pins.dir.path / (std::string("bad") + q);
    EXPECT_EQ(nav_cli(nav_args(pins.artifact, dir, {"--adv-hold-q", q}), nav_out, nav_err), 1)
        << q;
    EXPECT_NE(nav_err.str().find(malformed), std::string::npos) << q << ": " << nav_err.str();
    EXPECT_FALSE(std::filesystem::exists(dir)) << q;
  }
  const auto word = pins.dir.path / "word";
  EXPECT_EQ(nav_cli(nav_args(pins.artifact, word, {"--adv-hold-q", "x"}), out, err), 2);
  EXPECT_FALSE(std::filesystem::exists(word));
}

// --capacity-curve with --adv-hold-q (ruling E-15): the v7 parser passes the flag through to both
// passes, so the main pass and every capacity book record the cap (rule id, recipe adv_hold keys
// with the E-15 sentence, summary construction.adv_hold), and the x1 capacity book is still the
// main pass's S2 book bit for bit (both passes cap at the run's initial NAV).
TEST(AdvHold, CapacityCurveCarriesTheCapInBothPasses) {
  PinBench bench;
  const auto root = bench.dir.path / "curve";
  std::ostringstream out, err;
  ASSERT_EQ(nav_cli(nav_args(bench.artifact, root, {"--adv-hold-q", ".1", "--capacity-curve"}),
                    out, err), 0) << err.str();
  const std::array<std::filesystem::path, 2> passes{root, root / "capacity"};
  for (const auto& dir : passes) {
    const auto recipe = read_json(dir / "recipe.json");
    EXPECT_EQ(recipe.at("adv_hold_q"), 0.1) << dir;
    EXPECT_NE(recipe.at("rule").get<std::string>().find("+adv-hold-0.1"), std::string::npos)
        << dir;
    EXPECT_NE(recipe.at("adv_hold_rule").get<std::string>().find("every capacity book"),
              std::string::npos) << dir;
    const auto summary = read_json(dir / "summary.json");
    ASSERT_FALSE(summary.at("scenarios").empty()) << dir;
    for (const auto& s : summary.at("scenarios")) {
      const auto& cap = s.at("construction").at("adv_hold");
      EXPECT_EQ(cap.at("q"), 0.1) << dir;
      EXPECT_GT(cap.at("decisions").get<usize>(), 0U) << dir;
    }
  }
  EXPECT_EQ(read_json(root / "capacity" / "summary.json").at("scenarios").size(),
            st::cost_v2::capacity_multiples.size());
  const auto extras = read_json(root / "v7_extras.json");
  EXPECT_EQ(extras.at("capacity").size(), st::cost_v2::capacity_multiples.size());
  EXPECT_EQ(extras.at("capacity_x1_equals_primary_bit_for_bit"), true);
  EXPECT_TRUE(std::filesystem::exists(root / "capacity_curve.csv"));
  std::ostringstream help, quiet;
  ASSERT_EQ(nav_cli({"nav", "--help"}, help, quiet), 0);
  EXPECT_NE(help.str().find("the cap uses the run's initial NAV for every book, each "
                            "--capacity-curve book included"), std::string::npos);
}
