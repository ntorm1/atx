// spo-v3 (platform v8 R-6): target tracking toward the aim through the spo engine and the v7
// hook -- the aim itself without costs or limits (a direct Engine::plan), the gross sanity
// bound slack on the fixture and its breach voiding the run, the tracking error, trade-limit
// share and aim correlation with their per-book report, gamma on the first scored decision
// under a warm start (review A-2), the CLI refusals of the registered
// constants, and (v8 E-26) the aim shaped by --hold-band / --adv-hold-q exactly as the
// aim-partial-v5 path shapes desired. The spo-v1 / spo-v2 digest guard is
// strategy_spo_v3_pin_test.cpp.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_spo_v3.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace co = atx::core;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace v7 = atx::impl::strategy::v7;
using Json = nlohmann::json;

std::shared_ptr<const sp::RiskStore> clean_model(const Directory& dir, const Role& role,
                                                 u64 seed) {
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", seed);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  EXPECT_TRUE(store) << store.error().to_string();
  return store ? std::make_shared<const sp::RiskStore>(std::move(*store)) : nullptr;
}

// The observed book's rebalance decisions (a NavHoldingsSink): per session index, every
// reported name's shared desired target (NaN: not reported), every name's held weight (what
// its DECIDE read; 0: not reported, as the stream declares) and, given the spo engine, the aim
// its tracker received at that decision (Engine::last_aim, read right after the book's DECIDE).
class Recorder final : public st::NavHoldingsSink {
public:
  explicit Recorder(usize names) : n_(names) {}
  const sp::Engine* engine{};
  std::map<usize, std::vector<f64>> desired, held, aim;
  [[nodiscard]] co::Status session(const st::NavReplayDay& day,
                                   std::span<const st::NavHolding> names) override {
    if (!day.decision || !day.rebalance) return co::Ok();
    auto& row = desired[day.session_index];
    row.assign(n_, std::numeric_limits<f64>::quiet_NaN());
    for (const auto& h : names) row[h.index] = h.desired;
    auto& book = held[day.session_index];
    book.assign(n_, 0.0);
    for (const auto& h : names) book[h.index] = h.held_weight;
    if (engine != nullptr) {
      const auto received = engine->last_aim();
      aim[day.session_index].assign(received.begin(), received.end());
    }
    return co::Ok();
  }

private:
  usize n_{};
};

// A replay of the fixture (nav_config: S1, S2, S3, cadence 1, theta .25, L 1.2, NAV 1e8, or
// `cfg`) under spo-v3 through the v7 hook, with what the extension would publish; with a
// recorder, its primary book alone (replay_nav_scenarios, bit-identical) observed. The role's
// first scored row is `decision_begin` (0: the fixture's own).
struct Replay {
  std::vector<sp::TrackingRow> rows;
  usize legacy_rows{}; // spo-v1/v2 rows (none under spo-v3)
  sp::Calibration calibration;
  f64 horizon{}, bound{};
  Json parameters, summary, tripwire, calibration_json;
  std::string csv, declaration;
  bool captured{};
  st::NavReplayResult result; // the (first) book's
};
Replay replay_v3(const Role& role, std::shared_ptr<const sp::RiskStore> risk,
                 const sp::SpoParams& params, const st::NavReplayConfig& cfg = nav_config(),
                 Recorder* recorder = nullptr, usize decision_begin = 0) {
  auto input = role.nav();
  input.target.decision_begin = decision_begin;
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_params = params;
  o.spo_risk = std::move(risk);
  const v7::ScopedNavExtension extension(o);
  Replay out;
  const auto* engine = extension.spo_engine();
  if (engine == nullptr) {
    ADD_FAILURE() << "no spo engine";
    return out;
  }
  if (recorder == nullptr) {
    auto result = st::replay_nav(input, cfg);
    EXPECT_TRUE(result) << result.error().to_string();
    if (result) out.result = std::move(*result);
  } else {
    recorder->engine = engine;
    const std::array<st::NavScenario, 1> primary{cfg.scenario};
    auto results = st::replay_nav_scenarios(input, cfg, primary, *recorder, 0);
    EXPECT_TRUE(results) << results.error().to_string();
    if (results && results->size() == 1) out.result = std::move(results->front());
  }
  const auto rows = engine->tracking_rows();
  out.rows.assign(rows.begin(), rows.end());
  out.legacy_rows = engine->rows().size();
  out.calibration = engine->calibration();
  out.horizon = engine->horizon();
  out.bound = engine->gross_budget();
  out.parameters = engine->rule_parameters_json();
  out.summary = engine->rows_summary_json();
  out.tripwire = engine->rows_tripwire_json();
  out.csv = engine->rows_csv();
  out.declaration = engine->rule_declaration();
  out.calibration_json = engine->rule_calibration_json();
  out.captured = static_cast<bool>(v7::capture({}, {}, {})); // the seam before publication
  return out;
}

// nav_config without aim-partial-v5's dust band (dust 0, hence exit rate 1), which the desired
// target never reads: the plain aim-partial-v5 replay then plans every member whose desired
// target is nonzero, so its holdings carry every member's desired at the first decision too
// (after it, a declared hold band reports every ranked name).
st::NavReplayConfig undusted_config() {
  auto cfg = nav_config();
  cfg.target.dust_multiple = 0.0;
  cfg.target.exit_rate = 1.0;
  return cfg;
}
u64 bits(f64 x) { return std::bit_cast<u64>(x); }
// Pearson correlation as the engine computes it (NaN below 3 pairs or without dispersion).
f64 pearson(std::span<const f64> a, std::span<const f64> b) {
  const usize n = std::min(a.size(), b.size());
  if (n < 3) return std::numeric_limits<f64>::quiet_NaN();
  f64 ma = 0, mb = 0;
  for (usize i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
  ma /= static_cast<f64>(n); mb /= static_cast<f64>(n);
  f64 ab = 0, aa = 0, bb = 0;
  for (usize i = 0; i < n; ++i) {
    const f64 x = a[i] - ma, y = b[i] - mb;
    ab += x * y; aa += x * x; bb += y * y;
  }
  return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : std::numeric_limits<f64>::quiet_NaN();
}

// No trading cost, no borrow and no binding limit (ADV 1e15, beta band +-1, every name a
// member with a risk row, from flat, no fixed position): the tracking optimum is the aim
// L x desired itself, reached to 1e-8 with the registered solver constants.
TEST(SpoV3, ZeroCostNoLimitsReturnsAimTo1e8) {
  const Directory dir;
  const Role role(20, 12, 83);
  const auto risk = clean_model(dir, role, 13);
  ASSERT_NE(risk, nullptr);
  auto params = sp::v3_params();
  params.beta_max = 1.0;
  sp::Engine engine(params, risk);
  auto cfg = nav_config();
  cfg.scenario.financing.flat_short_bps = 0.0; // no borrow
  auto law = cfg.scenario;                      // the primary S2 law without its costs
  law.half_spread_bps = 0.0; law.commission_bps = 0.0; law.impact_y = 0.0;
  const auto x = role.target();
  const usize n = role.n, d = 2; // every name a member (name n - 1 leaves on sessions 6..9)
  std::vector<f64> desired(n);
  f64 mean = 0;
  for (usize i = 0; i < n; ++i) {
    desired[i] = role.signal[d * n + i];
    mean += desired[i];
  }
  for (f64& v : desired) v -= mean / static_cast<f64>(n);
  // Gross 1, as every desired target (detail::desired_target), so L x desired is inside 2 x L.
  f64 gross = 0;
  for (const f64 v : desired) gross += std::abs(v);
  for (f64& v : desired) v /= gross;
  const st::cost_v2::DecisionLiquidity liquidity{std::vector<f64>(n, 1e15),
                                                 std::vector<f64>(n, 0.02)};
  const sp::BookDecision in{x, cfg, law, d, 1e8, desired, {}, {}, liquidity, "S2"};
  std::vector<f64> planned(n, 0.0);
  st::TargetReplayDay day;
  const auto status = engine.plan(in, planned, day);
  ASSERT_TRUE(status) << status.error().to_string();
  f64 worst = 0;
  for (usize i = 0; i < n; ++i)
    worst = std::max(worst, std::abs(planned[i] - cfg.target.aim_leverage * desired[i]));
  EXPECT_LT(worst, 1e-8);
  EXPECT_TRUE(engine.rows().empty()); // spo-v3 writes its own rows only
  const auto rows = engine.tracking_rows();
  ASSERT_EQ(rows.size(), 1U);
  const auto& r = rows.front();
  EXPECT_TRUE(r.converged) << r.iterations;
  EXPECT_TRUE(r.limits_met) << r.limit_violation;
  EXPECT_EQ(r.optimized, n);
  EXPECT_EQ(r.at_trade_limit, 0U);
  EXPECT_EQ(r.trade_cost, 0.0);
  EXPECT_EQ(r.borrow, 0.0);
  EXPECT_LT(r.tracking_error, 1e-6);
  EXPECT_NEAR(r.aim_correlation, 1.0, 1e-9);
  EXPECT_FALSE(r.gross_bound_breached);
  const auto& c = engine.calibration();
  ASSERT_TRUE(c.done);
  EXPECT_GT(c.aim_vol, 0.0);
  EXPECT_DOUBLE_EQ(c.gamma, sp::v3_sharpe_prior / c.aim_vol);
  EXPECT_DOUBLE_EQ(r.gamma, c.gamma);
  EXPECT_DOUBLE_EQ(r.tracking_error_current, c.aim_vol); // from flat: the aim's own vol
  EXPECT_EQ(engine.horizon(), sp::v3_horizon);            // fixed, not 1 / theta (4 here)
  EXPECT_DOUBLE_EQ(engine.gross_budget(), sp::v3_gross_bound_multiple * cfg.target.aim_leverage);
}

// The gross cap is the sanity bound 2 x L, never a solver constraint: on the fixture's replay
// (every book from flat, the S2 law's costs and the 1% ADV trade limit) the planned gross stays
// well inside it on every scored decision and the tripwire is clear. A breach (forced on the
// recorded rows) voids the run with the void on and is recorded, not voiding, with it off.
TEST(SpoV3, GrossCapIsSlackOnFixture) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto params = sp::v3_params();
  const Replay run = replay_v3(role, risk, params);
  ASSERT_FALSE(run.rows.empty());
  const f64 bound = sp::v3_gross_bound_multiple * nav_config().target.aim_leverage;
  EXPECT_DOUBLE_EQ(run.bound, bound);
  EXPECT_EQ(run.parameters.at("gross_bound"), bound);
  f64 largest = 0;
  for (const auto& r : run.rows) {
    EXPECT_FALSE(r.gross_bound_breached) << r.session << ' ' << r.book;
    EXPECT_LT(r.gross, bound) << r.session << ' ' << r.book;
    largest = std::max(largest, r.gross);
  }
  EXPECT_GT(largest, 0.0);
  EXPECT_LT(largest, 0.75 * bound) << "the bound is slack, not near binding";
  EXPECT_TRUE(run.captured);
  EXPECT_EQ(run.tripwire.at("status"), "clear");
  EXPECT_EQ(run.tripwire.at("gross_bound_breaches"), 0U);
  EXPECT_EQ(run.tripwire.at("capped_specific_decisions"), 0U);
  auto breached = run.rows;
  breached[breached.size() / 2].gross_bound_breached = true;
  const auto status = sp::tracking_tripwire(params, breached);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), co::ErrorCode::Unavailable);
  EXPECT_NE(status.error().message().find("VOID"), std::string::npos) << status.error().message();
  const auto record = sp::tracking_tripwire_json(params, breached);
  EXPECT_EQ(record.at("status"), "void");
  EXPECT_EQ(record.at("gross_bound_breaches"), 1U);
  auto off = params;
  off.void_on_capped = false;
  EXPECT_TRUE(sp::tracking_tripwire(off, breached));
  EXPECT_EQ(sp::tracking_tripwire_json(off, breached).at("status").get<std::string>().rfind(
                "tripped", 0),
            0U);
}

// The diagnostics: the annualised tracking error of the plan and of the current book to the
// aim, the share of optimized names at their 1% ADV trade limit and the aim correlation, one
// CSV row per scored (decision, book), and the per-book report of their mean / max (min for
// the correlation) in the summary and the tripwire record, where the cell's mechanical
// criterion is read.
TEST(SpoV3, ReportsTrackingErrorAndShareAtTradeLimit) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const Replay run = replay_v3(role, risk, sp::v3_params());
  ASSERT_FALSE(run.rows.empty());
  EXPECT_EQ(run.legacy_rows, 0U);
  EXPECT_EQ(run.horizon, sp::v3_horizon);
  const auto& c = run.calibration;
  ASSERT_TRUE(c.done);
  EXPECT_GT(c.aim_vol, 0.0);
  EXPECT_DOUBLE_EQ(c.gamma, sp::v3_sharpe_prior / c.aim_vol);
  // The first scored row is the calibration decision, planned from flat: its current
  // distance to the aim is the aim's own vol.
  EXPECT_DOUBLE_EQ(run.rows.front().tracking_error_current, c.aim_vol);
  // A book's first trading decision starts from flat (every earlier plan was flat): trading
  // closes part of the gap, and the 1% ADV limit binds on some names.
  const std::string first_book = run.rows.front().book;
  const auto trading = std::find_if(run.rows.begin(), run.rows.end(), [&](const auto& r) {
    return r.book == first_book && r.turnover > 0;
  });
  ASSERT_NE(trading, run.rows.end());
  EXPECT_TRUE(trading->converged) << trading->iterations;
  EXPECT_TRUE(trading->limits_met) << trading->limit_violation;
  EXPECT_LT(trading->tracking_error, trading->tracking_error_current);
  EXPECT_GT(trading->at_trade_limit, 0U);
  EXPECT_GT(trading->trade_limit_share, 0.0);
  std::map<std::string, std::vector<const sp::TrackingRow*>> by_book;
  for (const auto& r : run.rows) {
    // The fixture redraws every style exposure daily, so a book's beta can jump by more than
    // one session's trade limits can repair: such a solve is reported (limits unmet, counted
    // in the report below), never refused. Where the limits were met they hold.
    if (r.limits_met) {
      EXPECT_LT(std::abs(r.net), 1e-9) << r.session << ' ' << r.book;
      EXPECT_LE(r.abs_beta, sp::v3_beta_max + 1e-9) << r.session << ' ' << r.book;
    }
    EXPECT_TRUE(std::isfinite(r.tracking_error) && r.tracking_error >= 0.0) << r.session;
    EXPECT_TRUE(std::isfinite(r.tracking_error_current)) << r.session;
    EXPECT_TRUE(std::isfinite(r.tracking_error_shadow)) << r.session;
    EXPECT_GE(r.trade_cost, 0.0) << r.session;
    EXPECT_DOUBLE_EQ(r.gamma, c.gamma) << r.session;
    EXPECT_GT(r.optimized, 0U) << r.session;
    if (r.optimized > 0) {
      EXPECT_DOUBLE_EQ(r.trade_limit_share,
                       static_cast<f64>(r.at_trade_limit) / static_cast<f64>(r.optimized))
          << r.session;
    }
    if (std::isfinite(r.aim_correlation)) { // NaN only for a flat plan (no dispersion)
      EXPECT_GE(r.aim_correlation, -1.0 - 1e-12) << r.session;
      EXPECT_LE(r.aim_correlation, 1.0 + 1e-12) << r.session;
    } else {
      EXPECT_EQ(r.turnover, 0.0) << r.session;
    }
    by_book[r.book].push_back(&r);
  }
  // One CSV row per scored (decision, book); 42 columns (review A-4 added
  // aim_correlation_traded after aim_correlation).
  EXPECT_EQ(static_cast<usize>(std::count(run.csv.begin(), run.csv.end(), '\n')),
            run.rows.size() + 1);
  const std::string header = run.csv.substr(0, run.csv.find('\n'));
  EXPECT_EQ(std::count(header.begin(), header.end(), ','), 41);
  for (const char* column : {",tracking_error,", ",tracking_error_current,", ",aim_correlation,",
                             ",aim_correlation_traded,", ",at_trade_limit,",
                             ",trade_limit_share,", ",gross_bound_breached,",
                             ",tracking_error_shadow,", ",aim_correlation_shadow"})
    EXPECT_NE(header.find(column), std::string::npos) << column;
  // The per-book report: summary and tripwire record agree with the rows.
  ASSERT_EQ(run.summary.size(), by_book.size());
  const auto& report = run.tripwire.at("report_only");
  ASSERT_EQ(report.size(), by_book.size());
  for (const auto& [book, list] : by_book) {
    f64 te_sum = 0, te_max = 0, share_max = 0, corr_sum = 0, corr_min = 2.0;
    usize corr_n = 0, unconverged = 0, unmet = 0;
    for (const auto* r : list) {
      unconverged += r->converged ? 0U : 1U;
      unmet += r->limits_met ? 0U : 1U;
      te_sum += r->tracking_error;
      te_max = std::max(te_max, r->tracking_error);
      share_max = std::max(share_max, r->trade_limit_share);
      if (!std::isfinite(r->aim_correlation)) continue;
      corr_sum += r->aim_correlation;
      corr_min = std::min(corr_min, r->aim_correlation);
      ++corr_n;
    }
    ASSERT_GT(corr_n, 0U) << book;
    const auto& entry = run.summary.at(book);
    EXPECT_EQ(entry.at("decisions"), list.size()) << book;
    EXPECT_NEAR(entry.at("tracking_error").at("mean").get<f64>(),
                te_sum / static_cast<f64>(list.size()), 1e-12 * (1.0 + te_max))
        << book;
    EXPECT_EQ(entry.at("tracking_error").at("max").get<f64>(), te_max) << book;
    EXPECT_EQ(entry.at("trade_limit_share").at("max").get<f64>(), share_max) << book;
    EXPECT_EQ(entry.at("aim_correlation").at("min").get<f64>(), corr_min) << book;
    EXPECT_NEAR(entry.at("aim_correlation").at("mean").get<f64>(),
                corr_sum / static_cast<f64>(corr_n), 1e-12)
        << book;
    EXPECT_EQ(entry.at("aim_correlation").at("n"), corr_n) << book;
    EXPECT_EQ(entry.at("unconverged"), unconverged) << book;
    EXPECT_EQ(entry.at("limits_unmet"), unmet) << book;
    EXPECT_TRUE(entry.at("shadow").at("mean_tracking_error").is_number()) << book;
    EXPECT_EQ(report.at(book).at("aim_correlation"), entry.at("aim_correlation")) << book;
    EXPECT_EQ(report.at(book).at("tracking_error"), entry.at("tracking_error")) << book;
    EXPECT_EQ(report.at(book).at("trade_limit_share"), entry.at("trade_limit_share")) << book;
  }
  EXPECT_EQ(run.tripwire.at("status"), "clear");
}

// Review A-2 (v8 D-0 warm start): the warm-up decisions move the book as aim-partial-v5 and read
// no risk row, and gamma is calibrated on the first scored decision. On a risk store with no
// forecast before decision_begin (every earlier row unforecast, where a read refuses) the warm
// start runs: its calibration is the flat start's at decision_begin bit for bit (the same aim
// and slice), its rows are exactly the flat start's scored decisions, the book it scores from
// is the plain aim-partial-v5 warm start's (not flat), and its calibration block names the
// warm-up (the flat start's has no such key).
TEST(SpoV3, WarmStartCalibratesOnTheFirstScoredDecision) {
  constexpr usize begin = 12, warm = 8;
  const Role role(40, 12, 53);
  std::vector<u8> forecast(role.d, u8{1});
  for (usize d = 0; d < begin; ++d) forecast[d] = 0;
  const Directory dir;
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 3);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  const auto risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const auto params = sp::v3_params();
  const auto flat_cfg = nav_config();
  auto warm_cfg = flat_cfg;
  warm_cfg.warm_start_sessions = warm;
  const Replay flat = replay_v3(role, risk, params, flat_cfg, nullptr, begin);
  const Replay warmed = replay_v3(role, risk, params, warm_cfg, nullptr, begin);
  ASSERT_FALSE(flat.rows.empty());
  ASSERT_FALSE(warmed.rows.empty());
  // gamma of the first scored decision, not of row begin - K (unforecast: it would refuse).
  const auto& c = warmed.calibration;
  ASSERT_TRUE(c.done);
  EXPECT_TRUE(c.warm_up);
  EXPECT_FALSE(flat.calibration.warm_up);
  EXPECT_EQ(c.session, role.sessions[begin]);
  EXPECT_EQ(flat.calibration.session, role.sessions[begin]);
  EXPECT_EQ(bits(c.gamma), bits(flat.calibration.gamma));
  EXPECT_EQ(bits(c.aim_vol), bits(flat.calibration.aim_vol));
  EXPECT_EQ(c.names, flat.calibration.names);
  // Rows: the scored decisions only, the flat start's.
  ASSERT_EQ(warmed.rows.size(), flat.rows.size());
  for (usize k = 0; k < warmed.rows.size(); ++k) {
    EXPECT_EQ(warmed.rows[k].session, flat.rows[k].session) << k;
    EXPECT_EQ(warmed.rows[k].book, flat.rows[k].book) << k;
    EXPECT_GE(warmed.rows[k].session, role.sessions[begin]) << k;
    EXPECT_EQ(bits(warmed.rows[k].gamma), bits(c.gamma)) << k;
  }
  EXPECT_EQ(warmed.rows.front().session, role.sessions[begin]);
  // The warm-up built the book the scored window starts from (the flat start's is empty).
  ASSERT_FALSE(warmed.result.days.empty());
  ASSERT_FALSE(flat.result.days.empty());
  EXPECT_EQ(warmed.result.days.front().session, role.sessions[begin]);
  EXPECT_GT(warmed.result.days.front().pretrade_gross_dollars, 0.0);
  EXPECT_EQ(flat.result.days.front().pretrade_gross_dollars, 0.0);
  EXPECT_NE(bits(warmed.rows.front().tracking_error_current),
            bits(flat.rows.front().tracking_error_current));
  // That book is the plain aim-partial-v5 warm start's (no extension), bit for bit: row
  // score_begin's book entering it, its EXECUTE of the last warm-up orders and its gross.
  auto input = role.nav();
  input.target.decision_begin = begin;
  const auto plain = st::replay_nav(input, warm_cfg);
  ASSERT_TRUE(plain) << plain.error().to_string();
  ASSERT_FALSE(plain->days.empty());
  const auto& a = plain->days.front();
  const auto& b = warmed.result.days.front();
  EXPECT_EQ(a.session_index, b.session_index);
  EXPECT_EQ(bits(a.pretrade_gross_dollars), bits(b.pretrade_gross_dollars));
  EXPECT_EQ(bits(a.traded_dollars), bits(b.traded_dollars));
  EXPECT_EQ(bits(a.gross_leverage), bits(b.gross_leverage));
  // The calibration block: the warm-up is named only when there was one.
  ASSERT_TRUE(warmed.calibration_json.contains("warm_up"));
  EXPECT_EQ(warmed.calibration_json.at("warm_up").get<std::string>(),
            std::string(sp::warm_up_calibration_text));
  EXPECT_FALSE(flat.calibration_json.contains("warm_up"));
  EXPECT_EQ(warmed.calibration_json.at("session"), role.sessions[begin]);
  EXPECT_TRUE(warmed.captured);
}

// Review A-4: Ruling E-14's criterion reads the traded book, not the plan. Each row's
// aim_correlation_traded is the correlation of the holdings its DECIDE read (the observed
// book's held weights, which the holdings stream reports as the plan's current weights bit for
// bit) with the aim the tracker received, over every name either holds: recomputed here from
// the stream. From flat (the first decision) it is NaN; on later decisions it differs from the
// plan's aim_correlation (fills trail the plan under the 1% ADV trade limit, and the book
// drifts). The report carries both, and its criterion is the traded mean against .9.
TEST(SpoV3, CriterionReadsTheTradedBook) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  Recorder recorder(role.n);
  const Replay run = replay_v3(role, risk, sp::v3_params(), nav_config(), &recorder);
  ASSERT_FALSE(run.rows.empty());
  std::map<i64, usize> row_of;
  for (usize t = 0; t < role.d; ++t) row_of[role.sessions[t]] = t;
  usize compared = 0, differ = 0;
  f64 sum = 0, lowest = 2.0;
  for (const auto& r : run.rows) {
    const usize t = row_of.at(r.session);
    ASSERT_EQ(recorder.held.count(t), 1U) << t;
    ASSERT_EQ(recorder.aim.count(t), 1U) << t;
    const auto& held = recorder.held.at(t);
    const auto& aim = recorder.aim.at(t);
    ASSERT_EQ(aim.size(), role.n) << t;
    std::vector<f64> traded, aimed;
    for (usize i = 0; i < role.n; ++i) {
      if (aim[i] == 0 && held[i] == 0) continue;
      traded.push_back(held[i]);
      aimed.push_back(aim[i]);
    }
    const f64 expected = pearson(traded, aimed);
    if (std::isnan(expected)) {
      EXPECT_TRUE(std::isnan(r.aim_correlation_traded)) << t;
      continue;
    }
    EXPECT_NEAR(r.aim_correlation_traded, expected, 1e-12) << t;
    ++compared;
    sum += r.aim_correlation_traded;
    lowest = std::min(lowest, r.aim_correlation_traded);
    if (std::isfinite(r.aim_correlation) &&
        std::abs(r.aim_correlation_traded - r.aim_correlation) > 1e-9)
      ++differ;
  }
  EXPECT_TRUE(std::isnan(run.rows.front().aim_correlation_traded)); // the book is flat
  ASSERT_GT(compared, 0U);
  EXPECT_GT(differ, 0U) << "the traded book never differs from the plan";
  // The report (one book): both correlations; the criterion reads the traded mean.
  ASSERT_EQ(run.summary.size(), 1U);
  const auto& entry = run.summary.begin().value();
  EXPECT_EQ(entry.at("aim_correlation_traded").at("n"), compared);
  EXPECT_NEAR(entry.at("aim_correlation_traded").at("mean").get<f64>(),
              sum / static_cast<f64>(compared), 1e-12);
  EXPECT_EQ(entry.at("aim_correlation_traded").at("min").get<f64>(), lowest);
  EXPECT_TRUE(entry.contains("aim_correlation")); // the plan's, as before
  const auto& criterion = entry.at("aim_correlation_criterion");
  EXPECT_EQ(criterion.at("reads"), "aim_correlation_traded.mean");
  EXPECT_EQ(criterion.at("threshold").get<f64>(), sp::v3_aim_correlation_min);
  EXPECT_EQ(sp::v3_aim_correlation_min, 0.9);
  const f64 value = criterion.at("value").get<f64>();
  EXPECT_EQ(value, entry.at("aim_correlation_traded").at("mean").get<f64>());
  EXPECT_EQ(criterion.at("met").get<bool>(), value >= 0.9);
  EXPECT_EQ(run.tripwire.at("report_only").begin().value().at("aim_correlation_criterion"),
            criterion);
}

// The CLI: --rule spo-v3 takes spo::v3_params (S_prior 20 by Ruling E-14, H 20, p .01, beta
// .02, ceiling 1 with the void on); --spo-alpha implied-aim is its only alpha (refused with
// spo-v1/v2 and for any other value); every spo flag that would move a registered constant is
// refused; the allowed flags override; as spo-v1/v2 it refuses the capacity curve, a per-name
// rate and the holdings stream with the void on; its blocks are keyed spo_v3.
TEST(SpoV3, ParseRefusesTheRegisteredConstantsAndRoutesTheImpliedAim) {
  const std::vector<std::string> tail{"--risk-model", "risk", "--risk-model-sha256", "abc",
                                      "--output", "x"};
  const auto parse = [&](std::vector<std::string> head) {
    head.insert(head.end(), tail.begin(), tail.end());
    std::vector<char*> argv;
    for (auto& a : head) argv.push_back(a.data());
    return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
  };
  EXPECT_EQ(sp::v3_sharpe_prior, 20.0); // Ruling E-14 (not the 1.0 first declared)
  const auto v3 = parse({"nav", "--rule", "spo-v3", "--spo-alpha", "implied-aim"});
  ASSERT_TRUE(v3) << v3.error().to_string();
  const auto& p = v3->options.spo_params;
  const auto registered = sp::v3_params();
  EXPECT_TRUE(v3->options.spo_v1);
  EXPECT_FALSE(v3->options.aim_v6);
  EXPECT_EQ(p.version, 3U);
  EXPECT_EQ(p.sharpe_prior, sp::v3_sharpe_prior);
  EXPECT_EQ(p.horizon, sp::v3_horizon);
  EXPECT_EQ(p.adv_trade_p, 0.01);
  EXPECT_EQ(p.beta_max, 0.02);
  EXPECT_EQ(p.specific_ceiling, 1.0);
  EXPECT_TRUE(p.void_on_capped);
  EXPECT_EQ(p.max_iterations, registered.max_iterations);
  EXPECT_EQ(p.tolerance, registered.tolerance);
  EXPECT_TRUE(p.all_books);
  EXPECT_TRUE(sp::validate_params(p));
  EXPECT_STREQ(sp::rule_name(p), "spo-v3");
  EXPECT_STREQ(sp::json_key(p), "spo_v3");
  EXPECT_EQ(v3->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5", "--output",
                                                "x"}));
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v3"})); // --spo-alpha is optional
  // The allowed flags override the rule's defaults.
  const auto allowed = parse({"nav", "--rule", "spo-v3", "--spo-iters", "3000", "--spo-tol",
                              "1e-10", "--spo-books", "primary", "--specific-ceiling", "2",
                              "--specific-ceiling-void", "off"});
  ASSERT_TRUE(allowed) << allowed.error().to_string();
  const auto& a = allowed->options.spo_params;
  EXPECT_EQ(a.max_iterations, 3000U);
  EXPECT_EQ(a.tolerance, 1e-10);
  EXPECT_FALSE(a.all_books);
  EXPECT_EQ(a.specific_ceiling, 2.0);
  EXPECT_FALSE(a.void_on_capped);
  EXPECT_EQ(a.sharpe_prior, sp::v3_sharpe_prior);
  // Refused with spo-v3: every flag that would move a registered constant (still spo-v2's).
  for (const char* flag : {"--gamma", "--ic-book", "--w-max", "--adv-cap-q", "--adv-trade-p",
                           "--target-vol", "--spo-horizon", "--alpha-horizon", "--spo-gross"}) {
    const auto refused = parse({"nav", "--rule", "spo-v3", flag, "0.5"});
    ASSERT_FALSE(refused) << flag;
    EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument) << flag;
    EXPECT_NE(refused.error().message().find(flag), std::string::npos)
        << refused.error().to_string();
    const auto early = parse({"nav", flag, "0.5", "--rule", "spo-v3"}); // wherever it stands
    EXPECT_FALSE(early) << flag;
  }
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v2", "--gamma", "5"}));
  // --spo-alpha: implied-aim only, spo-v3 only, once.
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--spo-alpha", "fitted"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v1", "--spo-alpha", "implied-aim"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v2", "--spo-alpha", "implied-aim"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--spo-alpha", "implied-aim", "--spo-alpha",
                      "implied-aim"}));
  {
    std::vector<std::string> args{"nav", "--spo-alpha", "implied-aim", "--output", "x"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    EXPECT_FALSE(v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data())); // no rule
    EXPECT_TRUE(v7::claims_nav_args(static_cast<int>(argv.size()), argv.data()));
  }
  {
    std::vector<std::string> args{"nav", "--rule", "spo-v3", "--output", "x"};
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    EXPECT_FALSE(v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data())); // no model
    EXPECT_TRUE(v7::claims_nav_args(static_cast<int>(argv.size()), argv.data()));
  }
  // As spo-v1/v2: no capacity curve, the fixed rate, no holdings stream with the void on.
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--capacity-curve"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--rate", "per-name-v1"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--emit-holdings", "h"}));
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v3", "--emit-holdings", "h",
                     "--specific-ceiling-void", "off"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v3", "--rule", "aim-partial-v6"}));
  // The blocks are keyed spo_v3 and the rule is relabelled.
  Json recipe{{"rule", "aim-partial-v5+neutral-price-risk-v1"}};
  {
    const v7::ScopedNavExtension extension(v3->options);
    v7::extend_recipe(recipe);
  }
  EXPECT_EQ(recipe["rule"], "spo-v3+neutral-price-risk-v1");
  ASSERT_TRUE(recipe.contains("v7"));
  ASSERT_TRUE(recipe["v7"].contains("spo_v3"));
  EXPECT_FALSE(recipe["v7"].contains("spo_v2"));
  EXPECT_EQ(recipe["v7"]["spo_v3"]["parameters"]["sharpe_prior"], 20.0);
  EXPECT_EQ(recipe["v7"]["spo_v3"]["parameters"]["alpha"].get<std::string>().rfind("implied-aim",
                                                                                   0),
            0U);
  const auto declared = recipe["v7"]["spo_v3"]["rule"].get<std::string>();
  EXPECT_NE(declared.find("S_prior = 20 "), std::string::npos) << declared;
  EXPECT_NE(declared.find("Ruling E-14"), std::string::npos) << declared;
  EXPECT_EQ(recipe["v7"]["spo_v3"]["calibration"]["rule"].get<std::string>().rfind(
                "gamma = S_prior / sigma_aim", 0),
            0U);
}

// v8 E-26: spo-v3's aim is L x the desired target the aim-partial-v5 NAV path forms, its
// shaping included. On the fixture with the hold band b = .1 and the ADV cap Q = .05, both
// binding (members keep their band value, names are clipped), the aim the tracker receives at
// every rebalance decision equals L x the plain aim-partial-v5 replay's desired target bit for
// bit on every member (each one reported), and 0 off the members; the construction record of
// every decision agrees.
TEST(SpoV3, AimIncludesHoldBandAndAdvCapWhenDeclared) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  auto cfg = undusted_config();
  cfg.target.hold_band = 0.1;
  cfg.target.adv_hold_q = 0.05;
  Recorder reference(role.n); // the aim-partial-v5 path itself: no extension installed
  const std::array<st::NavScenario, 1> primary{cfg.scenario};
  const auto plain = st::replay_nav_scenarios(role.nav(), cfg, primary, reference, 0);
  ASSERT_TRUE(plain) << plain.error().to_string();
  ASSERT_EQ(plain->size(), 1U);
  usize kept = 0, clipped = 0;
  for (const auto& day : plain->front().days) {
    kept += day.construction.hold_kept;
    clipped += day.construction.adv_clipped;
  }
  EXPECT_GT(kept, 0U) << "the hold band keeps no member";
  EXPECT_GT(clipped, 0U) << "the ADV cap clips no name";
  Recorder tracked(role.n);
  const Replay run = replay_v3(role, risk, sp::v3_params(), cfg, &tracked);
  ASSERT_FALSE(run.rows.empty());
  ASSERT_FALSE(tracked.aim.empty());
  ASSERT_EQ(tracked.aim.size(), reference.desired.size());
  const f64 leverage = cfg.target.aim_leverage;
  usize compared = 0;
  for (const auto& [t, aim] : tracked.aim) {
    const auto found = reference.desired.find(t);
    ASSERT_NE(found, reference.desired.end()) << "session " << t;
    const auto& desired = found->second;
    ASSERT_EQ(aim.size(), role.n) << "session " << t;
    for (usize i = 0; i < role.n; ++i) {
      if (role.member[t * role.n + i] == 0) {
        EXPECT_EQ(bits(aim[i]), bits(0.0)) << "session " << t << " name " << i;
        continue;
      }
      ASSERT_FALSE(std::isnan(desired[i])) << "member not reported: " << t << ' ' << i;
      EXPECT_EQ(bits(aim[i]), bits(leverage * desired[i])) << "session " << t << " name " << i;
      ++compared;
    }
  }
  EXPECT_GT(compared, 0U);
  const auto& a = plain->front().days;
  const auto& b = run.result.days;
  ASSERT_EQ(a.size(), b.size());
  for (usize k = 0; k < a.size(); ++k) {
    EXPECT_EQ(a[k].rebalance, b[k].rebalance) << k;
    EXPECT_EQ(a[k].construction.hold_kept, b[k].construction.hold_kept) << k;
    EXPECT_EQ(a[k].construction.hold_moved, b[k].construction.hold_moved) << k;
    EXPECT_EQ(a[k].construction.adv_clipped, b[k].construction.adv_clipped) << k;
    EXPECT_EQ(bits(a[k].construction.adv_clipped_mass), bits(b[k].construction.adv_clipped_mass))
        << k;
  }
}

// v8 E-26, both flags absent: spo-v3 publishes what it did. The shaping reaches the rule only
// through the shared desired target and the rule's own blocks carry no shaping key, so the
// band declared at its identity b = 0 (the kernel runs and carries its state; no key) with
// Q = 0 (off) gives every published block and every received aim bit for bit, and the rule id
// stays "spo-v3"; declaring b = .1 and Q = .05 changes the diagnostics and extends the rule id
// exactly as the aim-partial path's.
TEST(SpoV3, ShapingFlagsAbsentIsByteIdentical) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_model(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto params = sp::v3_params();
  const auto absent_cfg = nav_config();
  ASSERT_FALSE(absent_cfg.target.hold_band.has_value());
  ASSERT_EQ(absent_cfg.target.adv_hold_q, 0.0);
  auto identity_cfg = absent_cfg;
  identity_cfg.target.hold_band = 0.0;
  auto shaped_cfg = absent_cfg;
  shaped_cfg.target.hold_band = 0.1;
  shaped_cfg.target.adv_hold_q = 0.05;
  Recorder absent_aims(role.n), identity_aims(role.n);
  const Replay absent = replay_v3(role, risk, params, absent_cfg, &absent_aims);
  const Replay identity = replay_v3(role, risk, params, identity_cfg, &identity_aims);
  const Replay shaped = replay_v3(role, risk, params, shaped_cfg);
  ASSERT_FALSE(absent.rows.empty());
  EXPECT_EQ(identity.csv, absent.csv);
  EXPECT_EQ(identity.parameters.dump(), absent.parameters.dump());
  EXPECT_EQ(identity.summary.dump(), absent.summary.dump());
  EXPECT_EQ(identity.tripwire.dump(), absent.tripwire.dump());
  EXPECT_EQ(identity.calibration_json.dump(), absent.calibration_json.dump());
  EXPECT_EQ(identity.declaration, absent.declaration);
  ASSERT_FALSE(absent_aims.aim.empty());
  ASSERT_EQ(identity_aims.aim.size(), absent_aims.aim.size());
  for (const auto& [t, aim] : absent_aims.aim) {
    const auto found = identity_aims.aim.find(t);
    ASSERT_NE(found, identity_aims.aim.end()) << "session " << t;
    ASSERT_EQ(found->second.size(), aim.size()) << "session " << t;
    for (usize i = 0; i < aim.size(); ++i)
      EXPECT_EQ(bits(found->second[i]), bits(aim[i])) << "session " << t << " name " << i;
  }
  // The rule's blocks name no shaping, declared or not.
  for (const Replay* run : {&absent, &shaped}) {
    for (const char* key : {"hold_band", "hold-band", "adv_hold", "adv-hold"}) {
      EXPECT_EQ(run->parameters.dump().find(key), std::string::npos) << key;
      EXPECT_EQ(run->declaration.find(key), std::string::npos) << key;
    }
  }
  ASSERT_FALSE(shaped.rows.empty());
  EXPECT_NE(shaped.csv, absent.csv); // the shaped aim reached the tracker
  // The rule id: the NAV replay's, relabelled by the extension as every spo-v3 id.
  const auto rule = [&](const st::NavReplayConfig& cfg, bool v3) {
    Json recipe{{"rule", st::detail::construction_rule_id(cfg.target)}};
    if (v3) {
      v7::NavV7Options o;
      o.spo_v1 = true;
      o.spo_params = params;
      const v7::ScopedNavExtension extension(o);
      v7::extend_recipe(recipe);
    }
    return recipe.at("rule").get<std::string>();
  };
  EXPECT_EQ(rule(absent_cfg, true), "spo-v3");
  EXPECT_EQ(rule(identity_cfg, true), "spo-v3");
  EXPECT_EQ(rule(shaped_cfg, false), "aim-partial-v5+hold-band-0.1+adv-hold-0.05");
  EXPECT_EQ(rule(shaped_cfg, true), "spo-v3+hold-band-0.1+adv-hold-0.05");
}

// v8 E-26 CLI: --hold-band and --adv-hold-q pass through to the replay under spo-v3 (in the
// order given; the replay's own parser and validation read them) and are refused with spo-v1
// and spo-v2 whatever their value or place.
TEST(SpoV3, ShapingFlagsPassThroughAndSpoV1V2RefuseThem) {
  const std::vector<std::string> tail{"--risk-model", "risk", "--risk-model-sha256", "abc",
                                      "--output", "x"};
  const auto parse = [&](std::vector<std::string> head) {
    head.insert(head.end(), tail.begin(), tail.end());
    std::vector<char*> argv;
    for (auto& a : head) argv.push_back(a.data());
    return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
  };
  const auto v3 =
      parse({"nav", "--hold-band", ".1", "--rule", "spo-v3", "--adv-hold-q", ".05"});
  ASSERT_TRUE(v3) << v3.error().to_string();
  EXPECT_EQ(v3->options.spo_params.version, 3U);
  EXPECT_EQ(v3->args, (std::vector<std::string>{"nav", "--hold-band", ".1", "--rule",
                                                "aim-partial-v5", "--adv-hold-q", ".05",
                                                "--output", "x"}));
  EXPECT_TRUE(parse({"nav", "--rule", "spo-v3", "--hold-band", "0"}));
  for (const char* spo : {"spo-v1", "spo-v2"}) {
    EXPECT_TRUE(parse({"nav", "--rule", spo})) << spo; // without the flags, as before
    for (const char* flag : {"--hold-band", "--adv-hold-q"}) {
      for (const char* value : {"0", ".1"}) {
        const auto refused = parse({"nav", "--rule", spo, flag, value});
        ASSERT_FALSE(refused) << spo << ' ' << flag << ' ' << value;
        EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument) << spo << ' ' << flag;
        EXPECT_NE(refused.error().message().find(flag), std::string::npos)
            << refused.error().to_string();
        EXPECT_NE(refused.error().message().find("spo-v3"), std::string::npos)
            << refused.error().to_string();
        EXPECT_FALSE(parse({"nav", flag, value, "--rule", spo})) << spo << ' ' << flag;
      }
    }
  }
}
} // namespace
