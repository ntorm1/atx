#pragma once
// Bridges atx::core::Result<T> (tl::expected<T, Error>) to Python: returns the
// value on Ok, raises atxpy.AtxError on Err. The module registers an exception
// translator (see module.cpp) that maps AtxException to the Python AtxError type.

#include <stdexcept>
#include <utility>

#include "atx/core/error.hpp"

namespace atxpy {

// Carries an atx::core::Error's text across the C++->Python boundary. The
// translator registered in module.cpp turns this into atxpy.AtxError.
struct AtxException : std::runtime_error {
  explicit AtxException(const atx::core::Error &e) : std::runtime_error(e.to_string()) {}
};

template <class T> [[nodiscard]] T unwrap(atx::core::Result<T> r) {
  if (!r) {
    throw AtxException(r.error());
  }
  return std::move(*r);
}

} // namespace atxpy
