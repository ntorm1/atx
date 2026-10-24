#pragma once

// Helpers shared by the lane-PY binding TUs (2026-10-23): the enum-name registry the
// reflection tests read, the fitted-curve handle, and read-only zero-copy numpy views.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "sides.hpp"

namespace atxvol::python {

// ── numpy column inputs (the pricing.cpp batch conventions) ─────────────────────────

using F64Column = pybind11::array_t<double, pybind11::array::c_style | pybind11::array::forcecast>;

inline std::span<const double> column(const F64Column &array, const char *name,
                                      std::size_t expected, const char *ref) {
  if (array.ndim() != 1) {
    throw pybind11::value_error(std::string{name} + " must be a one-dimensional array");
  }
  if (static_cast<std::size_t>(array.size()) != expected) {
    throw pybind11::value_error(std::string{name} + " must have the same length as " + ref);
  }
  return {array.data(), static_cast<std::size_t>(array.size())};
}

// A bare Side broadcasts; otherwise an integer column of Side codes (sides.hpp refuses
// float kinds and unknown codes), one per lane.
inline std::vector<atx::vol::Side> side_column(const pybind11::object &raw, std::size_t expected,
                                               const char *ref) {
  if (pybind11::isinstance<atx::vol::Side>(raw)) {
    return std::vector<atx::vol::Side>(expected, raw.cast<atx::vol::Side>());
  }
  const SideCodes array = as_side_codes(raw);
  if (array.ndim() != 1) throw pybind11::value_error("side must be a one-dimensional array");
  if (static_cast<std::size_t>(array.size()) != expected) {
    throw pybind11::value_error(std::string{"side must have the same length as "} + ref);
  }
  std::vector<atx::vol::Side> out(expected);
  const auto *data = array.data();
  for (std::size_t i = 0; i < expected; ++i) out[i] = decode_side(data[i], i);
  return out;
}

// ── Enum reflection ──────────────────────────────────────────────────────────────
//
// Every bound enum whose C++ side offers `to_string` registers here under its Python
// name. `_core._enum_names(name)` then answers {value: C++ name} for EVERY value of the
// underlying type that to_string names, so a test can prove the Python enum lists every
// current enumerator: an enumerator added in C++ and not bound shows up as a named value
// the Python enum lacks.

using EnumNames = std::map<std::uint64_t, std::string>;

inline std::map<std::string, std::function<EnumNames()>> &enum_name_registry() {
  static std::map<std::string, std::function<EnumNames()>> registry;
  return registry;
}

template <class E> void register_enum_names(const std::string &python_name) {
  static_assert(std::is_enum_v<E>);
  enum_name_registry()[python_name] = [] {
    using U = std::underlying_type_t<E>;
    static_assert(std::is_unsigned_v<U>);
    const std::uint64_t hi =
        std::min<std::uint64_t>(std::numeric_limits<U>::max(), std::uint64_t{65535});
    EnumNames out;
    for (std::uint64_t v = 0; v <= hi; ++v) {
      // A fixed underlying type makes every value of U a valid E (no UB in the cast).
      const std::string name{to_string(static_cast<E>(static_cast<U>(v)))};
      if (name != "unknown" && name != "Unrecognized") out.emplace(v, name);
    }
    return out;
  };
}

// ── Fitted curve handle ──────────────────────────────────────────────────────────
//
// A read-only IVolCurve shared with whatever owns it (a decoded archive fit, a serving
// record): the aliasing shared_ptr keeps that owner alive for the handle's lifetime.
struct CurveHandle {
  std::shared_ptr<const atx::vol::IVolCurve> curve;
};

// ── Read-only numpy views ────────────────────────────────────────────────────────

// Wrap `count` elements of `stride` bytes at `data` as a read-only array whose base is
// `base` (an object that keeps the bytes alive). An empty span gives an empty array.
inline pybind11::array readonly_view(const pybind11::dtype &dtype, const void *data,
                                     std::size_t count, std::size_t stride,
                                     const pybind11::object &base) {
  static const double kEmpty = 0.0;
  const void *ptr = count == 0 ? static_cast<const void *>(&kEmpty) : data;
  // SAFETY: numpy's constructor takes a non-const pointer; the array is flagged
  // read-only before it is returned, so the borrowed bytes are never written.
  pybind11::array out(dtype, {static_cast<pybind11::ssize_t>(count)},
                      {static_cast<pybind11::ssize_t>(stride)}, const_cast<void *>(ptr), base);
  out.attr("setflags")(pybind11::arg("write") = false);
  return out;
}

} // namespace atxvol::python
