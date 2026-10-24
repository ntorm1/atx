// explain — Taylor P&L explain between two RiskFrames of one book: the eight analytic
// terms on the base Greeks (simd/pnl_batch), the exact unexplained remainder, the
// level / skew / curvature vega split, share legs, exclusions with premium, and
// Neumaier totals / buckets in leg input order.

#include "atx/vol/api/risk/pnl_explain.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "risk/risk_neumaier.hpp"
#include "simd/pnl_batch.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// One vega-split pivot sample: a run of prepared groups sharing (uid, expiry_ns).
struct PivotGroup {
  std::uint32_t row_begin{0};
  std::uint32_t row_end{0};
  std::uint32_t uid{0};
  std::uint32_t uid_slot{0};
  std::int64_t expiry_ns{0};
  bool sampled{false};    // a pivot sample was attempted (surface or marks path)
  bool usable{false};     // a0..a2 apply to the group's legs
  bool from_marks{false}; // the pivots are marks (flag PivotFromMarks)
  bool edge{false};       // a pivot resolved NaN: a0 = a1 = a2 = 0
  double F_b{0.0};
  double sigma_atm{0.0};
  double sqrt_T{0.0};
  double a0{0.0};
  double a1{0.0};
  double a2{0.0};
};

// Column order of the accumulated totals (ExplainTotals' double fields).
constexpr std::size_t kPvBase = 0;
constexpr std::size_t kPvTarget = 1;
constexpr std::size_t kTotal = 2;
constexpr std::size_t kDelta = 3;
constexpr std::size_t kGamma = 4;
constexpr std::size_t kVega = 5;
constexpr std::size_t kVolga = 6;
constexpr std::size_t kVanna = 7;
constexpr std::size_t kTheta = 8;
constexpr std::size_t kRho = 9;
constexpr std::size_t kCharm = 10;
constexpr std::size_t kUnexplained = 11;
constexpr std::size_t kLevel = 12;
constexpr std::size_t kSkew = 13;
constexpr std::size_t kCurv = 14;
constexpr std::size_t kResid = 15;
constexpr std::size_t kShares = 16;
constexpr std::size_t kExcludedPremium = 17;
constexpr std::size_t kTotalsCount = 18;

class TotalsAcc {
public:
  void add_ok(const ExplainResult &e, std::size_t i) noexcept {
    const std::array<double, kExcludedPremium> row{
        e.pv_base[i],   e.pv_target[i], e.pnl_total[i], e.pnl_delta[i],      e.pnl_gamma[i],
        e.pnl_vega[i],  e.pnl_volga[i], e.pnl_vanna[i], e.pnl_theta[i],      e.pnl_rho[i],
        e.pnl_charm[i], e.pnl_unexplained[i], e.vol_level[i], e.vol_skew[i], e.vol_curv[i],
        e.vol_resid[i], e.kind[i] == ExplainLegKind::Share ? e.pnl_total[i] : 0.0};
    for (std::size_t k = 0; k < row.size(); ++k) {
      sums_[k].add(row[k]);
    }
    ++n_ok_;
  }
  void add_excluded(double premium) noexcept {
    sums_[kExcludedPremium].add(premium);
    ++n_excluded_;
  }
  [[nodiscard]] ExplainTotals totals() const noexcept {
    ExplainTotals t;
    t.pv_base = sums_[kPvBase].value();
    t.pv_target = sums_[kPvTarget].value();
    t.pnl_total = sums_[kTotal].value();
    t.pnl_delta = sums_[kDelta].value();
    t.pnl_gamma = sums_[kGamma].value();
    t.pnl_vega = sums_[kVega].value();
    t.pnl_volga = sums_[kVolga].value();
    t.pnl_vanna = sums_[kVanna].value();
    t.pnl_theta = sums_[kTheta].value();
    t.pnl_rho = sums_[kRho].value();
    t.pnl_charm = sums_[kCharm].value();
    t.pnl_unexplained = sums_[kUnexplained].value();
    t.vol_level = sums_[kLevel].value();
    t.vol_skew = sums_[kSkew].value();
    t.vol_curv = sums_[kCurv].value();
    t.vol_resid = sums_[kResid].value();
    t.shares_pnl = sums_[kShares].value();
    t.n_ok = n_ok_;
    t.n_excluded = n_excluded_;
    t.excluded_premium = sums_[kExcludedPremium].value();
    return t;
  }

private:
  std::array<risk_detail::NeumaierSum, kTotalsCount> sums_{};
  std::uint32_t n_ok_{0};
  std::uint32_t n_excluded_{0};
};

// explain_into's scratch. The frozen API gives it no workspace, so it lives per
// calling thread and is grow-only: a steady-state explain on one thread allocates
// nothing. During a call the executor's workers reach it only through the calling
// thread's captured reference: the pivot fan-out writes disjoint PivotGroup slots, the
// per-leg fan-out only reads them, and the reduction is serial on the calling thread.
struct ExplainScratch {
  std::vector<PivotGroup> pivots;
  std::vector<std::int64_t> expiries; // distinct option expiries, ascending
  std::vector<TotalsAcc> by_uid;
  std::vector<TotalsAcc> by_expiry;
};

[[nodiscard]] ExplainScratch &scratch() noexcept {
  thread_local ExplainScratch s;
  return s;
}

// First marks row with the exact key, or npos (marks sorted by (expiry, side, K)).
[[nodiscard]] std::size_t find_mark(const ContractMarks &m, std::int64_t expiry, Side side,
                                    double K) noexcept {
  std::size_t lo = 0;
  std::size_t hi = m.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2u;
    const bool less = m.expiry_ns[mid] != expiry
                          ? m.expiry_ns[mid] < expiry
                          : (m.side[mid] != side ? static_cast<std::uint8_t>(m.side[mid]) <
                                                       static_cast<std::uint8_t>(side)
                                                 : m.strike[mid] < K);
    if (less) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  if (lo < m.size() && m.expiry_ns[lo] == expiry && m.side[lo] == side && m.strike[lo] == K) {
    return lo;
  }
  return std::numeric_limits<std::size_t>::max();
}

[[nodiscard]] bool finite_positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }

// The exact quadratic through three pivots at z in {-h, 0, +h} (pnl_attribution's).
void fit_symmetric(PivotGroup &pg, double d_m, double d_0, double d_p, double h) noexcept {
  pg.a0 = d_0;
  pg.a1 = (d_p - d_m) / (2.0 * h);
  pg.a2 = (d_p + d_m - 2.0 * d_0) / (2.0 * h * h);
}

void sample_surface(PivotGroup &pg, const SurfaceRef &sb, const SurfaceRef &st, double T_b,
                    double z_ref) noexcept {
  pg.sampled = true;
  pg.sigma_atm = sb->iv(pg.F_b, T_b);
  std::array<double, 3> d{};
  const std::array<double, 3> z{-z_ref, 0.0, z_ref};
  bool ok = finite_positive(pg.sigma_atm);
  for (std::size_t j = 0; ok && j < 3u; ++j) {
    const double K = pg.F_b * std::exp(z[j] * pg.sigma_atm * pg.sqrt_T);
    d[j] = st->iv(K, T_b) - sb->iv(K, T_b); // shifted - base at the SAME (K_z, T_b)
    ok = std::isfinite(d[j]);
  }
  if (!ok) {
    pg.edge = true;
    return;
  }
  fit_symmetric(pg, d[0], d[1], d[2], z_ref);
  pg.usable = true;
}

// Marks-only fallback: the three pivots are the group's marked contracts nearest to
// z in {-z_ref, 0, +z_ref}; fewer than three distinct usable pivots => no split.
void sample_marks(PivotGroup &pg, const ContractMarks &mb, const ContractMarks &ms,
                  double z_ref) noexcept {
  pg.sampled = true;
  pg.from_marks = true;
  const auto first = std::lower_bound(mb.expiry_ns.begin(), mb.expiry_ns.end(), pg.expiry_ns);
  const auto last = std::upper_bound(first, mb.expiry_ns.end(), pg.expiry_ns);
  const auto lo = static_cast<std::size_t>(first - mb.expiry_ns.begin());
  const auto hi = static_cast<std::size_t>(last - mb.expiry_ns.begin());
  double best_atm = std::numeric_limits<double>::infinity();
  for (std::size_t i = lo; i < hi; ++i) {
    const double dist = std::fabs(std::log(mb.strike[i] / pg.F_b));
    if (finite_positive(mb.sigma[i]) && dist < best_atm) {
      best_atm = dist;
      pg.sigma_atm = mb.sigma[i];
    }
  }
  if (!finite_positive(pg.sigma_atm)) {
    return;
  }
  const std::array<double, 3> target{-z_ref, 0.0, z_ref};
  std::array<double, 3> pz{};
  std::array<double, 3> pd{};
  std::array<double, 3> best{};
  best.fill(std::numeric_limits<double>::infinity());
  for (std::size_t i = lo; i < hi; ++i) {
    const std::size_t j = find_mark(ms, mb.expiry_ns[i], mb.side[i], mb.strike[i]);
    if (j == std::numeric_limits<std::size_t>::max() || !finite_positive(mb.sigma[i]) ||
        !finite_positive(ms.sigma[j])) {
      continue;
    }
    const double z = std::log(mb.strike[i] / pg.F_b) / (pg.sigma_atm * pg.sqrt_T);
    for (std::size_t t = 0; t < 3u; ++t) {
      const double dist = std::fabs(z - target[t]);
      if (dist < best[t]) {
        best[t] = dist;
        pz[t] = z;
        pd[t] = ms.sigma[j] - mb.sigma[i];
      }
    }
  }
  if (!(std::isfinite(best[0]) && std::isfinite(best[1]) && std::isfinite(best[2])) ||
      !(pz[0] < pz[1] && pz[1] < pz[2])) {
    return; // fewer than three distinct pivots: everything stays in vol_resid
  }
  // Newton divided differences through the three (z, d) points, exact quadratic.
  const double s01 = (pd[1] - pd[0]) / (pz[1] - pz[0]);
  const double s12 = (pd[2] - pd[1]) / (pz[2] - pz[1]);
  pg.a2 = (s12 - s01) / (pz[2] - pz[0]);
  pg.a1 = s01 - pg.a2 * (pz[0] + pz[1]);
  pg.a0 = pd[0] - pg.a1 * pz[0] - pg.a2 * pz[0] * pz[0];
  pg.usable = true;
}

void sample_pivots(PivotGroup &pg, const MarketState &base_state, const RiskFrame &base,
                   const MarketState &shifted_state, double z_ref) noexcept {
  std::uint32_t r0 = pg.row_end;
  for (std::uint32_t r = pg.row_begin; r < pg.row_end; ++r) {
    if (base.status[r] == RiskLaneStatus::Ok) {
      r0 = r;
      break;
    }
  }
  if (r0 == pg.row_end) {
    return; // no Ok base lane: no group legs to split
  }
  const double T_b = base.T[r0];
  pg.F_b = base.forward[r0];
  pg.sqrt_T = std::sqrt(T_b);
  const SurfaceRef sb = base_state.surface(pg.uid);
  const SurfaceRef st = shifted_state.surface(pg.uid);
  if (sb.valid() && st.valid()) {
    sample_surface(pg, sb, st, T_b, z_ref);
    return;
  }
  const ContractMarks *mb = base_state.marks(pg.uid);
  const ContractMarks *ms = shifted_state.marks(pg.uid);
  if (mb != nullptr && ms != nullptr) {
    sample_marks(pg, *mb, *ms, z_ref);
  }
}

// An excluded row: NaN in every column but pv_base, which keeps the base valuation
// (w P_b, or qty S_b for a share) when the base side had one, NaN otherwise; the
// serial reduction reads its |.| as the exclusion premium (0 when NaN).
void write_excluded(ExplainResult &out, std::size_t i, RiskLaneStatus status,
                    double pv_base) noexcept {
  for (std::vector<double> *col :
       {&out.pv_base, &out.pv_target, &out.pnl_total, &out.pnl_delta, &out.pnl_gamma,
        &out.pnl_vega, &out.pnl_volga, &out.pnl_vanna, &out.pnl_theta, &out.pnl_rho,
        &out.pnl_charm, &out.pnl_unexplained, &out.vol_level, &out.vol_skew, &out.vol_curv,
        &out.vol_resid, &out.d_spot, &out.d_vol, &out.d_time, &out.d_rate}) {
    (*col)[i] = kNaN;
  }
  out.pv_base[i] = std::isfinite(pv_base) ? pv_base : kNaN;
  out.status[i] = status;
}

struct LegCtx {
  const PreparedRiskBook &book;
  const MarketState &shifted_state;
  const RiskFrame &base;
  const RiskFrame &shifted;
  const ExplainOptions &opts;
  std::span<const PivotGroup> pivots;
  bool split;
  double dt;
  ExplainResult &out;
};

[[nodiscard]] const PivotGroup &pivot_of(std::span<const PivotGroup> pivots,
                                         std::uint32_t row) noexcept {
  const auto it = std::upper_bound(
      pivots.begin(), pivots.end(), row,
      [](std::uint32_t r, const PivotGroup &pg) noexcept { return r < pg.row_end; });
  return *it; // every prepared row belongs to exactly one pivot group
}

void explain_option_leg(const LegCtx &c, std::size_t i) noexcept {
  const OptionLeg &leg = c.book.book().option_legs()[i];
  const std::uint32_t r = c.book.leg_prepared_row()[i];
  const double w = c.book.leg_weight()[i];
  ExplainResult &out = c.out;
  out.id[i] = leg.id;
  out.uid[i] = leg.uid;
  out.kind[i] = ExplainLegKind::Option;
  out.route_base[i] = c.base.route[r];
  out.route_shifted[i] = c.shifted.route[r];
  RiskFlags flags = c.base.flags[r] | c.shifted.flags[r];

  // The first non-Ok reason, base first; a leg with a non-finite weight is the leg's
  // own InvalidContract (its contract row may be shared with valid legs).
  RiskLaneStatus status = RiskLaneStatus::Ok;
  if (c.base.status[r] != RiskLaneStatus::Ok) {
    status = c.base.status[r];
  } else if (c.shifted.status[r] != RiskLaneStatus::Ok) {
    status = c.shifted.status[r];
  } else if (!std::isfinite(w)) {
    status = RiskLaneStatus::InvalidContract;
  }
  double dsig = kNaN;
  if (status == RiskLaneStatus::Ok) {
    if (c.opts.vol_move == VolMoveConvention::ContractOwn) {
      dsig = c.shifted.sigma[r] - c.base.sigma[r];
    } else {
      const MarketState::SigmaAt sa = c.shifted_state.sigma_at(
          leg.uid, c.book.strike()[r], c.book.expiry_ns()[r], c.book.side()[r], c.base.T[r]);
      flags |= sa.flags;
      if (sa.status != RiskLaneStatus::Ok) {
        status = sa.status;
      } else {
        dsig = sa.sigma - c.base.sigma[r];
      }
    }
  }
  out.flags[i] = flags;
  if (status != RiskLaneStatus::Ok) {
    const bool base_ok = c.base.status[r] == RiskLaneStatus::Ok;
    write_excluded(out, i, status, base_ok ? w * c.base.price[r] : kNaN);
    return;
  }

  const PivotGroup &pg = pivot_of(c.pivots, r);
  const double dS = c.shifted.uid_spot[pg.uid_slot] - c.base.uid_spot[pg.uid_slot];
  const double dr = c.shifted.rate[r] - c.base.rate[r];
  double gamma = c.base.gamma[r];
  double volga = c.base.volga[r];
  double vanna = c.base.vanna[r];
  if (c.opts.trapezoid_second_order) {
    gamma = 0.5 * (c.base.gamma[r] + c.shifted.gamma[r]);
    volga = 0.5 * (c.base.volga[r] + c.shifted.volga[r]);
    vanna = 0.5 * (c.base.vanna[r] + c.shifted.vanna[r]);
  }
  // rho_b is read only when the row's rate moved (a frame without rho has 0 there).
  const double rho = dr != 0.0 ? c.base.rho[r] : 0.0;
  const double dt = c.dt;
  // One lane through simd/pnl_batch: the kernel is bit-identical for any lane
  // position and route, so this equals the row's value inside a full batch.
  std::array<double, 8> term{};
  double explained_ps = 0.0;
  const simd::PnlExplainInputs in{.delta = &c.base.delta[r],
                                  .gamma = &gamma,
                                  .vega = &c.base.vega[r],
                                  .volga = &volga,
                                  .vanna = &vanna,
                                  .theta = &c.base.theta[r],
                                  .rho = &rho,
                                  .charm = &c.base.charm[r],
                                  .qty = nullptr,
                                  .dS = &dS,
                                  .dSigma = &dsig,
                                  .dt = &dt,
                                  .dr = &dr};
  const simd::PnlExplainOutputs ko{.delta_pnl = &term[0],
                                   .gamma_pnl = &term[1],
                                   .vega_pnl = &term[2],
                                   .volga_pnl = &term[3],
                                   .vanna_pnl = &term[4],
                                   .theta_pnl = &term[5],
                                   .rho_pnl = &term[6],
                                   .charm_pnl = &term[7],
                                   .total = &explained_ps};
  simd::pnl_taylor_explain_batch(in, ko, 1);

  const double P_b = c.base.price[r];
  const double P_t = c.shifted.price[r];
  out.pnl_delta[i] = w * term[0];
  out.pnl_gamma[i] = w * term[1];
  out.pnl_vega[i] = w * term[2];
  out.pnl_volga[i] = w * term[3];
  out.pnl_vanna[i] = w * term[4];
  out.pnl_theta[i] = w * term[5];
  out.pnl_rho[i] = w * term[6];
  out.pnl_charm[i] = w * term[7];
  out.pv_base[i] = w * P_b;
  out.pv_target[i] = w * P_t;
  out.pnl_total[i] = w * (P_t - P_b);
  // The remainder is taken against THIS row's own eight columns (left to right, the
  // kernel's order), so a row's columns reproduce its pnl_total to ~1 ulp.
  const double explained = ((((((out.pnl_delta[i] + out.pnl_gamma[i]) + out.pnl_vega[i]) +
                                out.pnl_volga[i]) +
                               out.pnl_vanna[i]) +
                              out.pnl_theta[i]) +
                             out.pnl_rho[i]) +
                            out.pnl_charm[i];
  out.pnl_unexplained[i] = out.pnl_total[i] - explained;
  out.d_spot[i] = dS;
  out.d_vol[i] = dsig;
  out.d_time[i] = dt;
  out.d_rate[i] = dr;

  // Vega split: fractions of the row's own pnl_vega, remainder in vol_resid.
  const double v = out.pnl_vega[i];
  double level = 0.0;
  double skew = 0.0;
  double curv = 0.0;
  if (c.split && pg.usable && dsig != 0.0) {
    const double z = std::log(c.book.strike()[r] / pg.F_b) / (pg.sigma_atm * pg.sqrt_T);
    // Forming each ratio first keeps a pure move exact (a0 == dsig => a0/dsig == 1).
    level = v * (pg.a0 / dsig);
    skew = v * ((pg.a1 * z) / dsig);
    curv = v * ((pg.a2 * z * z) / dsig);
  }
  if (c.split && pg.usable && pg.from_marks) {
    out.flags[i] |= RiskFlag::PivotFromMarks;
  }
  out.vol_level[i] = level;
  out.vol_skew[i] = skew;
  out.vol_curv[i] = curv;
  out.vol_resid[i] = v - ((level + skew) + curv);
  out.status[i] = RiskLaneStatus::Ok;
}

void explain_share_leg(const PreparedRiskBook &book, const RiskFrame &base,
                       const RiskFrame &shifted, double dt, std::size_t j, std::size_t i,
                       ExplainResult &out) noexcept {
  const ShareLeg &leg = book.book().share_legs()[j];
  out.id[i] = leg.id;
  out.uid[i] = leg.uid;
  out.kind[i] = ExplainLegKind::Share;
  out.route_base[i] = RiskRoute::Share;
  out.route_shifted[i] = RiskRoute::Share;
  out.flags[i] = 0u;
  RiskLaneStatus status = RiskLaneStatus::Ok;
  if (base.share_status[j] != RiskLaneStatus::Ok) {
    status = base.share_status[j];
  } else if (shifted.share_status[j] != RiskLaneStatus::Ok) {
    status = shifted.share_status[j];
  } else if (!std::isfinite(leg.qty)) {
    status = RiskLaneStatus::InvalidContract;
  }
  if (status != RiskLaneStatus::Ok) {
    const bool base_ok = base.share_status[j] == RiskLaneStatus::Ok;
    write_excluded(out, i, status, base_ok ? leg.qty * base.share_mark[j] : kNaN);
    return;
  }
  const double S_b = base.share_mark[j];
  const double S_t = shifted.share_mark[j];
  const double dS = S_t - S_b;
  const double pnl = leg.qty * dS;
  out.pv_base[i] = leg.qty * S_b;
  out.pv_target[i] = leg.qty * S_t;
  out.pnl_total[i] = pnl;
  out.pnl_delta[i] = pnl;
  out.pnl_gamma[i] = out.pnl_vega[i] = out.pnl_volga[i] = out.pnl_vanna[i] = 0.0;
  out.pnl_theta[i] = out.pnl_rho[i] = out.pnl_charm[i] = out.pnl_unexplained[i] = 0.0;
  out.vol_level[i] = out.vol_skew[i] = out.vol_curv[i] = out.vol_resid[i] = 0.0;
  out.d_spot[i] = dS;
  out.d_vol[i] = 0.0;
  out.d_time[i] = dt;
  out.d_rate[i] = 0.0;
  out.status[i] = RiskLaneStatus::Ok;
}

// The frame's shape is exactly RiskFrame::reserve(n_unique, n_uids, n_shares): every
// column is checked (RiskFrame is a public aggregate; a hand-built or partially
// resized frame must be refused, never read out of bounds).
[[nodiscard]] Status validate_frame(const PreparedRiskBook &book, const RiskFrame &f) {
  const std::size_t n = book.n_unique();
  const std::size_t n_uids = book.uids().size();
  const std::size_t n_shares = book.share_uid_slot().size();
  bool ok = f.book_logical_id == book.book_logical_id();
  for (const std::vector<double> *col :
       {&f.T, &f.sigma, &f.forward, &f.q_eff, &f.rate, &f.price, &f.delta, &f.gamma, &f.vega,
        &f.theta, &f.rho, &f.vanna, &f.volga, &f.charm, &f.vendor_price, &f.vendor_delta}) {
    ok = ok && col->size() == n;
  }
  ok = ok && f.route.size() == n && f.flags.size() == n && f.status.size() == n;
  ok = ok && f.uid_spot.size() == n_uids && f.uid_flat_rate.size() == n_uids &&
       f.uid_status.size() == n_uids;
  ok = ok && f.share_mark.size() == n_shares && f.share_status.size() == n_shares;
  if (!ok) {
    return Err(ErrorCode::InvalidArgument,
               "explain: frame not evaluated for this book (logical id / shape mismatch)");
  }
  return Ok();
}

void build_pivot_groups(const PreparedRiskBook &book, std::vector<PivotGroup> &pivots) {
  pivots.clear();
  for (const RiskGroup &g : book.groups()) {
    if (!pivots.empty() && pivots.back().uid == g.uid && pivots.back().expiry_ns == g.expiry_ns) {
      pivots.back().row_end = g.end;
      continue;
    }
    PivotGroup pg;
    pg.row_begin = g.begin;
    pg.row_end = g.end;
    pg.uid = g.uid;
    pg.uid_slot = g.uid_slot;
    pg.expiry_ns = g.expiry_ns;
    pivots.push_back(pg);
  }
}

[[nodiscard]] std::size_t index_of(std::span<const std::uint32_t> sorted,
                                   std::uint32_t v) noexcept {
  return static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), v) -
                                  sorted.begin());
}

void reduce_totals(const PreparedRiskBook &book, std::size_t n_opt, std::size_t n_sh,
                   ExplainScratch &s, ExplainResult &out) {
  const std::span<const std::uint32_t> uids = book.uids();
  const std::span<const OptionLeg> legs = book.book().option_legs();
  const std::size_t share_bucket = 0; // by_expiry slot 0 when shares are present
  const std::size_t exp_offset = n_sh > 0u ? 1u : 0u;
  TotalsAcc total;
  s.by_uid.assign(uids.size(), TotalsAcc{});
  s.by_expiry.assign(s.expiries.size() + exp_offset, TotalsAcc{});
  std::array<risk_detail::NeumaierSum, kRiskLaneStatusCount> premium{};
  out.excluded_count.fill(0u);
  bool rate_moved = false;
  // Serial, in leg input order (options, then shares): deterministic for any thread count.
  for (std::size_t i = 0; i < n_opt + n_sh; ++i) {
    const bool is_share = i >= n_opt;
    const std::size_t uslot = index_of(uids, out.uid[i]);
    std::size_t eslot = share_bucket;
    if (!is_share) {
      const auto it =
          std::lower_bound(s.expiries.begin(), s.expiries.end(), legs[i].expiry_ns);
      eslot = exp_offset + static_cast<std::size_t>(it - s.expiries.begin());
    }
    if (out.status[i] == RiskLaneStatus::Ok) {
      total.add_ok(out, i);
      s.by_uid[uslot].add_ok(out, i);
      s.by_expiry[eslot].add_ok(out, i);
      rate_moved = rate_moved || (!is_share && out.d_rate[i] != 0.0);
      continue;
    }
    // |w P_b| (|qty S_b| for a share), 0 when the base mark itself is unavailable.
    const double prem = std::isfinite(out.pv_base[i]) ? std::fabs(out.pv_base[i]) : 0.0;
    total.add_excluded(prem);
    s.by_uid[uslot].add_excluded(prem);
    s.by_expiry[eslot].add_excluded(prem);
    const auto st = static_cast<std::size_t>(out.status[i]);
    ++out.excluded_count[st];
    premium[st].add(prem);
  }
  out.total = total.totals();
  for (std::size_t k = 0; k < kRiskLaneStatusCount; ++k) {
    out.excluded_premium[k] = premium[k].value();
  }
  out.rate_move_without_rho = rate_moved;
}

} // namespace

void ExplainResult::reserve(std::size_t n_option_legs, std::size_t n_share_legs,
                            std::size_t n_uids, std::size_t n_expiries) {
  const std::size_t n = n_option_legs + n_share_legs;
  id.resize(n);
  uid.resize(n);
  kind.resize(n);
  for (std::vector<double> *col :
       {&pv_base, &pv_target, &pnl_total, &pnl_delta, &pnl_gamma, &pnl_vega, &pnl_volga,
        &pnl_vanna, &pnl_theta, &pnl_rho, &pnl_charm, &pnl_unexplained, &vol_level, &vol_skew,
        &vol_curv, &vol_resid, &d_spot, &d_vol, &d_time, &d_rate}) {
    col->resize(n);
  }
  route_base.resize(n);
  route_shifted.resize(n);
  flags.resize(n);
  status.resize(n);
  by_uid.reserve(n_uids);
  by_expiry.reserve(n_expiries);
}

Status explain_into(const PreparedRiskBook &book, const MarketState &base_state,
                    const RiskFrame &base, const MarketState &shifted_state,
                    const RiskFrame &shifted, const ExplainOptions &opts, ExplainResult &out) {
  const auto t0 = std::chrono::steady_clock::now();
  if (book.book_logical_id() == 0u) {
    return Err(ErrorCode::InvalidArgument, "explain: unprepared book");
  }
  if (Status v = validate_frame(book, base); !v.has_value()) {
    return v;
  }
  if (Status v = validate_frame(book, shifted); !v.has_value()) {
    return v;
  }
  if (!finite_positive(opts.z_ref)) {
    return Err(ErrorCode::InvalidArgument, "explain: z_ref must be finite > 0");
  }
  const Result<double> dt_res = time_to_expiry_years(base.ts_ns, shifted.ts_ns, base_state.time());
  if (!dt_res.has_value() || !std::isfinite(*dt_res)) {
    return Err(ErrorCode::InvalidArgument, "explain: the base TimeSpec refuses the step");
  }
  const double dt = *dt_res;
  // The same state twice is a frame-only explain: there is no second vol to pivot on.
  const bool split = opts.vega_split && (&base_state != &shifted_state);
  const std::size_t n_opt = book.leg_prepared_row().size();
  const std::size_t n_sh = opts.include_shares ? book.share_uid_slot().size() : 0u;

  ExplainScratch &s = scratch();
  build_pivot_groups(book, s.pivots);
  s.expiries.clear();
  for (const PivotGroup &pg : s.pivots) {
    s.expiries.push_back(pg.expiry_ns);
  }
  std::sort(s.expiries.begin(), s.expiries.end());
  s.expiries.erase(std::unique(s.expiries.begin(), s.expiries.end()), s.expiries.end());
  if (split) {
    pricing_executor().run_blocks(s.pivots.size(), opts.n_threads, [&](std::size_t k) {
      sample_pivots(s.pivots[k], base_state, base, shifted_state, opts.z_ref);
    });
  }

  const std::size_t n_buckets_exp = s.expiries.size() + (n_sh > 0u ? 1u : 0u);
  out.reserve(n_opt, n_sh, book.uids().size(), n_buckets_exp);
  out.base_ts_ns = base.ts_ns;
  out.shifted_ts_ns = shifted.ts_ns;
  out.dt_years = dt;

  // Per-leg rows: disjoint slots, any worker count gives the same bits.
  const LegCtx c{book, shifted_state, base, shifted, opts, s.pivots, split, dt, out};
  pricing_executor().run_blocks(n_opt, opts.n_threads,
                                [&c](std::size_t i) { explain_option_leg(c, i); });
  for (std::size_t j = 0; j < n_sh; ++j) {
    explain_share_leg(book, base, shifted, dt, j, n_opt + j, out);
  }

  out.n_pivot_groups = 0u;
  out.n_pivot_edge_fallback = 0u;
  for (const PivotGroup &pg : s.pivots) {
    out.n_pivot_groups += pg.sampled ? 1u : 0u;
    out.n_pivot_edge_fallback += pg.edge ? 1u : 0u;
  }

  reduce_totals(book, n_opt, n_sh, s, out);
  out.rate_move_without_rho = out.rate_move_without_rho && !base.needs.rho;

  const std::span<const std::uint32_t> uids = book.uids();
  out.by_uid.resize(uids.size());
  for (std::size_t k = 0; k < uids.size(); ++k) {
    out.by_uid[k] = ExplainBucket{uids[k], 0, s.by_uid[k].totals()};
  }
  out.by_expiry.resize(n_buckets_exp);
  std::size_t b = 0;
  if (n_sh > 0u) {
    // Shares have no expiry: one "delta-one" bucket keyed 0, ahead of every expiry.
    out.by_expiry[b] = ExplainBucket{0u, 0, s.by_expiry[b].totals()};
    ++b;
  }
  for (const std::int64_t e : s.expiries) {
    out.by_expiry[b] = ExplainBucket{static_cast<std::uint64_t>(e), e, s.by_expiry[b].totals()};
    ++b;
  }
  out.explain_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return Ok();
}

Result<ExplainResult> explain(const PreparedRiskBook &book, const MarketState &base_state,
                              const RiskFrame &base, const MarketState &shifted_state,
                              const RiskFrame &shifted, const ExplainOptions &opts) {
  ExplainResult out;
  if (Status st = explain_into(book, base_state, base, shifted_state, shifted, opts, out);
      !st.has_value()) {
    return Err(st.error());
  }
  return out;
}

} // namespace atx::vol
