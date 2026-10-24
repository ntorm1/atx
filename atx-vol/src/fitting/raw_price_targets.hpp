#pragma once

#include <cstdint>

#include "fitting/raw_price_draft.hpp"

namespace atx::vol::detail {

// Private target construction. Neither selector changes original quotes or boxes.
enum class RawPriceDraftTarget : std::uint8_t { OriginalMid, FeasibleBoxMid };

// Before projection only. FeasibleBoxMid centers each effective quote interval
// after intersecting its lower edge with proven intrinsic/terminal floors.
// Already selected C3 floors and explicitly recovered asks remain in the boxes.
// Invalid/infeasible input leaves the complete draft unchanged. Original
// raw_targets are immutable provenance, even after selecting another target.
// OutOfRange specifically proves an empty floor/quote interval; other malformed
// inputs use InvalidArgument. Neither failure mutates any node.
[[nodiscard]] Status select_raw_price_targets(RawPriceDraft &draft,
                                               RawPriceDraftTarget target);

} // namespace atx::vol::detail
