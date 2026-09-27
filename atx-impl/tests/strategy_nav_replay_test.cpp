#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
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
    return {d, n, begin, end, signal, member, sessions, ids, close, raw, present};
  }
  [[nodiscard]] st::NavReplayInput nav() const { return {target(), volume}; }
};
st::NavScenario flat(f64 bps, f64 borrow_bps, usize stale = 5) {
  st::NavScenario s;
  s.id = "test-flat"; s.cost = st::NavCostRule::FlatBpsV1; s.flat_bps = bps;
  s.max_participation = uncapped; s.annual_borrow_bps = borrow_bps;
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
} // namespace

// Design fixture 1: zero cost/borrow/drift at NAV 1 -> the NAV path's planned side is
// the target replay bit-for-bit, and each decision's plan is exactly the next fill.
TEST(StrategyNavReplay, ConstantPricesNoCostReproducesTargetReplayPlanned) {
  Panel p(27, 7);
  for (usize t = 0; t < p.d; ++t) for (usize i = 0; i < p.n; ++i) {
    p.signal[p.k(t, i)] = static_cast<f64>((i + t / 3) % 4);
    if ((i == 1 && t >= 7) || (i == 6 && t < 8)) p.nonmember(t, i);
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
    EXPECT_GT(planned->forced_turnover, 0);
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
  EXPECT_EQ(d[4].blocked_absent, 1U);
  EXPECT_NEAR(d[5].mark_pnl_dollars, 100, 1e-9);
  EXPECT_NEAR(d[5].net_return, 0.1, 1e-14);
  EXPECT_NEAR(d[5].traded_dollars, 600, 1e-9); // exit at the reprint close
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
    EXPECT_EQ(scenario.annual_borrow_bps, 300) << scenario.id;
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
