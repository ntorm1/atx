#pragma once

#include "atx/vol/api/fitting/quote_slice.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace quote_bench {

// Shared offline DTOs. These are populated only by the closed raw CSV parser.
struct InputRow {
  std::uint64_t id{};
  std::uint64_t chain{};
  atx::vol::QuoteSliceMarket market{};
  atx::vol::QuoteSliceQuote quote{};
  // Raw `ddiv` (undiscounted cash dividends to expiry). Read only by the opt-in
  // inferred-schedule dividend routing; NaN when the column is absent, the cell
  // is empty, or the cell is not a number. A malformed cell never aborts a run:
  // the default path ignores the column, and the opt-in schedule of its
  // snapshot is refused (non_finite_input) with the row counted.
  double dividend_sum{std::numeric_limits<double>::quiet_NaN()};
  bool dividend_sum_malformed{false};
  std::string input_status;
  // Underlier, deliverable root, canonical expiry day, canonical exact snapshot.
  std::array<std::string, 4> identity;
};

struct Prediction {
  double price{std::numeric_limits<double>::quiet_NaN()};
  double vol{std::numeric_limits<double>::quiet_NaN()};
  std::string status{"not_evaluated"};
  std::string quote_status{"not_attempted"};
  bool admitted{false};
  std::string resolved_profile;
  std::string policy_source;
  std::string configured_curve;
  std::string actual_curve;
  std::string vol_status{"not_evaluated"};
  std::string model{"quote-slice"};
  std::string grade; // graded admission only: A, B or C when served
};

} // namespace quote_bench
