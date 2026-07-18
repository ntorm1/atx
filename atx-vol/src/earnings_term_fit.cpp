#include "atx/vol/earnings_term_fit.hpp"

#include <algorithm>
#include <cmath>

#include "atx/vol/event_vol.hpp" // censored_total_variance

namespace atx::vol {

double censored_atm_vol(const CensorObsInput &o, double emove, double wcen_floor) noexcept {
  // censored_total_variance already floors at the fixed kWCenFloor; flooring
  // again against the caller-supplied wcen_floor lets a caller impose a
  // stricter bound without touching event_vol.hpp's own constant (see the
  // header's self-review notes).
  const double w_cen = censored_total_variance(o.w_dirty, o.n, emove);
  const double floored = std::max(w_cen, wcen_floor);
  return std::sqrt(floored / o.T);
}

} // namespace atx::vol
