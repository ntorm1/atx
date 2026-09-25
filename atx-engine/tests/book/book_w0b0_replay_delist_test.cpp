// W0-B0 (B-04): a missing held close and a locate breach no longer abort the
// replay. DelistingPolicy::TerminalReturn (the default) liquidates a vanished
// held name at its last accounted value times (1 + r), with r from the
// supplied terminal-return table or the flagged Shumway fallback (-30 %
// NYSE/AMEX, -55 % Nasdaq, adverse-for-the-side when the venue is unknown);
// LocateBreach::ClipV2 (the default) clips a short to its locate and reports.
// Abort / AbortV1 stay reachable. Synthetic fixtures only.

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/borrow_schedule.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/book/replay_cost.hpp"

namespace atx_test_w0_b0_replay_delist {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's algorithm).
constexpr atx::i64 days_from_civil(atx::i64 y, atx::i64 m, atx::i64 d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const atx::i64 era = (y >= 0 ? y : y - 399) / 400;
  const atx::i64 yoe = y - era * 400;
  const atx::i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const atx::i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void expect_identity(const book::ReplayResult &result) {
  for (const auto &row : result.intervals) {
    EXPECT_NEAR(row.cash + row.assets, row.nav, 1.0e-9);
    EXPECT_NEAR(row.pretrade_nav + row.gross_pnl - row.trade_cost - row.borrow_cost, row.nav,
                1.0e-9);
  }
}

// ---------------------------------------------------------------------------
// The PCS 2013-05-01 fixture. MetroPCS (NYSE: PCS) stopped printing after the
// 2013-04-30 close (the T-Mobile USA combination re-listed the entity as TMUS on
// 2013-05-01). A panel keyed on the PCS identity loses the name mid-run with no
// delisting record: exactly the case that used to abort the replay and push
// research toward a survivor-filtered universe. Ten NYSE sessions
// 2013-04-24 .. 2013-05-07; names: 0 = PCS (NYSE), 1 = AAA (NYSE), 2 = BBB
// (Nasdaq). Prices are synthetic.
// ---------------------------------------------------------------------------
struct PcsFixture {
  static constexpr atx::usize kDates = 10;
  static constexpr atx::usize kNames = 3;
  static constexpr atx::usize kPcsLast = 4;  // 2013-04-30
  static constexpr atx::usize kPcsGone = 5;  // 2013-05-01
  std::vector<atx::i64> session_keys;
  std::vector<atx::f64> close;
  std::vector<atx::u8> universe;
  std::vector<atx::usize> decisions;
  std::vector<atx::f64> targets;
  std::array<book::ListingExchange, kNames> venues{
      book::ListingExchange::NyseAmex, book::ListingExchange::NyseAmex,
      book::ListingExchange::Nasdaq};

  PcsFixture() {
    const std::array<std::array<atx::i64, 3>, kDates> ymd{{{2013, 4, 24}, {2013, 4, 25},
                                                           {2013, 4, 26}, {2013, 4, 29},
                                                           {2013, 4, 30}, {2013, 5, 1},
                                                           {2013, 5, 2}, {2013, 5, 3},
                                                           {2013, 5, 6}, {2013, 5, 7}}};
    for (const auto &date : ymd) {
      // 16:00 New York (EDT, UTC-4) close == 20:00 UTC.
      session_keys.push_back(days_from_civil(date[0], date[1], date[2]) * kDay +
                             20LL * 3'600'000'000'000LL);
    }
    const std::array<atx::f64, kDates> pcs{10.00, 10.10, 10.20, 10.30, 10.40,
                                           kNaN,  kNaN,  kNaN,  kNaN,  kNaN};
    const std::array<atx::f64, kDates> aaa{50, 51, 52, 51, 50, 51, 52, 53, 54, 55};
    const std::array<atx::f64, kDates> bbb{20, 20, 21, 21, 22, 22, 23, 23, 24, 24};
    for (atx::usize t = 0; t < kDates; ++t) {
      close.insert(close.end(), {pcs[t], aaa[t], bbb[t]});
      const auto pcs_listed = static_cast<atx::u8>(t <= kPcsLast ? 1U : 0U);
      universe.insert(universe.end(), {pcs_listed, atx::u8{1}, atx::u8{1}});
    }
    // Daily decisions 0..8 (delay 1), 30 % in every eligible name.
    for (atx::usize d = 0; d + 1 < kDates; ++d) {
      decisions.push_back(d);
      targets.insert(targets.end(), {d <= kPcsLast ? 0.3 : 0.0, 0.3, 0.3});
    }
  }

  [[nodiscard]] Panel panel() const {
    return Panel::create(kDates, kNames, {"close"}, {close}, universe).value();
  }
};

TEST(BookReplayDelist, Pcs20130501FixtureRunsToTheEnd) {
  const PcsFixture fx;
  book::ReplayConfig cfg;
  cfg.initial_nav = 1'000'000.0;
  cfg.trade_bps = 5.0;
  cfg.listing_exchange = fx.venues;
  ASSERT_EQ(cfg.delisting_policy, book::DelistingPolicy::TerminalReturn); // The default.
  const auto result = book::replay_scheduled_targets(fx.panel(), fx.session_keys, fx.decisions,
                                                     fx.targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->intervals.size(), PcsFixture::kDates - 1); // Ran to 2013-05-07.
  EXPECT_EQ(result->effective_rebalances, PcsFixture::kDates - 2);
  ASSERT_EQ(result->delistings.size(), 1U);
  const auto &gone = result->delistings[0];
  EXPECT_EQ(gone.period, PcsFixture::kPcsGone);
  EXPECT_EQ(gone.instrument, 0U);
  EXPECT_DOUBLE_EQ(gone.delist_return, -0.30);
  EXPECT_EQ(gone.source, book::TerminalReturnSource::ShumwayNyseAmex);
  EXPECT_TRUE(gone.flagged);
  EXPECT_EQ(result->flagged_delistings, 1U);
  EXPECT_GT(gone.last_value, 0.0);
  EXPECT_NEAR(gone.proceeds, gone.last_value * 0.70, 1.0e-6);
  // The last-print value is the 04-30 mark of the units held.
  EXPECT_NEAR(gone.last_value, gone.tri_units * 10.40, 1.0e-6);
  // The 04-30 decision's PCS leg executes on 05-01 with no close: unfillable,
  // stays in cash and is reported instead of failing the run.
  ASSERT_EQ(result->unfilled_targets.size(), 1U);
  EXPECT_EQ(result->unfilled_targets[0].period, PcsFixture::kPcsGone);
  EXPECT_EQ(result->unfilled_targets[0].decision_period, PcsFixture::kPcsLast);
  EXPECT_EQ(result->unfilled_targets[0].instrument, 0U);
  EXPECT_DOUBLE_EQ(result->unfilled_targets[0].weight, 0.3);
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], 0.0);
  // The interval that realizes the delisting books exactly -30 % of the PCS
  // leg on top of the survivors' moves: AAA (30 % of the 04-30 pretrade NAV,
  // bought at 50) gains 1 per unit into 05-01, BBB is flat at 22.
  const auto &hit = result->intervals[PcsFixture::kPcsGone - 1];
  const atx::f64 aaa_move = 0.3 * hit.pretrade_nav / 50.0 * (51.0 - 50.0);
  EXPECT_NEAR(hit.gross_pnl, (gone.proceeds - gone.last_value) + aaa_move, 1.0e-6);
  EXPECT_LT(hit.gross_pnl, 0.0);
  expect_identity(*result);
  std::printf("[measured] PCS last_value=%.2f proceeds=%.2f pnl(04-30->05-01)=%.2f "
              "final_nav=%.2f\n",
              gone.last_value, gone.proceeds, hit.gross_pnl, result->final_nav);

  // The pre-W0 contract, still reachable explicitly, aborts at the same cell.
  cfg.delisting_policy = book::DelistingPolicy::Abort;
  const auto aborted = book::replay_scheduled_targets(fx.panel(), fx.session_keys, fx.decisions,
                                                      fx.targets, cfg);
  ASSERT_FALSE(aborted.has_value());
  EXPECT_NE(aborted.error().message().find("period=5 instrument=0"), std::string::npos)
      << aborted.error().message();
}

// One held name that vanishes after period 1; `short_side` flips the book.
book::ReplayResult vanish(book::ListingExchange venue, bool short_side,
                          std::span<const book::DelistingEvent> table = {}) {
  const std::vector<atx::f64> close{100, 50, 100, 50, 100, kNaN, 100, kNaN};
  const auto panel = Panel::create(4, 2, {"close"}, {close}, {}).value();
  std::vector<atx::i64> keys{0, kDay, 2 * kDay, 3 * kDay};
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.5, short_side ? -0.4 : 0.4};
  const std::vector<book::ListingExchange> venues{book::ListingExchange::NyseAmex, venue};
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.listing_exchange = venues;
  cfg.delistings = table;
  auto result = book::replay_scheduled_targets(panel, keys, decisions, targets, cfg);
  EXPECT_TRUE(result.has_value()) << result.error().message();
  return result.has_value() ? std::move(*result) : book::ReplayResult{};
}

TEST(BookReplayDelist, MissingCloseWithoutEvidenceIsShumwayFlaggedNeverZeroNeverAbort) {
  struct Case {
    book::ListingExchange venue;
    bool short_side;
    atx::f64 expected;
    book::TerminalReturnSource source;
  };
  const std::array<Case, 6> cases{{
      {book::ListingExchange::NyseAmex, false, -0.30, book::TerminalReturnSource::ShumwayNyseAmex},
      {book::ListingExchange::NyseAmex, true, -0.30, book::TerminalReturnSource::ShumwayNyseAmex},
      {book::ListingExchange::Nasdaq, false, -0.55, book::TerminalReturnSource::ShumwayNasdaq},
      {book::ListingExchange::Nasdaq, true, -0.55, book::TerminalReturnSource::ShumwayNasdaq},
      {book::ListingExchange::Unknown, false, -0.55,
       book::TerminalReturnSource::ShumwayUnknownAdverse},
      {book::ListingExchange::Unknown, true, -0.30,
       book::TerminalReturnSource::ShumwayUnknownAdverse},
  }};
  for (const auto &c : cases) {
    const auto result = vanish(c.venue, c.short_side);
    ASSERT_EQ(result.intervals.size(), 3U); // Never an abort: every interval exists.
    ASSERT_EQ(result.delistings.size(), 1U);
    const auto &gone = result.delistings[0];
    EXPECT_EQ(gone.period, 2U);
    EXPECT_EQ(gone.instrument, 1U);
    EXPECT_DOUBLE_EQ(gone.delist_return, c.expected);
    EXPECT_NE(gone.delist_return, 0.0); // Never zero.
    EXPECT_EQ(gone.source, c.source);
    EXPECT_TRUE(gone.flagged);
    // Units bought at period 1 (price 50) with 40 % of NAV 1000 = +-8 units.
    EXPECT_NEAR(gone.last_value, c.short_side ? -400.0 : 400.0, 1.0e-9);
    EXPECT_NEAR(gone.proceeds, gone.last_value * (1.0 + c.expected), 1.0e-9);
    expect_identity(result);
    std::printf("[measured] venue=%d short=%d r=%.2f proceeds=%.2f final_nav=%.4f\n",
                static_cast<int>(c.venue), c.short_side ? 1 : 0, gone.delist_return,
                gone.proceeds, result.final_nav);
  }
}

TEST(BookReplayDelist, SuppliedTerminalReturnTableWinsAndIsNotFlagged) {
  const std::vector<book::DelistingEvent> known{{1, 1, -0.9}};
  const auto table = vanish(book::ListingExchange::Nasdaq, false, known);
  ASSERT_EQ(table.delistings.size(), 1U);
  EXPECT_DOUBLE_EQ(table.delistings[0].delist_return, -0.9);
  EXPECT_EQ(table.delistings[0].source, book::TerminalReturnSource::Table);
  EXPECT_FALSE(table.delistings[0].flagged);
  EXPECT_EQ(table.flagged_delistings, 0U);
  EXPECT_NEAR(table.delistings[0].proceeds, 40.0, 1.0e-9);
  // A table row with an unknown (NaN) return falls back to Shumway, flagged.
  const std::vector<book::DelistingEvent> unknown{{1, 1, kNaN}};
  const auto fallback = vanish(book::ListingExchange::Nasdaq, false, unknown);
  ASSERT_EQ(fallback.delistings.size(), 1U);
  EXPECT_DOUBLE_EQ(fallback.delistings[0].delist_return, -0.55);
  EXPECT_TRUE(fallback.delistings[0].flagged);
}

TEST(BookReplayDelist, TableAndExchangeInputsAreValidated) {
  const std::vector<atx::f64> close{100, 50, 100, 50, 100, kNaN, 100, kNaN};
  const auto panel = Panel::create(4, 2, {"close"}, {close}, {}).value();
  const std::vector<atx::i64> keys{0, kDay, 2 * kDay, 3 * kDay};
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.5, 0.4};
  book::ReplayConfig cfg;
  const std::vector<book::ListingExchange> short_list{book::ListingExchange::Nasdaq};
  cfg.listing_exchange = short_list;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, keys, decisions, targets, cfg));
  cfg.listing_exchange = {};
  const std::vector<book::DelistingEvent> infinite{{1, 1, kInf}};
  cfg.delistings = infinite;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, keys, decisions, targets, cfg));
  const std::vector<book::DelistingEvent> below{{1, 1, -1.5}};
  cfg.delistings = below;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, keys, decisions, targets, cfg));
  cfg.delistings = {};
  cfg.delisting_policy = static_cast<book::DelistingPolicy>(9);
  EXPECT_FALSE(book::replay_scheduled_targets(panel, keys, decisions, targets, cfg));
}

TEST(BookReplayDelist, IntentPolicyTargetOnAVanishedNameIsUnfilledNotFatal) {
  const std::vector<atx::f64> close{100, 50, 100, 50, 100, kNaN, 100, kNaN};
  const auto panel = Panel::create(4, 2, {"close"}, {close}, {}).value();
  const std::vector<atx::i64> keys{0, kDay, 2 * kDay, 3 * kDay};
  const std::vector<atx::usize> decisions{1};
  const std::vector<atx::f64> preference{0.5, 0.4};
  const book::ReplayIntentPolicy intents = [](const book::ReplayAllocationState &) {
    return atx::core::Result<std::vector<book::ReplayTargetIntent>>(
        std::vector<book::ReplayTargetIntent>{{book::ReplayTargetAction::TargetWeight, 0.5},
                                              {book::ReplayTargetAction::TargetWeight, 0.4}});
  };
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  const auto result =
      book::replay_scheduled_intents(panel, keys, decisions, preference, intents, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->replay.unfilled_targets.size(), 1U);
  EXPECT_EQ(result->replay.unfilled_targets[0].instrument, 1U);
  ASSERT_EQ(result->allocations.size(), 1U);
  EXPECT_DOUBLE_EQ(result->allocations[0].target_weights[1], 0.0); // Resolved: not held.
  EXPECT_DOUBLE_EQ(result->replay.final_tri_units[1], 0.0);
  EXPECT_NEAR(result->replay.final_cash, 500.0, 1.0e-9); // 40 % stays in cash.
  expect_identity(result->replay);
}

// ---------------------------------------------------------------------------
// Locate breaches (B-04): clip and report, never abort.
// ---------------------------------------------------------------------------
Panel flat(atx::usize dates, atx::usize names) {
  return Panel::create(dates, names, {"close"},
                       {std::vector<atx::f64>(dates * names, 100.0)}, {})
      .value();
}

std::vector<atx::i64> day_axis(atx::usize count) {
  std::vector<atx::i64> keys;
  for (atx::usize i = 0; i < count; ++i) keys.push_back(static_cast<atx::i64>(i) * kDay);
  return keys;
}

TEST(BookReplayDelist, LocateBreachClipsToTheLocateAndReports) {
  const auto panel = flat(4, 2);
  book::BorrowSchedule borrow;
  borrow.dates = 4;
  borrow.instruments = 2;
  borrow.locate_dollars.assign(8, kInf);
  borrow.locate_dollars[1 * 2 + 1] = 150.0; // Name 1 locates 150$ at execution period 1.
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.borrow_schedule = &borrow;
  ASSERT_EQ(cfg.locate_breach, book::LocateBreach::ClipV2); // The default.
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.2, -0.2}; // Wants a 200$ short.
  const auto result = book::replay_scheduled_targets(panel, day_axis(4), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->locate_clips.size(), 1U);
  const auto &clip = result->locate_clips[0];
  EXPECT_EQ(clip.period, 1U);
  EXPECT_EQ(clip.instrument, 1U);
  EXPECT_DOUBLE_EQ(clip.requested_value, -200.0);
  EXPECT_DOUBLE_EQ(clip.allowed_value, -150.0);
  EXPECT_DOUBLE_EQ(clip.locate, 150.0);
  EXPECT_DOUBLE_EQ(result->final_tri_units[1], -1.5);
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], 2.0); // The long leg is untouched.
  expect_identity(*result);

  cfg.locate_breach = book::LocateBreach::AbortV1; // The pre-W0 contract.
  const auto aborted = book::replay_scheduled_targets(panel, day_axis(4), decisions, targets, cfg);
  ASSERT_FALSE(aborted.has_value());
  EXPECT_NE(aborted.error().message().find("locate"), std::string::npos);
}

TEST(BookReplayDelist, ShrunkenLocateHoldsTheCarriedShortWithoutGrowingOrCovering) {
  const auto panel = flat(5, 1);
  book::BorrowSchedule borrow;
  borrow.dates = 5;
  borrow.instruments = 1;
  borrow.locate_dollars = {kInf, kInf, 50.0, 0.0, 0.0};
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0, 1, 2};
  const std::vector<atx::f64> targets{-0.1, -0.3, -0.05}; // 100$, then 300$, then 50$.
  const auto result = book::replay_scheduled_targets(panel, day_axis(5), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  // Period 2: locate 50 < carried 100 -> hold 100 (no growth, no forced cover).
  ASSERT_EQ(result->locate_clips.size(), 1U);
  EXPECT_EQ(result->locate_clips[0].period, 2U);
  EXPECT_DOUBLE_EQ(result->locate_clips[0].requested_value, -300.0);
  EXPECT_DOUBLE_EQ(result->locate_clips[0].allowed_value, -100.0);
  // Period 3: a shrinking target (50$) is a cover and is never clipped.
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], -0.5);
  ASSERT_EQ(result->trades.size(), 2U);
  expect_identity(*result);
}

// A capped test model: fills at most 100$ per request, at zero cost.
class CappedFill final : public book::ReplayCostModel {
public:
  [[nodiscard]] book::TradeCost cost(atx::usize, atx::usize, atx::f64 trade_dollars,
                                     const book::LiquidityRow &) const noexcept override {
    const atx::f64 capped = trade_dollars < -100.0 ? -100.0
                          : trade_dollars > 100.0  ? 100.0
                                                   : trade_dollars;
    return {capped, 0.0};
  }
  [[nodiscard]] bool needs_liquidity() const noexcept override { return false; }
};

TEST(BookReplayDelist, WorkingOrderIsClippedToTheCurrentLocateBeforePricing) {
  const auto panel = flat(5, 1);
  book::BorrowSchedule borrow;
  borrow.dates = 5;
  borrow.instruments = 1;
  borrow.locate_dollars = {kInf, 250.0, 150.0, 150.0, 150.0};
  const CappedFill model;
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.borrow_schedule = &borrow;
  cfg.cost_model = &model;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{-0.3}; // Wants 300$ short.
  const auto result = book::replay_scheduled_targets(panel, day_axis(5), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  // Period 1: goal clipped 300 -> 250, fills 100. Period 2: the working order
  // (250) is re-clipped to the shrunken 150 locate and fills the last 50.
  ASSERT_EQ(result->locate_clips.size(), 2U);
  EXPECT_DOUBLE_EQ(result->locate_clips[0].allowed_value, -250.0);
  EXPECT_EQ(result->locate_clips[1].period, 2U);
  EXPECT_DOUBLE_EQ(result->locate_clips[1].requested_value, -250.0);
  EXPECT_DOUBLE_EQ(result->locate_clips[1].allowed_value, -150.0);
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], -1.5);
  EXPECT_EQ(result->open_working_orders, 0U);
  expect_identity(*result);
}

TEST(BookReplayDelist, ClaimsPathKeepsAbortSemanticsAndRejectsExtensions) {
  // The claims path admits the default policy (run as Abort) but no table.
  const std::vector<atx::f64> close{100, 50, 100, 50, 100, kNaN, 100, kNaN};
  const auto panel = Panel::create(4, 2, {"close"}, {close}, {}).value();
  const std::vector<atx::i64> keys{0, kDay, 2 * kDay, 3 * kDay};
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> preference{0.5, 0.4};
  const book::ReplayIntentPolicy intents = [](const book::ReplayAllocationState &) {
    return atx::core::Result<std::vector<book::ReplayTargetIntent>>(
        std::vector<book::ReplayTargetIntent>{{book::ReplayTargetAction::TargetWeight, 0.5},
                                              {book::ReplayTargetAction::TargetWeight, 0.4}});
  };
  const book::ReplayMandatoryEventPolicy none = [](const book::ReplayEventContext &) {
    return atx::core::Result<book::ReplayEventBatch>(book::ReplayEventBatch{});
  };
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  const auto aborted = book::replay_scheduled_intents_with_events(
      panel, keys, decisions, preference, intents, none, cfg, book::ReplayClaimsConfig{});
  ASSERT_FALSE(aborted.has_value());
  EXPECT_NE(aborted.error().message().find("period=2 instrument=1"), std::string::npos);
  const std::vector<book::ListingExchange> venues{book::ListingExchange::NyseAmex,
                                                  book::ListingExchange::Nasdaq};
  cfg.listing_exchange = venues;
  const auto rejected = book::replay_scheduled_intents_with_events(
      panel, keys, decisions, preference, intents, none, cfg, book::ReplayClaimsConfig{});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().message().find("not supported"), std::string::npos);
}

} // namespace atx_test_w0_b0_replay_delist
