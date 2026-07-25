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
// FIX-5 (final review, Minor) — the residue below is CLOSED, and the trade it
// described was a false dichotomy. It used to read:
//
//   KNOWN RESIDUE: `side` columns are declared `forcecast`, so a float array is
//   truncated to int32 BEFORE this decoder sees it. [...] Dropping `forcecast`
//   would fix it and would also reject the very common int64 column, which is a
//   worse trade.
//
// Both halves of that were true. Verified on this host, numpy 1.26.4 /
// pybind11 numpy.h: `array_t<T, ExtraFlags>::raw_array_t` calls `PyArray_FromAny`
// with `NPY_ARRAY_ENSUREARRAY | ExtraFlags`, so without NPY_ARRAY_FORCECAST the
// conversion is held to numpy's default SAFE casting — and
// `np.can_cast(int64, int32, 'safe')` is False, so an int64 column really would be
// rejected. With forcecast, `np.array([0.5]).astype(int32)` is `0`, i.e. exactly
// `int(Side.CALL)`, and by the time `decode_side` sees it the float origin is gone.
//
// But those were never the only two options. The array can be accepted UNTYPED, so
// the caller's own dtype is still visible, and rejected on its KIND before any cast
// — `as_side_codes` below. int64 (kind 'i') keeps working; float64 (kind 'f') is
// rejected with the same coded AtxError; the cost is one branch per CALL, not per
// element. Every binding that takes a `side` column now goes through it.

#include <cstddef>
#include <cstdint>
#include <string>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "atx/core/error.hpp"
#include "atx/vol/types.hpp"
#include "result.hpp"

namespace atxvol::python {

namespace py = pybind11;

// The int32 view every `side` consumer wants. Kept as one alias so the forcecast
// flag lives in exactly one place.
using SideCodes = py::array_t<std::int32_t, py::array::c_style | py::array::forcecast>;

// Validate the caller's dtype KIND, then cast. Integer ('i'), unsigned ('u') and
// boolean ('b') kinds are accepted — every one of them represents its values
// exactly, so the cast to int32 cannot invent a valid Side code that the caller did
// not write. A float column is refused here rather than silently truncated.
//
// The parameter is `py::object`, not `py::array`: pybind11's caster for a bare
// `py::array` argument is the generic object caster (`isinstance<array>` — it does
// NOT convert), which would newly reject the plain Python lists that `forcecast`
// used to accept. `py::array::ensure` reproduces the old acceptance surface (a list
// of ints materializes as an int64 array, kind 'i', and is admitted; a list of
// floats materializes as float64 and is refused, which is the point).
[[nodiscard]] inline SideCodes as_side_codes(const py::object &src,
                                             const char *what = "side") {
  const py::array side = py::array::ensure(src);
  if (!side) {
    throw py::value_error(std::string(what) + " must be an array or array-like");
  }
  const char kind = side.dtype().kind();
  if (kind != 'i' && kind != 'u' && kind != 'b') {
    throw AtxException(atx::core::Error{
        atx::core::ErrorCode::InvalidArgument,
        std::string(what) + " must have an integer dtype; got dtype kind '" +
            std::string(1, kind) +
            "'. A float column is truncated toward zero on the way in, so 0.5 would "
            "arrive as int(Side.CALL) == 0 and price silently as a call. Cast the "
            "column yourself (e.g. .astype(np.int32)) so the rounding is yours."});
  }
  SideCodes out = SideCodes::ensure(side);
  if (!out) {
    throw py::value_error(std::string(what) +
                          " could not be converted to a contiguous int32 array");
  }
  return out;
}

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
