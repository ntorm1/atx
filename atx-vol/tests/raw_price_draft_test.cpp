#include "fitting/raw_price_draft.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "fitting/counters.hpp"

namespace atx::vol {
namespace {

QuoteSliceConfig draft_config() {
  QuoteSliceConfig out;
  out.repair.certificate = QuoteSliceCertificate::Box;
  return out;
}

std::vector<QuoteSliceQuote> draft_quotes() {
  return {{90, Side::Call, 11.75, 12.25}, {90, Side::Put, 1.75, 2.25},
          {100, Side::Call, 5.75, 6.25}, {100, Side::Put, 5.75, 6.25},
          {110, Side::Call, 1.75, 2.25}, {110, Side::Put, 11.75, 12.25}};
}

TEST(RawPriceDraft, FeasibleRawMidpointsStayExactAndNeverPublishAFit) {
  const QuoteSliceMarket market{100, .5, .5, 0, 0, 0};
  const auto quotes = draft_quotes();
  auto result = detail::make_raw_price_draft(market, quotes, draft_config());
  ASSERT_EQ(result.refusal, detail::RawPriceDraftRefusal::None) << result.error;
  ASSERT_TRUE(result.draft);
  auto &draft = *result.draft;
  ASSERT_EQ(draft.nodes.size(), 3u);
  ASSERT_EQ(draft.original_quotes.size(), quotes.size());
  EXPECT_EQ(draft.nodes[0].call, 12);
  EXPECT_EQ(draft.nodes[0].put, 2);
  EXPECT_EQ(draft.boxes[0].call_lo, quotes[0].bid);
  EXPECT_EQ(draft.boxes[0].call_hi, quotes[0].ask);
  EXPECT_EQ(draft.quote_nodes, (std::vector<std::size_t>{0, 0, 1, 1, 2, 2}));
  const auto projected = detail::project_raw_price_drafts(std::span(&draft, 1));
  ASSERT_EQ(projected.outcome, JointProjectionOutcome::Feasible);
  ASSERT_TRUE(draft.projected);
  EXPECT_EQ(draft.nodes[0].call, 12);
  EXPECT_EQ(draft.nodes[1].call, 6);
  EXPECT_NE(draft.prepared.fit_status, QuoteSliceFitStatus::Success);
  EXPECT_FALSE(draft.prepared.curve);
  EXPECT_FALSE(draft.prepared.prices);
  EXPECT_FALSE(query_quote_slice(draft.prepared, 100, Side::Call));
}

TEST(RawPriceDraft, DuplicateBoxesIntersectAndKeepEveryOriginalRow) {
  auto quotes = draft_quotes();
  quotes.push_back({90, Side::Call, 12, 12.2});
  auto result = detail::make_raw_price_draft({100, .5, .5, 0, 0, 0}, quotes, draft_config());
  ASSERT_TRUE(result.draft) << result.error;
  EXPECT_EQ(result.draft->original_quotes.size(), 7u);
  EXPECT_EQ(result.draft->quote_nodes.back(), 0u);
  EXPECT_EQ(result.draft->boxes[0].call_lo, 12);
  EXPECT_EQ(result.draft->boxes[0].call_hi, 12.2);
  EXPECT_EQ(result.draft->nodes[0].call, 12.1);
  quotes.back() = {90, Side::Call, 13, 14};
  result = detail::make_raw_price_draft({100, .5, .5, 0, 0, 0}, quotes, draft_config());
  EXPECT_EQ(result.refusal, detail::RawPriceDraftRefusal::EmptyQuoteIntersection);
  EXPECT_FALSE(result.draft);
}

TEST(RawPriceDraft, MissingPairedSideIsATypedFallbackWithoutDroppingRows) {
  auto quotes = draft_quotes();
  quotes.pop_back();
  const auto result = detail::make_raw_price_draft(
      {100, .5, .5, 0, 0, 0}, quotes, draft_config());
  EXPECT_EQ(result.refusal, detail::RawPriceDraftRefusal::MissingPairedSide);
  EXPECT_FALSE(result.draft);
}

TEST(RawPriceDraft, RecoveryKeepsRawTargetsAndIntersectsUnchangedDuplicateAsks) {
  const QuoteSliceMarket market{100, .5, .5, .02, .01, 0};
  const double floor = 100 * std::exp(-.005) - 90 * std::exp(-.01);
  const double allowance = 1e-8 + 1e-9 * 100;
  auto quotes = draft_quotes();
  quotes[0] = {90, Side::Call, floor - .2, floor - .1};
  quotes.push_back({90, Side::Call, floor - .15, floor + .25 * allowance});
  auto config = draft_config();
  config.repair.terminal_floor_recovery = true;
  auto result = detail::make_raw_price_draft(market, quotes, config);
  ASSERT_TRUE(result.draft) << result.error;
  const auto &draft = *result.draft;
  EXPECT_EQ(draft.recovery.raised_asks, 1u);
  EXPECT_DOUBLE_EQ(draft.boxes[0].call_hi, quotes.back().ask);
  EXPECT_DOUBLE_EQ(draft.boxes[0].call_lo, quotes.back().bid);
  EXPECT_DOUBLE_EQ(draft.nodes[0].call, .5 * quotes.back().bid + .5 * quotes[0].ask);
  EXPECT_EQ(draft.original_quotes[0].ask, quotes[0].ask);
  config.repair.terminal_floor_recovery = false;
  result = detail::make_raw_price_draft(market, quotes, config);
  ASSERT_TRUE(result.draft);
  EXPECT_EQ(detail::project_raw_price_drafts(std::span(&*result.draft, 1)).outcome,
            JointProjectionOutcome::InfeasibleData);
}

TEST(RawPriceDraft, NonconvexTargetsProjectJointlyAndExhaustionRollsBack) {
  auto quotes = draft_quotes();
  for (auto &quote : quotes) {
    quote.bid = std::max(0.0, quote.bid - 5);
    quote.ask += 5;
  }
  quotes[2] = {100, Side::Call, 7, 13};
  quotes[3] = {100, Side::Put, 7, 13};
  auto result = detail::make_raw_price_draft({100, .5, .5, 0, 0, 0}, quotes, draft_config());
  ASSERT_TRUE(result.draft);
  auto &draft = *result.draft;
  const auto original = draft.nodes;
  EXPECT_EQ(detail::project_raw_price_drafts(std::span(&draft, 1), 1).outcome,
            JointProjectionOutcome::NonConverged);
  EXPECT_FALSE(draft.projected);
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(draft.nodes[i].call, original[i].call);
    EXPECT_EQ(draft.nodes[i].put, original[i].put);
  }
  ASSERT_EQ(detail::project_raw_price_drafts(std::span(&draft, 1)).outcome,
            JointProjectionOutcome::Feasible);
  EXPECT_TRUE(draft.projected);
  EXPECT_LT(draft.nodes[1].call, original[1].call);
  ASSERT_EQ(draft.raw_targets.size(), original.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(draft.raw_targets[i].strike, original[i].strike);
    EXPECT_EQ(draft.raw_targets[i].call, original[i].call);
    EXPECT_EQ(draft.raw_targets[i].put, original[i].put);
  }
}

TEST(RawPriceDraft, PartialDomainsAndReversedMaturitiesKeepAllPairCertificates) {
  auto near = draft_quotes();
  auto far = draft_quotes();
  far.erase(far.begin(), far.begin() + 2);
  for (auto &q : far) { q.bid = std::max(0.0, q.bid - .1); q.ask += .3; }
  auto a = detail::make_raw_price_draft({100, .5, .5, 0, 0, 0}, near, draft_config());
  auto b = detail::make_raw_price_draft({100, 1, 1, 0, 0, 0}, far, draft_config());
  ASSERT_TRUE(a.draft && b.draft);
  std::vector<detail::RawPriceDraft> drafts;
  drafts.push_back(std::move(*b.draft));
  drafts.push_back(std::move(*a.draft));
  ASSERT_EQ(detail::project_raw_price_drafts(drafts).outcome, JointProjectionOutcome::Feasible);
  ASSERT_TRUE(drafts[0].projected && drafts[1].projected);
  EXPECT_TRUE(check_american_calendar(*drafts[1].projected, *drafts[0].projected));
  EXPECT_EQ(drafts[0].nodes.front().strike, 100);
  EXPECT_EQ(drafts[1].nodes.front().strike, 90);
}

TEST(RawPriceDraft, UnsupportedInputsAreTypedAndNeverPartiallyDrafted) {
  const QuoteSliceMarket market{100, .5, .5, 0, 0, 0};
  auto quotes = draft_quotes();
  auto config = draft_config();
  config.dividend_model = QuoteSliceDividendModel::DatedCash;
  EXPECT_EQ(detail::make_raw_price_draft(market, quotes, config).refusal,
            detail::RawPriceDraftRefusal::UnsupportedModel);
  config = draft_config();
  quotes[0].ask = quotes[0].bid - 1;
  EXPECT_EQ(detail::make_raw_price_draft(market, quotes, config).refusal,
            detail::RawPriceDraftRefusal::InvalidQuote);
  quotes = draft_quotes();
  quotes[0].continuous_yield = .03;
  quotes[1].continuous_yield = .04;
  EXPECT_EQ(detail::make_raw_price_draft(market, quotes, config).refusal,
            detail::RawPriceDraftRefusal::InvalidInput);
  quotes = draft_quotes();
  EXPECT_EQ(detail::make_raw_price_draft({100, .5, .5, -.01, -.02, 0}, quotes, config).refusal,
            detail::RawPriceDraftRefusal::UnsupportedRegime);
}

TEST(RawPriceDraft, SignedCarryUsesSharedActualTerminalBounds) {
  auto config = draft_config();
  config.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
  const auto result = detail::make_raw_price_draft(
      {100, .25, .5, -.01, .02, 2}, draft_quotes(), config);
  ASSERT_TRUE(result.draft) << result.error;
  const auto &draft = *result.draft;
  EXPECT_DOUBLE_EQ(draft.prepared.diagnostics.discount, std::exp(.005));
  EXPECT_DOUBLE_EQ(draft.bounds.discount_min, 1);
  EXPECT_DOUBLE_EQ(draft.bounds.discount_max, std::exp(.005));
  EXPECT_NEAR(draft.terminal_prepaid[0], 100 * std::exp(-.01) - 2, 1e-12);
  EXPECT_DOUBLE_EQ(draft.bounds.prepaid_min, draft.terminal_prepaid[0]);
}

TEST(RawPriceDraft, ExternalNormalizedQuotesProjectWithoutAnyInitialIvFit) {
  std::string path;
#ifdef _WIN32
  std::size_t length = 0;
  ASSERT_EQ(getenv_s(&length, nullptr, 0, "ATX_VOL_RAW_DRAFT_FIXTURE"), 0);
  if (length > 0u) {
    std::vector<char> buffer(length);
    ASSERT_EQ(getenv_s(&length, buffer.data(), buffer.size(), "ATX_VOL_RAW_DRAFT_FIXTURE"), 0);
    path = buffer.data();
  }
#else
  if (const char *value = std::getenv("ATX_VOL_RAW_DRAFT_FIXTURE")) path = value;
#endif
  if (path.empty()) GTEST_SKIP() << "Set ATX_VOL_RAW_DRAFT_FIXTURE to normalized raw inputs";
  std::ifstream input(path);
  ASSERT_TRUE(input.good());
  nlohmann::json fixture;
  input >> fixture;
  namespace ledger = counters::ledger;
  const auto initial_al = ledger::this_thread(ledger::Solve::AlBoundarySolves);
  const auto initial_iv = ledger::this_thread(ledger::Solve::IvNewtonIters);
  std::vector<detail::RawPriceDraft> drafts;
  std::size_t rows = 0;
  std::size_t raised = 0;
  for (const auto &slice : fixture.at("slices")) {
    const auto &m = slice.at("market");
    QuoteSliceMarket market{m.at("spot"), m.at("variance_years"), m.at("calendar_years"),
                            m.at("rate"), m.at("continuous_yield"), m.at("dividend_pv")};
    std::vector<QuoteSliceQuote> quotes;
    for (const auto &q : slice.at("quotes")) {
      quotes.push_back({q.at("strike"), q.at("is_call").get<bool>() ? Side::Call : Side::Put,
                       q.at("bid"), q.at("ask"), q.at("continuous_yield").get<double>()});
    }
    auto config = draft_config();
    config.dividend_model = static_cast<QuoteSliceDividendModel>(
        slice.at("dividend_model_enum").get<int>());
    config.repair.terminal_floor_recovery = true;
    auto result = detail::make_raw_price_draft(market, quotes, config);
    ASSERT_TRUE(result.draft) << result.error;
    rows += quotes.size();
    raised += result.draft->recovery.raised_asks;
    drafts.push_back(std::move(*result.draft));
  }
  ASSERT_EQ(drafts.size(), 33u);
  ASSERT_EQ(rows, 12866u);
  EXPECT_EQ(raised, 15u);
  const auto projected = detail::project_raw_price_drafts(drafts);
  ASSERT_EQ(projected.outcome, JointProjectionOutcome::Feasible);
  RecordProperty("projection_sweeps", static_cast<int>(projected.sweeps));
  std::size_t outside = 0;
  for (const auto &draft : drafts) {
    ASSERT_TRUE(draft.projected);
    EXPECT_FALSE(draft.prepared.curve || draft.prepared.prices);
    EXPECT_NE(draft.prepared.fit_status, QuoteSliceFitStatus::Success);
    for (const auto &q : draft.original_quotes) {
      const auto price = draft.projected->price(q.strike, q.side);
      ASSERT_TRUE(price);
      if (*price < q.bid - 1e-6 || *price > q.ask + 1e-6) ++outside;
    }
  }
  EXPECT_EQ(outside, 15u);
  EXPECT_EQ(ledger::this_thread(ledger::Solve::AlBoundarySolves), initial_al);
  EXPECT_EQ(ledger::this_thread(ledger::Solve::IvNewtonIters), initial_iv);
}

} // namespace
} // namespace atx::vol
