// MarketState — one instant of the market: validation, the uid index, sigma
// resolution (marks first, then the surface) and the carry precedence.

#include "atx/vol/api/risk/market_state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kNpos = std::numeric_limits<std::size_t>::max();

[[nodiscard]] constexpr std::uint8_t side_rank(Side s) noexcept {
  return static_cast<std::uint8_t>(s);
}

// (expiry_ns, side, strike) order of a marks row. Strikes are validated finite > 0
// before any comparison, so `<` is a strict weak order here and value equality is
// bit equality.
[[nodiscard]] bool mark_key_less(std::int64_t ea, Side sa, double ka, std::int64_t eb, Side sb,
                                 double kb) noexcept {
  if (ea != eb) {
    return ea < eb;
  }
  if (sa != sb) {
    return side_rank(sa) < side_rank(sb);
  }
  return ka < kb;
}

[[nodiscard]] bool finite_positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }

[[nodiscard]] Status validate_marks(const ContractMarks &m) {
  const std::size_t n = m.sigma.size();
  if (m.strike.size() != n || m.expiry_ns.size() != n || m.side.size() != n) {
    return Err(ErrorCode::InvalidArgument, "MarketState: marks spans differ in length");
  }
  if ((!m.vendor_price.empty() && m.vendor_price.size() != n) ||
      (!m.vendor_delta.empty() && m.vendor_delta.size() != n)) {
    return Err(ErrorCode::InvalidArgument, "MarketState: vendor span length mismatch");
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (!finite_positive(m.strike[i])) {
      return Err(ErrorCode::InvalidArgument, "MarketState: marks strike not finite > 0");
    }
    if (m.side[i] != Side::Call && m.side[i] != Side::Put) {
      return Err(ErrorCode::InvalidArgument, "MarketState: marks side out of range");
    }
    if (i > 0 && mark_key_less(m.expiry_ns[i], m.side[i], m.strike[i], m.expiry_ns[i - 1],
                               m.side[i - 1], m.strike[i - 1])) {
      return Err(ErrorCode::InvalidArgument,
                 "MarketState: marks not sorted by (expiry_ns, side, strike)");
    }
  }
  return Ok();
}

[[nodiscard]] Status validate_underlier(const UnderlierState &u) {
  if (!finite_positive(u.spot) || !std::isfinite(u.flat_rate) || !std::isfinite(u.flat_q)) {
    return Err(ErrorCode::InvalidArgument,
               "MarketState: underlier spot must be finite > 0, rate / q finite");
  }
  if (u.carry != CarryFrom::State && u.carry != CarryFrom::Surface) {
    return Err(ErrorCode::InvalidArgument, "MarketState: CarryFrom out of range");
  }
  for (std::size_t i = 0; i < u.term_carry.size(); ++i) {
    const ExpiryCarry &row = u.term_carry[i];
    if (!std::isfinite(row.pv_divs) || !std::isfinite(row.rate) || !std::isfinite(row.q)) {
      return Err(ErrorCode::InvalidArgument, "MarketState: term_carry row not finite");
    }
    if (i > 0 && !(u.term_carry[i - 1].expiry_ns < row.expiry_ns)) {
      return Err(ErrorCode::InvalidArgument,
                 "MarketState: term_carry not strictly ascending by expiry_ns");
    }
  }
  for (const DividendEvent &d : u.cash_divs) {
    if (!std::isfinite(d.amount)) {
      return Err(ErrorCode::InvalidArgument, "MarketState: cash dividend amount not finite");
    }
  }
  return Ok();
}

// Index of the first marks row whose key equals (expiry, side, K), or npos.
[[nodiscard]] std::size_t find_mark(const ContractMarks &m, std::int64_t expiry, Side side,
                                    double K) noexcept {
  std::size_t lo = 0;
  std::size_t hi = m.sigma.size();
  while (lo < hi) { // lower_bound; bounded by log2(n) halvings
    const std::size_t mid = lo + (hi - lo) / 2u;
    if (mark_key_less(m.expiry_ns[mid], m.side[mid], m.strike[mid], expiry, side, K)) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  if (lo < m.sigma.size() && m.expiry_ns[lo] == expiry && m.side[lo] == side &&
      m.strike[lo] == K) {
    return lo;
  }
  return kNpos;
}

[[nodiscard]] const ExpiryCarry *find_term_row(std::span<const ExpiryCarry> rows,
                                               std::int64_t expiry) noexcept {
  const auto it = std::lower_bound(
      rows.begin(), rows.end(), expiry,
      [](const ExpiryCarry &r, std::int64_t e) noexcept { return r.expiry_ns < e; });
  return (it != rows.end() && it->expiry_ns == expiry) ? &*it : nullptr;
}

[[nodiscard]] ResolvedContract refused(RiskLaneStatus status, RiskFlags flags) noexcept {
  ResolvedContract rc;
  rc.T = rc.sigma = rc.forward = rc.q_eff = rc.rate = rc.spot = kNaN;
  rc.vendor_price = rc.vendor_delta = kNaN;
  rc.flags = flags;
  rc.status = status;
  return rc;
}


// Index of `uid` in the uid-sorted underlier vector, or kNpos.
[[nodiscard]] std::size_t find_slot(std::span<const UnderlierState> sorted,
                                    std::uint32_t uid) noexcept {
  const auto it = std::lower_bound(
      sorted.begin(), sorted.end(), uid,
      [](const UnderlierState &u, std::uint32_t v) noexcept { return u.uid < v; });
  if (it == sorted.end() || it->uid != uid) {
    return kNpos;
  }
  return static_cast<std::size_t>(it - sorted.begin());
}

// The carry precedence of `u` at T (header "Carry precedence under CarryFrom::State").
// `row` is the exact-expiry term_carry row, when the caller found one.
[[nodiscard]] MarketState::Carry carry_for(const MarketState &state, const UnderlierState &u,
                                           const SurfaceRef &surf, double T,
                                           const ExpiryCarry *row) noexcept {
  MarketState::Carry c;
  c.forward = c.q_eff = c.rate = kNaN;
  if (!(std::isfinite(T) && T > 0.0)) {
    c.status = RiskLaneStatus::Rolled;
    c.flags = static_cast<RiskFlags>(RiskFlag::Rolled);
    return c;
  }
  switch (u.carry) {
  case CarryFrom::State: {
    double rate = 0.0;
    double q = 0.0;
    double pv_divs = 0.0;
    if (row != nullptr) {
      rate = row->rate;
      q = row->q;
      pv_divs = row->pv_divs;
    } else {
      rate = u.yield != nullptr ? u.yield->zero(T) : u.flat_rate;
      q = u.flat_q;
      for (const DividendEvent &d : u.cash_divs) {
        const Result<double> t_ex = state.time_to_expiry(d.ex_date_ns);
        if (!t_ex.has_value()) {
          c.status = RiskLaneStatus::NumericError;
          return c;
        }
        if (*t_ex > 0.0 && *t_ex <= T) {
          pv_divs += d.amount * std::exp(-rate * *t_ex);
        }
      }
    }
    const double forward = (u.spot - pv_divs) * std::exp((rate - q) * T);
    if (!finite_positive(forward) || !std::isfinite(rate)) {
      c.status = RiskLaneStatus::NumericError;
      return c;
    }
    c.forward = forward;
    c.q_eff = rate - std::log(forward / u.spot) / T;
    c.rate = rate;
    c.status = RiskLaneStatus::Ok;
    return c;
  }
  case CarryFrom::Surface: {
    if (!surf.valid()) {
      c.status = RiskLaneStatus::ModelUnavailable;
      c.flags = static_cast<RiskFlags>(RiskFlag::SurfaceMissing);
      return c;
    }
    c.forward = surf->forward_at(T);
    c.q_eff = surf->q_eff_at(T);
    c.rate = surf->rate_at(T);
    c.flags = static_cast<RiskFlags>(RiskFlag::CarryFromSurface);
    c.status = (finite_positive(c.forward) && std::isfinite(c.q_eff) && std::isfinite(c.rate))
                   ? RiskLaneStatus::Ok
                   : RiskLaneStatus::NumericError;
    return c;
  }
  }
  c.status = RiskLaneStatus::NumericError; // unreachable: validated enumerator
  return c;
}

} // namespace

Result<MarketState> MarketState::create(std::int64_t ts_ns, const TimeSpec &time,
                                        std::span<const UnderlierState> underliers,
                                        std::span<const ContractMarks> marks,
                                        std::span<const SurfaceRef> surfaces) {
  MarketState s;
  s.ts_ns_ = ts_ns;
  s.time_ = time;
  s.underliers_.assign(underliers.begin(), underliers.end());
  std::sort(s.underliers_.begin(), s.underliers_.end(),
            [](const UnderlierState &a, const UnderlierState &b) noexcept { return a.uid < b.uid; });
  for (std::size_t i = 0; i < s.underliers_.size(); ++i) {
    if (Status v = validate_underlier(s.underliers_[i]); !v.has_value()) {
      return Err(v.error());
    }
    if (i > 0 && s.underliers_[i - 1].uid == s.underliers_[i].uid) {
      return Err(ErrorCode::InvalidArgument, "MarketState: duplicate underlier uid");
    }
  }

  s.marks_.assign(marks.begin(), marks.end());
  std::sort(s.marks_.begin(), s.marks_.end(),
            [](const ContractMarks &a, const ContractMarks &b) noexcept { return a.uid < b.uid; });
  s.marks_slot_.assign(s.underliers_.size(), kNoSlot);
  for (std::size_t i = 0; i < s.marks_.size(); ++i) {
    const ContractMarks &m = s.marks_[i];
    if (i > 0 && s.marks_[i - 1].uid == m.uid) {
      return Err(ErrorCode::InvalidArgument, "MarketState: duplicate marks uid");
    }
    if (Status v = validate_marks(m); !v.has_value()) {
      return Err(v.error());
    }
    const std::size_t slot = find_slot(s.underliers_, m.uid);
    if (slot == kNpos) {
      return Err(ErrorCode::InvalidArgument, "MarketState: marks uid absent from underliers");
    }
    s.marks_slot_[slot] = static_cast<std::uint32_t>(i);
  }

  s.surfaces_.assign(s.underliers_.size(), SurfaceRef{});
  for (const SurfaceRef &ref : surfaces) {
    if (!ref.valid()) {
      return Err(ErrorCode::InvalidArgument, "MarketState: null surface");
    }
    const std::size_t slot = find_slot(s.underliers_, ref->uid());
    if (slot == kNpos) {
      return Err(ErrorCode::InvalidArgument, "MarketState: surface uid absent from underliers");
    }
    if (s.surfaces_[slot].valid()) {
      return Err(ErrorCode::InvalidArgument, "MarketState: duplicate surface uid");
    }
    s.surfaces_[slot] = ref;
  }
  return s;
}

const UnderlierState *MarketState::underlier(std::uint32_t uid) const noexcept {
  const std::size_t slot = find_slot(underliers_, uid);
  return slot == kNpos ? nullptr : &underliers_[slot];
}

const ContractMarks *MarketState::marks(std::uint32_t uid) const noexcept {
  const std::size_t slot = find_slot(underliers_, uid);
  if (slot == kNpos || marks_slot_[slot] == kNoSlot) {
    return nullptr;
  }
  return &marks_[marks_slot_[slot]];
}

SurfaceRef MarketState::surface(std::uint32_t uid) const noexcept {
  const std::size_t slot = find_slot(underliers_, uid);
  return slot == kNpos ? SurfaceRef{} : surfaces_[slot];
}

Result<double> MarketState::time_to_expiry(std::int64_t expiry_ns) const noexcept {
  return time_to_expiry_years(ts_ns_, expiry_ns, time_);
}

MarketState::Carry MarketState::carry_at(std::uint32_t uid, double T) const noexcept {
  const std::size_t slot = find_slot(underliers_, uid);
  if (slot == kNpos) {
    Carry c;
    c.status = RiskLaneStatus::ModelUnavailable;
    return c;
  }
  // A term_carry row applies when T is exactly that row's year-fraction from this
  // state, which is what `resolve` sees for the row's expiry.
  const ExpiryCarry *row = nullptr;
  if (underliers_[slot].carry == CarryFrom::State) {
    for (const ExpiryCarry &r : underliers_[slot].term_carry) {
      const Result<double> t = time_to_expiry(r.expiry_ns);
      if (t.has_value() && *t == T) {
        row = &r;
        break;
      }
    }
  }
  return carry_for(*this, underliers_[slot], surfaces_[slot], T, row);
}

ResolvedContract MarketState::resolve(std::uint32_t uid, double K, std::int64_t expiry_ns,
                                      Side side) const noexcept {
  const std::size_t slot = find_slot(underliers_, uid);
  if (slot == kNpos) {
    return refused(RiskLaneStatus::ModelUnavailable, RiskFlag::MarkMissing | RiskFlag::SurfaceMissing);
  }
  if (!finite_positive(K) || (side != Side::Call && side != Side::Put)) {
    return refused(RiskLaneStatus::InvalidContract, 0u);
  }
  const Result<double> T_res = time_to_expiry(expiry_ns);
  if (!T_res.has_value() || !std::isfinite(*T_res)) {
    return refused(RiskLaneStatus::NumericError, 0u);
  }
  const double T = *T_res;
  if (!(T > 0.0)) {
    return refused(RiskLaneStatus::Rolled, static_cast<RiskFlags>(RiskFlag::Rolled));
  }

  RiskFlags flags = 0u;
  double sigma = kNaN;
  double vendor_price = kNaN;
  double vendor_delta = kNaN;
  bool from_mark = false;
  const SurfaceRef surf = surfaces_[slot];
  if (marks_slot_[slot] != kNoSlot) {
    const ContractMarks &m = marks_[marks_slot_[slot]];
    const std::size_t i = find_mark(m, expiry_ns, side, K);
    if (i != kNpos && finite_positive(m.sigma[i])) {
      sigma = m.sigma[i];
      from_mark = true;
      if (!m.vendor_price.empty() || !m.vendor_delta.empty()) {
        vendor_price = m.vendor_price.empty() ? kNaN : m.vendor_price[i];
        vendor_delta = m.vendor_delta.empty() ? kNaN : m.vendor_delta[i];
        flags |= RiskFlag::VendorParityAvailable;
      }
    }
  }
  if (from_mark) {
    if (!surf.valid()) {
      flags |= RiskFlag::SurfaceMissing;
    }
  } else if (surf.valid()) {
    sigma = surf->iv(K, T);
    flags |= RiskFlag::MarkMissing;
    if (surf->extrapolates_tenor(T)) {
      flags |= RiskFlag::TenorExtrapolated;
    }
  } else {
    return refused(RiskLaneStatus::ModelUnavailable,
                   flags | RiskFlag::MarkMissing | RiskFlag::SurfaceMissing);
  }
  if (!finite_positive(sigma)) {
    return refused(RiskLaneStatus::NumericError, flags);
  }

  const ExpiryCarry *row = underliers_[slot].carry == CarryFrom::State
                               ? find_term_row(underliers_[slot].term_carry, expiry_ns)
                               : nullptr;
  const Carry c = carry_for(*this, underliers_[slot], surf, T, row);
  flags |= c.flags;
  if (c.status != RiskLaneStatus::Ok) {
    return refused(c.status, flags);
  }
  ResolvedContract rc;
  rc.T = T;
  rc.sigma = sigma;
  rc.forward = c.forward;
  rc.q_eff = c.q_eff;
  rc.rate = c.rate;
  rc.spot = underliers_[slot].spot;
  rc.vendor_price = vendor_price;
  rc.vendor_delta = vendor_delta;
  rc.flags = flags;
  rc.status = RiskLaneStatus::Ok;
  return rc;
}

MarketState::SigmaAt MarketState::sigma_at(std::uint32_t uid, double K, std::int64_t expiry_ns,
                                           Side side, double T) const noexcept {
  SigmaAt out;
  out.sigma = kNaN;
  const std::size_t slot = find_slot(underliers_, uid);
  if (slot == kNpos) {
    out.status = RiskLaneStatus::ModelUnavailable;
    return out;
  }
  if (!finite_positive(K)) {
    out.status = RiskLaneStatus::InvalidContract;
    return out;
  }
  if (marks_slot_[slot] != kNoSlot) {
    const ContractMarks &m = marks_[marks_slot_[slot]];
    const std::size_t i = find_mark(m, expiry_ns, side, K);
    if (i != kNpos && finite_positive(m.sigma[i])) {
      out.sigma = m.sigma[i];
      out.status = RiskLaneStatus::Ok;
      return out;
    }
  }
  const SurfaceRef surf = surfaces_[slot];
  if (!surf.valid()) {
    out.status = RiskLaneStatus::ModelUnavailable;
    return out;
  }
  if (!(std::isfinite(T) && T > 0.0)) {
    out.status = RiskLaneStatus::Rolled;
    return out;
  }
  const double sigma = surf->iv(K, T);
  if (!finite_positive(sigma)) {
    out.status = RiskLaneStatus::NumericError;
    return out;
  }
  out.sigma = sigma;
  if (surf->extrapolates_tenor(T)) {
    out.flags |= RiskFlag::TenorExtrapolated;
  }
  out.status = RiskLaneStatus::Ok;
  return out;
}

void MarketStateStorage::normalize_marks() {
  for (MarksColumns &m : marks) {
    const std::size_t n = m.sigma.size();
    if (m.strike.size() != n || m.expiry_ns.size() != n || m.side.size() != n) {
      continue; // ragged: left for make_market_state to refuse
    }
    // Total order on the key: a NaN strike sorts last (by bits) so the sort's
    // comparator stays a strict weak order whatever the caller stored.
    const auto strike_less = [&](std::size_t a, std::size_t b) noexcept {
      const double ka = m.strike[a];
      const double kb = m.strike[b];
      const bool na = std::isnan(ka);
      const bool nb = std::isnan(kb);
      if (na || nb) {
        return !na ? true
                   : (!nb ? false
                          : std::bit_cast<std::uint64_t>(ka) < std::bit_cast<std::uint64_t>(kb));
      }
      return ka < kb;
    };
    std::vector<std::size_t> perm(n);
    std::iota(perm.begin(), perm.end(), std::size_t{0});
    std::stable_sort(perm.begin(), perm.end(), [&](std::size_t a, std::size_t b) noexcept {
      if (m.expiry_ns[a] != m.expiry_ns[b]) {
        return m.expiry_ns[a] < m.expiry_ns[b];
      }
      if (m.side[a] != m.side[b]) {
        return side_rank(m.side[a]) < side_rank(m.side[b]);
      }
      return strike_less(a, b);
    });
    std::vector<std::size_t> keep;
    keep.reserve(n);
    for (const std::size_t i : perm) {
      if (!keep.empty()) {
        const std::size_t j = keep.back();
        if (m.expiry_ns[i] == m.expiry_ns[j] && m.side[i] == m.side[j] &&
            std::bit_cast<std::uint64_t>(m.strike[i]) == std::bit_cast<std::uint64_t>(m.strike[j])) {
          continue; // exact duplicate key: the first (stable order) wins
        }
      }
      keep.push_back(i);
    }
    const auto gather = [&keep](auto &col) {
      using Col = std::decay_t<decltype(col)>;
      Col out;
      out.reserve(keep.size());
      for (const std::size_t i : keep) {
        out.push_back(col[i]);
      }
      col = std::move(out);
    };
    gather(m.strike);
    gather(m.expiry_ns);
    gather(m.side);
    gather(m.sigma);
    if (m.vendor_price.size() == n) {
      gather(m.vendor_price);
    }
    if (m.vendor_delta.size() == n) {
      gather(m.vendor_delta);
    }
  }
}

Result<MarketState> make_market_state(const MarketStateStorage &storage) {
  std::vector<ContractMarks> views;
  views.reserve(storage.marks.size());
  for (const MarketStateStorage::MarksColumns &m : storage.marks) {
    ContractMarks v;
    v.uid = m.uid;
    v.strike = m.strike;
    v.expiry_ns = m.expiry_ns;
    v.side = m.side;
    v.sigma = m.sigma;
    v.vendor_price = m.vendor_price;
    v.vendor_delta = m.vendor_delta;
    views.push_back(v);
  }
  return MarketState::create(storage.ts_ns, storage.time, storage.underliers, views,
                             storage.surfaces);
}

Result<MarketStateStorage> market_state_from_surfaces(std::int64_t ts_ns, const TimeSpec &time,
                                                      std::span<const SurfaceRef> surfaces,
                                                      bool allow_timestamp_mismatch) {
  MarketStateStorage st;
  st.ts_ns = ts_ns;
  st.time = time;
  st.underliers.reserve(surfaces.size());
  st.surfaces.reserve(surfaces.size());
  for (const SurfaceRef &ref : surfaces) {
    if (!ref.valid()) {
      return Err(ErrorCode::InvalidArgument, "market_state_from_surfaces: null surface");
    }
    const PricingContext &pc = ref->pricing();
    if (!allow_timestamp_mismatch && pc.now_ts_ns != ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "market_state_from_surfaces: surface timestamp differs from ts_ns");
    }
    UnderlierState u;
    u.uid = ref->uid();
    u.spot = pc.S;
    u.flat_rate = pc.r;
    u.carry = CarryFrom::Surface;
    st.underliers.push_back(u);
    st.surfaces.push_back(ref);
  }
  // Validate once (distinct uids, finite spots) through the same path a caller uses.
  if (const Result<MarketState> check = make_market_state(st); !check.has_value()) {
    return Err(check.error());
  }
  return st;
}

} // namespace atx::vol
