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
#include "../src/strategy_ic_composition.hpp"
#include "../src/strategy_target_replay.hpp"

namespace {
using namespace atx;
namespace st = atx::impl::strategy;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
i64 session(int year, unsigned month, unsigned day) {
  return std::chrono::sys_days{std::chrono::year{year} / month / day}
      .time_since_epoch().count() * day_ns;
}
struct Fixture {
  usize d{}, n{};
  std::vector<f64> signal, close, raw;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Fixture(usize dates, usize names) : d(dates), n(names), signal(dates * names),
      member(dates * names, 1), sessions(dates), ids(names) {
    for (usize t = 0; t < d; ++t) sessions[t] = session(2020, 1, 1) + static_cast<i64>(t) * day_ns;
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
  }
  st::TargetReplayInput input() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present};
  }
};
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_target_replay_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("unique fixture directory");
  }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
template<class T> Json write_payload(const std::filesystem::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
std::string write_json(const std::filesystem::path& path, const Json& value) {
  std::ofstream out(path, std::ios::binary); out << value.dump(2) << '\n'; out.close();
  if (!out) throw std::runtime_error("fixture JSON write");
  return co::sha256_file(path.string()).value();
}
st::TargetReplayRunConfig artifact(const std::filesystem::path& dir, const Fixture& f) {
  std::vector<u8> finite(f.signal.size()); u64 finite_count = 0, members = 0;
  for (usize k = 0; k < finite.size(); ++k) {
    finite[k] = static_cast<u8>(std::isfinite(f.signal[k])); finite_count += finite[k]; members += f.member[k];
  }
  Json files;
  files["train_combined.f64"] = write_payload(dir / "train_combined.f64", f.signal);
  files["train_combined_member.u8"] = write_payload(dir / "train_combined_member.u8", f.member);
  files["train_combined_finite.u8"] = write_payload(dir / "train_combined_finite.u8", finite);
  files["train_combined_sessions.i64"] = write_payload(dir / "train_combined_sessions.i64", f.sessions);
  files["train_combined_ids.u64"] = write_payload(dir / "train_combined_ids.u64", f.ids);
  const std::string pin(64, 'a');
  Json manifest{{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"}, {"role", "train"},
      {"layout", "date-major-little-endian"}, {"dates", f.d}, {"instruments", f.n},
      {"score_begin", 0}, {"score_end", f.d}, {"role_manifest_sha256", pin},
      {"source_sha256", pin}, {"library_sha256", pin}, {"train_manifest_sha256", pin},
      {"run_recipe_sha256", pin}, {"orientation_candidates_sha256", pin},
      {"orientations_artifact_sha256", nullptr}, {"role_window_required", true},
      {"signal_semantics", "exact-pre-target-composition;equal-family/equal-within;missing-or-unoriented-neutral-fixed-denominator"},
      {"member_semantics", "decision-member-and-source-present-and-finite-positive-close;independent-of-component-coverage"},
      {"finite_semantics", "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal"},
      {"actual_trades_or_returns", false}, {"finite_cells", finite_count}, {"member_cells", members},
      {"files", std::move(files)}};
  st::TargetReplayRunConfig cfg;
  cfg.combined_path = (dir / "train_combined.json").string();
  cfg.combined_sha256 = write_json(cfg.combined_path, manifest);
  cfg.output_directory = (dir / "out").string(); return cfg;
}
} // namespace

TEST(StrategyTargetReplay, BaselineBitMatchesExistingCompositionIncludingTiesAndExits) {
  Fixture f(27, 7);
  for (usize d = 0; d < f.d; ++d) for (usize i = 0; i < f.n; ++i) {
    f.signal[d * f.n + i] = static_cast<f64>((i + d / 3) % 4);
    if ((i == 1 && d >= 7) || (i == 6 && d < 8)) {
      f.member[d * f.n + i] = 0; f.signal[d * f.n + i] = missing;
    }
  }
  const std::vector<st::IcCompositionCandidate> candidates{{"one", "family"}};
  st::IcCompositionConfig cc; cc.dates = f.d; cc.instruments = f.n; cc.decision_end = f.d;
  auto c = st::IcComposition::create(cc, candidates, f.member); ASSERT_TRUE(c);
  ASSERT_TRUE(c->add(0, f.signal, 1)); auto baseline = c->finish(); ASSERT_TRUE(baseline);
  f.signal = baseline->signal;
  auto replay = st::replay_targets(f.input(), {}); ASSERT_TRUE(replay);
  ASSERT_EQ(replay->days.size(), f.d);
  for (usize d = 0; d < f.d; ++d) {
    EXPECT_EQ(std::bit_cast<u64>(replay->days[d].turnover),
              std::bit_cast<u64>(baseline->planned_turnover[d])) << d;
    EXPECT_EQ(std::bit_cast<u64>(replay->days[d].gross),
              std::bit_cast<u64>(baseline->planned_gross[d])) << d;
    EXPECT_EQ(std::bit_cast<u64>(replay->days[d].net),
              std::bit_cast<u64>(baseline->planned_net[d])) << d;
  }
  EXPECT_EQ(replay->deployment_date, baseline->deployment_date);
  EXPECT_EQ(replay->deployment_turnover, baseline->deployment_turnover);
  EXPECT_EQ(replay->total_turnover, baseline->total_planned_turnover);
  EXPECT_GT(replay->forced_turnover, 0);
}

TEST(StrategyTargetReplay, MonthBudgetChargesDeploymentForcedExitAndResetsWithoutFutureKnowledge) {
  Fixture f(5, 2);
  f.sessions = {session(2020, 1, 29), session(2020, 1, 30), session(2020, 1, 31),
                session(2020, 2, 3), session(2020, 2, 4)};
  f.signal = {-1, 1, 1, -1, missing, -1, -1, 1, -1, 1}; f.member[4] = 0;
  st::TargetReplayConfig cfg; cfg.rule = st::TargetReplayRule::MonthlyTargetBudgetV2;
  cfg.cadence = 1; cfg.trade_fraction = 1; cfg.monthly_budget = .30;
  auto result = st::replay_targets(f.input(), cfg); ASSERT_TRUE(result);
  const auto& days = result->days;
  EXPECT_NEAR(days[0].turnover, .30, 1e-15);
  EXPECT_EQ(days[0].deployment_turnover, days[0].turnover);
  EXPECT_EQ(days[1].turnover, 0); // no remaining January discretionary budget
  EXPECT_NEAR(days[2].forced_turnover, .15, 1e-15);
  EXPECT_EQ(days[2].discretionary_turnover, 0);
  EXPECT_NEAR(days[2].budget_excess, .15, 1e-15);
  EXPECT_NEAR(days[2].net, .15, 1e-15); // no hidden survivor re-neutralization
  EXPECT_EQ(days[3].calendar_month, 202002U);
  EXPECT_NEAR(days[3].month_turnover, .30, 1e-15);
  EXPECT_LE(days[3].budget_excess, 1e-15);
  EXPECT_NEAR(result->total_turnover, .75, 1e-14);
  auto changed = f; changed.signal[6] = 100; changed.signal[7] = -100;
  auto future = st::replay_targets(changed.input(), cfg); ASSERT_TRUE(future);
  for (usize d = 0; d < 3; ++d) {
    EXPECT_EQ(result->days[d].turnover, future->days[d].turnover);
    EXPECT_EQ(result->days[d].gross, future->days[d].gross);
  }
}

TEST(StrategyTargetReplay, DelayedRoughReturnsExposeAbsentBackingAndGuardedLongShortRisk) {
  Fixture f(5, 2);
  f.signal = {-1, 1, -1, 1, -1, 1, -1, 1, -1, 1};
  f.close = {100, 100, 100, 100, 777, 110, 777, 121, 777, 121};
  f.raw = f.close; f.present.assign(10, 1); f.present[4] = 0;
  st::TargetReplayConfig cfg; cfg.cadence = 5; cfg.trade_fraction = 1;
  cfg.one_way_bps = 10; cfg.annual_borrow_bps = 252;
  auto result = st::replay_targets(f.input(), cfg); ASSERT_TRUE(result);
  const auto& first = result->days[0];
  EXPECT_EQ(first.entry, 1U); EXPECT_EQ(first.endpoint, 2U);
  EXPECT_TRUE(first.return_mature); EXPECT_FALSE(first.return_complete);
  EXPECT_NEAR(first.observed_return_component, .05, 1e-14);
  EXPECT_EQ(first.missing_long, 0); EXPECT_EQ(first.missing_short, .5);
  EXPECT_EQ(first.missing_gross, .5); EXPECT_EQ(first.missing_names, 1U);
  EXPECT_TRUE(std::isnan(first.complete_net_return));
  EXPECT_NEAR(first.modeled_trade_cost, .001, 1e-16); // deployment included
  EXPECT_NEAR(first.modeled_borrow_cost, .00005, 1e-16);
  EXPECT_FALSE(result->days[3].return_mature); // no invented tail return
  f.present[4] = 1; f.close[4] = 120; f.raw[4] = 100;
  auto guarded = st::replay_targets(f.input(), cfg); ASSERT_TRUE(guarded);
  EXPECT_EQ(guarded->days[0].guarded_names, 1U); // adjusted-only >.10 log jump
  EXPECT_EQ(guarded->days[0].missing_short, .5);
  f.close[4] = 90; f.raw[4] = 90;
  auto complete = st::replay_targets(f.input(), cfg); ASSERT_TRUE(complete);
  EXPECT_TRUE(complete->days[0].return_complete);
  EXPECT_NEAR(complete->days[0].complete_gross_return, .10, 1e-14);
  EXPECT_NEAR(complete->days[0].complete_net_return, .10 - .001 - .00005, 1e-14);
}

TEST(StrategyTargetReplay, PinnedArtifactRunsWithoutPricesAndRefusesTamperingBeforePublication) {
  Directory dir; Fixture f(6, 3);
  for (usize d = 0; d < f.d; ++d) for (usize i = 0; i < f.n; ++i)
    f.signal[d * f.n + i] = static_cast<f64>(i);
  auto cfg = artifact(dir.path, f); std::ostringstream progress;
  ASSERT_TRUE(st::run_target_replay(cfg, progress));
  std::ifstream input(dir.path / "out" / "summary.json"); Json summary; input >> summary;
  EXPECT_EQ(summary.at("status"), "complete"); EXPECT_TRUE(summary.at("net_sharpe").is_null());
  EXPECT_EQ(summary.at("complete_return_days"), 0);
  EXPECT_EQ(summary.at("daily_csv_sha256"),
            co::sha256_file((dir.path / "out" / "daily.csv").string()).value());
  EXPECT_DOUBLE_EQ(summary.at("monthly_reconciled_total").get<f64>(),
                   summary.at("total_turnover").get<f64>());
  EXPECT_FALSE(st::run_target_replay(cfg, progress)); // exclusive output
  cfg.output_directory = (dir.path / "tampered").string();
  std::fstream payload(dir.path / "train_combined.f64", std::ios::binary | std::ios::in | std::ios::out);
  const char changed = 1; payload.write(&changed, 1); payload.close();
  EXPECT_FALSE(st::run_target_replay(cfg, progress));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "tampered"));
}

TEST(StrategyTargetReplay, AdmissionAndFlatSignalDoNotInventExposure) {
  Fixture f(5, 4); auto cfg = st::TargetReplayConfig{};
  auto flat = st::replay_targets(f.input(), cfg); ASSERT_TRUE(flat);
  for (const auto& d : flat->days) { EXPECT_EQ(d.gross, 0); EXPECT_EQ(d.turnover, 0); }
  cfg.max_working_bytes = 1; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  cfg = {}; f.member[0] = 2; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  f.member[0] = 1; f.signal[0] = missing; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  f.signal[0] = 0; f.sessions[1] = f.sessions[0]; EXPECT_FALSE(st::replay_targets(f.input(), cfg));
  Directory dir; Fixture small(3, 2); auto run = artifact(dir.path, small);
  run.target.max_working_bytes = 64ULL << 20;
  std::filesystem::remove(dir.path / "train_combined.f64");
  std::ostringstream progress; auto refused = st::run_target_replay(run, progress);
  ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code(), co::ErrorCode::OutOfRange);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "out")); // admitted before payload I/O
}

TEST(StrategyTargetReplay, RoughReturnsRespectDeclaredWindowEvenWithFuturePayload) {
  Fixture f(7, 2);
  for (usize d = 0; d < f.d; ++d) { f.signal[2 * d] = -1; f.signal[2 * d + 1] = 1; }
  f.close.assign(14, 100); f.raw = f.close; f.present.assign(14, 1);
  auto input = f.input(); input.decision_end = 4;
  auto before = st::replay_targets(input, {}); ASSERT_TRUE(before);
  ASSERT_EQ(before->days.size(), 4U);
  EXPECT_TRUE(before->days[0].return_mature); EXPECT_TRUE(before->days[1].return_mature);
  EXPECT_FALSE(before->days[2].return_mature); EXPECT_FALSE(before->days[3].return_mature);
  for (usize k = 8; k < f.close.size(); ++k) {
    f.close[k] = 1000 + static_cast<f64>(k); f.raw[k] = 1; f.present[k] = 0;
  }
  input = f.input(); input.decision_end = 4;
  auto after = st::replay_targets(input, {}); ASSERT_TRUE(after);
  for (usize d = 0; d < 4; ++d) {
    EXPECT_EQ(before->days[d].return_mature, after->days[d].return_mature);
    EXPECT_EQ(before->days[d].return_complete, after->days[d].return_complete);
    EXPECT_EQ(before->days[d].observed_return_component, after->days[d].observed_return_component);
    EXPECT_EQ(before->days[d].turnover, after->days[d].turnover);
  }
}
