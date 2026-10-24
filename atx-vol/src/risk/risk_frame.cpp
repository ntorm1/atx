// RiskFrame — sizing and reset of the per-state Greek frame.

#include "atx/vol/api/risk/risk_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace atx::vol {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

void RiskFrame::reserve(std::size_t n_unique, std::size_t n_uids, std::size_t n_shares) {
  // vector::resize never releases capacity, so a steady-state re-reserve for the
  // same book allocates nothing (the frame's grow-only contract).
  for (std::vector<double> *col : {&T, &sigma, &forward, &q_eff, &rate, &price, &delta, &gamma,
                                   &vega, &theta, &rho, &vanna, &volga, &charm, &vendor_price,
                                   &vendor_delta}) {
    col->resize(n_unique);
  }
  route.resize(n_unique);
  flags.resize(n_unique);
  status.resize(n_unique);
  uid_spot.resize(n_uids);
  uid_flat_rate.resize(n_uids);
  uid_status.resize(n_uids);
  share_mark.resize(n_shares);
  share_status.resize(n_shares);
}

void RiskFrame::clear_values() noexcept {
  for (std::vector<double> *col : {&T, &sigma, &forward, &q_eff, &rate, &price, &delta, &gamma,
                                   &vega, &theta, &rho, &vanna, &volga, &charm, &vendor_price,
                                   &vendor_delta, &uid_spot, &uid_flat_rate, &share_mark}) {
    std::fill(col->begin(), col->end(), kNaN);
  }
  std::fill(route.begin(), route.end(), RiskRoute::None);
  std::fill(flags.begin(), flags.end(), RiskFlags{0});
  std::fill(status.begin(), status.end(), RiskLaneStatus::Refused);
  std::fill(uid_status.begin(), uid_status.end(), RiskLaneStatus::Refused);
  std::fill(share_status.begin(), share_status.end(), RiskLaneStatus::Refused);
  counters = FrameCounters{};
}

} // namespace atx::vol
