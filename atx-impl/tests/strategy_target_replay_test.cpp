#include <algorithm>
#include <array>
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
#include "../src/strategy_price_exposures.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"

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

// ---- construction options (T4) ----
constexpr const char* default_daily_header =
    "decision,session_ns,month,entry,endpoint,turnover,forced,discretionary,deployment,"
    "month_turnover,budget_excess,applied_fraction,gross,net,long_weight,short_weight,"
    "max_abs_weight,effective_names,held_names,return_mature,return_complete,"
    "observed_return_component,missing_long,missing_short,missing_gross,missing_names,"
    "guarded_names,modeled_trade_cost,modeled_borrow_cost,complete_gross_return,complete_net_return";
std::string first_line(const std::filesystem::path& path) {
  std::ifstream in(path); std::string line; std::getline(in, line); return line;
}
Json read_json(const std::filesystem::path& path) {
  std::ifstream in(path); Json j; in >> j; return j;
}
u64 bits(f64 x) { return std::bit_cast<u64>(x); }
struct Lcg {
  u64 state{};
  f64 uniform() { // [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * 0x1.0p-53;
  }
};
// The strategy_price_exposures_test noisy panel, bit for bit (common factor with
// per-name loadings, noise scales and dollar volumes on coprime strides), plus
// random member signals and consecutive daily sessions.
struct Role {
  usize d{}, n{};
  std::vector<f64> signal, close, raw, volume;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Role(usize dates, usize names, u64 seed)
      : d(dates), n(names), signal(dates * names), close(dates * names), raw(dates * names),
        volume(dates * names), member(dates * names, 1), present(dates * names, 1),
        sessions(dates), ids(names) {
    Lcg rng{seed};
    for (usize i = 0; i < n; ++i) close[i] = 20.0 + static_cast<f64>(i);
    for (usize t = 1; t < d; ++t) {
      const f64 common = 0.03 * (rng.uniform() - 0.5);
      for (usize i = 0; i < n; ++i) {
        const f64 loading = 0.4 + 0.15 * static_cast<f64>((i * 5) % n);
        const f64 idio = 0.004 + 0.003 * static_cast<f64>((i * 7) % n);
        close[t * n + i] =
            close[(t - 1) * n + i] * (1 + loading * common + idio * (rng.uniform() - 0.5));
      }
    }
    for (usize t = 0; t < d; ++t)
      for (usize i = 0; i < n; ++i) {
        raw[t * n + i] = close[t * n + i];
        volume[t * n + i] = 1e4 * static_cast<f64>(1 + (i * 11) % n) * (0.5 + rng.uniform());
      }
    Lcg draws{seed + 1000};
    for (auto& s : signal) s = draws.uniform() - 0.5;
    for (usize t = 0; t < d; ++t) sessions[t] = session(2020, 1, 1) + static_cast<i64>(t) * day_ns;
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
  }
  void nonmember(usize t, usize i) { member[t * n + i] = 0; signal[t * n + i] = missing; }
  st::TargetReplayInput input() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present, volume};
  }
  st::PriceExposureInput prices() const { return {d, n, close, raw, volume, present}; }
};
// Every name has a full-window beta (20 pairs) from decision 20 on; before that no
// name has exposures and the neutralization refuses with too few usable names.
st::TargetReplayConfig neutral_daily() {
  st::TargetReplayConfig c; c.cadence = 1; c.trade_fraction = 1;
  c.neutralize = st::TargetNeutralize::PriceRiskV1;
  c.price_risk.beta_window = 40; c.price_risk.vol_window = 20; c.price_risk.adv_window = 10;
  c.price_risk.min_return_pairs = 20; c.price_risk.min_names = 5;
  return c;
}
usize count_outcome(const st::TargetReplayResult& r, st::NeutralizeOutcome outcome) {
  usize count = 0;
  for (const auto& day : r.days) count += day.construction.neutralize == outcome ? 1U : 0U;
  return count;
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

TEST(StrategyTargetReplay, OptionalPinnedPriceRolePreservesMissingExposureAndRequiresExactBinding) {
  Directory dir; Fixture f(5, 2);
  for (usize d = 0; d < f.d; ++d) { f.signal[2 * d] = -1; f.signal[2 * d + 1] = 1; }
  f.close.assign(10, 100); f.raw = f.close; f.present.assign(10, 1);
  // Missing future endpoint is absent in both saved decision support and source
  // prices; it must not alter the already chosen first decision's short holding.
  f.member[4] = 0; f.signal[4] = missing;
  f.present[4] = 0; f.close[4] = missing; f.raw[4] = missing;
  auto cfg = artifact(dir.path, f);
  Json files;
  files["sessions.i64"] = write_payload(dir.path / "sessions.i64", f.sessions);
  files["ids.u64"] = write_payload(dir.path / "ids.u64", f.ids);
  files["close.f64"] = write_payload(dir.path / "close.f64", f.close);
  files["raw_close.f64"] = write_payload(dir.path / "raw_close.f64", f.raw);
  files["present.u8"] = write_payload(dir.path / "present.u8", f.present);
  files["member.u8"] = write_payload(dir.path / "member.u8", f.member);
  Json role{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"source_sha256", std::string(64, 'a')}, {"instrument_namespace", "spiderrock.securityID"},
      {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
      {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
      {"common_stock_verified", false}, {"historical_vintage_verified", false},
      {"dates", f.d}, {"instruments", f.n}, {"score_begin", 0}, {"score_end", f.d},
      {"files", std::move(files)}};
  cfg.role_path = (dir.path / "role.json").string(); cfg.role_sha256 = write_json(cfg.role_path, role);
  std::ifstream saved(cfg.combined_path); Json manifest; saved >> manifest; saved.close();
  manifest["role_manifest_sha256"] = cfg.role_sha256;
  cfg.combined_sha256 = write_json(cfg.combined_path, manifest);
  std::ostringstream progress; ASSERT_TRUE(st::run_target_replay(cfg, progress));
  std::ifstream report(dir.path / "out" / "summary.json"); Json summary; report >> summary;
  EXPECT_GT(summary.at("incomplete_return_days").get<usize>(), 0U);
  EXPECT_GT(summary.at("summed_missing_gross_exposure").get<f64>(), 0);
  EXPECT_TRUE(summary.at("net_sharpe").is_null());
  role["source_sha256"] = std::string(64, 'b');
  cfg.role_sha256 = write_json(cfg.role_path, role);
  cfg.output_directory = (dir.path / "wrong_role").string();
  EXPECT_FALSE(st::run_target_replay(cfg, progress));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "wrong_role"));
}

// T4 (b): at a rebalance decision the desired target is the tied-rank target
// neutralized against that decision's own exposures (computed from the role at d):
// ~0 intercept/beta/vol/log-ADV moments, gross preserved, the recorded
// amplification is entry/residual gross, and the replay carries that record.
TEST(StrategyTargetReplay, PriceRiskNeutralizedTargetHasZeroExposureAndRecordsAmplification) {
  const Role role(70, 12, 21);
  const auto cfg = neutral_daily();
  constexpr usize d = 60;
  const auto in = role.input();
  // Independent construction from the tied rank and the T3 primitives.
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> ranked(role.n);
  st::detail::desired_target(std::span<const f64>(role.signal).subspan(d * role.n, role.n),
                             std::span<const u8>(role.member).subspan(d * role.n, role.n), row,
                             ranked);
  constexpr usize cols = st::kPriceExposureCount;
  std::vector<f64> exposures(role.n * cols);
  std::vector<u8> ok(role.n);
  st::PriceExposureScratch exposure_scratch;
  ASSERT_TRUE(st::compute_price_exposures(role.prices(), cfg.price_risk, d, exposure_scratch,
                                          exposures, ok));
  for (const u8 flag : ok) ASSERT_EQ(flag, 1);
  auto expected = ranked;
  const std::vector<u8> members(role.n, 1);
  st::NeutralizeScratch neutralize_scratch;
  st::NeutralizeStats stats;
  ASSERT_TRUE(st::neutralize_target(expected, members, exposures, ok, cfg.price_risk,
                                    neutralize_scratch, stats));
  // The shared construction step both replays call.
  std::vector<f64> desired(role.n);
  st::PriceRiskScratch scratch;
  st::ConstructionDay record;
  const auto rebalance = st::detail::form_desired(in, cfg, d, row, desired, scratch, record);
  ASSERT_TRUE(rebalance);
  EXPECT_TRUE(*rebalance);
  for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(desired[i]), bits(expected[i])) << i;
  EXPECT_EQ(record.neutralize, st::NeutralizeOutcome::Applied);
  EXPECT_EQ(record.neutralize_used, 12U);
  EXPECT_EQ(record.neutralize_excluded, 0U);
  EXPECT_EQ(record.neutralize_excluded_share, 0);
  EXPECT_EQ(bits(record.neutralize_amplification), bits(stats.gross / stats.residual_gross));
  EXPECT_GT(record.neutralize_amplification, 0);
  EXPECT_LE(record.neutralize_amplification, cfg.neutralize_max_amplification);
  // ~0 exposure on the clipped z design rebuilt from the T3 contract (mean and
  // sample SD over the used rows, clip +-clip_z).
  f64 gross = 0;
  std::array<f64, cols + 1> moment{};
  for (usize i = 0; i < role.n; ++i) { gross += std::abs(desired[i]); moment[0] += desired[i]; }
  for (usize k = 0; k < cols; ++k) {
    f64 sum = 0, squares = 0;
    for (usize i = 0; i < role.n; ++i) sum += exposures[i * cols + k];
    const f64 mean = sum / static_cast<f64>(role.n);
    for (usize i = 0; i < role.n; ++i)
      squares += (exposures[i * cols + k] - mean) * (exposures[i * cols + k] - mean);
    const f64 sd = std::sqrt(squares / static_cast<f64>(role.n - 1));
    for (usize i = 0; i < role.n; ++i)
      moment[k + 1] += desired[i] * std::clamp((exposures[i * cols + k] - mean) / sd,
                                               -cfg.price_risk.clip_z, cfg.price_risk.clip_z);
  }
  EXPECT_NEAR(gross, 1.0, 1e-12);
  for (usize k = 0; k < moment.size(); ++k) EXPECT_LE(std::abs(moment[k]), 1e-12) << k;
  // The replay records the same decision and trades to it in full (fraction 1).
  auto replay = st::replay_targets(in, cfg);
  ASSERT_TRUE(replay) << replay.error().to_string();
  const auto& day = replay->days[d];
  EXPECT_TRUE(day.construction.rebalance);
  EXPECT_EQ(day.construction.neutralize, st::NeutralizeOutcome::Applied);
  EXPECT_EQ(day.construction.neutralize_used, 12U);
  EXPECT_EQ(bits(day.construction.neutralize_amplification),
            bits(record.neutralize_amplification));
  EXPECT_NEAR(day.gross, 1.0, 1e-12);
  EXPECT_NEAR(day.net, 0.0, 1e-12);
  // Before a full beta window no name has exposures: too few names, no trade.
  for (usize t = 0; t < 20; ++t) {
    EXPECT_EQ(replay->days[t].construction.neutralize,
              st::NeutralizeOutcome::SkippedTooFewNames) << t;
    EXPECT_EQ(replay->days[t].construction.neutralize_used, 0U) << t;
    EXPECT_EQ(replay->days[t].gross, 0) << t;
  }
  EXPECT_EQ(count_outcome(*replay, st::NeutralizeOutcome::SkippedTooFewNames), 20U);
}

// T4 (c): a refused neutralization (here too few usable names while two held names
// sit out of membership) skips the rebalance: every member keeps its weight, the
// leavers are still exited, and the skip is counted by reason. An amplification
// cap below every entry/residual ratio skips every otherwise-applied rebalance.
TEST(StrategyTargetReplay, NeutralizeSkipKeepsWeightsAppliesForcedExitsAndCounts) {
  Role role(70, 12, 21);
  for (usize t = 40; t < 45; ++t) { role.nonmember(t, 0); role.nonmember(t, 1); }
  auto cfg = neutral_daily();
  cfg.price_risk.min_names = 11; // 10 members at decisions 40-44
  auto r = st::replay_targets(role.input(), cfg);
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_EQ(count_outcome(*r, st::NeutralizeOutcome::SkippedTooFewNames), 25U);
  for (usize t = 0; t < 20; ++t) {
    EXPECT_FALSE(r->days[t].construction.rebalance) << t;
    EXPECT_EQ(r->days[t].turnover, 0) << t;
    EXPECT_EQ(r->days[t].gross, 0) << t;
  }
  ASSERT_TRUE(r->days[39].construction.rebalance); // neutralized and traded
  const auto& leave = r->days[40];
  EXPECT_EQ(leave.construction.neutralize, st::NeutralizeOutcome::SkippedTooFewNames);
  EXPECT_EQ(leave.construction.neutralize_used, 10U);
  EXPECT_FALSE(leave.construction.rebalance);
  EXPECT_GT(leave.forced_turnover, 0);        // the leavers are exited anyway
  EXPECT_EQ(leave.discretionary_turnover, 0); // every member keeps its weight
  EXPECT_EQ(leave.applied_fraction, 0);
  EXPECT_NEAR(leave.gross, r->days[39].gross - leave.forced_turnover, 1e-12);
  for (usize t = 41; t < 45; ++t) {
    EXPECT_EQ(r->days[t].construction.neutralize, st::NeutralizeOutcome::SkippedTooFewNames);
    EXPECT_EQ(r->days[t].turnover, 0) << t;
    EXPECT_EQ(bits(r->days[t].gross), bits(leave.gross)) << t;
  }
  const Role plain(70, 12, 21);
  auto capped = neutral_daily();
  capped.neutralize_max_amplification = 1e-6;
  auto none = st::replay_targets(plain.input(), capped);
  ASSERT_TRUE(none) << none.error().to_string();
  EXPECT_EQ(none->total_turnover, 0);
  EXPECT_EQ(count_outcome(*none, st::NeutralizeOutcome::Applied), 0U);
  EXPECT_GT(count_outcome(*none, st::NeutralizeOutcome::SkippedAmplification), 0U);
  for (const auto& day : none->days) {
    EXPECT_FALSE(day.construction.rebalance);
    if (day.construction.neutralize == st::NeutralizeOutcome::SkippedAmplification) {
      EXPECT_GT(day.construction.neutralize_amplification, capped.neutralize_max_amplification);
    }
  }
}

// T4 (d): the no-trade band keeps members whose move is within band_multiple/N_d
// and moves the others by the rule's fraction; monthly-budget-v2 leaves banded
// names out of its distance. Hand-computed on four names (band 0.6/4 = .15).
TEST(StrategyTargetReplay, NoTradeBandKeepsSmallMovesAndLeavesThemOutOfTheBudgetDistance) {
  Fixture f(3, 4);
  const f64 a[] = {1, 2, 3, 4}, e[] = {1, 2, 4, 3};
  for (usize i = 0; i < 4; ++i) {
    f.signal[i] = a[i]; f.signal[4 + i] = e[i]; f.signal[8 + i] = e[i];
  }
  st::TargetReplayConfig base;
  base.cadence = 1; base.trade_fraction = 1; base.band_multiple = 0.6;
  auto r = st::replay_targets(f.input(), base); ASSERT_TRUE(r);
  // desired [-.375 -.125 .125 .375] from flat: the two .125 moves stay banded at 0.
  EXPECT_EQ(r->days[0].construction.banded_names, 2U);
  EXPECT_EQ(r->days[0].turnover, 0.75); EXPECT_EQ(r->days[0].gross, 0.75);
  EXPECT_EQ(r->days[0].net, 0);
  // desired [-.375 -.125 .375 .125] from [-.375 0 0 .375]: gaps 0 and .125 are
  // banded; .375 and .25 move in full.
  EXPECT_TRUE(r->days[1].construction.rebalance);
  EXPECT_EQ(r->days[1].construction.banded_names, 2U);
  EXPECT_EQ(r->days[1].turnover, 0.625); EXPECT_EQ(r->days[1].gross, 0.875);
  EXPECT_EQ(r->days[1].net, 0.125);
  auto v2 = base; v2.rule = st::TargetReplayRule::MonthlyTargetBudgetV2; v2.monthly_budget = .5;
  auto budget = st::replay_targets(f.input(), v2); ASSERT_TRUE(budget);
  // distance .75 (banded names excluded): fraction .5/.75, the two tails move 2/3.
  EXPECT_EQ(budget->days[0].construction.banded_names, 2U);
  EXPECT_NEAR(budget->days[0].applied_fraction, 2.0 / 3.0, 1e-15);
  EXPECT_NEAR(budget->days[0].turnover, .5, 1e-15);
  EXPECT_NEAR(budget->days[0].gross, .5, 1e-15);
  auto unbanded = v2; unbanded.band_multiple = 0;
  auto plain = st::replay_targets(f.input(), unbanded); ASSERT_TRUE(plain);
  EXPECT_EQ(plain->days[0].applied_fraction, .5); // distance 1 over all four names
  EXPECT_EQ(plain->days[0].construction.banded_names, 0U);
  st::TargetReplayConfig bad; bad.band_multiple = -1;
  EXPECT_FALSE(st::replay_targets(f.input(), bad));
}

// T4 (a)/CLI: the default run keeps the pre-T4 recipe keys, rule and CSV columns
// byte-for-byte; a non-default option suffixes the rule id and adds its recipe
// keys, CSV columns and summary diagnostics; price-risk neutralization needs the
// role, and an unknown --neutralize spelling is a usage error.
TEST(StrategyTargetReplay, ConstructionOptionsAreRecordedOnlyWhenNonDefault) {
  Fixture f(6, 3);
  for (usize d = 0; d < f.d; ++d) for (usize i = 0; i < f.n; ++i)
    f.signal[d * f.n + i] = static_cast<f64>((i + d) % 3);
  std::ostringstream progress;
  {
    Directory dir; auto cfg = artifact(dir.path, f);
    ASSERT_TRUE(st::run_target_replay(cfg, progress));
    const auto recipe = read_json(dir.path / "out" / "recipe.json");
    EXPECT_EQ(recipe.at("rule"), "baseline-target-v1");
    for (const auto* key : {"neutralize", "band_multiple", "price_risk", "neutralize_guard",
                            "band", "desired_target_postprocess"})
      EXPECT_FALSE(recipe.contains(key)) << key;
    EXPECT_EQ(first_line(dir.path / "out" / "daily.csv"), default_daily_header);
    EXPECT_FALSE(read_json(dir.path / "out" / "summary.json").contains("construction"));
  }
  {
    Directory dir; auto cfg = artifact(dir.path, f); cfg.target.band_multiple = 0.5;
    ASSERT_TRUE(st::run_target_replay(cfg, progress));
    const auto recipe = read_json(dir.path / "out" / "recipe.json");
    EXPECT_EQ(recipe.at("rule"), "baseline-target-v1+band-0.5");
    EXPECT_EQ(recipe.at("band_multiple"), 0.5);
    EXPECT_FALSE(recipe.contains("neutralize"));
    EXPECT_EQ(first_line(dir.path / "out" / "daily.csv"),
              std::string(default_daily_header) +
                  ",rebalance,neutralize,neutralize_used,neutralize_excluded,"
                  "neutralize_excluded_share,neutralize_amplification,banded_names");
    const auto construction = read_json(dir.path / "out" / "summary.json").at("construction");
    EXPECT_EQ(construction.at("rule_id"), "baseline-target-v1+band-0.5");
    EXPECT_EQ(construction.at("decisions"), 6);
    EXPECT_EQ(construction.at("neutralize_attempted_decisions"), 0);
    EXPECT_EQ(construction.at("neutralize_skipped_decisions"), 0);
  }
  {
    Directory dir; auto cfg = artifact(dir.path, f);
    cfg.target.neutralize = st::TargetNeutralize::PriceRiskV1;
    const auto refused = st::run_target_replay(cfg, progress);
    ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "out"));
  }
  std::ostringstream out, err;
  std::vector<std::string> args{"targets", "--neutralize", "bogus"};
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  EXPECT_EQ(st::dispatch_target_replay(static_cast<int>(argv.size()), argv.data(), out, err), 2);
}
