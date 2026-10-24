#include "fitting/raw_price_draft.hpp"
#include "fitting/raw_price_targets.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "fitting/counters.hpp"
#include "fitting/deam_iv_cache.hpp"
#include "pricing/american_iv_search.hpp"

namespace atx::vol {
namespace {

QuoteSliceConfig final_price_config(bool native = false) {
  QuoteSliceConfig config;
  config.floor_policy = QuoteSliceFloorPolicy::PriceOnly;
  config.repair.certificate = QuoteSliceCertificate::Box;
  config.reimply_price_cap = 0.001;
  config.reimply_spread_fraction = 0.01;
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  config.channel.deam_cache = false;
  config.channel.deam_profile = QuoteSliceDeamProfile::Screen;
  config.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
  config.channel.deam_family = native ? QuoteSliceDeamFamily::Always : QuoteSliceDeamFamily::Off;
  config.channel.deam_family_q = native;
  config.channel.deam_family_native = native;
  config.channel.deam_native_route = native;
  config.channel.deam_native_batch = native;
  return config;
}

std::vector<QuoteSliceQuote> final_price_quotes(const QuoteSliceMarket &market) {
  std::vector<QuoteSliceQuote> quotes;
  for (double strike = 80; strike <= 120; strike += 5) {
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(market.spot, strike, market.variance_years, 0.25,
                                      market.rate, market.continuous_yield, side);
      if (!price) return {};
      quotes.push_back({strike, side, std::max(0.0, *price - 0.1), *price + 0.1});
    }
  }
  return quotes;
}

detail::RawPriceDraft projected_draft(const QuoteSliceMarket &market,
                                    std::span<const QuoteSliceQuote> quotes,
                                    const QuoteSliceConfig &config) {
  auto result = detail::make_raw_price_draft(market, quotes, config);
  EXPECT_TRUE(result.draft) << result.error;
  if (!result.draft) return {};
  auto draft = std::move(*result.draft);
  EXPECT_EQ(detail::project_raw_price_drafts(std::span(&draft, 1)).outcome,
            JointProjectionOutcome::Feasible);
  return draft;
}

std::string final_diagnostics_path() {
#ifdef _WIN32
  std::size_t length = 0;
  if (getenv_s(&length, nullptr, 0, "ATX_VOL_FINAL_PRICE_DIAGNOSTICS") != 0 || length == 0) {
    return {};
  }
  std::vector<char> value(length);
  if (getenv_s(&length, value.data(), value.size(), "ATX_VOL_FINAL_PRICE_DIAGNOSTICS") != 0) {
    return {};
  }
  return value.data();
#else
  const char *value = std::getenv("ATX_VOL_FINAL_PRICE_DIAGNOSTICS");
  return value == nullptr ? std::string{} : std::string{value};
#endif
}

// Independent diagnostic reconstruction from the original row's declared carry.
double diagnostic_effective_yield(const QuoteSliceFit &fit, const QuoteSliceQuote &quote) {
  const auto &m = fit.market;
  const double calendar_yield = quote.continuous_yield.value_or(m.continuous_yield);
  double q = calendar_yield * (m.calendar_years / m.variance_years);
  if (m.dividend_pv != 0 &&
      (fit.dividend_model == QuoteSliceDividendModel::AggregatePresentValue ||
       fit.dividend_model == QuoteSliceDividendModel::AggregateFundingPresentValue)) {
    const double fraction = fit.dividend_model == QuoteSliceDividendModel::AggregatePresentValue
        ? m.dividend_pv / m.spot
        : std::exp(std::log(m.dividend_pv) - std::log(m.spot) + calendar_yield * m.calendar_years);
    q -= std::log1p(-fraction) / m.variance_years;
  }
  return q;
}

TEST(QuoteSliceFinalPrice, PublishesOnlyFinalIdentificationsAndTruthfulCurveObservations) {
  const QuoteSliceMarket market{100, 0.5, 0.5, 0, 0, 0};
  auto quotes = final_price_quotes(market);
  ASSERT_EQ(quotes.size(), 18u);
  quotes.push_back(quotes[4]); // duplicate rows never trigger a second classification
  auto config = final_price_config();
  config.reimply_price_cap = 0;
  config.reimply_spread_fraction = 0; // no family/budget still retains published IDs
  auto draft = projected_draft(market, quotes, config);
  ASSERT_TRUE(draft.projected);
  const auto expected = draft.nodes;
  namespace timing = counters::fit_timing;
  timing::ChainRecorder publication;
  auto fit = [&] {
    const timing::ChainScope scope(&publication);
    return detail::finalize_raw_price_draft(std::move(draft), config);
  }();
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success);
  ASSERT_TRUE(fit->curve && fit->prices);
  EXPECT_EQ(fit->curve->kind(), VolCurveKind::ConvexDense);
  EXPECT_EQ(fit->rows.size(), quotes.size());
  EXPECT_EQ(fit->diagnostics.quotes_seen, quotes.size());
  EXPECT_EQ(fit->diagnostics.quotes_admitted, 0u);
  EXPECT_EQ(fit->diagnostics.iv_attempted, 0u);
  EXPECT_EQ(fit->diagnostics.final_price_observations, expected.size());
  EXPECT_EQ(fit->diagnostics.final_price_identified_nodes, 2 * expected.size());
  EXPECT_EQ(fit->diagnostics.prices_produced, quotes.size());
  EXPECT_EQ(publication.result().reimply_stage_calls[
                static_cast<std::size_t>(timing::ReimplyStage::Search)], 2 * expected.size());
  ASSERT_EQ(fit->moved_nodes.size(), expected.size());
  for (const auto mask : fit->moved_nodes) EXPECT_EQ(mask, 3u);
  timing::ChainRecorder queries;
  {
    const timing::ChainScope scope(&queries);
    for (std::size_t i = 0; i < quotes.size(); ++i) {
      const auto &row = fit->rows[i];
      EXPECT_EQ(row.quote_status, QuoteSliceQuoteStatus::ProjectedPrice);
      EXPECT_FALSE(row.admitted);
      EXPECT_TRUE(std::isnan(row.market_iv));
      EXPECT_EQ(row.evaluation_status, QuoteSliceEvaluationStatus::Success);
      EXPECT_EQ(row.vol_source, QuoteSliceVolConvention::ReimpliedRepaired);
      const auto answer = query_quote_slice(*fit, quotes[i].strike, quotes[i].side);
      ASSERT_TRUE(answer);
      EXPECT_EQ(answer->volatility, row.fitted_iv);
      EXPECT_EQ(answer->price, row.fitted_price);
      EXPECT_EQ(answer->vol_convention, vol_convention(row));
      const auto node = static_cast<std::size_t>((quotes[i].strike - 80) / 5);
      EXPECT_EQ(row.fitted_price,
                quotes[i].side == Side::Call ? expected[node].call : expected[node].put);
    }
  }
  EXPECT_EQ(queries.result().iv_newton_iters, 0u);
  EXPECT_EQ(queries.result().al_price_calls, 0u);
  EXPECT_STREQ(to_string(QuoteSliceQuoteStatus::ProjectedPrice), "projected_price");
  EXPECT_EQ(static_cast<unsigned>(QuoteSliceQuoteStatus::LowVegaWeight), 11u);
}

TEST(QuoteSliceFinalPrice, NativeScalarBatchMatchesUnbatchedFinalPricesAndColdRepricing) {
  struct RestoreIsa {
    simd::SimdIsa saved = simd::simd_isa_override();
    ~RestoreIsa() { simd::set_simd_isa_override(saved); }
  } restore;
  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  const QuoteSliceMarket market{100, 0.5, 0.5, 0.05, 0.01, 0};
  const auto quotes = final_price_quotes(market);
  auto config = final_price_config(true);
  config.channel.deam_native_batch = false;
  auto scalar = detail::finalize_raw_price_draft(projected_draft(market, quotes, config), config);
  config.channel.deam_native_batch = true;
  namespace timing = counters::fit_timing;
  timing::ChainRecorder publication;
  auto batch = [&] {
    auto draft = projected_draft(market, quotes, config);
    const timing::ChainScope scope(&publication);
    return detail::finalize_raw_price_draft(std::move(draft), config);
  }();
  ASSERT_TRUE(scalar && batch);
  EXPECT_GT(publication.result().reimply_batch_lanes, 0u);
  EXPECT_EQ(publication.result().reimply_batch_hits, publication.result().reimply_batch_lanes);
  EXPECT_EQ(batch->diagnostics.deam.inversions, 0u);
  EXPECT_EQ(batch->diagnostics.deam.model_prices, 0u);
  const auto bits = [](double value) { return std::bit_cast<std::uint64_t>(value); };
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    EXPECT_EQ(batch->rows[i].evaluation_status, scalar->rows[i].evaluation_status);
    EXPECT_EQ(bits(batch->rows[i].fitted_price), bits(scalar->rows[i].fitted_price));
    EXPECT_EQ(bits(batch->rows[i].fitted_iv), bits(scalar->rows[i].fitted_iv));
    if (batch->rows[i].evaluation_status != QuoteSliceEvaluationStatus::Success) continue;
    const auto price = andersen_lake(market.spot, quotes[i].strike, market.variance_years,
        batch->rows[i].fitted_iv, market.rate, market.continuous_yield, quotes[i].side);
    ASSERT_TRUE(price);
    EXPECT_LE(std::abs(*price - batch->rows[i].fitted_price), 0.001);
  }
}

TEST(QuoteSliceFinalPrice, RejectsUnprojectedCorruptOrReconfiguredDrafts) {
  const QuoteSliceMarket market{100, 0.5, 0.5, 0, 0, 0};
  const auto quotes = final_price_quotes(market);
  auto config = final_price_config();
  auto unprojected = detail::make_raw_price_draft(market, quotes, config);
  ASSERT_TRUE(unprojected.draft);
  EXPECT_FALSE(detail::finalize_raw_price_draft(std::move(*unprojected.draft), config));
  auto corrupt = projected_draft(market, quotes, config);
  corrupt.quote_nodes[0] = corrupt.nodes.size();
  EXPECT_FALSE(detail::finalize_raw_price_draft(std::move(corrupt), config));
  auto wrong_bounds = projected_draft(market, quotes, config);
  wrong_bounds.bounds.spot += 1;
  EXPECT_FALSE(detail::finalize_raw_price_draft(std::move(wrong_bounds), config));
  auto changed = projected_draft(market, quotes, config);
  config.reimply_price_cap *= 2;
  EXPECT_FALSE(detail::finalize_raw_price_draft(std::move(changed), config));
}

TEST(QuoteSliceFinalPrice, NativeAvx2FinalTargetsKeepColdPriceAccuracyAndCachedQueryMetadata) {
  if (!simd::use_avx2()) GTEST_SKIP() << "AVX2 unavailable";
  const QuoteSliceMarket market{100, 0.5, 0.5, 0.05, 0.01, 0};
  const auto quotes = final_price_quotes(market);
  const auto config = final_price_config(true);
  auto draft = projected_draft(market, quotes, config);
  namespace timing = counters::fit_timing;
  timing::ChainRecorder publication;
  auto fit = [&] {
    const timing::ChainScope scope(&publication);
    return detail::finalize_raw_price_draft(std::move(draft), config);
  }();
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_GT(publication.result().reimply_batch_avx2_calls, 0u);
  EXPECT_EQ(publication.result().reimply_batch_hits, publication.result().reimply_batch_lanes);
  deam_iv_memo_clear();
  timing::ChainRecorder queries;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto answer = [&] {
      const timing::ChainScope scope(&queries);
      return query_quote_slice(*fit, quotes[i].strike, quotes[i].side);
    }();
    ASSERT_TRUE(answer);
    const auto &row = fit->rows[i];
    EXPECT_EQ(answer->status, row.evaluation_status);
    EXPECT_EQ(answer->reason, row.price_only_reason);
    EXPECT_EQ(answer->vol_convention, vol_convention(row));
    EXPECT_EQ(answer->price, row.fitted_price);
    if (row.evaluation_status != QuoteSliceEvaluationStatus::Success) continue;
    EXPECT_EQ(answer->volatility, row.fitted_iv);
    const auto cold = andersen_lake(market.spot, quotes[i].strike, market.variance_years,
        answer->volatility, market.rate, market.continuous_yield, quotes[i].side);
    ASSERT_TRUE(cold);
    EXPECT_LE(std::abs(*cold - answer->price), 0.001);
  }
  EXPECT_EQ(queries.result().al_price_calls, 0u);
  EXPECT_EQ(queries.result().iv_newton_iters, 0u);
}

TEST(QuoteSliceFinalPrice, LockedFloorAndBidlessRowsStayServedWithTypedPriceOnlyMetadata) {
  const QuoteSliceMarket market{100, 0.5, 0.5, 0, 0, 0};
  auto quotes = final_price_quotes(market);
  quotes.push_back({10, Side::Call, 90, 90});
  quotes.push_back({10, Side::Put, 0, 2e-8});
  const auto config = final_price_config();
  auto fit = detail::finalize_raw_price_draft(projected_draft(market, quotes, config), config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &floor = fit->rows[quotes.size() - 2];
  EXPECT_EQ(floor.quote_status, QuoteSliceQuoteStatus::ProjectedPrice);
  EXPECT_EQ(floor.evaluation_status, QuoteSliceEvaluationStatus::PriceOnly);
  EXPECT_EQ(floor.price_only_reason, QuoteSlicePriceOnlyReason::FloorVol);
  EXPECT_EQ(floor.fitted_price, 90);
  EXPECT_TRUE(std::isnan(floor.fitted_iv));
  const auto answer = query_quote_slice(*fit, 10, Side::Call);
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer->status, floor.evaluation_status);
  EXPECT_EQ(answer->reason, floor.price_only_reason);
  EXPECT_EQ(answer->iv_lower, floor.iv_lower);
  EXPECT_EQ(answer->iv_upper, floor.iv_upper);
  EXPECT_FALSE(evaluate_quote_slice(*fit, 10, Side::Call));
  EXPECT_EQ(fit->rows.back().quote_status, QuoteSliceQuoteStatus::ProjectedPrice);
  EXPECT_TRUE(std::isfinite(fit->rows.back().fitted_price));
  EXPECT_EQ(fit->diagnostics.prices_produced, quotes.size());
}

TEST(QuoteSliceFinalPrice, UsesProjectedPricesAndRefusesInsufficientFinalCurveEvidence) {
  const QuoteSliceMarket market{100, 0.5, 0.5, 0, 0, 0};
  auto quotes = final_price_quotes(market);
  for (auto &quote : quotes) {
    const double mid = 0.5 * (quote.bid + quote.ask);
    quote.bid = std::max(0.0, mid - 2);
    quote.ask = mid + 2;
    if (quote.strike == 100) {
      quote.bid = mid + 0.8;
      quote.ask = mid + 1.4;
    }
  }
  auto config = final_price_config();
  auto draft = projected_draft(market, quotes, config);
  ASSERT_TRUE(draft.projected);
  const auto projected = draft.nodes;
  auto fit = detail::finalize_raw_price_draft(std::move(draft), config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->diagnostics.box.outcome, QuoteSliceRepairOutcome::Applied);
  EXPECT_GT(fit->diagnostics.box.max_price_move, 0.0);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto node = static_cast<std::size_t>((quotes[i].strike - 80) / 5);
    EXPECT_EQ(fit->rows[i].fitted_price,
              quotes[i].side == Side::Call ? projected[node].call : projected[node].put);
  }
  config.min_quotes = 10;
  auto insufficient = projected_draft(market, final_price_quotes(market), config);
  EXPECT_FALSE(detail::finalize_raw_price_draft(std::move(insufficient), config));
}

void check_external_final_prices(detail::RawPriceDraftTarget target) {
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
  std::vector<detail::RawPriceDraft> drafts;
  std::vector<QuoteSliceConfig> configs;
  for (const auto &slice : fixture.at("slices")) {
    const auto &m = slice.at("market");
    QuoteSliceMarket market{m.at("spot"), m.at("variance_years"), m.at("calendar_years"),
                            m.at("rate"), m.at("continuous_yield"), m.at("dividend_pv")};
    std::vector<QuoteSliceQuote> quotes;
    for (const auto &quote : slice.at("quotes")) {
      quotes.push_back({quote.at("strike"),
          quote.at("is_call").get<bool>() ? Side::Call : Side::Put, quote.at("bid"),
          quote.at("ask"), quote.at("continuous_yield").get<double>()});
    }
    auto config = final_price_config(true);
    config.dividend_model = static_cast<QuoteSliceDividendModel>(
        slice.at("dividend_model_enum").get<int>());
    config.repair.terminal_floor_recovery = true;
    config.channel.deam_family = QuoteSliceDeamFamily::Adaptive;
    config.channel.deam_confirm_kernel = QuoteSliceDeamConfirmKernel::Fast;
    auto draft = detail::make_raw_price_draft(market, quotes, config);
    ASSERT_TRUE(draft.draft) << draft.error;
    ASSERT_TRUE(detail::select_raw_price_targets(*draft.draft, target));
    drafts.push_back(std::move(*draft.draft));
    configs.push_back(config);
  }
  ASSERT_EQ(drafts.size(), 33u);
  detail::JointInboxDiagnostics projection;
  const auto projected = detail::project_raw_price_drafts(drafts, 100, &projection);
  ASSERT_EQ(projected.outcome, JointProjectionOutcome::Feasible);
  const auto output_path = final_diagnostics_path();
  nlohmann::json diagnostic{{"schema", "raw-final-price-diagnostic-v1"},
      {"target", target == detail::RawPriceDraftTarget::OriginalMid
          ? "original_mid" : "feasible_box_mid"},
      {"input_sha256", fixture.at("input_sha256")},
      {"projection", {{"sweeps", projected.sweeps},
          {"primal_residual", projection.primal_residual},
          {"dual_residual", projection.dual_residual}, {"duality_gap", projection.duality_gap}}},
      {"rows", nlohmann::json::array()}};
  double distance = 0;
  std::size_t rows = 0;
  std::size_t identified = 0;
  std::size_t outside = 0;
  for (std::size_t s = 0; s < drafts.size(); ++s) {
    const auto quotes = drafts[s].original_quotes;
    const auto raw_targets = drafts[s].raw_targets;
    const auto quote_nodes = drafts[s].quote_nodes;
    const auto prepaid = drafts[s].terminal_prepaid;
    for (std::size_t node = 0; node < raw_targets.size(); ++node) {
      const double dc = drafts[s].nodes[node].call - raw_targets[node].call;
      const double dp = drafts[s].nodes[node].put - raw_targets[node].put;
      distance += dc * dc + dp * dp;
    }
    auto fit = detail::finalize_raw_price_draft(std::move(drafts[s]), configs[s]);
    ASSERT_TRUE(fit) << s << ": " << fit.error().to_string();
    ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << s;
    ASSERT_EQ(fit->rows.size(), quotes.size());
    EXPECT_EQ(fit->diagnostics.quotes_admitted, 0u);
    EXPECT_EQ(fit->diagnostics.iv_attempted, 0u);
    EXPECT_EQ(fit->diagnostics.deam.inversions, 0u);
    rows += quotes.size();
    for (std::size_t i = 0; i < quotes.size(); ++i) {
      const auto answer = query_quote_slice(*fit, quotes[i].strike, quotes[i].side);
      ASSERT_TRUE(answer) << s << ": " << i;
      EXPECT_EQ(answer->price, fit->rows[i].fitted_price);
      EXPECT_EQ(answer->status, fit->rows[i].evaluation_status);
      identified += answer->status == QuoteSliceEvaluationStatus::Success ? 1u : 0u;
      if (answer->price < quotes[i].bid - 1e-6 || answer->price > quotes[i].ask + 1e-6) ++outside;
      if (!output_path.empty()) {
        const auto node = quote_nodes[i];
        const bool call = quotes[i].side == Side::Call;
        const double intrinsic = std::max(call ? fit->market.spot - quotes[i].strike
                                               : quotes[i].strike - fit->market.spot, 0.0);
        const double funded = fit->diagnostics.discount * quotes[i].strike;
        const double terminal =
            std::max(call ? prepaid[node] - funded : funded - prepaid[node], 0.0);
        const double q = diagnostic_effective_yield(*fit, quotes[i]);
        const double budget = std::min(0.001, 0.01 * (quotes[i].ask - quotes[i].bid));
        nlohmann::json row{{"row_id", fixture.at("slices")[s].at("quotes")[i].at("row_id")},
            {"expiry", fixture.at("slices")[s].at("expiry")},
            {"chain_id", fixture.at("slices")[s].at("chain_id")}, {"strike", quotes[i].strike},
            {"side", call ? "call" : "put"}, {"bid", quotes[i].bid}, {"ask", quotes[i].ask},
            {"price", answer->price}, {"status", to_string(answer->status)},
            {"reason", to_string(answer->reason)}, {"iv", answer->volatility},
            {"intrinsic", intrinsic}, {"terminal_floor", terminal},
            {"raw_target", call ? raw_targets[node].call : raw_targets[node].put},
            {"spot", fit->market.spot}, {"variance_years", fit->market.variance_years},
            {"effective_rate", fit->diagnostics.effective_rate}, {"effective_yield", q},
            {"price_budget", budget}};
        if (answer->status == QuoteSliceEvaluationStatus::PriceOnly) {
          IvOpts options;
          const QuoteSliceConfig query;
          options.tol = query.iv_tolerance;
          options.max_iter = query.max_iv_iterations;
          options.al_opts = fit->al_opts;
          const auto candidate = american_implied_vol_price_candidate(answer->price,
              fit->market.spot, quotes[i].strike, fit->market.variance_years,
              fit->diagnostics.effective_rate, q, quotes[i].side, options, budget);
          row["independent_row_budget_fast_candidate"] = candidate ? nlohmann::json(*candidate)
                                                                   : nlohmann::json(nullptr);
        }
        diagnostic["rows"].push_back(std::move(row));
      }
    }
  }
  diagnostic["projection"]["squared_raw_target_distance"] = distance;
  diagnostic["identified_rows"] = identified;
  if (!output_path.empty()) {
    std::ofstream output(output_path);
    ASSERT_TRUE(output.good());
    output << diagnostic.dump(2);
  }
  EXPECT_EQ(rows, 12866u);
  EXPECT_EQ(outside, 15u);
  ::testing::Test::RecordProperty("identified_rows", static_cast<int>(identified));
  EXPECT_GE(identified, 12183u);
}

TEST(QuoteSliceFinalPrice, ExternalNormalizedQuotesFinalizeEveryProjectedSlice) {
  check_external_final_prices(detail::RawPriceDraftTarget::OriginalMid);
}

TEST(QuoteSliceFinalPrice, ExternalFeasibleBoxMidQuotesFinalizeEveryProjectedSlice) {
  check_external_final_prices(detail::RawPriceDraftTarget::FeasibleBoxMid);
}

} // namespace
} // namespace atx::vol
