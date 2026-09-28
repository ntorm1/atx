#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
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
#include "../src/strategy_live.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_replay_detail.hpp"
#include "../src/strategy_target_replay.hpp"

// v7 lane L3: --emit-holdings, the decide verb and the atx.book-deploy/v1 manifest.
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
// (atx.research-role-fields/v1 with code_sha256), every provenance pin `pin`.
Artifact write_artifact(const std::filesystem::path& dir, const Panel& p, const Fields& f) {
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
      {"dates", p.d}, {"instruments", p.n}, {"score_begin", 0}, {"score_end", p.d},
      {"files", role_files}};
  a.cfg.role_path = (dir / "role.json").string();
  a.cfg.role_sha256 = write_json(a.cfg.role_path, a.role);
  const Json manifest{{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"},
      {"role", "train"}, {"layout", "date-major-little-endian"}, {"dates", p.d},
      {"instruments", p.n}, {"score_begin", 0}, {"score_end", p.d},
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
      {"seal", {{"policy", "research-seal-v1"}, {"exclusive_session", "2025-01-01"}}},
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
Fixture make_fixture(const std::filesystem::path& dir) {
  auto panel = market_panel(200, 60, 17, 3e7);
  Fields fields(panel, 23);
  auto artifact = write_artifact(dir, panel, fields);
  Fixture fx{std::move(panel), std::move(fields), std::move(artifact), dir / "nav",
             dir / "holdings", dir / "deploy.json", Json{}};
  std::ostringstream out, err;
  if (nav_cli(nav_args(fx.artifact, fx.nav_dir, {"--emit-holdings", fx.holdings_dir.string()}),
              out, err) != 0)
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
  cfg.positions_path = (fx.holdings_dir / "holdings.csv").string();
  cfg.output_directory = out.string();
  cfg.executable_sha256 = exe_pin;
  cfg.build_source_sha = std::string(40, 'b');
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
// compare bytes), and publishes holdings.csv / holdings_days.csv with manifest.json last
// binding their SHAs and the NAV recipe SHA.
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
  EXPECT_EQ(manifest.at("schema"), "atx.nav-holdings/v1");
  EXPECT_EQ(manifest.at("status"), "complete");
  EXPECT_EQ(manifest.at("book"), "modeled-1bn-stale5-v1+swap-fin-v1");
  EXPECT_EQ(manifest.at("nav_recipe_sha256"),
            read_json(fx.nav_dir / "summary.json").at("recipe_sha256"));
  for (const auto* file : {"holdings.csv", "holdings_days.csv"})
    EXPECT_EQ(manifest.at("files").at(file),
              co::sha256_file((fx.holdings_dir / file).string()).value()) << file;
  const auto rows = lines(fx.holdings_dir / "holdings.csv");
  const auto days = lines(fx.holdings_dir / "holdings_days.csv");
  ASSERT_GT(rows.size(), 1U);
  EXPECT_EQ(rows.front(), st::detail::holdings_csv_columns());
  EXPECT_EQ(manifest.at("rows").get<usize>(), rows.size() - 1);
  EXPECT_EQ(manifest.at("sessions").get<usize>(), days.size() - 1);
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
    EXPECT_GT(summary.at("transfer_coefficient").at("names").get<usize>(), 2U);
    const auto tc = summary.at("transfer_coefficient").at("target");
    ASSERT_TRUE(tc.is_number());
    EXPECT_LE(std::abs(tc.get<f64>()), 1.0);
    EXPECT_EQ(summary.at("pins_verified").at("nav_recipe_sha256"),
              fx.deploy.at("nav").at("recipe_sha256"));
    EXPECT_EQ(summary.at("files").at("orders.csv"),
              co::sha256_file((out / "orders.csv").string()).value());
    EXPECT_EQ(lines(out / "orders.csv").size(),
              summary.at("orders").at("count").get<usize>() + 1);
  }
  // The live use: decide on the final row (the replay never decides there) from a plain
  // positions file and an explicit NAV.
  const usize held_row = fx.panel.d - 2;
  const auto rows = lines(fx.holdings_dir / "holdings.csv");
  std::ofstream positions(dir.path / "positions.csv", std::ios::binary);
  positions << "instrument_id,held_dollars\n";
  const std::string prefix = std::to_string(fx.panel.sessions[held_row]) + ",";
  std::string nav_text;
  for (usize r = 1; r < rows.size(); ++r) {
    if (rows[r].rfind(prefix, 0) != 0) continue;
    std::vector<std::string> cells;
    std::stringstream line(rows[r]); std::string cell;
    while (std::getline(line, cell, ',')) cells.push_back(cell);
    positions << cells[1] << ',' << cells[7] << '\n';
    nav_text = cells[9];
  }
  positions.close();
  ASSERT_FALSE(nav_text.empty());
  auto live = decide_config(fx, fx.panel.d - 1, dir.path / "decide-last");
  live.positions_path = (dir.path / "positions.csv").string();
  live.nav = std::stod(nav_text);
  std::ostringstream progress;
  const auto last = st::run_decide(live, progress);
  ASSERT_TRUE(last) << last.error().to_string();
  EXPECT_FALSE(last->parity_checked);
  EXPECT_TRUE(std::filesystem::exists(dir.path / "decide-last" / "decision.json"));
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
                        (fx.holdings_dir / "holdings.csv").string(), "--output",
                        (dir.path / "cli").string()}, out, err), 1);
  EXPECT_NE(err.str().find("pin mismatch executables.atx-equity-strategy-targets"),
            std::string::npos) << err.str();
  EXPECT_EQ(decide_cli({"decide", "--deploy", "x", "--bogus", "y"}, out, err), 2);
}

// B1 seal: a session past the research seal is refused without an owner_gate, and still
// refused with one (live sessions are not enabled in this build; kSeal is unchanged); a
// malformed owner_gate is refused. Nothing is written.
TEST(StrategyLive, SessionPastTheSealIsRefusedWithOrWithoutOwnerGate) {
  Directory dir;
  const auto fx = make_fixture(dir.path);
  EXPECT_EQ(st::research_seal_exclusive_ns,
            static_cast<i64>(std::chrono::sys_days{std::chrono::year{2025} / 1 / 1}
                                 .time_since_epoch().count()) * day_ns);
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
  EXPECT_NE(refused(fx.deploy, "2025-01-02", "sealed").find("no owner_gate"), std::string::npos);
  EXPECT_NE(refused(fx.deploy, "2025-01-01", "boundary").find("research seal"), std::string::npos);
  auto gated = fx.deploy;
  gated["owner_gate"] = {{"owner", "owner"}, {"ruling", "U-live-1"}, {"date", "2026-09-28"}};
  EXPECT_NE(refused(gated, "2025-01-02", "gated").find("not enabled in this build"),
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
  // Session 60: price-risk-v1 has too few return pairs, the book is still flat.
  auto early = decide_config(fx, 60, dir.path / "early");
  early.nav = 1e9;
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
  // Positions refusals: an unknown instrument, a NAV that differs from nav_post.
  std::ofstream unknown(dir.path / "unknown.csv", std::ios::binary);
  unknown << "instrument_id,held_dollars\n999999,1000\n";
  unknown.close();
  auto stray = decide_config(fx, 170, dir.path / "stray");
  stray.positions_path = (dir.path / "unknown.csv").string(); stray.nav = 1e9;
  EXPECT_FALSE(st::run_decide(stray, progress));
  auto nav = decide_config(fx, 170, dir.path / "nav");
  nav.nav = 123.0;
  EXPECT_FALSE(st::run_decide(nav, progress));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "stray"));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "nav"));
}
