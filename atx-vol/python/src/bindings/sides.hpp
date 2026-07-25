#pragma once

// I2: ONE decoder for the `side` int column, shared by every binding that takes
// one.
//
// Three bindings had independently written the same ternary —
// `v == int(Side::Put) ? Side::Put : Side::Call` — which tests only for Put and
// therefore maps EVERYTHING ELSE onto Call. `int(Side.CALL) == 0` and
// `int(Side.PUT) == 1`, so the `+1 call / -1 put` convention that external chain
// data commonly uses made every leg of an imported book price as a call, with
// `status == STATUS_OK` and no diagnostic anywhere. Measured on the pre-fix
// build: `np.full(n, 77)` and `np.full(n, 0.5)` (truncated to 0 by `forcecast`)
// both priced silently as calls.
//
// An unrecognised code is a caller contract violation, so it raises a CODED
// error — `atxvol.AtxError` carrying `ErrorCode.INVALID_ARGUMENT` — rather than a
// bare ValueError. That is PY-1's whole point: a caller dispatches on
// `err.code`, not on prose. (Shape/rank errors keep raising `ValueError`: those
// are malformed *calls*, not a rejected value.)
//
// KNOWN RESIDUE: `side` columns are declared `forcecast`, so a float array is
// truncated to int32 BEFORE this decoder sees it. A float `-1` still arrives as
// -1 and is rejected, but a float that truncates onto a VALID code — 0.5 -> 0 —
// is by then indistinguishable from a genuine CALL. Pass `side` with an integer
// dtype. Dropping `forcecast` would fix it and would also reject the very common
// int64 column, which is a worse trade.

#include <cstddef>
#include <cstdint>
#include <string>

#include "atx/core/error.hpp"
#include "atx/vol/types.hpp"
#include "result.hpp"

namespace atxvol::python {

[[nodiscard]] inline atx::vol::Side decode_side(std::int32_t code, std::size_t index) {
  if (code == static_cast<std::int32_t>(atx::vol::Side::Call)) {
    return atx::vol::Side::Call;
  }
  if (code == static_cast<std::int32_t>(atx::vol::Side::Put)) {
    return atx::vol::Side::Put;
  }
  throw AtxException(atx::core::Error{
      atx::core::ErrorCode::InvalidArgument,
      "side[" + std::to_string(index) + "] = " + std::to_string(code) +
          " is not a Side: expected int(Side.CALL) == " +
          std::to_string(static_cast<std::int32_t>(atx::vol::Side::Call)) +
          " or int(Side.PUT) == " +
          std::to_string(static_cast<std::int32_t>(atx::vol::Side::Put)) +
          ". A +1/-1 side convention is NOT silently accepted — map it before "
          "handing the column over, or every leg would price as a call."});
}

} // namespace atxvol::python
