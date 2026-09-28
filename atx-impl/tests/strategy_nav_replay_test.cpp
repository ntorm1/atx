#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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
#include "atx/engine/book/replay.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "atx/engine/cost/borrow_tiers.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_target_replay.hpp"

namespace {
using namespace atx;
namespace st = atx::impl::strategy;
namespace co = atx::core;
namespace bk = atx::engine::book;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 uncapped = std::numeric_limits<f64>::infinity();

i64 session(int year, unsigned month, unsigned day) {
  return static_cast<i64>(std::chrono::sys_days{std::chrono::year{year} / month / day}
      .time_since_epoch().count()) * day_ns;
}
std::vector<i64> weekdays(int year, unsigned month, unsigned day, usize count) {
  std::vector<i64> out;
  auto date = std::chrono::sys_days{std::chrono::year{year} / month / day};
  while (out.size() < count) {
    const std::chrono::weekday wd{date};
    if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
      out.push_back(static_cast<i64>(date.time_since_epoch().count()) * day_ns);
    date += std::chrono::days{1};
  }
  return out;
}
// Date-major synthetic role from 2020-01-02 on weekdays: present members, price 100,
// raw == adjusted, ample volume. Helpers keep member => present and NaN nonmembers.
struct Panel {
  usize d{}, n{}, begin{}, end{};
  std::vector<f64> signal, close, raw, volume;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Panel(usize dates, usize names)
      : d(dates), n(names), end(dates), signal(dates * names, 0.0), close(dates * names, 100.0),
        raw(dates * names, 100.0), volume(dates * names, 1e9), member(dates * names, 1),
        present(dates * names, 1), sessions(weekdays(2020, 1, 2, dates)), ids(names) {
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
  }
  [[nodiscard]] usize k(usize t, usize i) const { return t * n + i; }
  void by_name(const std::vector<f64>& values) {
    for (usize t = 0; t < d; ++t)
      for (usize i = 0; i < n; ++i) signal[k(t, i)] = member[k(t, i)] ? values[i] : missing;
  }
  void price(usize t, usize i, f64 value) { close[k(t, i)] = value; raw[k(t, i)] = value; }
  void absent(usize t, usize i) {
    const auto c = k(t, i);
    present[c] = 0; member[c] = 0; signal[c] = missing;
    close[c] = missing; raw[c] = missing; volume[c] = missing;
  }
  void nonmember(usize t, usize i) { member[k(t, i)] = 0; signal[k(t, i)] = missing; }
  [[nodiscard]] st::TargetReplayInput target() const {
    return {d, n, begin, end, signal, member, sessions, ids, close, raw, present, volume};
  }
  [[nodiscard]] st::NavReplayInput nav() const { return {target(), volume}; }
};
st::NavScenario flat(f64 bps, f64 borrow_bps, usize stale = 5) {
  st::NavScenario s;
  s.id = "test-flat"; s.cost = st::NavCostRule::FlatBpsV1; s.flat_bps = bps;
  s.max_participation = uncapped;
  s.financing.id = "test-flat-short"; s.financing.flat_short_bps = borrow_bps;
  s.stale_exit_sessions = stale;
  return s;
}
// Deploy fully at the first decision, then never rebalance again.
st::TargetReplayConfig hold_after_deployment() {
  st::TargetReplayConfig c; c.cadence = 1000; c.trade_fraction = 1; return c;
}
st::NavReplayConfig config(const st::NavScenario& s, f64 nav, st::TargetReplayConfig target = {}) {
  st::NavReplayConfig c;
  c.target = target; c.scenario = s; c.initial_nav = nav;
  c.liquidity_window = 4; c.min_vol_pairs = 2;
  return c;
}
u64 bits(f64 x) { return std::bit_cast<u64>(x); }

#define ATX_SAME_F64(field) \
  EXPECT_EQ(bits(a.field), bits(b.field)) << #field << " @" << a.session_index
void expect_same_day(const st::NavReplayDay& a, const st::NavReplayDay& b) {
  EXPECT_EQ(a.session_index, b.session_index);
  EXPECT_EQ(a.decision, b.decision); EXPECT_EQ(a.executed, b.executed);
  ATX_SAME_F64(pretrade_nav); ATX_SAME_F64(posttrade_nav); ATX_SAME_F64(net_return);
  ATX_SAME_F64(gross_return); ATX_SAME_F64(writeoff_return); ATX_SAME_F64(trade_cost_return);
  ATX_SAME_F64(borrow_return); ATX_SAME_F64(traded_dollars); ATX_SAME_F64(one_way_turnover);
  ATX_SAME_F64(trade_cost_dollars); ATX_SAME_F64(impact_cost_dollars);
  ATX_SAME_F64(unfilled_dollars); ATX_SAME_F64(planned_turnover); ATX_SAME_F64(planned_gross);
  ATX_SAME_F64(applied_fraction); ATX_SAME_F64(month_planned); ATX_SAME_F64(long_dollars);
  ATX_SAME_F64(short_dollars); ATX_SAME_F64(stale_long_dollars); ATX_SAME_F64(cash_ratio);
  EXPECT_EQ(a.fills, b.fills); EXPECT_EQ(a.capped_fills, b.capped_fills);
  EXPECT_EQ(a.blocked_absent, b.blocked_absent); EXPECT_EQ(a.held_names, b.held_names);
  EXPECT_EQ(a.stale_names, b.stale_names); EXPECT_EQ(a.guarded_intervals, b.guarded_intervals);
}
#undef ATX_SAME_F64
void expect_same_events_before(const st::NavReplayResult& a, const st::NavReplayResult& b,
                               i64 before_session) {
  std::vector<st::NavEvent> x, y;
  for (const auto& e : a.events) if (e.session < before_session) x.push_back(e);
  for (const auto& e : b.events) if (e.session < before_session) y.push_back(e);
  ASSERT_EQ(x.size(), y.size());
  for (usize k = 0; k < x.size(); ++k) {
    EXPECT_EQ(x[k].kind, y[k].kind); EXPECT_EQ(x[k].instrument_id, y[k].instrument_id);
    EXPECT_EQ(x[k].run_length, y[k].run_length);
    EXPECT_EQ(bits(x[k].exposure), bits(y[k].exposure));
    EXPECT_EQ(bits(x[k].pnl), bits(y[k].pnl));
  }
}
const st::NavEvent* find_event(const st::NavReplayResult& r, st::NavEventKind kind, u64 id) {
  for (const auto& e : r.events) if (e.kind == kind && e.instrument_id == id) return &e;
  return nullptr;
}
struct Lcg {
  u64 state;
  f64 next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * 0x1.0p-53;
  }
};
// Random walk prices/volumes/signals from row `from` on, with absent and
// present-nonmember cells, all satisfying the NAV input contract.
void randomize_rows(Panel& p, u64 seed, usize from) {
  Lcg g{seed};
  for (usize i = 0; i < p.n; ++i) {
    f64 level = 50 + 100 * g.next();
    for (usize t = from; t < p.d; ++t) {
      level *= 1 + 0.04 * (g.next() - 0.5);
      const auto c = p.k(t, i);
      p.present[c] = 1; p.member[c] = 1; p.close[c] = level; p.raw[c] = level;
      p.volume[c] = 1e4 + 1e5 * g.next(); p.signal[c] = g.next() - 0.5;
      const f64 u = g.next();
      if (u < 0.08) p.absent(t, i); else if (u < 0.12) p.nonmember(t, i);
    }
  }
}

// ---- pinned artifact helpers (the target-replay fixture layout plus volume) ----
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_nav_replay_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
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
constexpr const char* equal_semantics =
    "exact-pre-target-composition;equal-family/equal-within;"
    "missing-or-unoriented-neutral-fixed-denominator";
constexpr const char* pinned_semantics =
    "exact-pre-target-composition;pinned-candidate-weights;"
    "missing-or-unoriented-neutral-fixed-denominator";
struct Artifact {
  st::TargetReplayRunConfig cfg;
  Json manifest, role;
};
// Writes the combined blend, then the price role (with volume), then pins the
// combined manifest to the role SHA; `edit` may alter the manifest before pinning.
template<class Edit>
Artifact write_artifact(const std::filesystem::path& dir, const Panel& p, Edit edit,
                        const std::string& volume_basis = "raw-share-volume") {
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
  const std::string pin(64, 'a');
  Artifact a;
  a.manifest = Json{{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"},
      {"role", "train"}, {"layout", "date-major-little-endian"}, {"dates", p.d},
      {"instruments", p.n}, {"score_begin", 0}, {"score_end", p.d},
      {"role_manifest_sha256", pin}, {"source_sha256", pin}, {"library_sha256", pin},
      {"train_manifest_sha256", pin}, {"run_recipe_sha256", pin},
      {"orientation_candidates_sha256", pin}, {"orientations_artifact_sha256", nullptr},
      {"role_window_required", true}, {"signal_semantics", equal_semantics},
      {"member_semantics", "decision-member-and-source-present-and-finite-positive-close;"
                           "independent-of-component-coverage"},
      {"finite_semantics",
       "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal"},
      {"actual_trades_or_returns", false}, {"finite_cells", finite_count},
      {"member_cells", members}, {"files", std::move(files)}};
  Json role_files;
  role_files["sessions.i64"] = write_payload(dir / "sessions.i64", p.sessions);
  role_files["ids.u64"] = write_payload(dir / "ids.u64", p.ids);
  role_files["close.f64"] = write_payload(dir / "close.f64", p.close);
  role_files["raw_close.f64"] = write_payload(dir / "raw_close.f64", p.raw);
  role_files["present.u8"] = write_payload(dir / "present.u8", p.present);
  role_files["member.u8"] = write_payload(dir / "member.u8", p.member);
  role_files["volume.f64"] = write_payload(dir / "volume.f64", p.volume);
  a.role = Json{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"source_sha256", pin}, {"instrument_namespace", "spiderrock.securityID"},
      {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
      {"volume_basis", volume_basis},
      {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
      {"common_stock_verified", false}, {"historical_vintage_verified", false},
      {"dates", p.d}, {"instruments", p.n}, {"score_begin", 0}, {"score_end", p.d},
      {"files", std::move(role_files)}};
  a.cfg.role_path = (dir / "role.json").string();
  a.cfg.role_sha256 = write_json(a.cfg.role_path, a.role);
  a.manifest["role_manifest_sha256"] = a.cfg.role_sha256;
  edit(a.manifest);
  a.cfg.combined_path = (dir / "train_combined.json").string();
  a.cfg.combined_sha256 = write_json(a.cfg.combined_path, a.manifest);
  a.cfg.output_directory = (dir / "out").string();
  return a;
}
Artifact write_artifact(const std::filesystem::path& dir, const Panel& p) {
  return write_artifact(dir, p, [](Json&) {});
}
// Nine sessions, three names, a one-session gap on the short and a drifting long.
Panel publication_panel() {
  Panel p(9, 3);
  p.volume.assign(p.volume.size(), 1e6);
  for (usize t = 0; t < p.d; ++t) p.price(t, 2, 100 + static_cast<f64>(t));
  p.absent(4, 0);
  p.by_name({1, 2, 3});
  return p;
}
int dispatch(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  return st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err);
}

// ---- construction options and daily GMV turnover (T4) ----
// The T2 daily CSV columns; T4 only appends after them.
constexpr const char* t2_daily_header =
    "session_index,session_ns,exec_month,decision,rebalance,executed,return_observation,"
    "pretrade_nav,posttrade_nav,net_return,gross_return,writeoff_return,trade_cost_return,"
    "borrow_return,mark_pnl_dollars,writeoff_dollars,borrow_dollars,traded_dollars,"
    "one_way_turnover,trade_cost_dollars,linear_cost_dollars,impact_cost_dollars,"
    "unrationed_cost_dollars,unrationed_unpriced,fills,capped_fills,unfilled_dollars,"
    "blocked_absent,blocked_liquidity,fallback_vol_fills,planned_turnover,planned_forced,"
    "planned_discretionary,applied_fraction,planned_gross,planned_net,month_planned,"
    "budget_excess,long_dollars,"
    "short_dollars,gross_leverage,net_leverage,held_names,stale_names,stale_long_dollars,"
    "stale_short_dollars,guarded_intervals,cash_ratio";
constexpr const char* construction_columns =
    ",neutralize,neutralize_used,neutralize_excluded,neutralize_excluded_share,"
    "neutralize_amplification,banded_names";
std::string first_line(const std::filesystem::path& path) {
  std::ifstream in(path); std::string line; std::getline(in, line); return line;
}
// The strategy_price_exposures_test noisy panel, bit for bit (common factor with
// per-name loadings, noise scales and dollar volumes on coprime strides), on
// weekday sessions; every name a present member with a random signal.
Panel noisy_panel(usize dates, usize names, u64 seed) {
  Panel p(dates, names);
  Lcg rng{seed};
  for (usize i = 0; i < names; ++i) p.close[p.k(0, i)] = 20.0 + static_cast<f64>(i);
  for (usize t = 1; t < dates; ++t) {
    const f64 common = 0.03 * (rng.next() - 0.5);
    for (usize i = 0; i < names; ++i) {
      const f64 loading = 0.4 + 0.15 * static_cast<f64>((i * 5) % names);
      const f64 idio = 0.004 + 0.003 * static_cast<f64>((i * 7) % names);
      p.close[p.k(t, i)] =
          p.close[p.k(t - 1, i)] * (1 + loading * common + idio * (rng.next() - 0.5));
    }
  }
  for (usize t = 0; t < dates; ++t)
    for (usize i = 0; i < names; ++i) {
      p.raw[p.k(t, i)] = p.close[p.k(t, i)];
      p.volume[p.k(t, i)] =
          1e4 * static_cast<f64>(1 + (i * 11) % names) * (0.5 + rng.next());
    }
  Lcg draws{seed + 1000};
  for (auto& s : p.signal) s = draws.next() - 0.5;
  return p;
}
// Daily full rebalance, price-risk-v1 on short windows (every name has exposures
// from decision 20 on), band 0.5 x the average weight.
st::TargetReplayConfig construction_daily() {
  st::TargetReplayConfig c; c.cadence = 1; c.trade_fraction = 1;
  c.neutralize = st::TargetNeutralize::PriceRiskV1;
  c.price_risk.beta_window = 40; c.price_risk.vol_window = 20; c.price_risk.adv_window = 10;
  c.price_risk.min_return_pairs = 20; c.price_risk.min_names = 5;
  c.band_multiple = 0.5;
  return c;
}
void expect_same_construction(const st::ConstructionDay& a, const st::ConstructionDay& b,
                              usize t) {
  EXPECT_EQ(a.rebalance, b.rebalance) << t;
  EXPECT_EQ(a.neutralize, b.neutralize) << t;
  EXPECT_EQ(a.neutralize_used, b.neutralize_used) << t;
  EXPECT_EQ(a.neutralize_excluded, b.neutralize_excluded) << t;
  EXPECT_EQ(bits(a.neutralize_excluded_share), bits(b.neutralize_excluded_share)) << t;
  EXPECT_EQ(bits(a.neutralize_amplification), bits(b.neutralize_amplification)) << t;
}
// A lockstep book against the same scenario replayed on its own: bit-identical.
void expect_same_result(const st::NavReplayResult& a, const st::NavReplayResult& b) {
  ASSERT_EQ(a.days.size(), b.days.size());
  for (usize t = 0; t < a.days.size(); ++t) {
    const auto& x = a.days[t]; const auto& y = b.days[t];
    expect_same_day(x, y);
    expect_same_construction(x.construction, y.construction, t);
    EXPECT_EQ(x.construction.banded_names, y.construction.banded_names) << t;
    EXPECT_EQ(bits(x.pretrade_gross_dollars), bits(y.pretrade_gross_dollars)) << t;
    EXPECT_EQ(bits(x.one_way_turnover_gmv), bits(y.one_way_turnover_gmv)) << t;
  }
  expect_same_events_before(a, b, std::numeric_limits<i64>::max());
  EXPECT_EQ(a.deployment_index, b.deployment_index);
  EXPECT_EQ(bits(a.participation_p95), bits(b.participation_p95));
  EXPECT_EQ(bits(a.participation_max), bits(b.participation_max));
}

// ---- financing (T10) ----
// Borrow fields on a panel's geometry: 1e8 shares out and 1e6 short everywhere, so
// a name priced >= 10 has no flag (the fixture names are present from row 0, hence
// seasoned): GC.
struct Fields {
  std::vector<f64> shares_out, si_shares;
  explicit Fields(const Panel& p) : shares_out(p.d * p.n, 1e8), si_shares(p.d * p.n, 1e6) {}
  void set(const Panel& p, usize t, usize i, f64 shares, f64 short_shares) {
    shares_out[p.k(t, i)] = shares; si_shares[p.k(t, i)] = short_shares;
  }
  [[nodiscard]] st::NavFinancingFields view() const { return {shares_out, si_shares}; }
};
st::NavReplayInput with_fields(const Panel& p, const Fields& f) {
  return {p.target(), p.volume, f.view()};
}
// Shares out 1e6..1e9, SI ratio 0..0.2, 5% missing short interest: mixed tiers.
Fields random_fields(const Panel& p, u64 seed) {
  Fields f(p); Lcg g{seed};
  for (usize k = 0; k < f.shares_out.size(); ++k) {
    f.shares_out[k] = std::pow(10.0, 6 + 3 * g.next());
    f.si_shares[k] = f.shares_out[k] * 0.2 * g.next();
    if (g.next() < 0.05) f.si_shares[k] = missing;
  }
  return f;
}
st::NavFinancing swap_fin() { return st::nav_financing_scenarios()[0]; }
void expect_same_financing(const st::NavReplayDay& a, const st::NavReplayDay& b) {
  const auto t = a.session_index;
  EXPECT_EQ(bits(a.long_financing_dollars), bits(b.long_financing_dollars)) << t;
  EXPECT_EQ(bits(a.long_financing_return), bits(b.long_financing_return)) << t;
  for (usize k = 0; k < 3; ++k) {
    EXPECT_EQ(bits(a.short_dollars_by_tier[k]), bits(b.short_dollars_by_tier[k])) << t;
    EXPECT_EQ(bits(a.short_financing_by_tier[k]), bits(b.short_financing_by_tier[k])) << t;
  }
  EXPECT_EQ(a.missing_predictor_shorts, b.missing_predictor_shorts) << t;
  EXPECT_EQ(a.blocked_short_names, b.blocked_short_names) << t;
  EXPECT_EQ(bits(a.blocked_short_dollars), bits(b.blocked_short_dollars)) << t;
  EXPECT_EQ(a.member_tiers, b.member_tiers) << t;
  EXPECT_EQ(a.member_missing_predictors, b.member_missing_predictors) << t;
}
constexpr const char* financing_columns =
    ",long_financing_dollars,long_financing_return,short_gc_dollars,short_warm_dollars,"
    "short_special_dollars,short_financing_gc_dollars,short_financing_warm_dollars,"
    "short_financing_special_dollars,missing_predictor_shorts,missing_predictor_short_dollars,"
    "blocked_short_names,blocked_short_dollars,member_gc,member_warm,member_special,"
    "member_missing_predictors";
// The producer's atx.research-role-fields/v1 layout (c099cade): <name>.f64 payloads
// and manifest.json pinned to the artifact's role; an unused non-point-in-time entry
// rides along. `edit` may alter the manifest before it is pinned.
template<class Edit>
st::NavFieldsPin write_fields(const std::filesystem::path& dir, const Artifact& a, const Panel& p,
                              const Fields& f, Edit edit) {
  const auto root = dir / "fields";
  if (!std::filesystem::create_directory(root))
    throw std::runtime_error("fields fixture directory");
  Json files, entries = Json::array();
  const std::pair<const char*, const std::vector<f64>*> payloads[] = {
      {"shares_out", &f.shares_out}, {"si_shares", &f.si_shares}};
  for (const auto& [name, values] : payloads) {
    const std::string file = std::string(name) + ".f64";
    files[file] = write_payload(root / file, *values);
    entries.push_back({{"name", name}, {"file", file}, {"dtype", "<f8"},
        {"layout", "date-major"}, {"shape", Json::array({p.d, p.n})},
        {"sha256", files[file].at("sha256")}, {"point_in_time", true},
        {"non_pit_aspects", Json::array()}, {"clock", std::string("fixture-") + name}});
  }
  entries.push_back({{"name", "mktcap_lagged"}, {"file", "mktcap_lagged.f64"},
      {"point_in_time", false}, {"point_in_time_reason", "fixture: presence not PIT"}});
  const auto& receipts = a.role.at("files");
  Json manifest{{"schema", "atx.research-role-fields/v1"}, {"status", "complete"},
      {"role", {{"path", "fixture"}, {"manifest_sha256", a.cfg.role_sha256},
                {"sessions_sha256", receipts.at("sessions.i64").at("sha256")},
                {"ids_sha256", receipts.at("ids.u64").at("sha256")},
                {"member_sha256", receipts.at("member.u8").at("sha256")},
                {"dates", p.d}, {"instruments", p.n}}},
      {"visibility_mark", "fixture: session-date 22:00 UTC"},
      {"fields", std::move(entries)}, {"files", std::move(files)}};
  edit(manifest);
  st::NavFieldsPin pin;
  pin.manifest_path = (root / "manifest.json").string();
  pin.manifest_sha256 = write_json(pin.manifest_path, manifest);
  return pin;
}
std::vector<std::string> lines(const std::filesystem::path& path) {
  std::ifstream in(path); std::vector<std::string> out; std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}
} // namespace

// Design fixture 1: zero cost/borrow/drift at NAV 1 -> the NAV path's planned side is
// the target replay bit-for-bit, and each decision's plan is exactly the next fill.
// Name 2 is held under both rules when it leaves membership (a present nonmember) at
// decision 7, so a forced exit is planned and filled. Name 1, used before, has an
// exact-zero d0 target (middle tie group) and v2 spends January's budget at d0, so
// under v2 it was never held and its exit was empty (forced turnover exactly 0).
TEST(StrategyNavReplay, ConstantPricesNoCostReproducesTargetReplayPlanned) {
  Panel p(27, 7);
  constexpr usize exit_name = 2, exit_day = 7;
  for (usize t = 0; t < p.d; ++t) for (usize i = 0; i < p.n; ++i) {
    p.signal[p.k(t, i)] = static_cast<f64>((i + t / 3) % 4);
    if ((i == exit_name && t >= exit_day) || (i == 6 && t < 8)) p.nonmember(t, i);
  }
  st::TargetReplayConfig v2; v2.rule = st::TargetReplayRule::MonthlyTargetBudgetV2;
  v2.cadence = 1; v2.trade_fraction = .5; v2.monthly_budget = .30;
  for (const auto& rule : {st::TargetReplayConfig{}, v2}) {
    auto planned = st::replay_targets(p.target(), rule); ASSERT_TRUE(planned);
    auto nav = st::replay_nav(p.nav(), config(flat(0, 0), 1.0, rule)); ASSERT_TRUE(nav);
    ASSERT_EQ(nav->days.size(), p.d);
    for (usize t = 0; t + 2 < p.d; ++t) {
      const auto& a = planned->days[t]; const auto& b = nav->days[t];
      ASSERT_TRUE(b.decision) << t;
      EXPECT_EQ(bits(a.turnover), bits(b.planned_turnover)) << t;
      EXPECT_EQ(bits(a.gross), bits(b.planned_gross)) << t;
      EXPECT_EQ(bits(a.net), bits(b.planned_net)) << t;
      EXPECT_EQ(bits(a.forced_turnover), bits(b.planned_forced)) << t;
      EXPECT_EQ(bits(a.applied_fraction), bits(b.applied_fraction)) << t;
      EXPECT_EQ(bits(a.month_turnover), bits(b.month_planned)) << t;
      EXPECT_EQ(bits(a.budget_excess), bits(b.budget_excess)) << t;
      EXPECT_EQ(bits(nav->days[t + 1].traded_dollars), bits(a.turnover)) << t;
      EXPECT_EQ(bits(nav->days[t + 1].one_way_turnover), bits(a.turnover)) << t;
    }
    EXPECT_FALSE(nav->days[p.d - 2].decision); EXPECT_FALSE(nav->days[p.d - 1].executed);
    for (const auto& day : nav->days) {
      EXPECT_EQ(day.pretrade_nav, 1.0); EXPECT_EQ(day.posttrade_nav, 1.0);
      EXPECT_EQ(day.net_return, 0.0);
    }
    // The forced exit is the whole plan at decision 7 (baseline: not a cadence day;
    // v2: January budget already spent, exit taken anyway and reported as excess),
    // and it is the only fill at session 8, counted in executed one-way turnover.
    EXPECT_GT(planned->forced_turnover, 0);
    EXPECT_EQ(bits(planned->days[exit_day].forced_turnover), bits(planned->forced_turnover));
    const auto& decided = nav->days[exit_day]; const auto& filled = nav->days[exit_day + 1];
    EXPECT_GT(decided.planned_forced, 0);
    EXPECT_EQ(bits(decided.planned_forced), bits(decided.planned_turnover));
    EXPECT_EQ(bits(filled.one_way_turnover), bits(decided.planned_forced));
    EXPECT_EQ(filled.fills, 1U);
    EXPECT_EQ(filled.held_names + 1, decided.held_names);
    if (rule.rule == st::TargetReplayRule::MonthlyTargetBudgetV2) {
      EXPECT_GT(decided.budget_excess, 0);
    }
  }
}

// Design fixture 2: Thu/Fri/Mon/Tue. 6 bps fills at Friday's close land in Monday's
// return, with a three-calendar-day borrow accrual on the short; exact NAV path.
TEST(StrategyNavReplay, TwoNameHandComputedDriftCostBorrow) {
  Panel p(4, 2); p.by_name({1, 2});
  const f64 a[] = {100, 100, 110, 99}, b[] = {50, 50, 55, 60.5};
  for (usize t = 0; t < 4; ++t) { p.price(t, 0, a[t]); p.price(t, 1, b[t]); }
  auto r = st::replay_nav(p.nav(), config(flat(6, 300), 1000, hold_after_deployment()));
  ASSERT_TRUE(r); const auto& d = r->days; ASSERT_EQ(d.size(), 4U);
  EXPECT_EQ(p.sessions[2] - p.sessions[1], 3 * day_ns);
  EXPECT_TRUE(d[0].decision); EXPECT_FALSE(d[0].executed);
  EXPECT_NEAR(d[0].planned_turnover, 1.0, 1e-15);
  EXPECT_EQ(d[1].pretrade_nav, 1000); EXPECT_FALSE(d[1].return_observation);
  EXPECT_NEAR(d[1].traded_dollars, 1000, 1e-9);
  EXPECT_NEAR(d[1].one_way_turnover, 1, 1e-15);
  EXPECT_NEAR(d[1].trade_cost_dollars, 0.6, 1e-12);
  EXPECT_NEAR(d[1].posttrade_nav, 999.4, 1e-9);
  const f64 rate = 300 * 1e-4;
  const f64 borrow2 = 500 * (rate * 3.0 / 365.0);
  const f64 nav2 = 999.4 - borrow2;
  EXPECT_NEAR(d[2].borrow_dollars, borrow2, 1e-12);
  EXPECT_NEAR(d[2].mark_pnl_dollars, 0, 1e-9); // short -50, long +50
  EXPECT_NEAR(d[2].pretrade_nav, nav2, 1e-9);
  EXPECT_NEAR(d[2].net_return, nav2 / 1000 - 1, 1e-14);
  EXPECT_NEAR(d[2].trade_cost_return, 0.6 / 1000, 1e-15);
  EXPECT_NEAR(d[2].borrow_return, borrow2 / 1000, 1e-15);
  EXPECT_NEAR(d[2].gross_return, 0, 1e-14);
  const f64 borrow3 = 550 * (rate * 1.0 / 365.0);
  const f64 nav3 = nav2 + 110 - borrow3; // short 550 -> 495 (+55), long 550 -> 605 (+55)
  EXPECT_NEAR(d[3].borrow_dollars, borrow3, 1e-11);
  EXPECT_NEAR(d[3].mark_pnl_dollars, 110, 1e-9);
  EXPECT_NEAR(d[3].pretrade_nav, nav3, 1e-9);
  EXPECT_NEAR(d[3].net_return, nav3 / nav2 - 1, 1e-14);
  EXPECT_NEAR(d[3].long_dollars, 605, 1e-9); EXPECT_NEAR(d[3].short_dollars, 495, 1e-9);
  EXPECT_NEAR(d[3].cash_ratio, (999.4 - borrow2 - borrow3) / nav3, 1e-12);
  for (usize t = 2; t < 4; ++t) {
    EXPECT_TRUE(d[t].return_observation);
    EXPECT_NEAR(d[t].net_return,
                d[t].gross_return - d[t].trade_cost_return - d[t].borrow_return, 1e-15);
  }
  auto summary = st::summarize_nav(*r); ASSERT_TRUE(summary);
  EXPECT_EQ(summary->observations, 2U);
  EXPECT_NEAR(summary->final_nav, nav3, 1e-9);
  EXPECT_NEAR(summary->borrow_dollars, borrow2 + borrow3, 1e-11);
  EXPECT_LE(r->max_cash_book_error, 1e-13);
}

// Design fixture 3: a held long is absent for two sessions: carried at its stale
// mark (no return), its exit order blocked, then the cumulative +20% is realized at
// the reprint before the pending exit executes at that close.
TEST(StrategyNavReplay, InteriorGapCarriesStaleThenRealizesCumulativeReturn) {
  Panel p(8, 2);
  for (usize t = 0; t < p.d; ++t) p.price(t, 1, t < 3 ? 50 : 60);
  p.absent(3, 1); p.absent(4, 1); p.by_name({1, 2});
  auto r = st::replay_nav(p.nav(), config(flat(0, 0, 5), 1000, hold_after_deployment()));
  ASSERT_TRUE(r); const auto& d = r->days;
  for (usize t = 3; t <= 4; ++t) {
    EXPECT_EQ(d[t].net_return, 0) << t; EXPECT_EQ(d[t].pretrade_nav, 1000) << t;
    EXPECT_EQ(d[t].stale_names, 1U) << t;
    EXPECT_NEAR(d[t].stale_long_dollars, 500, 1e-9) << t;
  }
  EXPECT_EQ(d[3].blocked_absent, 0U); // the exit order is placed at decision 3
  EXPECT_EQ(d[3].planned_forced, 0.5); // absent => nonmember => forced exit of 500/1000
  EXPECT_EQ(d[4].blocked_absent, 1U);
  EXPECT_NEAR(d[5].mark_pnl_dollars, 100, 1e-9);
  EXPECT_NEAR(d[5].net_return, 0.1, 1e-14);
  EXPECT_NEAR(d[5].traded_dollars, 600, 1e-9); // exit at the reprint close
  EXPECT_NEAR(d[5].one_way_turnover, 600.0 / 1100.0, 1e-15); // forced exit counts
  EXPECT_EQ(d[5].stale_names, 0U); EXPECT_EQ(d[5].long_dollars, 0);
  const auto* gap = find_event(*r, st::NavEventKind::GapResolved, 101);
  ASSERT_NE(gap, nullptr);
  EXPECT_EQ(gap->run_length, 2U); EXPECT_EQ(gap->session, p.sessions[5]);
  EXPECT_NEAR(gap->r_adj, 0.2, 1e-14); EXPECT_NEAR(gap->pnl, 100, 1e-9);
  EXPECT_EQ(r->gap_run_2_4.count, 1U); EXPECT_EQ(r->written_off.count, 0U);
  EXPECT_EQ(r->unresolved.count, 0U);
}

// Design fixture 4: S3 (K=1, Shumway adverse) writes off at the FIRST absent mark
// using only past presence; rewriting everything after the write-off session leaves
// all earlier rows bit-identical; the later reprint is diagnostic only. Under S2
// (K=5) the same gap is carried and realized, and a still-stale short is unresolved.
TEST(StrategyNavReplay, TerminalWriteOffAfterKIsCausal) {
  Panel p(10, 2);
  for (usize t = 7; t < p.d; ++t) p.price(t, 1, 120);
  for (usize t = 4; t <= 6; ++t) p.absent(t, 1);
  for (usize t = 6; t < p.d; ++t) p.absent(t, 0);
  p.by_name({1, 2});
  const auto scenarios = st::fixed_nav_scenarios();
  const auto adverse = config(scenarios[2], 1e6, hold_after_deployment());
  auto r = st::replay_nav(p.nav(), adverse); ASSERT_TRUE(r); const auto& d = r->days;
  const f64 long_before = d[3].long_dollars;
  ASSERT_GT(long_before, 0);
  EXPECT_NEAR(d[4].writeoff_dollars, long_before * -0.55, 1e-6);
  EXPECT_EQ(d[4].long_dollars, 0);
  const auto* off = find_event(*r, st::NavEventKind::WriteOff, 101);
  ASSERT_NE(off, nullptr);
  EXPECT_EQ(off->session, p.sessions[4]); EXPECT_EQ(off->run_length, 1U);
  EXPECT_EQ(off->haircut, st::nav_adverse_long_return);
  const f64 short_before = d[5].short_dollars;
  ASSERT_GT(short_before, 0);
  EXPECT_NEAR(d[6].writeoff_dollars, -short_before * 0.30, 1e-6); // a short loses 30%
  const auto* back = find_event(*r, st::NavEventKind::ReappearedAfterWriteOff, 101);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->session, p.sessions[7]); EXPECT_EQ(back->run_length, 3U);
  EXPECT_NEAR(back->pnl, long_before * (0.2 + 0.55), 1e-6);
  EXPECT_EQ(d[7].net_return, 0); // the reprint never touches NAV
  auto future = p;
  for (usize t = 5; t < p.d; ++t) for (usize i = 0; i < p.n; ++i) {
    const auto c = future.k(t, i);
    future.present[c] = 1; future.member[c] = 1; future.signal[c] = i == 0 ? 9.0 : -9.0;
    future.price(t, i, 150 + static_cast<f64>(t + i)); future.volume[c] = 1e3;
  }
  auto rewritten = st::replay_nav(future.nav(), adverse); ASSERT_TRUE(rewritten);
  for (usize t = 0; t <= 4; ++t) expect_same_day(d[t], rewritten->days[t]);
  expect_same_events_before(*r, *rewritten, p.sessions[5]);
  EXPECT_NE(bits(d[5].pretrade_nav), bits(rewritten->days[5].pretrade_nav));
  auto stale = st::replay_nav(p.nav(), config(scenarios[1], 1e6, hold_after_deployment()));
  ASSERT_TRUE(stale);
  EXPECT_EQ(stale->written_off.count, 0U);
  const auto* gap = find_event(*stale, st::NavEventKind::GapResolved, 101);
  ASSERT_NE(gap, nullptr);
  EXPECT_EQ(gap->run_length, 3U); EXPECT_NEAR(gap->r_adj, 0.2, 1e-14);
  const auto* unresolved = find_event(*stale, st::NavEventKind::UnresolvedAtEnd, 100);
  ASSERT_NE(unresolved, nullptr);
  EXPECT_EQ(unresolved->run_length, 4U); EXPECT_TRUE(unresolved->short_side);
  EXPECT_EQ(stale->unresolved.count, 1U);
  EXPECT_EQ(stale->days.back().stale_names, 1U);
}

// Design fixture 5: replacing every signal, price, volume and presence row from m on
// cannot change any row before m, under both rules and both stale policies.
TEST(StrategyNavReplay, FutureSignalAndPricesDoNotChangePast) {
  Panel p(40, 8); randomize_rows(p, 7, 0); p.begin = 5;
  const usize m = 22;
  auto future = p; randomize_rows(future, 99, m);
  st::TargetReplayConfig v2; v2.rule = st::TargetReplayRule::MonthlyTargetBudgetV2;
  v2.cadence = 1; v2.trade_fraction = .5; v2.monthly_budget = .30;
  const auto scenarios = st::fixed_nav_scenarios();
  for (const auto& rule : {st::TargetReplayConfig{}, v2}) {
    for (usize s = 1; s < scenarios.size(); ++s) {
      auto cfg = config(scenarios[s], 1e6, rule);
      cfg.liquidity_window = 5; cfg.min_vol_pairs = 3;
      auto before = st::replay_nav(p.nav(), cfg); ASSERT_TRUE(before);
      auto after = st::replay_nav(future.nav(), cfg); ASSERT_TRUE(after);
      for (usize t = p.begin; t < m; ++t)
        expect_same_day(before->days[t - p.begin], after->days[t - p.begin]);
      expect_same_events_before(*before, *after, p.sessions[m]);
      EXPECT_NE(bits(before->days[m - p.begin].pretrade_nav),
                bits(after->days[m - p.begin].pretrade_nav));
    }
  }
}

// Design fixture 6: S2 fills at most 1% of the prior-window ADV; the residual stays a
// working order and completes next session; cost is exactly SqrtImpactCost on that
// row; zero volume blocks the fill; too few clean return pairs uses the fallback.
TEST(StrategyNavReplay, ParticipationCapWorkingOrderAndSqrtCost) {
  Panel p(9, 4); p.begin = 5;
  const f64 c_path[] = {100, 100, 101, 99, 102}, d_path[] = {100, 100, 102, 100, 103};
  for (usize t = 0; t < 5; ++t) { p.price(t, 2, c_path[t]); p.price(t, 3, d_path[t]); }
  for (usize t = 0; t < p.d; ++t) {
    p.volume[p.k(t, 0)] = 1e8; p.volume[p.k(t, 1)] = 0; p.volume[p.k(t, 2)] = 1e8;
    p.volume[p.k(t, 3)] = 2e5;
  }
  p.absent(3, 0); p.by_name({1, 2, 3, 4});
  auto cfg = config(st::fixed_nav_scenarios()[1], 1e6, hold_after_deployment());
  cfg.liquidity_window = 4; cfg.min_vol_pairs = 4;
  auto r = st::replay_nav(p.nav(), cfg); ASSERT_TRUE(r);
  const auto model = bk::SqrtImpactCost::create({0.6, 0.5}, 0.01, 1.0).value();
  // Same window/pair recipe as the replay (no pair is guarded in this fixture).
  const auto row = [&](usize t, usize i, bool& fallback) {
    f64 dollars = 0; std::vector<f64> returns;
    for (usize k = t - cfg.liquidity_window; k < t; ++k) {
      if (!p.present[p.k(k, i)]) continue;
      dollars += p.raw[p.k(k, i)] * p.volume[p.k(k, i)];
      if (p.present[p.k(k - 1, i)])
        returns.push_back(p.close[p.k(k, i)] / p.close[p.k(k - 1, i)] - 1);
    }
    f64 mean = 0, ss = 0;
    for (const auto x : returns) mean += x / static_cast<f64>(returns.size());
    for (const auto x : returns) ss += (x - mean) * (x - mean);
    fallback = returns.size() < cfg.min_vol_pairs;
    return bk::LiquidityRow{dollars / static_cast<f64>(cfg.liquidity_window),
        fallback ? 0.05 : std::sqrt(ss / static_cast<f64>(returns.size() - 1)), 5.0};
  };
  const f64 target[] = {-375000, -125000, 125000, 375000};
  bool fb_a = false, fb_b = false, fb_c = false, fb_d = false;
  const auto a6 = row(6, 0, fb_a), b6 = row(6, 1, fb_b), c6 = row(6, 2, fb_c);
  const auto d6 = row(6, 3, fb_d);
  EXPECT_TRUE(fb_a); EXPECT_FALSE(fb_c); EXPECT_FALSE(fb_d); EXPECT_EQ(b6.adv_dollars, 0);
  const auto ca = model.cost(0, 6, target[0], a6), cc = model.cost(2, 6, target[2], c6);
  const auto cd = model.cost(3, 6, target[3], d6);
  const f64 fill_d = 0.01 * d6.adv_dollars;
  ASSERT_LT(fill_d, target[3]); EXPECT_EQ(cd.filled_dollars, fill_d);
  const auto& six = r->days[6 - p.begin];
  EXPECT_EQ(six.fills, 3U); EXPECT_EQ(six.capped_fills, 1U);
  EXPECT_EQ(six.blocked_liquidity, 1U); EXPECT_EQ(six.fallback_vol_fills, 1U);
  EXPECT_EQ(six.unrationed_unpriced, 1U);
  EXPECT_NEAR(six.long_dollars, 125000 + fill_d, 1e-6);
  EXPECT_NEAR(six.short_dollars, 375000, 1e-6);
  EXPECT_NEAR(six.traded_dollars, 375000 + 125000 + fill_d, 1e-6);
  EXPECT_NEAR(six.unfilled_dollars, (375000 - fill_d) + 125000, 1e-6);
  const f64 expected_cost = ca.cost_dollars + cc.cost_dollars + cd.cost_dollars;
  EXPECT_NEAR(six.trade_cost_dollars, expected_cost, 1e-9 * expected_cost);
  EXPECT_NEAR(six.linear_cost_dollars, (375000 + 125000 + fill_d) * 6e-4, 1e-6);
  EXPECT_GT(six.impact_cost_dollars, 0);
  bool fb = false;
  const auto d7 = row(7, 3, fb);
  const auto rest = model.cost(3, 7, target[3] - fill_d, d7);
  EXPECT_NEAR(rest.filled_dollars, target[3] - fill_d, 1e-6); // the residual completes
  const auto& seven = r->days[7 - p.begin];
  EXPECT_EQ(seven.fills, 1U); EXPECT_EQ(seven.capped_fills, 0U);
  EXPECT_EQ(seven.blocked_liquidity, 1U); // the zero-volume short still cannot fill
  EXPECT_NEAR(seven.trade_cost_dollars, rest.cost_dollars, 1e-9 * rest.cost_dollars);
  EXPECT_NEAR(seven.long_dollars, 500000, 1e-6);
  EXPECT_NEAR(r->participation_max, 0.01, 1e-12);
  // Costs of the session-6 fills land in the session-7 return.
  EXPECT_NEAR(seven.trade_cost_return, six.trade_cost_dollars / six.pretrade_nav, 1e-15);
}

// Design fixture 7: turnover is booked to the calendar month of the FILL session; a
// Jan 31 decision executes Feb 3 and counts in February. Months reconcile to the
// total; the deployment month is flagged and excluded only from *_ex_deployment.
TEST(StrategyNavReplay, MonthlyTurnoverDefinitionDeploymentReconciles) {
  Panel p(50, 5);
  for (usize t = 0; t < p.d; ++t)
    for (usize i = 0; i < p.n; ++i) p.signal[p.k(t, i)] = static_cast<f64>((i + t) % 5);
  st::TargetReplayConfig rule; rule.cadence = 1; rule.trade_fraction = .25;
  auto r = st::replay_nav(p.nav(), config(flat(0, 0), 1.0, rule)); ASSERT_TRUE(r);
  const auto& d = r->days;
  const auto it = std::find(p.sessions.begin(), p.sessions.end(), session(2020, 1, 31));
  ASSERT_NE(it, p.sessions.end());
  const auto j = static_cast<usize>(it - p.sessions.begin());
  ASSERT_EQ(p.sessions[j + 1], session(2020, 2, 3));
  EXPECT_EQ(d[j].calendar_month, 202001U); EXPECT_TRUE(d[j].decision);
  EXPECT_GT(d[j].planned_turnover, 0);
  EXPECT_EQ(d[j + 1].calendar_month, 202002U); EXPECT_TRUE(d[j + 1].executed);
  EXPECT_EQ(bits(d[j + 1].one_way_turnover), bits(d[j].planned_turnover));
  auto s = st::summarize_nav(*r); ASSERT_TRUE(s);
  ASSERT_EQ(s->months.size(), 3U);
  EXPECT_EQ(s->months[0].month, 202001U); EXPECT_TRUE(s->months[0].is_deployment_month);
  EXPECT_FALSE(s->months[1].is_deployment_month); EXPECT_FALSE(s->months[2].is_deployment_month);
  f64 jan_planned = 0, feb_actual = 0, total = 0; usize sessions = 0, within = 0;
  for (const auto& day : d) {
    if (day.decision && day.calendar_month == 202001U) jan_planned += day.planned_turnover;
    if (day.executed && day.calendar_month == 202002U) feb_actual += day.one_way_turnover;
  }
  EXPECT_EQ(bits(s->months[0].planned_turnover), bits(jan_planned)); // includes Jan 31
  EXPECT_EQ(bits(s->months[1].one_way_turnover), bits(feb_actual));  // includes Feb 3
  for (const auto& month : s->months) {
    total += month.one_way_turnover; sessions += month.execution_sessions;
    within += month.one_way_turnover <= st::nav_monthly_turnover_target ? 1U : 0U;
  }
  EXPECT_EQ(sessions, p.d - 2); EXPECT_EQ(s->execution_months, 3U);
  EXPECT_NEAR(total, s->total_actual_turnover, 1e-12);
  EXPECT_TRUE(s->deployed); EXPECT_EQ(s->deployment_session, p.sessions[1]);
  EXPECT_EQ(s->deployment_turnover, d[1].one_way_turnover);
  EXPECT_NEAR(s->mean_monthly_turnover, total / 3, 1e-14);
  EXPECT_NEAR(s->mean_monthly_turnover_ex_deployment,
              (s->months[1].one_way_turnover + s->months[2].one_way_turnover) / 2, 1e-14);
  EXPECT_EQ(s->months_within_target, within);
  EXPECT_EQ(s->meets_turnover_target_all_months,
            s->max_monthly_turnover <= st::nav_monthly_turnover_target);
  EXPECT_EQ(s->meets_turnover_target_mean_ex_deployment,
            s->mean_monthly_turnover_ex_deployment <= st::nav_monthly_turnover_target);
}

// Design fixture 8: the pinned CLI path publishes recipe, per-scenario CSVs and the
// summary LAST, binds CSV SHAs, and refuses tampering/basis/budget before any output.
TEST(StrategyNavReplay, PinnedRunWithVolumePublishesLastAndRefusesTampering) {
  const auto p = publication_panel();
  std::ostringstream progress;
  {
    Directory dir; auto a = write_artifact(dir.path, p);
    const auto ok = st::run_nav_replay(a.cfg, progress);
    ASSERT_TRUE(ok) << ok.error().to_string();
    const auto out = dir.path / "out";
    const auto summary = read_json(out / "summary.json");
    const auto recipe = read_json(out / "recipe.json");
    EXPECT_EQ(summary.at("schema"), "atx.dsl-nav-replay-summary/v1");
    EXPECT_EQ(summary.at("status"), "complete");
    EXPECT_EQ(summary.at("primary_scenario"), "modeled-1bn-stale5-v1");
    EXPECT_EQ(summary.at("capacity_qualified"), false);
    EXPECT_TRUE(summary.at("composition_weights_sha256").is_null());
    EXPECT_EQ(summary.at("source_bindings").at("schema"), "atx.dsl-combined-signal/v1");
    EXPECT_EQ(recipe.at("schema"), "atx.dsl-nav-replay/v1");
    EXPECT_EQ(recipe.at("cost_input_status"), "declared-unfitted-scenario-no-locate");
    EXPECT_EQ(summary.at("recipe_sha256"), co::sha256_hex(recipe.dump()).value());
    const auto& scenarios = summary.at("scenarios");
    ASSERT_EQ(scenarios.size(), 3U);
    for (const auto& s : scenarios) {
      const auto id = s.at("scenario").get<std::string>();
      const auto daily = out / ("daily_" + id + ".csv"), events = out / ("events_" + id + ".csv");
      EXPECT_EQ(s.at("daily_csv_sha256"), co::sha256_file(daily.string()).value()) << id;
      EXPECT_EQ(s.at("events_csv_sha256"), co::sha256_file(events.string()).value()) << id;
      EXPECT_EQ(s.at("observations"), p.d - 2) << id;
      EXPECT_TRUE(s.contains("turnover_definition")) << id;
      EXPECT_TRUE(s.contains("mean_monthly_one_way_turnover")) << id;
      EXPECT_TRUE(s.contains("mean_monthly_one_way_turnover_ex_deployment_month")) << id;
      EXPECT_NEAR(s.at("planned_vs_actual").at("monthly_reconciliation_error").get<f64>(), 0,
                  1e-12) << id;
    }
    EXPECT_EQ(scenarios[1].at("primary"), true);
    EXPECT_EQ(scenarios[1].at("missing").at("gap_run_1").at("count"), 1);
    EXPECT_EQ(scenarios[2].at("missing").at("written_off").at("count"), 1);
    EXPECT_FALSE(st::run_nav_replay(a.cfg, progress)); // exclusive output
    auto targets = a.cfg; targets.output_directory = (dir.path / "targets").string();
    EXPECT_TRUE(st::run_target_replay(targets, progress)); // unchanged path, same artifact
  }
  { // tampered volume payload
    Directory dir; auto a = write_artifact(dir.path, p);
    std::fstream payload(dir.path / "volume.f64", std::ios::binary | std::ios::in | std::ios::out);
    const char changed = 7; payload.write(&changed, 1); payload.close();
    EXPECT_FALSE(st::run_nav_replay(a.cfg, progress));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "out"));
  }
  { // volume basis is part of the pinned role recipe
    Directory dir; auto a = write_artifact(dir.path, p, [](Json&) {}, "dollar-volume");
    EXPECT_FALSE(st::run_nav_replay(a.cfg, progress));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "out"));
  }
  { // admission before any payload or output
    Directory dir; auto a = write_artifact(dir.path, p); a.cfg.target.max_working_bytes = 1;
    const auto refused = st::run_nav_replay(a.cfg, progress);
    ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code(), co::ErrorCode::OutOfRange);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "out"));
  }
  std::ostringstream out, err;
  EXPECT_EQ(dispatch({"nav", "--combined", "x", "--one-way-bps", "5"}, out, err), 2);
  EXPECT_EQ(dispatch({"nav", "--combined", "x", "--combined-sha256", "y", "--output", "z"},
                     out, err), 2); // --role is required
}

// Design fixture 9: S3's haircuts are the engine's causal missing-price stress values,
// and the fixed scenarios carry exactly the registered numbers.
TEST(StrategyNavReplay, TerminalStressConstantsMatchBookReplay) {
  EXPECT_EQ(st::nav_adverse_long_return,
            bk::assumed_missing_price_return(bk::ListingExchange::Unknown, false).value);
  EXPECT_EQ(st::nav_adverse_short_return,
            bk::assumed_missing_price_return(bk::ListingExchange::Unknown, true).value);
  EXPECT_EQ(st::nav_adverse_long_return, bk::kShumwayNasdaqReturn);
  EXPECT_EQ(st::nav_adverse_short_return, -bk::kShumwayNyseAmexReturn);
  const auto s = st::fixed_nav_scenarios();
  ASSERT_EQ(s.size(), 3U);
  EXPECT_EQ(s[0].id, "linear-6bps-stale5-v1"); EXPECT_EQ(s[0].cost, st::NavCostRule::FlatBpsV1);
  EXPECT_EQ(s[0].flat_bps, 6); EXPECT_EQ(s[0].stale_exit_sessions, 5U);
  EXPECT_FALSE(s[0].adverse_terminal);
  const auto& primary = s[st::nav_primary_scenario_index];
  EXPECT_EQ(primary.id, "modeled-1bn-stale5-v1");
  EXPECT_EQ(primary.cost, st::NavCostRule::SqrtImpactV1);
  EXPECT_EQ(primary.half_spread_bps, 5); EXPECT_EQ(primary.commission_bps, 1);
  EXPECT_EQ(primary.impact_y, 0.6); EXPECT_EQ(primary.impact_delta, 0.5);
  EXPECT_EQ(primary.max_participation, 0.01); EXPECT_EQ(primary.stale_exit_sessions, 5U);
  EXPECT_FALSE(primary.adverse_terminal);
  EXPECT_EQ(s[2].id, "modeled-1bn-terminal-adverse-v1");
  EXPECT_EQ(s[2].stale_exit_sessions, 1U); EXPECT_TRUE(s[2].adverse_terminal);
  EXPECT_EQ(s[2].max_participation, 0.01);
  for (const auto& scenario : s) {
    EXPECT_EQ(scenario.financing.id, "flat-300-v0") << scenario.id;
    EXPECT_EQ(scenario.financing.rule, st::NavFinancingRule::FlatShortV0) << scenario.id;
    EXPECT_EQ(scenario.financing.flat_short_bps, 300) << scenario.id;
    EXPECT_EQ(scenario.financing.day_count, 365U) << scenario.id;
    EXPECT_EQ(scenario.fallback_daily_vol, 0.05) << scenario.id;
  }
}

// Root addendum: a blend composed with externally pinned per-candidate weights is
// admitted by the shared loader, and its weights SHA is bound in source_bindings.
// Inconsistent semantics/weights pairs are refused before any output.
TEST(StrategyNavReplay, PinnedCandidateWeightsBlendIsAdmittedAndBound) {
  const auto p = publication_panel();
  const std::string weights(64, 'c');
  std::ostringstream progress;
  {
    Directory dir;
    auto a = write_artifact(dir.path, p, [&](Json& m) {
      m["signal_semantics"] = pinned_semantics; m["composition_weights_sha256"] = weights;
    });
    const auto ok = st::run_nav_replay(a.cfg, progress);
    ASSERT_TRUE(ok) << ok.error().to_string();
    const auto summary = read_json(dir.path / "out" / "summary.json");
    EXPECT_EQ(summary.at("composition_weights_sha256"), weights);
    EXPECT_EQ(summary.at("signal_semantics"), pinned_semantics);
    EXPECT_EQ(summary.at("source_bindings").at("composition_weights_sha256"), weights);
    auto targets = a.cfg; targets.output_directory = (dir.path / "targets").string();
    ASSERT_TRUE(st::run_target_replay(targets, progress));
    EXPECT_EQ(read_json(dir.path / "targets" / "summary.json")
                  .at("source_bindings").at("composition_weights_sha256"), weights);
  }
  const auto refused = [&](auto edit) {
    Directory dir; auto a = write_artifact(dir.path, p, edit);
    const bool ran = static_cast<bool>(st::run_nav_replay(a.cfg, progress));
    return !ran && !std::filesystem::exists(dir.path / "out");
  };
  EXPECT_TRUE(refused([](Json& m) { m["signal_semantics"] = pinned_semantics; }));
  EXPECT_TRUE(refused([&](Json& m) { m["composition_weights_sha256"] = weights; }));
  EXPECT_TRUE(refused([](Json& m) {
    m["signal_semantics"] = pinned_semantics; m["composition_weights_sha256"] = "not-a-sha";
  }));
  EXPECT_TRUE(refused([](Json& m) {
    m["signal_semantics"] = pinned_semantics; m["composition_weights_sha256"] = nullptr;
  }));
  EXPECT_TRUE(refused([](Json& m) {
    m["signal_semantics"] =
        "exact-pre-target-composition;other;missing-or-unoriented-neutral-fixed-denominator";
  }));
}

// T4 (a): with default construction every scenario book run in lockstep is bit-
// identical to that scenario replayed alone, and the published outputs keep the T2
// CSV columns first and unchanged (only the GMV turnover columns are appended) and
// the T2 recipe without any construction key.
TEST(StrategyNavReplay, DefaultConstructionKeepsT2OutputsAndLockstepMatchesSingleRuns) {
  Panel p(40, 8); randomize_rows(p, 7, 0); p.begin = 5;
  st::TargetReplayConfig v2; v2.rule = st::TargetReplayRule::MonthlyTargetBudgetV2;
  v2.cadence = 1; v2.trade_fraction = .5; v2.monthly_budget = .30;
  const auto scenarios = st::fixed_nav_scenarios();
  for (const auto& rule : {st::TargetReplayConfig{}, v2}) {
    auto base = config(scenarios[0], 1e6, rule);
    base.liquidity_window = 5; base.min_vol_pairs = 3;
    auto together = st::replay_nav_scenarios(p.nav(), base, scenarios);
    ASSERT_TRUE(together) << together.error().to_string();
    ASSERT_EQ(together->size(), scenarios.size());
    for (usize k = 0; k < scenarios.size(); ++k) {
      auto single = base; single.scenario = scenarios[k];
      auto alone = st::replay_nav(p.nav(), single); ASSERT_TRUE(alone);
      expect_same_result((*together)[k], *alone);
      for (const auto& day : alone->days) {
        EXPECT_EQ(day.construction.neutralize, st::NeutralizeOutcome::NotAttempted);
        EXPECT_EQ(day.construction.banded_names, 0U);
      }
    }
  }
  Directory dir; auto a = write_artifact(dir.path, publication_panel());
  std::ostringstream progress;
  const auto ok = st::run_nav_replay(a.cfg, progress);
  ASSERT_TRUE(ok) << ok.error().to_string();
  const auto out = dir.path / "out";
  const auto recipe = read_json(out / "recipe.json");
  EXPECT_EQ(recipe.at("rule"), "baseline-target-v1");
  EXPECT_EQ(recipe.at("desired_target_postprocess"), "none");
  for (const auto* key : {"neutralize", "band_multiple", "price_risk", "neutralize_guard", "band"})
    EXPECT_FALSE(recipe.contains(key)) << key;
  EXPECT_EQ(recipe.at("daily_turnover_mean_max"), 0.20);
  EXPECT_EQ(recipe.at("daily_turnover_p95_max"), 0.30);
  const auto summary = read_json(out / "summary.json");
  EXPECT_EQ(summary.at("rule"), "baseline-target-v1");
  for (const auto& s : summary.at("scenarios")) {
    const auto id = s.at("scenario").get<std::string>();
    EXPECT_FALSE(s.contains("construction")) << id;
    EXPECT_TRUE(s.at("daily_turnover_gmv").contains("p95")) << id;
    EXPECT_TRUE(s.at("meets_daily_turnover_mean").is_boolean()) << id;
    EXPECT_TRUE(s.at("meets_daily_turnover_p95").is_boolean()) << id;
    EXPECT_TRUE(s.contains("months_le_0.30")) << id; // legacy field kept
    EXPECT_EQ(first_line(out / ("daily_" + id + ".csv")),
              std::string(t2_daily_header) + ",pretrade_gross_dollars,one_way_turnover_gmv")
        << id;
  }
}

// T4 (e): neutralize + band at cadence 1 end to end. The NAV path forms exactly
// the target replay's construction at its extension point (same outcome, used
// names and amplification at every decision); a skipped decision keeps weights; the
// band acts; lockstep books match single runs; the pinned CLI path publishes the
// composed rule id, the construction recipe keys, columns and diagnostics.
TEST(StrategyNavReplay, NeutralizeAndBandRunEndToEndAtCadenceOne) {
  const auto p = noisy_panel(70, 12, 21);
  const auto cfg = config(flat(6, 300), 1e6, construction_daily());
  auto r = st::replay_nav(p.nav(), cfg);
  ASSERT_TRUE(r) << r.error().to_string();
  auto planned = st::replay_targets(p.target(), cfg.target);
  ASSERT_TRUE(planned) << planned.error().to_string();
  usize applied = 0, banded = 0;
  for (usize t = 0; t + 2 < p.d; ++t) {
    const auto& day = r->days[t];
    const auto& c = day.construction;
    ASSERT_TRUE(day.decision) << t;
    expect_same_construction(c, planned->days[t].construction, t);
    EXPECT_EQ(day.rebalance, c.rebalance) << t;
    EXPECT_EQ(c.rebalance, c.neutralize == st::NeutralizeOutcome::Applied) << t;
    if (!c.rebalance) {
      EXPECT_EQ(day.planned_discretionary, 0) << t; // skipped: members keep weights
      EXPECT_EQ(c.banded_names, 0U) << t;
    }
    if (t < 20) {
      EXPECT_EQ(c.neutralize, st::NeutralizeOutcome::SkippedTooFewNames) << t;
      EXPECT_EQ(r->days[t + 1].traded_dollars, 0) << t;
    }
    applied += c.rebalance ? 1U : 0U;
    banded += c.banded_names;
  }
  EXPECT_GT(applied, 0U);
  EXPECT_GT(banded, 0U);
  EXPECT_GE(r->deployment_index, 21U);
  const auto scenarios = st::fixed_nav_scenarios();
  auto together = st::replay_nav_scenarios(p.nav(), cfg, scenarios);
  ASSERT_TRUE(together) << together.error().to_string();
  for (usize k = 0; k < scenarios.size(); ++k) {
    auto single = cfg; single.scenario = scenarios[k];
    auto alone = st::replay_nav(p.nav(), single); ASSERT_TRUE(alone);
    expect_same_result((*together)[k], *alone);
  }
  Directory dir; auto a = write_artifact(dir.path, p); a.cfg.target = cfg.target;
  std::ostringstream progress;
  const auto ok = st::run_nav_replay(a.cfg, progress);
  ASSERT_TRUE(ok) << ok.error().to_string();
  const auto out = dir.path / "out";
  const std::string id = "baseline-target-v1+neutral-price-risk-v1+band-0.5";
  const auto recipe = read_json(out / "recipe.json");
  EXPECT_EQ(recipe.at("rule"), id);
  EXPECT_EQ(recipe.at("neutralize"), "price-risk-v1");
  EXPECT_EQ(recipe.at("desired_target_postprocess"), "price-risk-v1");
  EXPECT_EQ(recipe.at("band_multiple"), 0.5);
  EXPECT_EQ(recipe.at("price_risk").at("beta_window"), 40);
  EXPECT_EQ(recipe.at("neutralize_guard").at("max_amplification"), 5.0);
  EXPECT_EQ(recipe.at("neutralize_guard").at("max_excluded_gross_share"), 0.5);
  const auto summary = read_json(out / "summary.json");
  EXPECT_EQ(summary.at("rule"), id);
  for (const auto& s : summary.at("scenarios")) {
    const auto name = s.at("scenario").get<std::string>();
    const auto& c = s.at("construction");
    EXPECT_EQ(c.at("rule_id"), id) << name;
    EXPECT_EQ(c.at("decisions"), p.d - 2) << name;
    EXPECT_EQ(c.at("neutralize_attempted_decisions"), p.d - 2) << name;
    EXPECT_EQ(c.at("neutralize_skipped_decisions").get<usize>() +
                  c.at("neutralize_applied_decisions").get<usize>(), p.d - 2) << name;
    EXPECT_GE(c.at("neutralize_skip_reasons").at("too-few-names").get<usize>(), 20U) << name;
    EXPECT_EQ(c.at("neutralize_used_names_min"), 0) << name;
    EXPECT_EQ(first_line(out / ("daily_" + name + ".csv")),
              std::string(t2_daily_header) + ",pretrade_gross_dollars,one_way_turnover_gmv" +
                  construction_columns) << name;
  }
  auto targets = a.cfg; targets.output_directory = (dir.path / "targets").string();
  ASSERT_TRUE(st::run_target_replay(targets, progress)); // the role carries volume
  EXPECT_EQ(read_json(dir.path / "targets" / "recipe.json").at("rule"), id);
  EXPECT_TRUE(read_json(dir.path / "targets" / "summary.json").contains("construction"));
  std::ostringstream o, e;
  EXPECT_EQ(dispatch({"nav", "--neutralize", "bogus"}, o, e), 2);
  EXPECT_EQ(dispatch({"nav", "--combined", "x", "--combined-sha256", "y", "--role", "r",
                      "--role-sha256", "s", "--output", (dir.path / "never").string(),
                      "--daily-turnover-mean-max", "0"}, o, e), 1);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "never"));
}

// T4 (f): tau_t = traded / pre-trade GMV on execution sessions, hand-checked at
// NAV 1000 with constant prices and no costs (GMV stays 1000): deployment (GMV 0)
// excluded; mean/median/p95/max over {0, .5, 0, .5, 2}; the ceiling flags flip at
// the declared thresholds. A forced exit is a fill and counts.
TEST(StrategyNavReplay, DailyGmvTurnoverStatisticsAndCeilingsHandChecked) {
  Panel p(8, 4);
  const f64 a[] = {1, 2, 3, 4}, b[] = {2, 1, 3, 4}, c[] = {4, 3, 2, 1};
  const f64* rows[] = {a, a, b, b, a, c, a, a};
  for (usize t = 0; t < p.d; ++t)
    for (usize i = 0; i < p.n; ++i) p.signal[p.k(t, i)] = rows[t][i];
  st::TargetReplayConfig daily; daily.cadence = 1; daily.trade_fraction = 1;
  auto r = st::replay_nav(p.nav(), config(flat(0, 0), 1000, daily)); ASSERT_TRUE(r);
  const auto& d = r->days;
  EXPECT_EQ(r->deployment_index, 1U);
  EXPECT_EQ(d[1].pretrade_gross_dollars, 0);
  EXPECT_TRUE(std::isnan(d[1].one_way_turnover_gmv)); // deployment: no pre-trade book
  const f64 expected[] = {0, .5, 0, .5, 2.0};         // sessions 2..6: A A B B A C
  for (usize t = 2; t <= 6; ++t) {
    EXPECT_EQ(d[t].pretrade_gross_dollars, 1000) << t;
    EXPECT_EQ(d[t].one_way_turnover_gmv, expected[t - 2]) << t;
    EXPECT_EQ(d[t].one_way_turnover_gmv, d[t].traded_dollars / d[t].pretrade_gross_dollars) << t;
  }
  EXPECT_EQ(d[0].one_way_turnover_gmv, 0); // not execution sessions
  EXPECT_EQ(d[7].one_way_turnover_gmv, 0);
  auto s = st::summarize_nav(*r); ASSERT_TRUE(s);
  EXPECT_EQ(s->daily_turnover_sessions, 5U);
  EXPECT_EQ(s->daily_turnover_zero_gmv_sessions, 0U);
  EXPECT_NEAR(s->daily_turnover_mean, 0.6, 1e-15);
  EXPECT_EQ(s->daily_turnover_median, 0.5);
  EXPECT_NEAR(s->daily_turnover_p95, 1.7, 1e-12); // .5 + .8 * (2 - .5) at (5 - 1) * .95
  EXPECT_EQ(s->daily_turnover_max, 2.0);
  EXPECT_EQ(s->daily_turnover_max_session, p.sessions[6]);
  EXPECT_FALSE(s->meets_daily_turnover_mean); // default ceilings .20 / .30
  EXPECT_FALSE(s->meets_daily_turnover_p95);
  const auto flags = [&](f64 mean_max, f64 p95_max) {
    const auto x = st::summarize_nav(*r, {mean_max, p95_max});
    return x ? std::pair{x->meets_daily_turnover_mean, x->meets_daily_turnover_p95}
             : std::pair{false, false};
  };
  EXPECT_EQ(flags(.6 + 1e-9, 1.7 + 1e-9), std::pair(true, true));
  EXPECT_EQ(flags(.6 - 1e-9, 1.7 + 1e-9), std::pair(false, true));
  EXPECT_EQ(flags(.6 + 1e-9, 1.7 - 1e-9), std::pair(true, false));
  EXPECT_FALSE(st::summarize_nav(*r, {0.0, .3})); // a declared ceiling must be positive
  // Forced exit: two names deployed at session 1; name 0 leaves membership (still
  // priced) at decision 2 and is exited alone at session 3: 500 of a 1000 book.
  Panel q(6, 2); q.by_name({1, 2});
  for (usize t = 2; t < q.d; ++t) q.nonmember(t, 0);
  auto exited = st::replay_nav(q.nav(), config(flat(0, 0), 1000, hold_after_deployment()));
  ASSERT_TRUE(exited);
  const auto& e = exited->days;
  EXPECT_EQ(e[2].planned_forced, 0.5);
  EXPECT_EQ(e[3].traded_dollars, 500);
  EXPECT_EQ(e[3].one_way_turnover_gmv, 0.5);
  EXPECT_EQ(e[4].pretrade_gross_dollars, 500);
  auto es = st::summarize_nav(*exited); ASSERT_TRUE(es);
  EXPECT_EQ(es->daily_turnover_sessions, 3U); // sessions 2, 3, 4
  EXPECT_NEAR(es->daily_turnover_mean, 0.5 / 3, 1e-15);
  EXPECT_NEAR(es->daily_turnover_p95, 0.45, 1e-15); // .9 of the way from 0 to .5
  EXPECT_EQ(es->daily_turnover_max, 0.5);
  EXPECT_EQ(es->daily_turnover_max_session, q.sessions[3]);
}

// ---- T10: financing (swap-fin-v1 primary; flat-300-v0 and engine-tiers-v1 stresses) ----

// T10 declared numbers: the financing specs, the matrix layout (primary S2 x
// swap-fin-v1) and engine-tiers-v1 = the engine BorrowTierRecipe default fees.
TEST(StrategyNavReplay, FinancingScenariosAndMatrixCarryTheDeclaredNumbers) {
  const auto f = st::nav_financing_scenarios();
  ASSERT_EQ(f.size(), 3U);
  EXPECT_EQ(f[0].id, "swap-fin-v1"); EXPECT_EQ(f[0].rule, st::NavFinancingRule::TieredSwapV1);
  EXPECT_EQ(f[0].flat_short_bps, 0); EXPECT_EQ(f[0].long_spread_bps, 40);
  EXPECT_EQ(f[0].short_spread_bps, 20); EXPECT_EQ(f[0].gc_bps, 30);
  EXPECT_EQ(f[0].warm_bps, 100); EXPECT_EQ(f[0].special_bps, 500);
  EXPECT_EQ(f[0].day_count, 360U); EXPECT_TRUE(f[0].block_special_shorts);
  EXPECT_EQ(f[1].id, "flat-300-v0"); EXPECT_EQ(f[1].rule, st::NavFinancingRule::FlatShortV0);
  EXPECT_EQ(f[1].flat_short_bps, 300); EXPECT_EQ(f[1].long_spread_bps, 0);
  EXPECT_EQ(f[1].day_count, 365U); EXPECT_FALSE(f[1].block_special_shorts);
  const atx::engine::cost::BorrowTierRecipe engine{};
  EXPECT_EQ(f[2].id, "engine-tiers-v1"); EXPECT_EQ(f[2].rule, st::NavFinancingRule::TieredSwapV1);
  EXPECT_EQ(f[2].gc_bps, engine.gc_annual_fraction * 1e4);
  EXPECT_EQ(f[2].warm_bps, engine.warm_annual_fraction * 1e4);
  EXPECT_EQ(f[2].special_bps, engine.special_annual_fraction * 1e4);
  EXPECT_EQ(f[2].long_spread_bps, 40); EXPECT_EQ(f[2].short_spread_bps, 20);
  EXPECT_EQ(f[2].day_count, 360U); EXPECT_TRUE(f[2].block_special_shorts);
  const auto legacy = st::nav_scenario_matrix(false);
  const auto trading = st::fixed_nav_scenarios();
  ASSERT_EQ(legacy.size(), 3U);
  for (usize k = 0; k < 3; ++k) {
    EXPECT_EQ(legacy[k].id, trading[k].id); EXPECT_EQ(legacy[k].financing.id, "flat-300-v0");
  }
  const auto m = st::nav_scenario_matrix(true);
  ASSERT_EQ(m.size(), 5U);
  const std::pair<std::string, std::string> expected[] = {
      {trading[0].id, "swap-fin-v1"}, {trading[1].id, "swap-fin-v1"},
      {trading[2].id, "swap-fin-v1"}, {trading[1].id, "flat-300-v0"},
      {trading[1].id, "engine-tiers-v1"}};
  for (usize k = 0; k < m.size(); ++k) {
    EXPECT_EQ(m[k].id, expected[k].first) << k;
    EXPECT_EQ(m[k].financing.id, expected[k].second) << k;
  }
  EXPECT_EQ(m[st::nav_primary_scenario_index].id, "modeled-1bn-stale5-v1");
  EXPECT_EQ(m[st::nav_primary_scenario_index].financing.id, "swap-fin-v1");
}

// T10 (a): flat-300-v0 is the legacy accrual bit for bit. Inside the five-book
// financing matrix (with borrow fields, tiers formed every decision) the S2 x
// flat-300-v0 book equals the legacy S2 replay without fields, and every legacy
// book is unchanged by the presence of fields (they only add tier diagnostics).
TEST(StrategyNavReplay, FlatLegacyFinancingIsBitIdenticalInsideTheMatrix) {
  Panel p(40, 8); randomize_rows(p, 7, 0); p.begin = 5;
  const auto f = random_fields(p, 11);
  st::TargetReplayConfig daily; daily.cadence = 1; daily.trade_fraction = .5;
  const auto legacy = st::fixed_nav_scenarios();
  const auto matrix = st::nav_scenario_matrix(true);
  for (const auto& rule : {st::TargetReplayConfig{}, daily}) {
    auto base = config(legacy[0], 1e6, rule);
    base.liquidity_window = 5; base.min_vol_pairs = 3;
    auto together = st::replay_nav_scenarios(with_fields(p, f), base, matrix);
    ASSERT_TRUE(together) << together.error().to_string();
    auto alone = base; alone.scenario = legacy[st::nav_primary_scenario_index];
    auto s2 = st::replay_nav(p.nav(), alone);
    ASSERT_TRUE(s2) << s2.error().to_string();
    expect_same_result((*together)[3], *s2);
    auto with = st::replay_nav_scenarios(with_fields(p, f), base, legacy);
    auto without = st::replay_nav_scenarios(p.nav(), base, legacy);
    ASSERT_TRUE(with); ASSERT_TRUE(without);
    for (usize k = 0; k < legacy.size(); ++k) {
      expect_same_result((*with)[k], (*without)[k]);
      EXPECT_EQ(bits((*with)[k].max_return_identity_error),
                bits((*without)[k].max_return_identity_error));
      for (const auto& day : (*with)[k].days) {
        EXPECT_EQ(day.long_financing_dollars, 0);
        EXPECT_EQ(day.blocked_short_names, 0U);
        const f64 by_tier = day.short_financing_by_tier[0] + day.short_financing_by_tier[1] +
                            day.short_financing_by_tier[2];
        EXPECT_NEAR(by_tier, day.borrow_dollars, 1e-9 * (1 + day.borrow_dollars));
      }
    }
    // The tiered books do charge differently (the matrix is not a relabeling).
    EXPECT_NE(bits((*together)[1].days.back().pretrade_nav),
              bits((*together)[3].days.back().pretrade_nav));
  }
}

// T10 (b): Thu/Fri/Mon/Tue, two names under swap-fin-v1 with 6 bps fills: the long
// pays the 40 bps long spread and the GC short 20 + 30 bps, ACT/360 on calendar
// days (three over the weekend), on pre-mark dollars; exact NAV path and identity.
TEST(StrategyNavReplay, SwapFinancingTwoNameWeekendHandComputedAct360) {
  Panel p(4, 2); p.by_name({1, 2});
  const f64 a[] = {100, 100, 110, 99}, b[] = {50, 50, 55, 60.5};
  for (usize t = 0; t < 4; ++t) { p.price(t, 0, a[t]); p.price(t, 1, b[t]); }
  const Fields f(p); // caps 5e9 / 1e10, SI 1%, seasoned: GC
  auto s = flat(6, 0); s.financing = swap_fin();
  auto r = st::replay_nav(with_fields(p, f), config(s, 1000, hold_after_deployment()));
  ASSERT_TRUE(r) << r.error().to_string();
  const auto& d = r->days; ASSERT_EQ(d.size(), 4U);
  EXPECT_EQ(p.sessions[2] - p.sessions[1], 3 * day_ns);
  EXPECT_EQ(d[0].member_tiers, (std::array<usize, 3>{2, 0, 0}));
  EXPECT_EQ(d[0].member_missing_predictors, 0U);
  EXPECT_EQ(d[0].blocked_short_names, 0U);
  EXPECT_NEAR(d[1].posttrade_nav, 999.4, 1e-9);
  EXPECT_EQ(d[1].borrow_dollars, 0); EXPECT_EQ(d[1].long_financing_dollars, 0); // flat at mark
  const f64 long_rate = 40 * 1e-4, short_rate = (20 + 30) * 1e-4;
  const f64 long2 = 500 * (long_rate * 3.0 / 360.0), short2 = 500 * (short_rate * 3.0 / 360.0);
  const f64 nav2 = 999.4 - short2 - long2;
  EXPECT_NEAR(d[2].long_financing_dollars, long2, 1e-12);
  EXPECT_NEAR(d[2].borrow_dollars, short2, 1e-12);
  EXPECT_NEAR(d[2].short_dollars_by_tier[0], 500, 1e-9);
  EXPECT_EQ(d[2].short_dollars_by_tier[1], 0); EXPECT_EQ(d[2].short_dollars_by_tier[2], 0);
  EXPECT_NEAR(d[2].short_financing_by_tier[0], short2, 1e-12);
  EXPECT_NEAR(d[2].mark_pnl_dollars, 0, 1e-9); // short -50, long +50
  EXPECT_NEAR(d[2].pretrade_nav, nav2, 1e-9);
  EXPECT_NEAR(d[2].net_return, nav2 / 1000 - 1, 1e-14);
  EXPECT_NEAR(d[2].trade_cost_return, 0.6 / 1000, 1e-15);
  EXPECT_NEAR(d[2].borrow_return, short2 / 1000, 1e-15);
  EXPECT_NEAR(d[2].long_financing_return, long2 / 1000, 1e-15);
  const f64 long3 = 550 * (long_rate * 1.0 / 360.0), short3 = 550 * (short_rate * 1.0 / 360.0);
  const f64 nav3 = nav2 + 110 - short3 - long3; // short 550 -> 495 (+55), long 550 -> 605 (+55)
  EXPECT_NEAR(d[3].long_financing_dollars, long3, 1e-12);
  EXPECT_NEAR(d[3].borrow_dollars, short3, 1e-12);
  EXPECT_NEAR(d[3].short_dollars_by_tier[0], 550, 1e-9);
  EXPECT_NEAR(d[3].mark_pnl_dollars, 110, 1e-9);
  EXPECT_NEAR(d[3].pretrade_nav, nav3, 1e-9);
  EXPECT_NEAR(d[3].net_return, nav3 / nav2 - 1, 1e-14);
  EXPECT_NEAR(d[3].cash_ratio, (999.4 - short2 - long2 - short3 - long3) / nav3, 1e-12);
  // ACT/360 on calendar days: the weekend accrues three one-day charges per dollar.
  EXPECT_NEAR(d[2].long_financing_dollars / 500, 3 * d[3].long_financing_dollars / 550, 1e-15);
  for (usize t = 2; t < 4; ++t) {
    EXPECT_NEAR(d[t].net_return, d[t].gross_return - d[t].trade_cost_return -
                                     d[t].borrow_return - d[t].long_financing_return, 1e-15);
  }
  auto summary = st::summarize_nav(*r); ASSERT_TRUE(summary);
  EXPECT_NEAR(summary->final_nav, nav3, 1e-9);
  EXPECT_NEAR(summary->long_financing_dollars, long2 + long3, 1e-12);
  EXPECT_NEAR(summary->borrow_dollars, short2 + short3, 1e-12);
  EXPECT_NEAR(summary->short_financing_by_tier[0], short2 + short3, 1e-12);
  EXPECT_NEAR(summary->summed_long_financing_return, long2 / 1000 + long3 / nav2, 1e-15);
  EXPECT_EQ(summary->short_share_sessions, 2U);
  EXPECT_EQ(summary->short_share_mean[0], 1.0); EXPECT_EQ(summary->short_share_p95[2], 0.0);
  EXPECT_EQ(summary->missing_predictor_short_name_days, 0U);
  EXPECT_LE(r->max_cash_book_error, 1e-13);
  // The same book under flat-300-v0 pays 300 bps/365 on the short and nothing long.
  auto legacy = st::replay_nav(with_fields(p, f), config(flat(6, 300), 1000,
                                                         hold_after_deployment()));
  ASSERT_TRUE(legacy);
  EXPECT_NEAR(legacy->days[2].borrow_dollars, 500 * (300 * 1e-4 * 3.0 / 365.0), 1e-12);
  EXPECT_EQ(legacy->days[2].long_financing_dollars, 0);
}

// T10 (c): tiers from the engine's flag count (0 GC, 1 warm, 2+ special) on
// market cap = shares_out x raw_close, raw price, SI = si_shares / shares_out and
// IPO age from the first present role session (present at row 0: seasoned), all
// as of row d only; a missing or out-of-domain predictor is warm and flagged; the
// replay's decision census counts members by tier.
TEST(StrategyNavReplay, BorrowTierMappingFlagsAndMissingPredictorIsWarm) {
  Panel p(8, 14);
  constexpr usize d = 5;
  Fields f(p);
  for (usize t = 0; t < p.d; ++t) {
    f.set(p, t, 1, 5e6, 1e5);   // cap 5e8: small cap only (SI 0.02)
    p.price(t, 2, 4.99); f.set(p, t, 2, 1e9, 1e6); // low price only (cap 4.99e9)
    f.set(p, t, 3, 1e8, 1e7);   // SI exactly 0.10: high SI only
    p.price(t, 5, 4.0);         // cap 4e8 and price 4: two flags
    p.price(t, 6, 2.0); f.set(p, t, 6, 1e6, 5e5); // small, low, SI 0.5, young: four
    f.set(p, t, 8, 9.9e4, 1e3); // shares_out below 1e5: missing
    f.set(p, t, 9, 6e10, 1e6);  // shares_out above 5e10: missing
    f.set(p, t, 11, 1e7, 1e5);  // cap exactly 1e9: not small (strict)
    f.set(p, t, 12, 1e5, 1e3);  // shares_out exactly 1e5: valid; cap 1e7 small only
    f.set(p, t, 13, 5e10, 1e6); // shares_out exactly 5e10: valid, GC
  }
  p.absent(0, 4); p.absent(0, 6); // first present at row 1: 6 calendar days old at d
  f.si_shares[p.k(d, 7)] = missing; // missing at d only
  p.absent(d, 10);                  // absent at d: no raw price
  f.set(p, d - 1, 0, missing, missing); f.set(p, d + 1, 0, missing, missing); // row d only
  auto tiers = st::nav_borrow_tiers(with_fields(p, f), d);
  ASSERT_TRUE(tiers) << tiers.error().to_string();
  const std::vector<u8> tier{1, 2, 2, 2, 2, 3, 3, 2, 2, 2, 2, 1, 2, 1};
  const std::vector<u8> gap{0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0};
  EXPECT_EQ(tiers->tier, tier);
  EXPECT_EQ(tiers->missing, gap);
  // Name 4 is young at d = 5 (Jan 3 -> Jan 9) but not before its first session.
  auto early = st::nav_borrow_tiers(with_fields(p, f), 0);
  ASSERT_TRUE(early);
  EXPECT_EQ(early->tier[4], 2); EXPECT_EQ(early->missing[4], 1); // absent at row 0
  EXPECT_EQ(early->tier[0], 1);
  // The replay forms the same tiers at its decision and counts members (name 10 is
  // absent, hence not a member, at d).
  auto s = flat(0, 0); s.financing = swap_fin();
  auto r = st::replay_nav(with_fields(p, f), config(s, 1e6));
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_EQ(r->days[d].member_tiers, (std::array<usize, 3>{3, 8, 2}));
  EXPECT_EQ(r->days[d].member_missing_predictors, 3U);
  // Without fields: no tiers, and a tiered scenario is refused.
  EXPECT_FALSE(st::nav_borrow_tiers(p.nav(), d));
  const auto refused = st::replay_nav(p.nav(), config(s, 1e6));
  ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
  auto bad = s; bad.financing.flat_short_bps = 300; // a spec may not mix rules
  EXPECT_FALSE(st::replay_nav(with_fields(p, f), config(bad, 1e6)));
  bad = s; bad.financing.warm_bps = 600; // warm above special
  EXPECT_FALSE(st::replay_nav(with_fields(p, f), config(bad, 1e6)));
}

// T10 (d): the locate rule. Name 0 is special throughout: its short is never
// opened (blocked dollars counted every decision). Name 1 is a GC short that turns
// special at decision 4: it may not grow (blocked), is charged special from the
// next mark until it exits, and may be reduced to zero at decision 6. Without the
// block the same book shorts both. Zero trading cost, constant prices.
TEST(StrategyNavReplay, LocateBlockRefusesSpecialShortGrowthAllowsReduction) {
  Panel p(12, 4);
  Fields f(p);
  const f64 early[] = {0, 1, 2, 3}, grow[] = {0, -1, 2, 3}, cut[] = {0, 2, 2, 3};
  for (usize t = 0; t < p.d; ++t) {
    const f64* row = t < 4 ? early : t < 6 ? grow : cut;
    for (usize i = 0; i < p.n; ++i) p.signal[p.k(t, i)] = row[i];
    f.set(p, t, 0, 1e6, 5e5);                // cap 1e8 + SI 0.5: special
    if (t >= 4) f.set(p, t, 1, 1e6, 5e5);    // special from decision 4
  }
  st::TargetReplayConfig daily; daily.cadence = 1; daily.trade_fraction = 1;
  auto s = flat(0, 0); s.financing = swap_fin();
  auto r = st::replay_nav(with_fields(p, f), config(s, 1e6, daily));
  ASSERT_TRUE(r) << r.error().to_string();
  const auto& d = r->days;
  EXPECT_EQ(d[0].member_tiers, (std::array<usize, 3>{3, 0, 1}));
  EXPECT_EQ(d[0].blocked_short_names, 1U);
  EXPECT_EQ(d[0].blocked_short_dollars, 375000); // desired -0.375 x 1e6, never opened
  EXPECT_EQ(d[1].short_dollars, 125000);          // only the GC name 1 is short
  EXPECT_EQ(d[1].long_dollars, 500000);
  EXPECT_EQ(d[1].net_leverage, 0.375);            // the block un-neutralizes
  EXPECT_EQ(d[1].blocked_short_names, 1U);
  // Decision 4: name 1 is special and wants -0.375: held at its current short.
  EXPECT_GT(d[4].short_dollars_by_tier[0], 0);    // marked at decision 3's GC tier
  EXPECT_EQ(d[4].short_dollars_by_tier[2], 0);
  const f64 nav4 = d[4].posttrade_nav, short4 = d[4].short_dollars;
  const f64 cur4 = -short4 / nav4;
  EXPECT_EQ(d[4].blocked_short_names, 2U);
  EXPECT_NEAR(d[4].blocked_short_dollars, 0.125 * nav4 + (cur4 + 0.375) * nav4, 1e-6);
  EXPECT_EQ(bits(d[5].short_dollars), bits(short4)); // cannot grow
  EXPECT_EQ(bits(d[5].short_dollars_by_tier[2]), bits(short4)); // charged special
  EXPECT_EQ(d[5].short_dollars_by_tier[0], 0);
  EXPECT_NEAR(d[5].short_financing_by_tier[2], short4 * ((20 + 500) * 1e-4 * 1.0 / 360.0),
              1e-9);
  EXPECT_EQ(bits(d[6].short_dollars), bits(short4));
  // Decision 6: name 1 wants 0 (a reduction passes); name 0 wants -0.5 (blocked).
  EXPECT_EQ(d[6].blocked_short_names, 1U);
  EXPECT_NEAR(d[6].blocked_short_dollars, 0.5 * d[6].posttrade_nav, 1e-6);
  EXPECT_EQ(bits(d[7].short_dollars_by_tier[2]), bits(short4)); // special until it exits
  EXPECT_EQ(d[7].short_dollars, 0);                              // exited at session 7
  for (usize t = 8; t < p.d; ++t) {
    EXPECT_EQ(d[t].short_dollars_by_tier[0] + d[t].short_dollars_by_tier[1] +
              d[t].short_dollars_by_tier[2], 0) << t;
  }
  auto summary = st::summarize_nav(*r); ASSERT_TRUE(summary);
  usize names = 0; f64 dollars = 0;
  for (const auto& day : d) {
    names += day.blocked_short_names; dollars += day.blocked_short_dollars;
  }
  EXPECT_EQ(summary->blocked_short_name_decisions, names);
  EXPECT_NEAR(summary->blocked_short_dollars, dollars, 1e-6);
  EXPECT_GT(summary->mean_net_leverage, 0.3);
  // The same book without the locate rule shorts both names.
  auto unblocked = s;
  unblocked.financing.id = "swap-no-block"; unblocked.financing.block_special_shorts = false;
  auto u = st::replay_nav(with_fields(p, f), config(unblocked, 1e6, daily));
  ASSERT_TRUE(u) << u.error().to_string();
  EXPECT_EQ(u->days[1].short_dollars, 500000);
  EXPECT_GT(u->days[5].short_dollars, 3 * d[5].short_dollars);
  for (const auto& day : u->days) EXPECT_EQ(day.blocked_short_names, 0U);
}

// T10 (e): tiers are as of the decision. Rewriting every field value from row m on
// (every present name special) leaves rows < m bit-identical, and row m's MARK too
// (it charges decision m-1's tiers); the tiers at d < m are unchanged and those at m
// see the rewrite, which reaches the next mark's financing.
TEST(StrategyNavReplay, BorrowTiersAreAsOfFieldRewritesAfterDoNotChangePast) {
  Panel p(40, 8); randomize_rows(p, 7, 0); p.begin = 5;
  const auto f = random_fields(p, 5);
  constexpr usize m = 22;
  auto g = f;
  for (usize t = m; t < p.d; ++t)
    for (usize i = 0; i < p.n; ++i) g.set(p, t, i, 1e6, 9e5);
  st::TargetReplayConfig daily; daily.cadence = 1; daily.trade_fraction = .5;
  auto cfg = config(st::nav_scenario_matrix(true)[st::nav_primary_scenario_index], 1e6, daily);
  cfg.liquidity_window = 5; cfg.min_vol_pairs = 3;
  auto before = st::replay_nav(with_fields(p, f), cfg);
  auto after = st::replay_nav(with_fields(p, g), cfg);
  ASSERT_TRUE(before) << before.error().to_string();
  ASSERT_TRUE(after) << after.error().to_string();
  for (usize t = p.begin; t < m; ++t) {
    expect_same_day(before->days[t - p.begin], after->days[t - p.begin]);
    expect_same_financing(before->days[t - p.begin], after->days[t - p.begin]);
  }
  expect_same_events_before(*before, *after, p.sessions[m]);
  const auto& bm = before->days[m - p.begin]; const auto& am = after->days[m - p.begin];
  EXPECT_EQ(bits(bm.pretrade_nav), bits(am.pretrade_nav));
  EXPECT_EQ(bits(bm.borrow_dollars), bits(am.borrow_dollars));
  EXPECT_EQ(bits(bm.long_financing_dollars), bits(am.long_financing_dollars));
  for (usize k = 0; k < 3; ++k)
    EXPECT_EQ(bits(bm.short_dollars_by_tier[k]), bits(am.short_dollars_by_tier[k])) << k;
  for (usize t = 0; t < m; ++t) {
    auto x = st::nav_borrow_tiers(with_fields(p, f), t);
    auto y = st::nav_borrow_tiers(with_fields(p, g), t);
    ASSERT_TRUE(x); ASSERT_TRUE(y);
    EXPECT_EQ(x->tier, y->tier) << t; EXPECT_EQ(x->missing, y->missing) << t;
  }
  auto x = st::nav_borrow_tiers(with_fields(p, f), m);
  auto y = st::nav_borrow_tiers(with_fields(p, g), m);
  ASSERT_TRUE(x); ASSERT_TRUE(y);
  EXPECT_NE(x->tier, y->tier);
  for (usize i = 0; i < p.n; ++i)
    if (p.present[p.k(m, i)]) EXPECT_EQ(y->tier[i], 3) << i;
  EXPECT_EQ(am.member_tiers[2], am.member_tiers[0] + am.member_tiers[1] + am.member_tiers[2]);
  EXPECT_NE(bits(before->days[m + 1 - p.begin].pretrade_nav),
            bits(after->days[m + 1 - p.begin].pretrade_nav));
}

// T10 (f): the pinned CLI path with --fields. The fields manifest must pin the same
// role (manifest, sessions, ids, member SHA) and declare the used fields point in
// time, else the run is refused before any output. A pinned run publishes the five
// books (primary S2 x swap-fin-v1), financing recipe keys and CSV columns, and its
// S2 x flat-300-v0 outputs extend the legacy run's S2 outputs line for line.
TEST(StrategyNavReplay, FieldsPinnedToRolePublishFinancingMatrixElseRefused) {
  const auto p = publication_panel();
  Fields f(p);
  for (usize t = 0; t < p.d; ++t) f.set(p, t, 0, 1e6, 5e5); // the short name is special
  std::ostringstream progress;
  {
    Directory dir; auto a = write_artifact(dir.path, p);
    const auto pin = write_fields(dir.path, a, p, f, [](Json&) {});
    auto legacy_cfg = a.cfg; legacy_cfg.output_directory = (dir.path / "legacy").string();
    const auto legacy_ok = st::run_nav_replay(legacy_cfg, progress);
    ASSERT_TRUE(legacy_ok) << legacy_ok.error().to_string();
    const auto ok = st::run_nav_replay(a.cfg, st::NavTurnoverLimits{}, pin, progress);
    ASSERT_TRUE(ok) << ok.error().to_string();
    const auto out = dir.path / "out", old = dir.path / "legacy";
    const auto summary = read_json(out / "summary.json");
    const auto recipe = read_json(out / "recipe.json");
    const auto legacy_summary = read_json(old / "summary.json");
    EXPECT_EQ(summary.at("primary_scenario"), "modeled-1bn-stale5-v1+swap-fin-v1");
    EXPECT_EQ(summary.at("primary_financing_available"), true);
    EXPECT_EQ(summary.at("financing_fields").at("manifest_sha256"), pin.manifest_sha256);
    EXPECT_EQ(summary.at("financing_fields").at("role_manifest_sha256"), a.cfg.role_sha256);
    EXPECT_EQ(summary.at("financing_fields").at("fields_used").at("shares_out")
                  .at("clock"), "fixture-shares_out");
    EXPECT_EQ(recipe.at("financing_fields"), summary.at("financing_fields"));
    EXPECT_EQ(recipe.at("primary_financing_available"), true);
    EXPECT_TRUE(recipe.contains("borrow_tiers"));
    EXPECT_EQ(summary.at("recipe_sha256"), co::sha256_hex(recipe.dump()).value());
    EXPECT_EQ(legacy_summary.at("primary_financing_available"), false);
    EXPECT_TRUE(legacy_summary.at("financing_fields").is_null());
    EXPECT_EQ(legacy_summary.at("primary_scenario"), "modeled-1bn-stale5-v1");
    const auto& scenarios = summary.at("scenarios");
    const std::vector<std::string> labels{
        "linear-6bps-stale5-v1+swap-fin-v1", "modeled-1bn-stale5-v1+swap-fin-v1",
        "modeled-1bn-terminal-adverse-v1+swap-fin-v1", "modeled-1bn-stale5-v1+flat-300-v0",
        "modeled-1bn-stale5-v1+engine-tiers-v1"};
    ASSERT_EQ(scenarios.size(), labels.size());
    for (usize k = 0; k < labels.size(); ++k) {
      const auto& s = scenarios[k];
      EXPECT_EQ(s.at("scenario"), labels[k]);
      EXPECT_EQ(s.at("primary"), k == st::nav_primary_scenario_index) << k;
      EXPECT_EQ(recipe.at("scenarios")[k].at("id"), labels[k]);
      const auto daily = out / ("daily_" + labels[k] + ".csv");
      const auto events = out / ("events_" + labels[k] + ".csv");
      EXPECT_EQ(s.at("daily_csv_sha256"), co::sha256_file(daily.string()).value()) << k;
      EXPECT_EQ(s.at("events_csv_sha256"), co::sha256_file(events.string()).value()) << k;
      EXPECT_EQ(first_line(daily), std::string(t2_daily_header) +
                    ",pretrade_gross_dollars,one_way_turnover_gmv" + financing_columns) << k;
      EXPECT_EQ(s.at("financing").at("tiers_available"), true) << k;
    }
    const auto& primary = scenarios[st::nav_primary_scenario_index].at("financing");
    EXPECT_GT(primary.at("blocked_short_dollars").get<f64>(), 0);
    EXPECT_GT(primary.at("long_financing_dollars").get<f64>(), 0);
    EXPECT_EQ(primary.at("spec").at("id"), "swap-fin-v1");
    EXPECT_EQ(scenarios[3].at("financing").at("blocked_short_dollars"), 0.0);
    // S2 x flat-300-v0 == the legacy S2 run: same events bytes, every daily line the
    // legacy line plus appended financing columns, same statistics.
    const auto& legacy_s2 = legacy_summary.at("scenarios")[st::nav_primary_scenario_index];
    EXPECT_EQ(legacy_s2.at("scenario"), "modeled-1bn-stale5-v1");
    EXPECT_EQ(scenarios[3].at("events_csv_sha256"), legacy_s2.at("events_csv_sha256"));
    const auto new_lines = lines(out / ("daily_" + labels[3] + ".csv"));
    const auto old_lines = lines(old / "daily_modeled-1bn-stale5-v1.csv");
    ASSERT_EQ(new_lines.size(), old_lines.size());
    for (usize k = 1; k < old_lines.size(); ++k) {
      ASSERT_GT(new_lines[k].size(), old_lines[k].size()) << k;
      EXPECT_EQ(new_lines[k].substr(0, old_lines[k].size() + 1), old_lines[k] + ",") << k;
    }
    for (const auto* key : {"net_sharpe", "final_nav", "total_net_return", "observations"})
      EXPECT_EQ(scenarios[3].at(key), legacy_s2.at(key)) << key;
    EXPECT_EQ(scenarios[3].at("costs").at("borrow_dollars"),
              legacy_s2.at("costs").at("borrow_dollars"));
    // The CLI verb carries the same pin.
    std::ostringstream o, e;
    EXPECT_EQ(dispatch({"nav", "--combined", a.cfg.combined_path, "--combined-sha256",
                        a.cfg.combined_sha256, "--role", a.cfg.role_path, "--role-sha256",
                        a.cfg.role_sha256, "--output", (dir.path / "cli").string(), "--fields",
                        pin.manifest_path, "--fields-sha256", pin.manifest_sha256}, o, e), 0)
        << e.str();
    EXPECT_EQ(read_json(dir.path / "cli" / "summary.json").at("primary_scenario"),
              "modeled-1bn-stale5-v1+swap-fin-v1");
    EXPECT_EQ(dispatch({"nav", "--combined", a.cfg.combined_path, "--combined-sha256",
                        a.cfg.combined_sha256, "--role", a.cfg.role_path, "--role-sha256",
                        a.cfg.role_sha256, "--output", (dir.path / "never").string(),
                        "--fields", pin.manifest_path}, o, e), 2); // no --fields-sha256
    EXPECT_FALSE(std::filesystem::exists(dir.path / "never"));
  }
  // Every refusal happens before the output directory exists.
  const auto refused = [&](auto edit, auto mutate) {
    Directory dir; auto a = write_artifact(dir.path, p);
    auto pin = write_fields(dir.path, a, p, f, edit);
    mutate(dir.path, pin);
    const bool ran = static_cast<bool>(st::run_nav_replay(a.cfg, {}, pin, progress));
    return !ran && !std::filesystem::exists(dir.path / "out");
  };
  const auto none = [](const std::filesystem::path&, st::NavFieldsPin&) {};
  const std::string other(64, 'b');
  EXPECT_TRUE(refused([&](Json& m) { m["role"]["manifest_sha256"] = other; }, none));
  EXPECT_TRUE(refused([&](Json& m) { m["role"]["sessions_sha256"] = other; }, none));
  EXPECT_TRUE(refused([&](Json& m) { m["role"]["ids_sha256"] = other; }, none));
  EXPECT_TRUE(refused([&](Json& m) { m["role"]["member_sha256"] = other; }, none));
  EXPECT_TRUE(refused([](Json& m) { m["fields"][0]["point_in_time"] = false; }, none));
  EXPECT_TRUE(refused([](Json& m) { m["fields"][1].erase("point_in_time"); }, none));
  EXPECT_TRUE(refused([](Json& m) { m["schema"] = "atx.research-role-fields/v0"; }, none));
  EXPECT_TRUE(refused([](Json&) {}, [&](const std::filesystem::path&, st::NavFieldsPin& pin) {
    pin.manifest_sha256 = other; // the external pin does not match the manifest
  }));
  EXPECT_TRUE(refused([](Json&) {}, [](const std::filesystem::path& dir, st::NavFieldsPin&) {
    std::fstream payload(dir / "fields" / "shares_out.f64",
                         std::ios::binary | std::ios::in | std::ios::out);
    const char changed = 7; payload.write(&changed, 1); payload.close();
  }));
  // A control with neither edit nor mutation runs.
  EXPECT_FALSE(refused([](Json&) {}, none));
}

// ---- aim-partial-v5 (T30) ----
namespace {
st::TargetReplayConfig aim_partial_nav(f64 theta, f64 dust, f64 leverage) {
  st::TargetReplayConfig c;
  c.rule = st::TargetReplayRule::AimPartialV5; c.cadence = 1;
  c.trade_fraction = theta; c.dust_multiple = dust; c.aim_leverage = leverage;
  return c;
}
// The untiered NAV recipe.json keys as the writer produced them before T30 (d4ec515d).
constexpr std::array<const char*, 32> baseline_nav_recipe_keys{
    "accounting", "cadence", "combined_sha256", "cost_input_status",
    "daily_turnover_definition", "daily_turnover_mean_max", "daily_turnover_p95_max",
    "desired_target_postprocess", "financing", "financing_fields", "guard", "initial_nav",
    "liquidity", "liquidity_window", "max_events", "max_working_bytes", "min_vol_pairs",
    "missing", "monthly_budget", "monthly_turnover_target", "monthly_turnover_target_status",
    "primary_financing_available", "primary_scenario", "role_sha256", "rule", "scenarios",
    "schema", "sharpe_target", "target", "timing", "trade_fraction", "turnover"};
constexpr std::array<const char*, 5> v5_recipe_keys{"theta", "dust_multiple", "aim_leverage",
                                                    "rate", "aim_partial"};
std::vector<std::string> sorted_keys(const Json& object) {
  std::vector<std::string> out;
  for (const auto& item : object.items()) out.push_back(item.key());
  std::sort(out.begin(), out.end());
  return out;
}
} // namespace

// Review focus 1 at the NAV level: every lockstep book of the fixed scenarios (the
// capped S2/S3 working orders and write-offs included) under v5 theta 1, dust 0,
// aim_leverage 1 is baseline-v1 fraction 1 bit for bit, at cadence 1 and 3.
TEST(NavV5, ThetaOneMatchesBaselineBooks) {
  Panel p(40, 8); randomize_rows(p, 7, 0); p.begin = 5;
  const auto scenarios = st::fixed_nav_scenarios();
  for (const usize cadence : {usize{1}, usize{3}}) {
    st::TargetReplayConfig rule; rule.cadence = cadence; rule.trade_fraction = 1;
    auto v5 = aim_partial_nav(1, 0, 1); v5.cadence = cadence;
    auto base = config(scenarios[0], 1e6, rule);
    base.liquidity_window = 5; base.min_vol_pairs = 3;
    auto aim = base; aim.target = v5;
    const auto a = st::replay_nav_scenarios(p.nav(), base, scenarios);
    const auto b = st::replay_nav_scenarios(p.nav(), aim, scenarios);
    ASSERT_TRUE(a) << a.error().to_string();
    ASSERT_TRUE(b) << b.error().to_string();
    ASSERT_EQ(a->size(), b->size());
    for (usize k = 0; k < a->size(); ++k) {
      const auto& x = (*a)[k]; const auto& y = (*b)[k];
      expect_same_result(x, y);
      EXPECT_EQ(bits(x.max_return_identity_error), bits(y.max_return_identity_error)) << k;
      for (usize t = 0; t < x.days.size(); ++t) {
        EXPECT_EQ(bits(x.days[t].planned_net), bits(y.days[t].planned_net)) << t;
        EXPECT_EQ(bits(x.days[t].planned_forced), bits(y.days[t].planned_forced)) << t;
        EXPECT_EQ(bits(x.days[t].net_return), bits(y.days[t].net_return)) << t;
        EXPECT_EQ(x.days[t].planned_held_names, y.days[t].planned_held_names) << t;
        EXPECT_EQ(x.days[t].decision_members, y.days[t].decision_members) << t;
      }
    }
  }
}

// The book deploys: on a constant aim with no costs and constant prices the planned
// gross at the j-th decision is L (1 - (1 - theta)^(j+1)) and the held book reaches
// aim_leverage x gross 1, where baseline-v1 at the same fraction stops at gross 1.
TEST(NavV5, DeploysTowardAimLeverage) {
  Panel p(30, 6); p.by_name({1, 2, 3, 4, 5, 6});
  const auto r = st::replay_nav(p.nav(), config(flat(0, 0), 1e6, aim_partial_nav(0.5, 0, 1.5)));
  ASSERT_TRUE(r) << r.error().to_string();
  usize decisions = 0;
  for (const auto& day : r->days) {
    if (!day.decision) continue;
    const f64 expected = 1.5 * (1 - std::pow(0.5, static_cast<f64>(decisions + 1)));
    EXPECT_NEAR(day.planned_gross, expected, 1e-9) << decisions;
    EXPECT_NEAR(day.planned_net, 0, 1e-9) << decisions;
    EXPECT_EQ(day.planned_held_names, 6U) << decisions;
    EXPECT_EQ(day.decision_members, 6U) << decisions;
    EXPECT_EQ(day.applied_fraction, 0.5) << decisions;
    ++decisions;
  }
  EXPECT_EQ(decisions, p.d - 2);
  EXPECT_NEAR(r->days.back().gross_leverage, 1.5, 1e-6);
  st::TargetReplayConfig partial; partial.cadence = 1; partial.trade_fraction = 0.5;
  const auto b = st::replay_nav(p.nav(), config(flat(0, 0), 1e6, partial));
  ASSERT_TRUE(b) << b.error().to_string();
  EXPECT_NEAR(b->days.back().gross_leverage, 1.0, 1e-6);
}

// Recipe/summary keys: v5 adds theta, dust_multiple, aim_leverage, rate (fixed) and
// aim_partial to the recipe, the construction CSV columns (banded_names = the dust
// count) and construction.v5 to every scenario summary; baseline carries none of
// them. The baseline recipe.json bytes (NAV and target replay) and the fixture pins
// are the pre-change bytes: SHA-256s hand-derived from the d4ec515d writers (an
// nlohmann dump(2) emulation byte-exact on 25 committed replay outputs; T30 report).
TEST(NavV5, RecipeAndSummaryKeys) {
  const auto p = publication_panel();
  std::ostringstream progress;
  Directory dir; const auto a = write_artifact(dir.path, p);
  EXPECT_EQ(a.cfg.role_sha256, "17349e657c9ee076c252f46a0688463b416bf55bf2792ae4d2a00f4608e2acdc");
  EXPECT_EQ(a.cfg.combined_sha256,
            "295599523de7b52f5caf46764bb390b53863efe49a72126a52a1f53180b91d91");
  auto baseline = a.cfg; baseline.output_directory = (dir.path / "baseline").string();
  const auto ok = st::run_nav_replay(baseline, progress);
  ASSERT_TRUE(ok) << ok.error().to_string();
  const auto base_out = dir.path / "baseline";
  EXPECT_EQ(co::sha256_file((base_out / "recipe.json").string()).value(),
            "73cb45f1182f659b1b66bf5adc17bc0539a95c987d191c10c00f846c6c8017c0");
  const auto base_recipe = read_json(base_out / "recipe.json");
  std::vector<std::string> expected(baseline_nav_recipe_keys.begin(),
                                    baseline_nav_recipe_keys.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(sorted_keys(base_recipe), expected);
  for (const auto* key : v5_recipe_keys) EXPECT_FALSE(base_recipe.contains(key)) << key;
  EXPECT_EQ(base_recipe.at("rule"), "baseline-target-v1");
  const auto base_summary = read_json(base_out / "summary.json");
  EXPECT_EQ(base_summary.at("rule"), "baseline-target-v1");
  for (const auto& s : base_summary.at("scenarios")) {
    const auto id = s.at("scenario").get<std::string>();
    EXPECT_FALSE(s.contains("construction")) << id;
    EXPECT_EQ(first_line(base_out / ("daily_" + id + ".csv")),
              std::string(t2_daily_header) + ",pretrade_gross_dollars,one_way_turnover_gmv")
        << id;
  }
  auto targets = a.cfg; targets.output_directory = (dir.path / "targets").string();
  ASSERT_TRUE(st::run_target_replay(targets, progress));
  EXPECT_EQ(co::sha256_file((dir.path / "targets" / "recipe.json").string()).value(),
            "ef16be1716d3d1fed90ad8af9aca3073b1c425e2fb163078ce0d13671f584bdf");

  auto v5 = a.cfg; v5.output_directory = (dir.path / "v5").string();
  v5.target = aim_partial_nav(0.5, 0.1, 1.5);
  const auto ran = st::run_nav_replay(v5, progress);
  ASSERT_TRUE(ran) << ran.error().to_string();
  const auto out = dir.path / "v5";
  const auto recipe = read_json(out / "recipe.json");
  auto with_v5 = expected;
  for (const auto* key : v5_recipe_keys) with_v5.emplace_back(key);
  std::sort(with_v5.begin(), with_v5.end());
  EXPECT_EQ(sorted_keys(recipe), with_v5);
  EXPECT_EQ(recipe.at("rule"), "aim-partial-v5");
  EXPECT_EQ(recipe.at("theta"), 0.5);
  EXPECT_EQ(recipe.at("trade_fraction"), 0.5);
  EXPECT_EQ(recipe.at("dust_multiple"), 0.1);
  EXPECT_EQ(recipe.at("aim_leverage"), 1.5);
  EXPECT_EQ(recipe.at("rate"), "fixed");
  EXPECT_EQ(recipe.at("cadence"), 1);
  const auto summary = read_json(out / "summary.json");
  EXPECT_EQ(summary.at("rule"), "aim-partial-v5");
  EXPECT_EQ(summary.at("recipe_sha256"), co::sha256_hex(recipe.dump()).value());
  const auto& scenarios = summary.at("scenarios");
  ASSERT_EQ(scenarios.size(), 3U);
  for (const auto& s : scenarios) {
    const auto id = s.at("scenario").get<std::string>();
    EXPECT_EQ(first_line(out / ("daily_" + id + ".csv")),
              std::string(t2_daily_header) + ",pretrade_gross_dollars,one_way_turnover_gmv" +
                  construction_columns) << id;
    const auto& construction = s.at("construction");
    EXPECT_EQ(construction.at("rule_id"), "aim-partial-v5") << id;
    EXPECT_GT(construction.at("banded_names_total").get<usize>(), 0U) << id; // dusted
    const auto& block = construction.at("v5");
    for (const auto* key : {"theta", "dust_multiple", "aim_leverage", "rate", "mean_gross",
                            "mean_net", "mean_held_share"})
      EXPECT_TRUE(block.contains(key)) << id << ' ' << key;
    EXPECT_EQ(block.at("theta"), 0.5) << id;
    EXPECT_EQ(block.at("dust_multiple"), 0.1) << id;
    EXPECT_EQ(block.at("aim_leverage"), 1.5) << id;
    EXPECT_EQ(block.at("rate"), "fixed") << id;
    EXPECT_EQ(block.at("decisions"), p.d - 2) << id;
    const f64 gross = block.at("mean_gross").get<f64>();
    const f64 held = block.at("mean_held_share").get<f64>();
    EXPECT_GT(gross, 0) << id;
    EXPECT_LE(gross, 1.5) << id;
    EXPECT_GT(held, 0.5) << id;
    EXPECT_LE(held, 1.0) << id;
    EXPECT_TRUE(block.at("mean_net").is_number()) << id;
  }
}

// CLI: --rule aim-partial-v5 --trade-fraction (theta) --dust-multiple --aim-leverage;
// a band under v5, v5 parameters under another rule or out of range are refused
// before any output; an unknown rule spelling is a usage error.
TEST(NavV5, CliFlagsAndRefusals) {
  const auto p = publication_panel();
  Directory dir; const auto a = write_artifact(dir.path, p);
  const auto args = [&](const std::string& output, std::vector<std::string> extra) {
    std::vector<std::string> all{"nav", "--combined", a.cfg.combined_path, "--combined-sha256",
                                 a.cfg.combined_sha256, "--role", a.cfg.role_path,
                                 "--role-sha256", a.cfg.role_sha256, "--output",
                                 (dir.path / output).string()};
    for (auto& e : extra) all.push_back(std::move(e));
    return all;
  };
  std::ostringstream out, err;
  EXPECT_EQ(dispatch(args("v5", {"--rule", "aim-partial-v5", "--cadence", "1",
                                 "--trade-fraction", ".25", "--dust-multiple", ".05",
                                 "--aim-leverage", "1.2"}), out, err), 0) << err.str();
  const auto recipe = read_json(dir.path / "v5" / "recipe.json");
  EXPECT_EQ(recipe.at("rule"), "aim-partial-v5");
  EXPECT_EQ(recipe.at("theta"), 0.25);
  EXPECT_EQ(recipe.at("dust_multiple"), 0.05);
  EXPECT_EQ(recipe.at("aim_leverage"), 1.2);
  EXPECT_EQ(read_json(dir.path / "v5" / "summary.json").at("rule"), "aim-partial-v5");
  EXPECT_EQ(dispatch(args("band", {"--rule", "aim-partial-v5", "--band-multiple", "1"}), out,
                     err), 1);
  EXPECT_EQ(dispatch(args("foreign", {"--aim-leverage", "1.5"}), out, err), 1);
  EXPECT_EQ(dispatch(args("dust", {"--rule", "aim-partial-v5", "--dust-multiple", ".6"}), out,
                     err), 1);
  EXPECT_EQ(dispatch(args("unknown", {"--rule", "aim-partial"}), out, err), 2);
  for (const auto* name : {"band", "foreign", "dust", "unknown"})
    EXPECT_FALSE(std::filesystem::exists(dir.path / name)) << name;
}

// ---- rate per-name-v1 (T36) ----
namespace {
// aim-partial-v5 at theta and dust (aim leverage 1) with a zero-cost flat scenario, the
// given initial NAV and the replay's default liquidity window (63 sessions, 20 pairs).
st::NavReplayConfig v5_config(f64 theta, f64 dust, f64 nav = 1e9) {
  auto c = config(flat(0, 0), nav, aim_partial_nav(theta, dust, 1));
  c.liquidity_window = 63; c.min_vol_pairs = 20;
  return c;
}
// Synthetic roles of the per-name rate fixtures on Panel (the NAV fixtures' date-major
// role builder); view() borrows the panel.
struct SyntheticRole {
  Panel p;
  [[nodiscard]] st::NavReplayInput view() const { return p.nav(); }
  // 66 sessions and one decision, d0 = 63, whose liquidity window is [0, 63). A (name
  // 0) alternates 100/102 on zero volume for the whole window (sigma ~2%, ADV 0); B
  // (name 1) is absent for the 63 sessions before d0 (no pairs: fallback, ADV 0); C
  // (name 2) alternates 100/102 on 1.25e6 shares (sigma ~2%, ADV ~$126m): theta_C
  // ~0.050 at NAV 1e9, inside [0.01, 0.15]. All three are members at d0.
  static SyntheticRole three_names_one_zero_volume_one_absent_window() {
    SyntheticRole r{Panel(66, 3)};
    auto& panel = r.p;
    for (usize t = 0; t < panel.d; ++t) {
      const f64 level = t % 2 ? 102.0 : 100.0;
      panel.price(t, 0, level); panel.price(t, 2, level);
      panel.volume[panel.k(t, 0)] = 0; panel.volume[panel.k(t, 2)] = 1.25e6;
    }
    for (usize t = 0; t < 63; ++t) panel.absent(t, 1);
    panel.by_name({1, 2, 3});
    panel.begin = 63;
    return r;
  }
  // The T30 lockstep role: random walks with absent and present-nonmember cells.
  static SyntheticRole default_role() {
    SyntheticRole r{Panel(40, 8)};
    randomize_rows(r.p, 7, 0); r.p.begin = 5;
    return r;
  }
};
// C's window statistics computed directly (two-pass sample SD), as the rate reads them.
f64 expected_rate_c(const Panel& p, f64 nav) {
  f64 dollars = 0, sum = 0;
  std::vector<f64> returns;
  for (usize k = 0; k < 63; ++k) {
    dollars += p.raw[p.k(k, 2)] * p.volume[p.k(k, 2)];
    if (k == 0) continue;
    returns.push_back(p.close[p.k(k, 2)] / p.close[p.k(k - 1, 2)] - 1);
    sum += returns.back();
  }
  const f64 mean = sum / static_cast<f64>(returns.size());
  f64 squares = 0;
  for (const f64 r : returns) squares += (r - mean) * (r - mean);
  const f64 sigma = std::sqrt(squares / static_cast<f64>(returns.size() - 1));
  return st::per_name_rate_v1(st::nav_rate_rra, st::nav_rate_lambda, nav, sigma, dollars / 63.0,
                              st::nav_rate_min, st::nav_rate_max);
}
} // namespace

// Research brief 4.B check values, the clip bounds, and never NaN: every unusable input
// (nonpositive, NaN or infinite liquidity, NAV, RRA or lambda) and an inf/inf quotient
// take rate_min; the rate falls as 1/sqrt(NAV).
TEST(NavV5, PerNameRate_Formula) {
  EXPECT_NEAR(st::per_name_rate_v1(10, 0.2, 1e9, 0.015, 5e7, 0.01, 0.15), 0.0237, 2e-4);
  EXPECT_NEAR(st::per_name_rate_v1(10, 0.2, 1e9, 0.015, 2e8, 0.01, 0.15), 0.0474, 2e-4);
  EXPECT_NEAR(st::per_name_rate_v1(10, 0.2, 1e9, 0.015, 1e9, 0.01, 0.15), 0.106, 1e-3);
  EXPECT_DOUBLE_EQ(st::per_name_rate_v1(10, 0.2, 1e9, 0.015, 1e6, 0.01, 0.15), 0.01);
  EXPECT_DOUBLE_EQ(st::per_name_rate_v1(10, 0.2, 1e9, 0.05, 5e10, 0.01, 0.15), 0.15);
  EXPECT_EQ(bits(st::per_name_rate_v1(10, 0.2, 1e9, 0.015, 5e7, 0.01, 0.15)),
            bits(std::sqrt(10 * 0.015 * 0.015 * 5e7 / (0.2 * 1e9))));
  EXPECT_NEAR(st::per_name_rate_v1(10, 0.2, 4e9, 0.015, 2e8, 0.01, 0.15),
              0.5 * st::per_name_rate_v1(10, 0.2, 1e9, 0.015, 2e8, 0.01, 0.15), 1e-15);
  EXPECT_EQ(st::per_name_rate_v1(10, 0.2, 1e9, 0.015, 5e7, 0.05, 0.05), 0.05);
  const f64 inf = std::numeric_limits<f64>::infinity();
  for (const f64 bad : {0.0, -1.0, missing, inf}) {
    EXPECT_EQ(st::per_name_rate_v1(10, 0.2, 1e9, bad, 5e7, 0.01, 0.15), 0.01) << bad;
    EXPECT_EQ(st::per_name_rate_v1(10, 0.2, 1e9, 0.015, bad, 0.01, 0.15), 0.01) << bad;
    EXPECT_EQ(st::per_name_rate_v1(10, 0.2, bad, 0.015, 5e7, 0.01, 0.15), 0.01) << bad;
    EXPECT_EQ(st::per_name_rate_v1(bad, 0.2, 1e9, 0.015, 5e7, 0.01, 0.15), 0.01) << bad;
    EXPECT_EQ(st::per_name_rate_v1(10, bad, 1e9, 0.015, 5e7, 0.01, 0.15), 0.01) << bad;
  }
  EXPECT_EQ(st::per_name_rate_v1(1e300, 1e300, 1e300, 1e300, 1e300, 0.01, 0.15), 0.01); // inf/inf
  EXPECT_EQ(st::per_name_rate_v1(1e300, 1, 1, 1e300, 1, 0.01, 0.15), 0.15);            // inf clips
}

// Review focus 3: names without liquidity (zero volume over the whole window; absent
// for the whole window) trade at rate_min, count in at_min_count and never produce NaN;
// the liquid name trades at its formula rate at the pre-trade NAV of the decision.
TEST(NavV5, PerNameRate_NoLiquidity_UsesMin) {
  const auto in = SyntheticRole::three_names_one_zero_volume_one_absent_window();
  st::NavReplayConfig cfg = v5_config(/*theta*/ 0.05, /*dust*/ 0.1);
  cfg.rate = st::NavRateRule::PerNameV1;
  const auto r = st::replay_nav(in.view(), cfg);
  ASSERT_TRUE(r) << r.error().to_string();
  const auto& stats = r->construction.rate_stats;
  const f64 theta_c = expected_rate_c(in.p, 1e9);
  ASSERT_GT(theta_c, cfg.rate_min);
  ASSERT_LT(theta_c, cfg.rate_max);
  EXPECT_EQ(stats.n, 3U); // one decision x three members
  EXPECT_DOUBLE_EQ(stats.min, cfg.rate_min);
  EXPECT_EQ(stats.at_min_count, 2U); // A and B
  EXPECT_EQ(stats.at_max_count, 0U);
  EXPECT_DOUBLE_EQ(stats.share_at_min, 2.0 / 3.0);
  EXPECT_EQ(stats.share_at_max, 0.0);
  EXPECT_NEAR(stats.max, theta_c, 1e-12);
  EXPECT_NEAR(stats.mean, (2 * cfg.rate_min + theta_c) / 3, 1e-12);
  EXPECT_EQ(stats.p05, cfg.rate_min);
  EXPECT_EQ(stats.p50, cfg.rate_min);
  EXPECT_NEAR(stats.p95, stats.max, (cfg.rate_max - cfg.rate_min) / 4096);
  EXPECT_LE(stats.p95, stats.max);
  usize decisions = 0;
  for (const auto& day : r->days) {
    EXPECT_TRUE(std::isfinite(day.gross_leverage)) << day.session_index;
    EXPECT_TRUE(std::isfinite(day.applied_fraction)) << day.session_index;
    if (!day.decision) continue;
    ++decisions;
    // From flat: A (desired -1/2) moves at rate_min, B (desired 0) is dusted, C (+1/2)
    // at theta_C; the reported fraction is the members' mean rate.
    EXPECT_NEAR(day.planned_gross, 0.5 * cfg.rate_min + 0.5 * theta_c, 1e-12);
    EXPECT_EQ(day.construction.banded_names, 1U);
    EXPECT_DOUBLE_EQ(day.applied_fraction, stats.mean);
  }
  EXPECT_EQ(decisions, 1U);
  // NAV_d scales the liquid name's rate by 1/sqrt(NAV); the others stay at rate_min.
  auto larger = cfg; larger.initial_nav = 4e9;
  const auto big = st::replay_nav(in.view(), larger);
  ASSERT_TRUE(big) << big.error().to_string();
  EXPECT_NEAR(big->construction.rate_stats.max, expected_rate_c(in.p, 4e9), 1e-12);
  EXPECT_NEAR(big->construction.rate_stats.max, 0.5 * theta_c, 1e-12);
  EXPECT_EQ(big->construction.rate_stats.at_min_count, 2U);
}

// Per-name rates clipped to one constant equal the fixed theta: every lockstep book of
// the fixed scenarios (capped S2/S3 working orders, write-offs, forced exits) is the
// fixed-rate book bit for bit (net return per day, and every compared field but
// applied_fraction, which is then the members' mean rate). This also pins the shared
// liquidity cache that per-name-v1 executes from to the per-book window arithmetic.
TEST(NavV5, PerNameRate_FixedEqualsTradeFraction) {
  const auto in = SyntheticRole::default_role();
  const auto scenarios = st::fixed_nav_scenarios();
  st::NavReplayConfig a = v5_config(0.05, 0.1, 1e7); a.rate = st::NavRateRule::Fixed;
  st::NavReplayConfig b = a; b.rate = st::NavRateRule::PerNameV1;
  b.rate_min = b.rate_max = 0.05; // per-name clipped to a constant == fixed
  const auto ra = st::replay_nav_scenarios(in.view(), a, scenarios);
  const auto rb = st::replay_nav_scenarios(in.view(), b, scenarios);
  ASSERT_TRUE(ra) << ra.error().to_string();
  ASSERT_TRUE(rb) << rb.error().to_string();
  ASSERT_EQ(ra->size(), rb->size());
  for (usize k = 0; k < ra->size(); ++k) {
    const auto& x = (*ra)[k];
    auto y = (*rb)[k];
    ASSERT_EQ(x.days.size(), y.days.size());
    usize samples = 0, capped = 0;
    for (usize d = 0; d < x.days.size(); ++d) {
      EXPECT_DOUBLE_EQ(x.days[d].net_return, y.days[d].net_return) << k << ' ' << d;
      EXPECT_NEAR(y.days[d].applied_fraction, x.days[d].applied_fraction, 1e-15) << d;
      y.days[d].applied_fraction = x.days[d].applied_fraction;
      if (y.days[d].decision) samples += y.days[d].decision_members;
      capped += y.days[d].capped_fills;
    }
    expect_same_result(x, y);
    if (k == st::nav_primary_scenario_index) EXPECT_GT(capped, 0U); // S2 caps exercised
    EXPECT_EQ(x.construction.rate_stats.n, 0U); // fixed: no rate statistics
    const auto& s = y.construction.rate_stats;
    EXPECT_GT(samples, 0U);
    EXPECT_EQ(s.n, samples) << k; // one sample per member per decision
    EXPECT_EQ(s.at_min_count, samples) << k;
    EXPECT_EQ(s.at_max_count, samples) << k;
    EXPECT_EQ(s.min, 0.05); EXPECT_EQ(s.max, 0.05);
    EXPECT_EQ(s.p05, 0.05); EXPECT_EQ(s.p50, 0.05); EXPECT_EQ(s.p95, 0.05);
    EXPECT_NEAR(s.mean, 0.05, 1e-15);
    EXPECT_EQ(s.share_at_min, 1.0); EXPECT_EQ(s.share_at_max, 1.0);
  }
}

// The rate's configuration contract: per-name-v1 only under aim-partial-v5, rra and
// lambda in (0, 1e6], 0 < rate_min <= rate_max <= 1 (NaN refused), and a fixed rate
// carries no rate parameters.
TEST(NavV5, PerNameRate_ConfigRefusals) {
  const auto in = SyntheticRole::default_role();
  const auto refused = [&](const st::NavReplayConfig& c) {
    const auto r = st::replay_nav(in.view(), c);
    return !r && r.error().code() == co::ErrorCode::InvalidArgument;
  };
  auto good = v5_config(0.05, 0.1, 1e7); good.rate = st::NavRateRule::PerNameV1;
  const auto ran = st::replay_nav(in.view(), good);
  ASSERT_TRUE(ran) << ran.error().to_string();
  auto edge = good; edge.rate_min = edge.rate_max = 1;
  EXPECT_TRUE(st::replay_nav(in.view(), edge));
  auto baseline = good; baseline.target = st::TargetReplayConfig{}; // baseline-v1
  EXPECT_TRUE(refused(baseline));
  const std::array<void (*)(st::NavReplayConfig&), 9> broken{
      [](st::NavReplayConfig& c) { c.rate_min = 0; },
      [](st::NavReplayConfig& c) { c.rate_min = 0.2; }, // above rate_max 0.15
      [](st::NavReplayConfig& c) { c.rate_max = 1.5; },
      [](st::NavReplayConfig& c) { c.rate_max = missing; },
      [](st::NavReplayConfig& c) { c.rate_rra = 0; },
      [](st::NavReplayConfig& c) { c.rate_rra = missing; },
      [](st::NavReplayConfig& c) { c.rate_lambda = -0.2; },
      [](st::NavReplayConfig& c) { c.rate_lambda = 2e6; },
      [](st::NavReplayConfig& c) { c.rate = static_cast<st::NavRateRule>(7); }};
  for (usize k = 0; k < broken.size(); ++k) {
    auto c = good; broken[k](c);
    EXPECT_TRUE(refused(c)) << k;
  }
  auto fixed = v5_config(0.05, 0.1, 1e7);
  EXPECT_TRUE(st::replay_nav(in.view(), fixed)); // control: fixed at the defaults
  fixed.rate_rra = 5;
  EXPECT_TRUE(refused(fixed)); // a fixed rate takes no rate parameters
  fixed = v5_config(0.05, 0.1, 1e7); fixed.rate_max = 0.2;
  EXPECT_TRUE(refused(fixed));
}

// Recipe, summary and CLI of the rate (rulings R-c, R-d). A fixed-rate v5 run publishes
// the T30 bytes: its recipe.json SHA-256 is the one hand-derived from the T30 writers
// (98277c45) by t36-sha/v5_fixed_recipe_sha.py over the validated T30 emulation, and
// its recipe and summary bytes are the same through the four-argument overload,
// NavRateOptions{} and the CLI's --rate fixed. per-name-v1 adds rate_rra, rate_lambda,
// rate_min and rate_max (rate "per-name-v1", aim_partial its own text) and
// construction.v5.rate_stats; the CLI refuses the rate outside aim-partial-v5 and its
// parameters outside per-name-v1 (usage, 2), and invalid values (1), before any output.
TEST(NavV5, PerNameRate_RecipeSummaryAndCli) {
  const auto p = publication_panel();
  std::ostringstream progress, out, err;
  Directory dir; const auto a = write_artifact(dir.path, p);
  const auto bytes = [&](const std::string& run, const char* file) {
    std::ifstream in(dir.path / run / file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  };
  const auto args = [&](const std::string& output, std::vector<std::string> extra) {
    std::vector<std::string> all{"nav", "--combined", a.cfg.combined_path, "--combined-sha256",
                                 a.cfg.combined_sha256, "--role", a.cfg.role_path,
                                 "--role-sha256", a.cfg.role_sha256, "--output",
                                 (dir.path / output).string()};
    for (auto& e : extra) all.push_back(std::move(e));
    return all;
  };
  const std::vector<std::string> v5_flags{"--rule", "aim-partial-v5", "--cadence", "1",
                                          "--trade-fraction", ".5", "--dust-multiple", ".1",
                                          "--aim-leverage", "1.5"};
  const auto with = [&](std::vector<std::string> extra) {
    auto all = v5_flags;
    for (auto& e : extra) all.push_back(std::move(e));
    return all;
  };
  auto fixed = a.cfg; fixed.target = aim_partial_nav(0.5, 0.1, 1.5);
  fixed.output_directory = (dir.path / "fixed4").string();
  const auto ran = st::run_nav_replay(fixed, progress);
  ASSERT_TRUE(ran) << ran.error().to_string();
  EXPECT_EQ(co::sha256_file((dir.path / "fixed4" / "recipe.json").string()).value(),
            "d53f0c09018244862e8974579e8a87184310778786b08545041c605f725fc71a");
  fixed.output_directory = (dir.path / "fixed5").string();
  ASSERT_TRUE(st::run_nav_replay(fixed, st::NavTurnoverLimits{}, st::NavFieldsPin{},
                                 st::NavRateOptions{}, progress));
  ASSERT_EQ(dispatch(args("fixedcli", with({"--rate", "fixed"})), out, err), 0) << err.str();
  for (const auto* file : {"recipe.json", "summary.json"}) {
    EXPECT_EQ(bytes("fixed5", file), bytes("fixed4", file)) << file;
    EXPECT_EQ(bytes("fixedcli", file), bytes("fixed4", file)) << file;
  }
  const auto fixed_recipe = read_json(dir.path / "fixed4" / "recipe.json");
  EXPECT_EQ(fixed_recipe.at("rate"), "fixed");
  const auto fixed_summary = read_json(dir.path / "fixed4" / "summary.json");
  for (const auto& s : fixed_summary.at("scenarios")) {
    const auto& block = s.at("construction").at("v5");
    EXPECT_EQ(block.at("rate"), "fixed");
    EXPECT_FALSE(block.contains("rate_stats"));
  }

  st::NavRateOptions rate; rate.rate = st::NavRateRule::PerNameV1;
  auto per_name = fixed; per_name.output_directory = (dir.path / "pername").string();
  const auto per = st::run_nav_replay(per_name, st::NavTurnoverLimits{}, st::NavFieldsPin{},
                                      rate, progress);
  ASSERT_TRUE(per) << per.error().to_string();
  const auto recipe = read_json(dir.path / "pername" / "recipe.json");
  auto keys = sorted_keys(fixed_recipe);
  for (const auto* key : {"rate_rra", "rate_lambda", "rate_min", "rate_max"})
    keys.emplace_back(key);
  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(sorted_keys(recipe), keys);
  EXPECT_EQ(recipe.at("rule"), "aim-partial-v5");
  EXPECT_EQ(recipe.at("rate"), "per-name-v1");
  EXPECT_EQ(recipe.at("rate_rra"), 10.0);
  EXPECT_EQ(recipe.at("rate_lambda"), 0.2);
  EXPECT_EQ(recipe.at("rate_min"), 0.01);
  EXPECT_EQ(recipe.at("rate_max"), 0.15);
  EXPECT_NE(recipe.at("aim_partial"), fixed_recipe.at("aim_partial"));
  for (const auto& item : fixed_recipe.items())
    if (item.key() != "rate" && item.key() != "aim_partial")
      EXPECT_EQ(recipe.at(item.key()), item.value()) << item.key();
  const auto summary = read_json(dir.path / "pername" / "summary.json");
  EXPECT_EQ(summary.at("recipe_sha256"), co::sha256_hex(recipe.dump()).value());
  // 7 decisions x 3 members, name 0 absent at session 4. Nine sessions never reach 20
  // return pairs: every sample is the fallback's rate_min.
  const usize samples = 3 * (p.d - 2) - 1;
  const std::vector<std::string> stat_keys{"at_max_count", "at_min_count", "max", "mean", "min",
                                           "n", "p05", "p50", "p95", "share_at_max",
                                           "share_at_min"};
  for (const auto& s : summary.at("scenarios")) {
    const auto id = s.at("scenario").get<std::string>();
    EXPECT_EQ(first_line(dir.path / "pername" / ("daily_" + id + ".csv")),
              first_line(dir.path / "fixed4" / ("daily_" + id + ".csv"))) << id;
    const auto& block = s.at("construction").at("v5");
    EXPECT_EQ(block.at("rate"), "per-name-v1") << id;
    const auto& stats = block.at("rate_stats");
    EXPECT_EQ(sorted_keys(stats), stat_keys) << id;
    EXPECT_EQ(stats.at("n"), samples) << id;
    EXPECT_EQ(stats.at("at_min_count"), samples) << id;
    EXPECT_EQ(stats.at("at_max_count"), 0) << id;
    for (const auto* key : {"min", "max", "p05", "p50", "p95"})
      EXPECT_EQ(stats.at(key), 0.01) << id << ' ' << key;
    EXPECT_NEAR(stats.at("mean").get<f64>(), 0.01, 1e-15) << id;
    EXPECT_EQ(stats.at("share_at_min"), 1.0) << id;
    EXPECT_EQ(stats.at("share_at_max"), 0.0) << id;
  }
  ASSERT_EQ(dispatch(args("pernamecli", with({"--rate", "per-name-v1", "--rate-rra", "10",
                                              "--rate-min", ".01", "--rate-max", ".15"})),
                     out, err), 0) << err.str();
  for (const auto* file : {"recipe.json", "summary.json"})
    EXPECT_EQ(bytes("pernamecli", file), bytes("pername", file)) << file;

  EXPECT_EQ(dispatch(args("baseline_rate", {"--rate", "per-name-v1"}), out, err), 2);
  EXPECT_EQ(dispatch(args("baseline_fixed", {"--rate", "fixed"}), out, err), 2);
  EXPECT_EQ(dispatch(args("no_rate", with({"--rate-rra", "5"})), out, err), 2);
  EXPECT_EQ(dispatch(args("fixed_param", with({"--rate", "fixed", "--rate-min", ".02"})), out,
                     err), 2);
  EXPECT_EQ(dispatch(args("spelling", with({"--rate", "per-name"})), out, err), 2);
  EXPECT_EQ(dispatch(args("inverted", with({"--rate", "per-name-v1", "--rate-min", ".2",
                                            "--rate-max", ".1"})), out, err), 1);
  EXPECT_EQ(dispatch(args("zero_rra", with({"--rate", "per-name-v1", "--rate-rra", "0"})), out,
                     err), 1);
  for (const auto* name : {"baseline_rate", "baseline_fixed", "no_rate", "fixed_param",
                           "spelling", "inverted", "zero_rra"})
    EXPECT_FALSE(std::filesystem::exists(dir.path / name)) << name;
}

// ---- v6 prereg C1-C3 and review F8: order basis, exit rate, locate-in-aim, cache ----
namespace {
st::TargetReplayConfig v6_aim(f64 theta, f64 dust, f64 exit_rate = 1) {
  auto c = aim_partial_nav(theta, dust, 1);
  c.exit_rate = exit_rate;
  return c;
}
std::string file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::vector<std::string> file_names(const std::filesystem::path& dir) {
  std::vector<std::string> out;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
    out.push_back(entry.path().filename().string());
  std::sort(out.begin(), out.end());
  return out;
}
// Two published run directories hold the same files, byte for byte.
void expect_same_files(const std::filesystem::path& a, const std::filesystem::path& b) {
  const auto names = file_names(a);
  ASSERT_FALSE(names.empty()) << a;
  ASSERT_EQ(names, file_names(b)) << a << " vs " << b;
  for (const auto& name : names)
    EXPECT_TRUE(file_bytes(a / name) == file_bytes(b / name)) << name;
}
std::vector<std::string> nav_args(const Artifact& a, const std::filesystem::path& output,
                                  const std::vector<std::string>& extra) {
  std::vector<std::string> all{"nav", "--combined", a.cfg.combined_path, "--combined-sha256",
                               a.cfg.combined_sha256, "--role", a.cfg.role_path,
                               "--role-sha256", a.cfg.role_sha256, "--output", output.string()};
  all.insert(all.end(), extra.begin(), extra.end());
  return all;
}
std::vector<std::string> joined(std::vector<std::string> a, const std::vector<std::string>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
// The v5 reference CLI flags of the T36 fixed-rate pin (d53f0c09).
const std::vector<std::string> v5_flags{"--rule", "aim-partial-v5", "--cadence", "1",
                                        "--trade-fraction", ".5", "--dust-multiple", ".1",
                                        "--aim-leverage", "1.5"};
// The recipe's aim_partial text contains `text`.
bool aim_text_has(const Json& recipe, const char* text) {
  return recipe.at("aim_partial").get<std::string>().find(text) != std::string::npos;
}
} // namespace

// (a) The v6 defaults are the v5 path: passing --order-basis target --exit-rate 1
// publishes every file byte for byte as the same run without them, under baseline-v1
// and under the v5 flags, and those recipes are the pre-change bytes NavV5 pins
// (baseline 73cb45f1, v5 fixed d53f0c09); NavExecutionOptions{} through the six-argument
// overload is the plain run byte for byte; no v6 key appears.
TEST(NavV6, OrderBasisTargetAndExitRateOneAreBitIdentical) {
  const auto p = publication_panel();
  Directory dir; const auto a = write_artifact(dir.path, p);
  std::ostringstream out, err, progress;
  const std::vector<std::string> defaults{"--order-basis", "target", "--exit-rate", "1"};
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "base", {}), out, err), 0) << err.str();
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "base6", defaults), out, err), 0) << err.str();
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "v5", v5_flags), out, err), 0) << err.str();
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "v56", joined(v5_flags, defaults)), out, err), 0)
      << err.str();
  expect_same_files(dir.path / "base", dir.path / "base6");
  expect_same_files(dir.path / "v5", dir.path / "v56");
  EXPECT_EQ(co::sha256_file((dir.path / "base" / "recipe.json").string()).value(),
            "73cb45f1182f659b1b66bf5adc17bc0539a95c987d191c10c00f846c6c8017c0");
  EXPECT_EQ(co::sha256_file((dir.path / "v5" / "recipe.json").string()).value(),
            "d53f0c09018244862e8974579e8a87184310778786b08545041c605f725fc71a");
  auto api = a.cfg; api.output_directory = (dir.path / "api").string();
  const auto ran = st::run_nav_replay(api, st::NavTurnoverLimits{}, st::NavFieldsPin{},
                                      st::NavRateOptions{}, st::NavExecutionOptions{}, progress);
  ASSERT_TRUE(ran) << ran.error().to_string();
  expect_same_files(dir.path / "base", dir.path / "api");
  const auto recipe = read_json(dir.path / "v5" / "recipe.json");
  for (const auto* key : {"order_basis", "order_basis_rule", "locate_in_aim",
                          "locate_in_aim_rule", "exit_rate", "exit_rate_rule", "exit_rule"})
    EXPECT_FALSE(recipe.contains(key)) << key;
  EXPECT_TRUE(aim_text_has(recipe, "nonmembers exit to 0"));
  const auto summary = read_json(dir.path / "v5" / "summary.json");
  EXPECT_FALSE(summary.contains("order_basis"));
  EXPECT_FALSE(summary.contains("locate_in_aim"));
  for (const auto& s : summary.at("scenarios"))
    EXPECT_FALSE(s.at("construction").at("v5").contains("exit_rate"));
}

// Review F8: the shared execution liquidity cache at a FIXED rate is bit-identical. Every
// lockstep book (capped S2/S3 working orders, write-offs, forced exits) under baseline,
// the v5 reference rule, the exit rate and both order bases equals the uncached book in
// every compared field; the pinned CLI with --liquidity-cache publishes every file byte
// for byte (the financing matrix included).
TEST(NavV6, LiquidityCacheAtFixedRateIsBitIdentical) {
  const auto in = SyntheticRole::default_role();
  const auto scenarios = st::fixed_nav_scenarios();
  st::TargetReplayConfig baseline; baseline.cadence = 1; baseline.trade_fraction = .5;
  for (const auto& rule : {baseline, v6_aim(0.05, 0.1), v6_aim(0.05, 0.1, 0.1)}) {
    for (const auto basis : {st::NavOrderBasis::Target, st::NavOrderBasis::Delta}) {
      auto off = v5_config(0.05, 0.1, 1e7);
      off.target = rule; off.order_basis = basis;
      auto on = off; on.liquidity_cache = true;
      const auto a = st::replay_nav_scenarios(in.view(), off, scenarios);
      const auto b = st::replay_nav_scenarios(in.view(), on, scenarios);
      ASSERT_TRUE(a) << a.error().to_string();
      ASSERT_TRUE(b) << b.error().to_string();
      ASSERT_EQ(a->size(), b->size());
      usize capped = 0;
      for (usize k = 0; k < a->size(); ++k) {
        const auto& x = (*a)[k]; const auto& y = (*b)[k];
        expect_same_result(x, y);
        EXPECT_EQ(bits(x.max_return_identity_error), bits(y.max_return_identity_error)) << k;
        EXPECT_EQ(bits(x.max_cash_book_error), bits(y.max_cash_book_error)) << k;
        for (usize d = 0; d < x.days.size(); ++d) {
          const auto& u = x.days[d]; const auto& v = y.days[d];
          EXPECT_EQ(bits(u.net_return), bits(v.net_return)) << k << ' ' << d;
          EXPECT_EQ(bits(u.linear_cost_dollars), bits(v.linear_cost_dollars)) << k << ' ' << d;
          EXPECT_EQ(bits(u.unrationed_cost_dollars), bits(v.unrationed_cost_dollars)) << d;
          EXPECT_EQ(u.blocked_liquidity, v.blocked_liquidity) << k << ' ' << d;
          EXPECT_EQ(u.fallback_vol_fills, v.fallback_vol_fills) << k << ' ' << d;
          if (k == st::nav_primary_scenario_index) capped += u.capped_fills;
        }
      }
      EXPECT_GT(capped, 0U); // S2 working residuals exercised
    }
  }
  const auto p = publication_panel();
  Fields f(p);
  for (usize t = 0; t < p.d; ++t) f.set(p, t, 0, 1e6, 5e5);
  Directory dir; const auto a = write_artifact(dir.path, p);
  const auto pin = write_fields(dir.path, a, p, f, [](Json&) {});
  const auto fields = joined(v5_flags, {"--fields", pin.manifest_path, "--fields-sha256",
                                        pin.manifest_sha256});
  std::ostringstream out, err;
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "plain", fields), out, err), 0) << err.str();
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "cached", joined(fields, {"--liquidity-cache"})),
                     out, err), 0) << err.str();
  expect_same_files(dir.path / "plain", dir.path / "cached");
  EXPECT_EQ(file_names(dir.path / "plain").size(), 12U); // recipe, summary, 5 books x 2 CSVs
}

// (b) Without drift the delta basis IS the target basis, bit for bit: constant prices
// (absent names reprint at the same price), thin names capped by S2/S3 (residuals worked
// across sessions and kept on non-rebalance decisions), forced and absent exits, S3
// write-offs and the financing matrix with the locate rule (special names), under
// baseline (cadence 1 and 3) and v5 (theta 1; theta .05 with the exit rate). Only drift
// separates the bases, so this pins every delta code path to the target arithmetic.
TEST(NavV6, DeltaWithoutDriftIsTargetBitForBit) {
  Panel p(40, 8); p.begin = 3;
  Lcg g{17};
  for (usize t = 0; t < p.d; ++t)
    for (usize i = 0; i < p.n; ++i) {
      const auto c = p.k(t, i);
      p.signal[c] = g.next() - 0.5;
      p.volume[c] = i < 3 ? 2e3 : 1e6; // names 0-2 are thin: the 1% ADV cap binds
      const f64 u = g.next();
      if (t > 0 && u < 0.05) p.absent(t, i); else if (u < 0.10) p.nonmember(t, i);
    }
  const auto f = random_fields(p, 23);
  st::TargetReplayConfig daily; daily.cadence = 1; daily.trade_fraction = 1;
  st::TargetReplayConfig slow; slow.cadence = 3; slow.trade_fraction = .5;
  const auto matrix = st::nav_scenario_matrix(true);
  usize capped = 0, blocked = 0, written_off = 0;
  f64 forced = 0;
  for (const auto& rule : {daily, slow, v6_aim(1, 0), v6_aim(0.05, 0.1, 0.05)}) {
    auto target = config(matrix[0], 1e6, rule);
    target.liquidity_window = 5; target.min_vol_pairs = 3;
    auto delta = target; delta.order_basis = st::NavOrderBasis::Delta;
    const auto a = st::replay_nav_scenarios(with_fields(p, f), target, matrix);
    const auto b = st::replay_nav_scenarios(with_fields(p, f), delta, matrix);
    ASSERT_TRUE(a) << a.error().to_string();
    ASSERT_TRUE(b) << b.error().to_string();
    ASSERT_EQ(a->size(), b->size());
    for (usize k = 0; k < a->size(); ++k) {
      const auto& x = (*a)[k]; const auto& y = (*b)[k];
      expect_same_result(x, y);
      EXPECT_EQ(bits(x.max_cash_book_error), bits(y.max_cash_book_error)) << k;
      for (usize d = 0; d < x.days.size(); ++d) {
        const auto& u = x.days[d]; const auto& v = y.days[d];
        expect_same_financing(u, v);
        EXPECT_EQ(bits(u.net_return), bits(v.net_return)) << k << ' ' << d;
        EXPECT_EQ(bits(u.planned_forced), bits(v.planned_forced)) << k << ' ' << d;
        EXPECT_EQ(bits(u.planned_net), bits(v.planned_net)) << k << ' ' << d;
        capped += u.capped_fills; blocked += u.blocked_short_names; forced += u.planned_forced;
      }
      written_off += x.written_off.count;
    }
  }
  EXPECT_GT(capped, 0U);
  EXPECT_GT(blocked, 0U);
  EXPECT_GT(written_off, 0U);
  EXPECT_GT(forced, 0);
}

// (b) theta 1 with drift, hand-computed at NAV 1000 without costs. Both bases plan the
// aim (gross 1, net 0) at every decision. A (the short) moves 100, 100, 120, 132, 132, 132
// and B stays at 100. Decision 2 cuts A from -600 to -450 and B from 500 to 450 (NAV 900);
// A rises 10% before the fill: the target book buys 210 of A (the drift traded back)
// and holds -450 / 450; the delta book buys the 150 decided and holds -510 / 450 (the
// -60 drift rides). After the drift-free session 4 both hold -420 / 420 at NAV 840.
TEST(NavV6, DeltaThetaOnePlansTheAimAndRidesOneDayOfDrift) {
  Panel p(6, 2); p.by_name({1, 2});
  const f64 a[] = {100, 100, 120, 132, 132, 132};
  for (usize t = 0; t < p.d; ++t) p.price(t, 0, a[t]);
  const auto target = config(flat(0, 0), 1000, v6_aim(1, 0));
  auto delta = target; delta.order_basis = st::NavOrderBasis::Delta;
  const auto rt = st::replay_nav(p.nav(), target);
  const auto rd = st::replay_nav(p.nav(), delta);
  ASSERT_TRUE(rt) << rt.error().to_string();
  ASSERT_TRUE(rd) << rd.error().to_string();
  const auto& t = rt->days; const auto& d = rd->days;
  ASSERT_EQ(t.size(), 6U); ASSERT_EQ(d.size(), 6U);
  for (usize k = 0; k < t.size(); ++k) {
    EXPECT_EQ(bits(t[k].pretrade_nav), bits(d[k].pretrade_nav)) << k; // same marks, no costs
    if (!t[k].decision) continue;
    EXPECT_NEAR(t[k].planned_gross, 1.0, 1e-12) << k;
    EXPECT_NEAR(d[k].planned_gross, 1.0, 1e-12) << k;
    EXPECT_NEAR(t[k].planned_net, 0.0, 1e-12) << k;
    EXPECT_NEAR(d[k].planned_net, 0.0, 1e-12) << k;
  }
  EXPECT_NEAR(t[3].pretrade_nav, 840, 1e-9);
  for (const auto* days : {&t, &d}) {
    EXPECT_NEAR((*days)[1].traded_dollars, 1000, 1e-9); // deployment: nothing drifted yet
    EXPECT_NEAR((*days)[2].traded_dollars, 0, 1e-9);    // decision 1 is on the aim
    EXPECT_NEAR((*days)[2].short_dollars, 600, 1e-9);
    EXPECT_NEAR((*days)[2].long_dollars, 500, 1e-9);
    EXPECT_NEAR((*days)[4].short_dollars, 420, 1e-9); // back on the target
    EXPECT_NEAR((*days)[4].long_dollars, 420, 1e-9);
  }
  EXPECT_NEAR(t[3].traded_dollars, 210 + 50, 1e-9);
  EXPECT_NEAR(t[3].short_dollars, 450, 1e-9);
  EXPECT_NEAR(t[3].long_dollars, 450, 1e-9);
  EXPECT_NEAR(d[3].traded_dollars, 150 + 50, 1e-9);
  EXPECT_NEAR(d[3].short_dollars, 510, 1e-9);
  EXPECT_NEAR(d[3].long_dollars, 450, 1e-9);
  EXPECT_NEAR(t[4].traded_dollars, 30 + 30, 1e-9);
  EXPECT_NEAR(d[4].traded_dollars, 90 + 30, 1e-9);
}

// (b) A capped residual under delta keeps its decision-dollar delta. B (thin, the long,
// target 5000 at NAV 1e4) deploys over three S2 sessions at 1% of ADV (2000, 2000, then
// the rest; ADV 2e5, 2e5, 2.05e5) while its price rises 10% twice (sessions 7 and 8). The
// target book fills 2000, 2000, 380 and ends exactly on 5000; the delta book fills 2000,
// 2000, 1000 (the 5000 decided) and ends on 5000 plus its fills' drift, 4620 - 4000.
// Non-rebalance decisions (cadence 1000) keep both working orders.
TEST(NavV6, DeltaCappedResidualKeepsItsDecisionDollarDelta) {
  Panel p(11, 2); p.begin = 5; p.by_name({1, 2});
  for (usize t = 0; t < p.d; ++t) {
    p.volume[p.k(t, 0)] = 1e8; p.volume[p.k(t, 1)] = 2e3;
    p.price(t, 1, t <= 6 ? 100.0 : t == 7 ? 110.0 : 121.0);
  }
  const auto target = config(st::fixed_nav_scenarios()[st::nav_primary_scenario_index], 1e4,
                             hold_after_deployment());
  auto delta = target; delta.order_basis = st::NavOrderBasis::Delta;
  const auto rt = st::replay_nav(p.nav(), target);
  const auto rd = st::replay_nav(p.nav(), delta);
  ASSERT_TRUE(rt) << rt.error().to_string();
  ASSERT_TRUE(rd) << rd.error().to_string();
  const auto& t = rt->days; const auto& d = rd->days; // row k is session 5 + k
  for (const auto* days : {&t, &d}) {
    const auto& r = *days;
    EXPECT_NEAR(r[1].traded_dollars, 5000 + 2000, 1e-6); // A complete, B capped
    EXPECT_EQ(r[1].capped_fills, 1U);
    EXPECT_NEAR(r[2].traded_dollars, 2000, 1e-6);
    EXPECT_EQ(r[2].capped_fills, 1U);
    EXPECT_EQ(r[3].fills, 1U);
    EXPECT_EQ(r[3].capped_fills, 0U);
    EXPECT_EQ(r[4].fills, 0U); // both orders complete
    EXPECT_NEAR(r[4].short_dollars, 5000, 1e-6);
  }
  EXPECT_NEAR(t[3].traded_dollars, 5000 - (2000 * 1.21 + 2000 * 1.1), 1e-6);
  EXPECT_NEAR(t[3].long_dollars, 5000, 1e-6);
  EXPECT_NEAR(d[3].traded_dollars, 1000, 1e-6);
  EXPECT_NEAR(d[3].long_dollars, 5000 + (4620 - 4000), 1e-6);
  EXPECT_NEAR(d[1].traded_dollars - 5000 + d[2].traded_dollars + d[3].traded_dollars, 5000,
              1e-6); // B's fills sum to the decided delta
}

// Under the locate rule a special-tier name's orders stay target orders. Name 0 (special,
// long) is cut from +1/3 to +1/6 of NAV at decision 2 and its price falls 60% before the
// fill. A delta order would sell the decided 1/6 x NAV through zero and open a short of
// .6 x 1/3 - 1/6 of NAV (the unguarded book: special but without the locate rule); the
// guarded book's target order ends long on its target. Everything else is the same.
TEST(NavV6, DeltaSpecialTierOrdersStayTargetUnderTheLocateRule) {
  Panel p(6, 5);
  for (usize t = 0; t < p.d; ++t) {
    const f64 row[] = {t < 2 ? 5.0 : 3.5, 1, 2, 3, 4};
    for (usize i = 0; i < p.n; ++i) p.signal[p.k(t, i)] = row[i];
    p.price(t, 0, t < 3 ? 100.0 : 40.0);
  }
  Fields f(p);
  for (usize t = 0; t < p.d; ++t) f.set(p, t, 0, 1e6, 5e5); // cap 1e8 / 4e7, SI .5: special
  auto guarded = flat(0, 0); guarded.financing = swap_fin();
  auto unguarded = guarded;
  unguarded.financing.id = "swap-no-block"; unguarded.financing.block_special_shorts = false;
  auto cfg = config(guarded, 1e6, v6_aim(1, 0)); cfg.order_basis = st::NavOrderBasis::Delta;
  const auto g = st::replay_nav(with_fields(p, f), cfg);
  cfg.scenario = unguarded;
  const auto u = st::replay_nav(with_fields(p, f), cfg);
  ASSERT_TRUE(g) << g.error().to_string();
  ASSERT_TRUE(u) << u.error().to_string();
  EXPECT_EQ(g->days[0].member_tiers, (std::array<usize, 3>{4, 0, 1}));
  for (usize k = 0; k < 3; ++k) { // identical books before the fill of decision 2
    EXPECT_EQ(bits(g->days[k].short_dollars), bits(u->days[k].short_dollars)) << k;
    EXPECT_EQ(bits(g->days[k].long_dollars), bits(u->days[k].long_dollars)) << k;
    EXPECT_EQ(g->days[k].blocked_short_names, 0U) << k; // a long cut: nothing to block
  }
  const f64 nav2 = g->days[2].posttrade_nav;
  const f64 opened = 0.6 * 1e6 / 3.0 - nav2 / 6.0; // drift minus the decided sale
  EXPECT_GT(opened, 30000);
  EXPECT_NEAR(u->days[3].short_dollars - g->days[3].short_dollars, opened, 1.0);
  EXPECT_NEAR(g->days[3].long_dollars - u->days[3].long_dollars, nav2 / 6.0, 1.0);
}

// (c) The exit rate in the NAV book: exit_rate 1 is the default book bit for bit; at .05
// (theta 1, dust .1, constant prices, no costs) name 5 (present; leaves membership at
// decision 3) decays 5% per decision until |next| <= .1 / N_d sets it to 0, its planned
// forced turnover exactly that decay; name 4, absent at decision 3 only, exits at once.
// Six centered ranks have gross 1.8: name 5's aim is .5 / 1.8, name 4's .3 / 1.8.
TEST(NavV6, ExitRateDecaysPresentNonmemberInTheNavBook) {
  Panel p(70, 6); p.by_name({1, 2, 3, 4, 5, 6});
  for (usize t = 3; t < p.d; ++t) p.nonmember(t, 5);
  p.absent(3, 4);
  const auto immediate = config(flat(0, 0), 1e6, v6_aim(1, 0.1));
  auto one = immediate; one.target.exit_rate = 1;
  auto slow = immediate; slow.target.exit_rate = 0.05;
  const auto ri = st::replay_nav(p.nav(), immediate);
  const auto r1 = st::replay_nav(p.nav(), one);
  const auto rs = st::replay_nav(p.nav(), slow);
  ASSERT_TRUE(ri) << ri.error().to_string();
  ASSERT_TRUE(r1) << r1.error().to_string();
  ASSERT_TRUE(rs) << rs.error().to_string();
  expect_same_result(*ri, *r1);
  EXPECT_NEAR(ri->days[3].planned_forced, (0.5 + 0.3) / 1.8, 1e-12);
  EXPECT_EQ(ri->days[4].planned_forced, 0);
  const auto& d = rs->days;
  f64 prev = 0.5 / 1.8;
  usize snapped = 0;
  for (usize t = 3; t + 2 < p.d; ++t) {
    const f64 band = 0.1 / (t == 3 ? 4.0 : 5.0); // N_d: name 4 is back from decision 4
    f64 next = prev * (1 - 0.05);
    if (std::abs(next) <= band) next = 0;
    EXPECT_NEAR(d[t].planned_forced, prev - next + (t == 3 ? 0.3 / 1.8 : 0.0), 1e-12) << t;
    if (next == 0 && prev != 0) {
      snapped = t;
      EXPECT_EQ(d[t].planned_held_names + 1, d[t - 1].planned_held_names) << t;
    }
    prev = next;
  }
  EXPECT_EQ(snapped, 54U); // .5 / 1.8 x .95^k <= .02 first at k = 52
  for (usize t = 0; t < 3; ++t) expect_same_day(ri->days[t], d[t]);
}

// (d) Locate-in-aim against the post-block path. Name 6 (mid-range exposures) is special
// at every decision and always ranks lowest, so its desired weight is the largest short.
// The post-block book cannot short it after neutralization and runs net long by about
// that weight; zeroing the aim before neutralization lets the regression re-balance net,
// so the mean |net| leverage is smaller and the safety-net block refuses fewer dollars.
// Every decision zeroes exactly that aim; without the borrow fields or a neutralizing
// construction locate-in-aim is refused.
TEST(NavV6, LocateInAimLeavesLessNetThanThePostBlock) {
  auto p = noisy_panel(70, 12, 21);
  constexpr usize special = 6;
  for (usize t = 0; t < p.d; ++t) p.signal[p.k(t, special)] = -10;
  Fields f(p);
  for (usize t = 0; t < p.d; ++t) f.set(p, t, special, 1e6, 5e5); // small cap, SI .5
  auto rule = construction_daily(); rule.band_multiple = 0;
  auto s = flat(0, 0); s.financing = swap_fin();
  const auto post = config(s, 1e6, rule);
  auto aim = post; aim.locate_in_aim = true;
  const auto a = st::replay_nav(with_fields(p, f), post);
  const auto b = st::replay_nav(with_fields(p, f), aim);
  ASSERT_TRUE(a) << a.error().to_string();
  ASSERT_TRUE(b) << b.error().to_string();
  f64 net_post = 0, net_aim = 0, blocked_post = 0, blocked_aim = 0;
  usize rows = 0, decisions = 0;
  for (usize t = 0; t < p.d; ++t) {
    const auto& x = a->days[t]; const auto& y = b->days[t];
    blocked_post += x.blocked_short_dollars; blocked_aim += y.blocked_short_dollars;
    EXPECT_EQ(x.construction.locate_zeroed, 0U) << t;
    if (y.decision) {
      EXPECT_EQ(y.construction.locate_zeroed, 1U) << t;
      ++decisions;
    }
    if (t < 22) continue; // neutralized from decision 20, deployed at session 21
    net_post += std::abs(x.net_leverage); net_aim += std::abs(y.net_leverage); ++rows;
  }
  ASSERT_GT(rows, 0U);
  EXPECT_EQ(decisions, p.d - 2);
  EXPECT_GT(net_post / static_cast<f64>(rows), 0.05); // the post block leaves a net long
  EXPECT_LT(net_aim, 0.5 * net_post);
  EXPECT_LT(blocked_aim, blocked_post);
  auto no_fields = aim; no_fields.scenario = flat(0, 0);
  const auto unfielded = st::replay_nav(p.nav(), no_fields);
  ASSERT_FALSE(unfielded);
  EXPECT_EQ(unfielded.error().code(), co::ErrorCode::InvalidArgument);
  auto no_neutral = aim; no_neutral.target.neutralize = st::TargetNeutralize::None;
  const auto unneutral = st::replay_nav(with_fields(p, f), no_neutral);
  ASSERT_FALSE(unneutral);
  EXPECT_EQ(unneutral.error().code(), co::ErrorCode::InvalidArgument);
}

// Recipe/summary keys of the v6 options (only when non-default) and CLI refusals, all
// before any output: delta adds order_basis + order_basis_rule (every other recipe value
// unchanged) and summary order_basis; the exit rate adds exit_rate + exit_rate_rule and
// construction.v5.exit_rate, and the fixed and per-name aim_partial texts then say
// "nonmembers follow exit_rate_rule"; locate-in-aim adds locate_in_aim + locate_in_aim_rule and
// summary locate_in_aim.zeroed_special_short_aims (cadence 1: seven decisions, name 0
// the special lowest-ranked member at six of them; it is absent at session 4).
TEST(NavV6, RecipeSummaryKeysAndCliRefusals) {
  const auto p = publication_panel();
  Fields f(p);
  for (usize t = 0; t < p.d; ++t) f.set(p, t, 0, 1e6, 5e5); // name 0 (the short) special
  Directory dir; const auto a = write_artifact(dir.path, p);
  const auto pin = write_fields(dir.path, a, p, f, [](Json&) {});
  const std::vector<std::string> fields{"--fields", pin.manifest_path, "--fields-sha256",
                                        pin.manifest_sha256};
  std::ostringstream out, err;
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "base", {}), out, err), 0) << err.str();
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "delta", {"--order-basis", "delta"}), out, err), 0)
      << err.str();
  const auto base = read_json(dir.path / "base" / "recipe.json");
  const auto delta = read_json(dir.path / "delta" / "recipe.json");
  auto keys = sorted_keys(base);
  keys.emplace_back("order_basis"); keys.emplace_back("order_basis_rule");
  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(sorted_keys(delta), keys);
  EXPECT_EQ(delta.at("order_basis"), "delta");
  EXPECT_TRUE(delta.at("order_basis_rule").is_string());
  for (const auto& item : base.items()) EXPECT_EQ(delta.at(item.key()), item.value()) << item.key();
  const auto delta_summary = read_json(dir.path / "delta" / "summary.json");
  EXPECT_EQ(delta_summary.at("order_basis"), "delta");
  EXPECT_EQ(delta_summary.at("recipe_sha256"), co::sha256_hex(delta.dump()).value());
  EXPECT_FALSE(read_json(dir.path / "base" / "summary.json").contains("order_basis"));

  ASSERT_EQ(dispatch(nav_args(a, dir.path / "exit", joined(v5_flags, {"--exit-rate", ".05"})),
                     out, err), 0) << err.str();
  const auto exits = read_json(dir.path / "exit" / "recipe.json");
  EXPECT_EQ(exits.at("exit_rate"), 0.05);
  EXPECT_TRUE(exits.at("exit_rate_rule").is_string());
  EXPECT_FALSE(exits.contains("exit_rule"));
  EXPECT_FALSE(exits.contains("forced_exits")); // the target replay's own key
  EXPECT_TRUE(aim_text_has(exits, "nonmembers follow exit_rate_rule"));
  EXPECT_FALSE(aim_text_has(exits, "nonmembers exit to 0"));
  // Named: a range-for over .at() of a temporary dangles in C++20 (no P2718).
  const auto exit_summary = read_json(dir.path / "exit" / "summary.json");
  for (const auto& s : exit_summary.at("scenarios"))
    EXPECT_EQ(s.at("construction").at("v5").at("exit_rate"), 0.05);
  ASSERT_EQ(dispatch(nav_args(a, dir.path / "exitpn",
                              joined(v5_flags, {"--rate", "per-name-v1", "--exit-rate", ".05"})),
                     out, err), 0) << err.str();
  const auto exit_per_name = read_json(dir.path / "exitpn" / "recipe.json");
  EXPECT_EQ(exit_per_name.at("exit_rate_rule"), exits.at("exit_rate_rule"));
  EXPECT_TRUE(aim_text_has(exit_per_name, "; rate per-name-v1: theta_i = clip("));
  EXPECT_TRUE(aim_text_has(exit_per_name, "keep member weights; nonmembers follow exit_rate_rule"));
  EXPECT_FALSE(aim_text_has(exit_per_name, "nonmembers exit to 0"));

  ASSERT_EQ(dispatch(nav_args(a, dir.path / "locate",
                              joined(fields, {"--neutralize", "price-risk-v1", "--cadence", "1",
                                              "--locate-in-aim"})), out, err), 0) << err.str();
  const auto locate = read_json(dir.path / "locate" / "recipe.json");
  EXPECT_EQ(locate.at("locate_in_aim"), true);
  EXPECT_TRUE(locate.at("locate_in_aim_rule").is_string());
  const auto locate_summary = read_json(dir.path / "locate" / "summary.json");
  EXPECT_EQ(locate_summary.at("locate_in_aim").at("zeroed_special_short_aims"), 6);

  const std::vector<std::pair<std::string, std::vector<std::string>>> refused{
      {"r1", {"--neutralize", "price-risk-v1", "--locate-in-aim"}}, // no borrow fields
      {"r2", joined(fields, {"--locate-in-aim"})},                  // no neutralization
      {"r4", {"--exit-rate", ".05"}},                               // baseline-v1
      {"r5", {"--rule", "aim-partial-v5", "--exit-rate", ".05"}},   // no dust band
      {"r6", joined(v5_flags, {"--exit-rate", "0"})},
      {"r7", joined(v5_flags, {"--exit-rate", "1.5"})}};
  for (const auto& [name, extra] : refused)
    EXPECT_EQ(dispatch(nav_args(a, dir.path / name, extra), out, err), 1) << name;
  const std::vector<std::pair<std::string, std::vector<std::string>>> usage{
      {"u1", {"--order-basis", "drift"}},
      {"u2", {"--liquidity-cache", "--liquidity-cache"}},
      {"u3", joined(v5_flags, {"--exit-rate", "fast"})}};
  for (const auto& [name, extra] : usage)
    EXPECT_EQ(dispatch(nav_args(a, dir.path / name, extra), out, err), 2) << name;
  for (const auto* name : {"r1", "r2", "r4", "r5", "r6", "r7", "u1", "u2", "u3"})
    EXPECT_FALSE(std::filesystem::exists(dir.path / name)) << name;
}

// The target replay's own CLI (targets verb) carries the exit rate: --exit-rate 1 is the
// plain run byte for byte; .05 records exit_rate, exit_rate_rule and the decaying forced_exits
// spelling in the recipe and construction.v5.exit_rate in the summary, and spreads the
// exit of name 2 (a present nonmember from decision 5) beyond the window, so its forced
// turnover is smaller; without --role (no presence) it is refused before any output.
TEST(NavV6, TargetsVerbCarriesTheExitRate) {
  auto p = publication_panel();
  for (usize t = 5; t < p.d; ++t) p.nonmember(t, 2);
  Directory dir; const auto a = write_artifact(dir.path, p);
  std::ostringstream out, err;
  const auto targets = [&](const std::string& output, bool role,
                           const std::vector<std::string>& extra) {
    std::vector<std::string> all{"targets", "--combined", a.cfg.combined_path,
                                 "--combined-sha256", a.cfg.combined_sha256, "--output",
                                 (dir.path / output).string(), "--rule", "aim-partial-v5",
                                 "--cadence", "1", "--trade-fraction", ".5", "--dust-multiple",
                                 ".1"};
    if (role) {
      for (const auto& arg : {std::string("--role"), a.cfg.role_path,
                              std::string("--role-sha256"), a.cfg.role_sha256})
        all.push_back(arg);
    }
    all.insert(all.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& arg : all) argv.push_back(arg.data());
    return st::dispatch_target_replay(static_cast<int>(argv.size()), argv.data(), out, err);
  };
  ASSERT_EQ(targets("plain", true, {}), 0) << err.str();
  ASSERT_EQ(targets("one", true, {"--exit-rate", "1"}), 0) << err.str();
  ASSERT_EQ(targets("slow", true, {"--exit-rate", ".05"}), 0) << err.str();
  expect_same_files(dir.path / "plain", dir.path / "one");
  const auto plain = read_json(dir.path / "plain" / "recipe.json");
  const auto slow = read_json(dir.path / "slow" / "recipe.json");
  EXPECT_EQ(plain.at("forced_exits"), "immediate;charged-before-discretionary;may-breach");
  EXPECT_FALSE(plain.contains("exit_rate"));
  EXPECT_EQ(slow.at("forced_exits"), "decay-at-exit-rate;snap-inside-dust-band;absent-immediate");
  EXPECT_EQ(slow.at("exit_rate"), 0.05);
  EXPECT_TRUE(slow.at("exit_rate_rule").is_string());
  EXPECT_FALSE(slow.contains("exit_rule"));
  EXPECT_TRUE(aim_text_has(plain, "nonmembers exit to 0"));
  EXPECT_TRUE(aim_text_has(slow, "nonmembers follow exit_rate_rule"));
  const auto plain_summary = read_json(dir.path / "plain" / "summary.json");
  const auto slow_summary = read_json(dir.path / "slow" / "summary.json");
  EXPECT_FALSE(plain_summary.at("construction").at("v5").contains("exit_rate"));
  EXPECT_EQ(slow_summary.at("construction").at("v5").at("exit_rate"), 0.05);
  EXPECT_LT(slow_summary.at("forced_turnover").get<f64>(),
            plain_summary.at("forced_turnover").get<f64>());
  EXPECT_EQ(targets("norole", false, {"--exit-rate", ".05"}), 1);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "norole"));
}

// ---- v6 C2: workspace reserve at the run geometry, industry neutralization ----
namespace {
constexpr u64 mib = 1ULL << 20;
// grp_ff12 of noisy-panel names on every row: 0-5 id 4 (6 names), 6-9 id 7 (4 names,
// pooled into the fallback) and 10-11 unknown (2 names, pooled too).
std::vector<f64> panel_industry(const Panel& p) {
  std::vector<f64> ids(p.d * p.n);
  for (usize t = 0; t < p.d; ++t)
    for (usize i = 0; i < p.n; ++i) ids[p.k(t, i)] = i < 6 ? 4.0 : i < 10 ? 7.0 : missing;
  return ids;
}
// write_fields' edit: adds the grp_ff12 payload and its manifest entry (point in time
// unless `pit` is false) next to shares_out and si_shares.
auto add_industry(const std::filesystem::path& dir, const Panel& p,
                  const std::vector<f64>& ids, bool pit = true) {
  return [root = dir / "fields", dates = p.d, names = p.n, values = ids, pit](Json& m) {
    const auto receipt = write_payload(root / "grp_ff12.f64", values);
    m["files"]["grp_ff12.f64"] = receipt;
    m["fields"].push_back({{"name", "grp_ff12"}, {"file", "grp_ff12.f64"}, {"dtype", "<f8"},
        {"layout", "date-major"}, {"shape", Json::array({dates, names})},
        {"sha256", receipt.at("sha256")}, {"point_in_time", pit},
        {"non_pit_aspects", Json::array()}, {"clock", "fixture-grp_ff12"}});
  };
}
st::TargetReplayConfig industry_daily() {
  auto c = construction_daily(); c.neutralize = st::TargetNeutralize::PriceRiskIndV1; return c;
}
} // namespace

// (d) C4: the pinned run reserves its workspace at the role's own geometry (names x
// sessions x books) instead of max_names x max_dates. A budget the fixed reserve left
// short of the loader's 64 MiB metadata charge now runs; a budget at the geometry
// reserve, or one byte-short of reserve + metadata + blend, is refused before output.
TEST(NavV6, WorkspaceReserveIsChargedAtTheRunGeometry) {
  const auto p = publication_panel(); // 9 sessions x 3 names, score window [0, 9)
  const st::NavReplayConfig base{};   // the pinned run's defaults (fixed rate, event cap)
  const usize books = st::nav_scenario_matrix(false).size();
  const auto reserve = [&](usize names, usize sessions, bool tiered = false) {
    return st::nav_workspace_reserve_bytes(base, books, tiered, names, sessions);
  };
  const u64 actual = reserve(p.n, p.d), fixed = reserve(20000, 4096);
  EXPECT_EQ(reserve(p.n, p.d + 1) - actual, books * sizeof(st::NavReplayDay));
  EXPECT_LT(actual, reserve(p.n + 1, p.d));
  EXPECT_LT(actual, reserve(p.n, p.d, true));
  EXPECT_GE(fixed - actual, books * (4096 - p.d) * sizeof(st::NavReplayDay));
  // Premise: under the fixed reserve this budget left less than the 64 MiB metadata
  // charge the loader makes first, so that path refused it.
  const u64 budget = actual + 65 * mib;
  ASSERT_LT(budget, fixed + 64 * mib);
  std::ostringstream progress;
  {
    Directory dir; auto a = write_artifact(dir.path, p); a.cfg.target.max_working_bytes = budget;
    const auto ok = st::run_nav_replay(a.cfg, progress);
    ASSERT_TRUE(ok) << ok.error().to_string();
    EXPECT_EQ(read_json(dir.path / "out" / "recipe.json").at("max_working_bytes"), budget);
  }
  for (const u64 short_budget : {actual, actual + 64 * mib}) {
    Directory dir; auto a = write_artifact(dir.path, p);
    a.cfg.target.max_working_bytes = short_budget;
    const auto refused = st::run_nav_replay(a.cfg, progress);
    ASSERT_FALSE(refused) << short_budget;
    EXPECT_EQ(refused.error().code(), co::ErrorCode::OutOfRange) << short_budget;
    EXPECT_FALSE(std::filesystem::exists(dir.path / "out")) << short_budget;
  }
}

// Review I2 (the C1 x C2 merge): the reserve charges the shared liquidity cache whenever
// the run holds it -- rate per-name-v1, or --liquidity-cache at a fixed rate -- with the
// predicate validate_nav_input's budget uses, one cache row per name; a fixed rate
// without the cache charges none.
TEST(NavV6, WorkspaceReserveChargesTheLiquidityCacheAtAFixedRate) {
  const usize books = st::nav_scenario_matrix(true).size(), names = 11, sessions = 9;
  const auto reserve = [&](const st::NavReplayConfig& base) {
    return st::nav_workspace_reserve_bytes(base, books, true, names, sessions);
  };
  const st::NavReplayConfig fixed{};
  auto cached = fixed; cached.liquidity_cache = true;
  auto per_name = fixed; per_name.rate = st::NavRateRule::PerNameV1;
  auto both = per_name; both.liquidity_cache = true;
  ASSERT_GT(reserve(cached), reserve(fixed));
  EXPECT_EQ((reserve(cached) - reserve(fixed)) % names, 0U);
  EXPECT_EQ(reserve(cached), reserve(per_name));
  EXPECT_EQ(reserve(both), reserve(per_name));
}

// price-risk-ind-v1 end to end: the NAV forms exactly the target replay's industry
// construction (outcomes, amplification, group record) in every lockstep book; the
// pinned run loads grp_ff12 from --fields and publishes the id, the industry recipe and
// the group diagnostics; without the field (or with it not point in time) it is refused
// before output, and price-risk-v1 on the same fields never loads it.
TEST(NavV6, IndustryNeutralizeRunsWithTheFieldAndIsRefusedWithout) {
  const auto p = noisy_panel(70, 12, 21);
  const auto ids = panel_industry(p);
  const auto target = industry_daily();
  auto in = p.nav();
  in.target.industry = ids;
  const auto cfg = config(flat(6, 300), 1e6, target);
  auto r = st::replay_nav(in, cfg);
  ASSERT_TRUE(r) << r.error().to_string();
  auto planned_in = p.target();
  planned_in.industry = ids;
  auto planned = st::replay_targets(planned_in, target);
  ASSERT_TRUE(planned) << planned.error().to_string();
  usize applied = 0;
  for (usize t = 0; t + 2 < p.d; ++t) {
    const auto& c = r->days[t].construction;
    expect_same_construction(c, planned->days[t].construction, t);
    if (c.neutralize != st::NeutralizeOutcome::Applied) continue;
    ++applied;
    EXPECT_EQ(c.neutralize_groups, 2U) << t;
    EXPECT_EQ(c.neutralize_fallback_names, 6U) << t;
    EXPECT_EQ(c.neutralize_unknown_group_names, 2U) << t;
  }
  EXPECT_GT(applied, 0U);
  const auto scenarios = st::fixed_nav_scenarios();
  auto together = st::replay_nav_scenarios(in, cfg, scenarios);
  ASSERT_TRUE(together) << together.error().to_string();
  for (usize k = 0; k < scenarios.size(); ++k) {
    auto single = cfg; single.scenario = scenarios[k];
    auto alone = st::replay_nav(in, single); ASSERT_TRUE(alone);
    expect_same_result((*together)[k], *alone);
  }
  const auto no_field = st::replay_nav(p.nav(), cfg);
  ASSERT_FALSE(no_field);
  EXPECT_EQ(no_field.error().code(), co::ErrorCode::InvalidArgument);

  std::ostringstream progress;
  const Fields f(p);
  {
    Directory dir; auto a = write_artifact(dir.path, p); a.cfg.target = target;
    const auto pin = write_fields(dir.path, a, p, f, add_industry(dir.path, p, ids));
    const auto ok = st::run_nav_replay(a.cfg, st::NavTurnoverLimits{}, pin, progress);
    ASSERT_TRUE(ok) << ok.error().to_string();
    const auto out = dir.path / "out";
    const std::string id = "baseline-target-v1+neutral-price-risk-ind-v1+band-0.5";
    const auto recipe = read_json(out / "recipe.json");
    EXPECT_EQ(recipe.at("rule"), id);
    EXPECT_EQ(recipe.at("neutralize"), "price-risk-ind-v1");
    EXPECT_EQ(recipe.at("desired_target_postprocess"), "price-risk-ind-v1");
    EXPECT_EQ(recipe.at("industry").at("field"), "grp_ff12");
    EXPECT_EQ(recipe.at("industry").at("min_group_names"), 5);
    EXPECT_EQ(recipe.at("price_risk").at("vol_window"), 20);
    const auto& used = recipe.at("financing_fields").at("fields_used");
    EXPECT_EQ(used.at("grp_ff12").at("clock"), "fixture-grp_ff12");
    EXPECT_TRUE(used.contains("shares_out"));
    const auto summary = read_json(out / "summary.json");
    EXPECT_EQ(summary.at("rule"), id);
    for (const auto& s : summary.at("scenarios")) {
      const auto name = s.at("scenario").get<std::string>();
      const auto& c = s.at("construction");
      EXPECT_EQ(c.at("neutralize"), "price-risk-ind-v1") << name;
      const auto& industry = c.at("neutralize_industry");
      EXPECT_EQ(industry.at("applied_decisions"), applied) << name;
      EXPECT_EQ(industry.at("groups").at("median"), 2.0) << name;
      EXPECT_EQ(industry.at("fallback_names").at("max"), 6.0) << name;
      EXPECT_EQ(industry.at("unknown_group_names").at("min"), 2.0) << name;
      EXPECT_EQ(first_line(out / ("daily_" + name + ".csv")),
                std::string(t2_daily_header) + ",pretrade_gross_dollars,one_way_turnover_gmv" +
                    construction_columns + financing_columns) << name;
    }
  }
  { // price-risk-v1 on the same fields: grp_ff12 is not loaded, the recipe carries no industry
    Directory dir; auto a = write_artifact(dir.path, p); a.cfg.target = construction_daily();
    const auto pin = write_fields(dir.path, a, p, f, add_industry(dir.path, p, ids));
    ASSERT_TRUE(st::run_nav_replay(a.cfg, st::NavTurnoverLimits{}, pin, progress));
    const auto recipe = read_json(dir.path / "out" / "recipe.json");
    EXPECT_FALSE(recipe.at("financing_fields").at("fields_used").contains("grp_ff12"));
    EXPECT_FALSE(recipe.contains("industry"));
    EXPECT_FALSE(read_json(dir.path / "out" / "summary.json").at("scenarios")[0]
                     .at("construction").contains("neutralize_industry"));
  }
  const auto refused = [&](bool fields, auto edit) {
    Directory dir; auto a = write_artifact(dir.path, p); a.cfg.target = target;
    st::NavFieldsPin pin;
    if (fields) pin = write_fields(dir.path, a, p, f, edit(dir.path));
    const bool ran = static_cast<bool>(st::run_nav_replay(a.cfg, {}, pin, progress));
    return !ran && !std::filesystem::exists(dir.path / "out");
  };
  const auto none = [](const std::filesystem::path&) { return [](Json&) {}; };
  EXPECT_TRUE(refused(false, none)); // no --fields
  EXPECT_TRUE(refused(true, none));  // the fields lack grp_ff12
  EXPECT_TRUE(refused(true, [&](const std::filesystem::path& dir) {
    return add_industry(dir, p, ids, false); // declared not point in time
  }));
  EXPECT_FALSE(refused(true, [&](const std::filesystem::path& dir) {
    return add_industry(dir, p, ids); // control
  }));
  std::ostringstream o, e;
  Directory dir; const auto a = write_artifact(dir.path, p);
  EXPECT_EQ(dispatch({"nav", "--combined", a.cfg.combined_path, "--combined-sha256",
                      a.cfg.combined_sha256, "--role", a.cfg.role_path, "--role-sha256",
                      a.cfg.role_sha256, "--output", (dir.path / "never").string(),
                      "--neutralize", "price-risk-ind-v1"}, o, e), 1); // needs --fields
  EXPECT_FALSE(std::filesystem::exists(dir.path / "never"));
}
