#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include "atx/core/error.hpp"

namespace atxvol::python {

class AtxException : public std::runtime_error {
public:
  explicit AtxException(const atx::core::Error &error)
      : std::runtime_error(error.to_string()), code_(error.code()) {}

  [[nodiscard]] atx::core::ErrorCode code() const noexcept { return code_; }

private:
  atx::core::ErrorCode code_;
};

template <class T> [[nodiscard]] T unwrap(atx::core::Result<T> result) {
  if (!result) {
    throw AtxException(result.error());
  }
  return std::move(*result);
}

inline void unwrap(atx::core::Status status) {
  if (!status) {
    throw AtxException(status.error());
  }
}

} // namespace atxvol::python
