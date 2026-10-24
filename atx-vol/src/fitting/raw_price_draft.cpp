#include "fitting/raw_price_draft.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

#include "fitting/quote_slice_call_floor.hpp"

namespace atx::vol::detail {
namespace {

RawPriceDraftResult refused(RawPriceDraftRefusal why, const char *message) {
  return {why, message, std::nullopt};
}

bool valid_quote(const QuoteSliceQuote &q) {
  return std::isfinite(q.strike) && q.strike > 0.0 &&
      (q.side == Side::Call || q.side == Side::Put) && std::isfinite(q.bid) &&
      std::isfinite(q.ask) && q.bid >= 0.0 && q.ask > 0.0 && q.bid <= q.ask;
}

double recovered_ask(RawPriceDraft &draft, const QuoteSliceQuote &quote,
                      std::size_t node, bool recover) {
  if (!recover) return quote.ask;
  const double spot = draft.prepared.market.spot;
  const double prepaid = draft.terminal_prepaid[node];
  const double funded_strike = draft.prepared.diagnostics.discount * quote.strike;
  const bool call = quote.side == Side::Call;
  const double intrinsic = call ? spot - quote.strike : quote.strike - spot;
  const double european = call ? prepaid - funded_strike : funded_strike - prepaid;
  const double floor = std::max({0.0, intrinsic, european});
  if (quote.ask >= floor) return quote.ask;
  const double allowance = 1e-8 + 1e-9 * std::max(spot, quote.strike);
  const double raised = floor + allowance;
  ++draft.recovery.raised_asks;
  draft.recovery.max_ask_raise = std::max(draft.recovery.max_ask_raise, raised - quote.ask);
  return raised;
}

} // namespace

RawPriceDraftResult make_raw_price_draft(const QuoteSliceMarket &market,
    std::span<const QuoteSliceQuote> quotes, const QuoteSliceConfig &config,
    std::span<const CashDividend> dividends) {
  if (config.dividend_model == QuoteSliceDividendModel::DatedCash || !dividends.empty()) {
    return refused(RawPriceDraftRefusal::UnsupportedModel, "raw draft requires continuous carry");
  }
  if (config.carry.mode != QuoteSliceCarryMode::Supplied) {
    return refused(RawPriceDraftRefusal::UnsupportedCarryPolicy, "raw draft needs resolved carry");
  }
  if (quotes.empty() || !std::all_of(quotes.begin(), quotes.end(), valid_quote)) {
    return refused(RawPriceDraftRefusal::InvalidQuote, "raw draft cannot discard invalid quotes");
  }
  RawPriceDraft draft;
  draft.original_quotes.assign(quotes.begin(), quotes.end());
  for (const auto &q : quotes) draft.nodes.push_back({q.strike, 0.0, 0.0});
  std::sort(draft.nodes.begin(), draft.nodes.end(), [](const auto &a, const auto &b) {
    return a.strike < b.strike;
  });
  const auto last = std::unique(draft.nodes.begin(), draft.nodes.end(),
      [](const auto &a, const auto &b) { return a.strike == b.strike; });
  draft.nodes.erase(last, draft.nodes.end());
  if (draft.nodes.size() < 2u) {
    return refused(RawPriceDraftRefusal::InvalidInput, "raw draft needs two distinct strikes");
  }
  const auto prepared = prepare_raw_price_draft_metadata(draft, market, config, dividends);
  if (!prepared) {
    RawPriceDraftResult result;
    result.refusal = prepared.error().code() == ErrorCode::NotImplemented
        ? RawPriceDraftRefusal::UnsupportedRegime : RawPriceDraftRefusal::InvalidInput;
    result.error = prepared.error().to_string();
    return result;
  }
  constexpr double inf = std::numeric_limits<double>::infinity();
  draft.boxes.assign(draft.nodes.size(), AmericanPriceBox{0.0, inf, 0.0, inf});
  auto original_boxes = draft.boxes;
  std::vector<std::uint8_t> sides(draft.nodes.size(), 0u);
  draft.quote_nodes.reserve(quotes.size());
  for (const auto &q : quotes) {
    const auto at = std::lower_bound(draft.nodes.begin(), draft.nodes.end(), q.strike,
        [](const auto &node, double k) { return node.strike < k; });
    const auto i = static_cast<std::size_t>(at - draft.nodes.begin());
    draft.quote_nodes.push_back(i);
    const bool call = q.side == Side::Call;
    sides[i] = static_cast<std::uint8_t>(sides[i] | (call ? 1u : 2u));
    auto &box = draft.boxes[i];
    auto &original = original_boxes[i];
    (call ? original.call_lo : original.put_lo) =
        std::max(call ? original.call_lo : original.put_lo, q.bid);
    (call ? original.call_hi : original.put_hi) =
        std::min(call ? original.call_hi : original.put_hi, q.ask);
    double &lo = call ? box.call_lo : box.put_lo;
    double &hi = call ? box.call_hi : box.put_hi;
    lo = std::max(lo, q.bid);
    hi = std::min(hi, recovered_ask(draft, q, i, config.repair.terminal_floor_recovery));
  }
  for (std::size_t i = 0; i < draft.nodes.size(); ++i) {
    if (sides[i] != 3u) {
      return refused(RawPriceDraftRefusal::MissingPairedSide, "raw draft requires paired sides");
    }
    auto &box = draft.boxes[i];
    box.call_lo = std::max(box.call_lo,
        call_floor_at(draft.prepared.diagnostics.call_floor_lines, draft.nodes[i].strike));
    if (box.call_lo > box.call_hi || box.put_lo > box.put_hi) {
      return refused(RawPriceDraftRefusal::EmptyQuoteIntersection,
                     "raw draft has conflicting duplicate quotes or a declared call floor");
    }
    // Recovery changes admissible bounds, never the original target. Duplicate
    // quotes use the midpoint of their original intersection deterministically.
    const auto &original = original_boxes[i];
    draft.nodes[i].call = 0.5 * original.call_lo + 0.5 * original.call_hi;
    draft.nodes[i].put = 0.5 * original.put_lo + 0.5 * original.put_hi;
  }
  draft.raw_targets = draft.nodes;
  return {RawPriceDraftRefusal::None, {}, std::move(draft)};
}

JointProjectionResult project_raw_price_drafts(std::span<RawPriceDraft> drafts,
    std::size_t max_iterations, JointInboxDiagnostics *diagnostics) {
  if (diagnostics != nullptr) *diagnostics = {};
  if (drafts.empty()) return {};
  std::vector<std::size_t> order(drafts.size());
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return drafts[a].prepared.market.calendar_years < drafts[b].prepared.market.calendar_years;
  });
  std::vector<std::vector<AmericanPriceNode>> nodes(drafts.size());
  std::vector<JointInboxSlice> slices;
  slices.reserve(drafts.size());
  for (std::size_t s = 0; s < order.size(); ++s) {
    const auto &draft = drafts[order[s]];
    if (s > 0u && !(draft.prepared.market.calendar_years >
                   drafts[order[s - 1u]].prepared.market.calendar_years)) return {};
    nodes[s] = draft.nodes;
    slices.push_back({nodes[s], draft.boxes, draft.bounds,
                     draft.prepared.diagnostics.discount, draft.terminal_prepaid});
  }
  auto result = project_joint_in_box(slices, max_iterations, diagnostics);
  if (result.outcome != JointProjectionOutcome::Feasible) return result;
  std::vector<AmericanPriceSlice> certified;
  certified.reserve(drafts.size());
  for (std::size_t s = 0; s < slices.size(); ++s) {
    auto price = AmericanPriceSlice::create(nodes[s], slices[s].bounds);
    if (!price) {
      result.outcome = JointProjectionOutcome::NonConverged;
      return result;
    }
    certified.push_back(std::move(*price));
  }
  for (std::size_t s = 0; s < order.size(); ++s) {
    auto &draft = drafts[order[s]];
    draft.nodes = std::move(nodes[s]);
    draft.projected = std::move(certified[s]);
  }
  return result;
}

} // namespace atx::vol::detail
