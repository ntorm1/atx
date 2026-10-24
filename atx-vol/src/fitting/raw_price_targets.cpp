#include "fitting/raw_price_targets.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace atx::vol::detail {
namespace {

[[nodiscard]] bool valid(const RawPriceDraft &draft, RawPriceDraftTarget target) {
  if ((target != RawPriceDraftTarget::OriginalMid &&
       target != RawPriceDraftTarget::FeasibleBoxMid) || draft.projected ||
      draft.prepared.prices || draft.prepared.curve ||
      draft.prepared.fit_status == QuoteSliceFitStatus::Success || draft.nodes.size() < 2u ||
      draft.raw_targets.size() != draft.nodes.size() || draft.boxes.size() != draft.nodes.size() ||
      draft.terminal_prepaid.size() != draft.nodes.size()) return false;
  const double spot = draft.prepared.market.spot;
  const double discount = draft.prepared.diagnostics.discount;
  if (!std::isfinite(spot) || spot <= 0 || !std::isfinite(discount) || discount <= 0) return false;
  for (std::size_t i = 0; i < draft.nodes.size(); ++i) {
    const auto &raw = draft.raw_targets[i];
    const auto &box = draft.boxes[i];
    if (!std::isfinite(raw.strike) || raw.strike <= 0 ||
        raw.strike != draft.nodes[i].strike || !std::isfinite(raw.call) ||
        !std::isfinite(raw.put) || !std::isfinite(draft.terminal_prepaid[i]) ||
        draft.terminal_prepaid[i] < 0 || !std::isfinite(box.call_lo) ||
        !std::isfinite(box.call_hi) || !std::isfinite(box.put_lo) ||
        !std::isfinite(box.put_hi) || box.call_lo > box.call_hi || box.put_lo > box.put_hi) {
      return false;
    }
  }
  return true;
}

} // namespace

Status select_raw_price_targets(RawPriceDraft &draft, RawPriceDraftTarget target) {
  if (!valid(draft, target)) {
    return atx::core::Err(ErrorCode::InvalidArgument, "raw target selector: invalid draft or mode");
  }
  auto nodes = draft.raw_targets;
  if (target == RawPriceDraftTarget::FeasibleBoxMid) {
    const double spot = draft.prepared.market.spot;
    const double discount = draft.prepared.diagnostics.discount;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const double strike = nodes[i].strike;
      const double prepaid = draft.terminal_prepaid[i];
      const double funded = discount * strike;
      if (!std::isfinite(funded)) {
        return atx::core::Err(ErrorCode::InvalidArgument, "raw target selector: invalid funding");
      }
      const auto &box = draft.boxes[i];
      const double call_lo = std::max({box.call_lo, 0.0, spot - strike, prepaid - funded});
      const double put_lo = std::max({box.put_lo, 0.0, strike - spot, funded - prepaid});
      if (call_lo > box.call_hi || put_lo > box.put_hi) {
        return atx::core::Err(ErrorCode::OutOfRange, "raw target selector: empty interval");
      }
      nodes[i].call = .5 * call_lo + .5 * box.call_hi;
      nodes[i].put = .5 * put_lo + .5 * box.put_hi;
    }
  }
  draft.nodes = std::move(nodes);
  return atx::core::Ok();
}

} // namespace atx::vol::detail
