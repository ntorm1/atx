#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "atx/vol/api/pricing/american.hpp"

namespace atxvol::python {

// Owning NumPy columns in native bit order: Delta..Charm, Price. Construction
// and publish require the GIL. write() touches only the hoisted native pointers,
// so a caller can fill independent rows while its pricing loop releases the GIL.
class GreekMetadataColumns {
public:
  explicit GreekMetadataColumns(std::size_t count)
      : requested_(static_cast<pybind11::ssize_t>(count)),
        supported_(static_cast<pybind11::ssize_t>(count)),
        reasons_(pybind11::array::ShapeContainer{
            static_cast<pybind11::ssize_t>(count), pybind11::ssize_t{9}}),
        requested_data_(requested_.mutable_data()), supported_data_(supported_.mutable_data()),
        reasons_data_(reasons_.mutable_data()), count_(count) {}

  void write(std::size_t i, atx::vol::GreekFieldMask requested,
             atx::vol::GreekFieldMask supported,
             const std::array<atx::vol::AmericanGreekReason, 9> &reasons) noexcept {
    assert(i < count_);
    requested_data_[i] = static_cast<std::uint32_t>(requested);
    supported_data_[i] = static_cast<std::uint32_t>(supported);
    for (std::size_t j = 0; j < reasons.size(); ++j)
      reasons_data_[9 * i + j] = static_cast<std::uint8_t>(reasons[j]);
  }

  void write(std::size_t i, const atx::vol::AmericanGreeks &g) noexcept {
    write(i, g.requested, g.supported, g.reasons);
  }

  void publish(pybind11::dict &out) && {
    out["requested"] = std::move(requested_);
    out["supported"] = std::move(supported_);
    out["reasons"] = std::move(reasons_);
  }

private:
  pybind11::array_t<std::uint32_t> requested_, supported_;
  pybind11::array_t<std::uint8_t> reasons_;
  std::uint32_t *requested_data_, *supported_data_;
  std::uint8_t *reasons_data_;
  [[maybe_unused]] std::size_t count_;
};

} // namespace atxvol::python
