#pragma once

// PY-3: the one NaN + per-lane-status convention every vectorized binding uses.
//
// The C++ layer already works this way (`batch.hpp` "SoA, parallel status": NaN
// in the value slot, the lane's `Error` in a parallel status span, and the
// function's own return reserved for argument validation). The bindings must not
// erase it by raising on the first bad lane — production chains routinely carry
// uninvertible quotes, and discarding every good lane makes the vectorized path
// useless for exactly the workload it exists for.
//
// Python contract:
//   values[i] is NaN and status[i] != STATUS_OK   -> that lane failed
//   status[i] == STATUS_OK                        -> that lane converged
//   otherwise status[i] == int(ErrorCode)         -> why it failed
// A raised exception from one of these functions means the CALL was malformed
// (shape mismatch, wrong rank) — never that one lane misbehaved.

#include <cstdint>
#include <span>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "atx/core/error.hpp"

namespace atxvol::python {

// `ErrorCode::Unknown` is 0, so success needs a value outside the enum.
inline constexpr std::int32_t kStatusOk = -1;

[[nodiscard]] inline std::int32_t status_code(const atx::core::Status &status) noexcept {
  return status ? kStatusOk : static_cast<std::int32_t>(status.error().code());
}

// Fold a parallel `Status` span into the int32 array handed back to Python.
[[nodiscard]] inline pybind11::array_t<std::int32_t>
to_status_array(std::span<const atx::core::Status> statuses) {
  pybind11::array_t<std::int32_t> out(static_cast<pybind11::ssize_t>(statuses.size()));
  auto *data = out.mutable_data();
  for (std::size_t i = 0; i < statuses.size(); ++i) {
    data[i] = status_code(statuses[i]);
  }
  return out;
}

} // namespace atxvol::python
