// pnl_attribution implementation — one public pnl_explain solve + a cheap pivot
// sampling pass + the exact vega partition.
//
// Design (mirrors scenario_grid.cpp's "build on the PUBLIC API" discipline): the
// only heavy work is `PortfolioPricer::pnl_explain`, which dedups the book on
// (uid,K,T,side), solves each unique base-greeks bundle ONCE, and returns the full
// 19-column per-position PnlFrame. The extra machinery here is:
//
//   1. per unique (uid, T) group referenced by the book: 6 iv() reads (three pivots
//      on each of base + shifted) -> the exact 3-point quadratic (a0, a1, a2);
//   2. per position: split the frame's own `pnl_vega` into level/skew/curv/resid as
//      fractions of dvol, and regroup the remaining PnlFrame columns 1:1.
//
// Everything after the solve is serial + deterministic, so the frame is bit-
// identical across `n_threads` (the solve's own determinism is PortfolioPricer's).

#include "atx/vol/api/analytics/pnl_attribution.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface

namespace atx::vol {

namespace {

// The exact quadratic through three pivots at k in {-h, 0, +h}, given
// d_m = dsigma(-h), d_0 = dsigma(0), d_p = dsigma(+h):
//   f(x) = a0 + a1*x + a2*x^2 with f(0)=d_0, f(h)=d_p, f(-h)=d_m.
//   f(h)-f(-h) = 2*a1*h            => a1 = (d_p - d_m) / (2h)
//   f(h)+f(-h) = 2*a0 + 2*a2*h^2   => a2 = (d_p + d_m - 2*a0) / (2*h^2)
//   f(0)       = a0 = d_0
// `edge` is set when any pivot IV is non-finite (a domain-edge strike); the caller
// then zeroes a0/a1/a2 so the whole vega P&L falls to vol_resid (no NaN poisoning).
struct GroupCoef {
  double a0{0.0};
  double a1{0.0};
  double a2{0.0};
  bool edge{false};
};

[[nodiscard]] GroupCoef compute_coef(const SurfaceRef sb, const SurfaceRef ss, double T,
                                     double k_ref) noexcept {
  GroupCoef c{};
  if (sb == nullptr || ss == nullptr) {
    c.edge = true;
    return c;
  }
  const double F = sb->forward_at(T);
  if (!(std::isfinite(F) && F > 0.0)) {
    c.edge = true;
    return c;
  }
  const auto dsig = [&](double k) noexcept -> double {
    const double K = F * std::exp(k);
    return ss->iv(K, T) - sb->iv(K, T); // shifted - base at the SAME (K, T)
  };
  const double d_m = dsig(-k_ref);
  const double d_0 = dsig(0.0);
  const double d_p = dsig(k_ref);
  if (!(std::isfinite(d_m) && std::isfinite(d_0) && std::isfinite(d_p))) {
    c.edge = true; // domain edge -> a0=a1=a2=0, everything to resid
    return c;
  }
  c.a0 = d_0;
  c.a1 = (d_p - d_m) / (2.0 * k_ref);
  c.a2 = (d_p + d_m - 2.0 * d_0) / (2.0 * k_ref * k_ref);
  return c;
}

// A (uid, T) group key with T compared by its exact bits (never a tolerance), so
// contracts that share a maturity share one pivot sample.
[[nodiscard]] std::uint64_t t_bits(double T) noexcept {
  std::uint64_t b = 0;
  std::memcpy(&b, &T, sizeof b);
  return b;
}

} // namespace

Result<AttributionFrame> pnl_attribution(const std::vector<Position> &book, const SurfaceSet &base,
                                         const SurfaceSet &shifted,
                                         const AttributionOptions &opts) {
  if (!(std::isfinite(opts.k_ref) && opts.k_ref > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "pnl_attribution: k_ref must be finite and > 0");
  }

  // ── One solve: dedup + pnl_explain the book against (base, shifted). ─────────
  ATX_TRY(auto pf, Portfolio::create(book));
  const PortfolioPricer pricer(std::move(pf));
  PriceOptions popts;
  popts.n_threads = opts.n_threads;
  popts.analytic_greeks = opts.analytic_greeks;
  ATX_TRY(auto frame, pricer.pnl_explain(base, shifted, popts));

  const std::span<const Position> positions = pricer.portfolio().positions();
  const std::size_t n = positions.size();

  AttributionFrame out;
  out.rows.resize(n);

  // ── Per unique (uid, T) group: pivot sample -> quadratic coefficients. ───────
  // Memoized in a std::map keyed on (uid, T-bits): deterministic order, computed
  // once per distinct group the first position that references it is visited. Edge
  // groups (NaN pivot / missing surface) are counted once here.
  std::map<std::pair<std::uint32_t, std::uint64_t>, GroupCoef> memo;
  std::size_t n_edge = 0;

  const auto coef_for = [&](std::uint32_t uid, double T) -> const GroupCoef & {
    const std::pair<std::uint32_t, std::uint64_t> key{uid, t_bits(T)};
    auto it = memo.find(key);
    if (it == memo.end()) {
      const GroupCoef c = compute_coef(base.find(uid), shifted.find(uid), T, opts.k_ref);
      if (c.edge) {
        ++n_edge;
      }
      it = memo.emplace(key, c).first;
    }
    return it->second;
  };

  // ── Per position: vega split + 1:1 regroup of the remaining columns. ─────────
  for (std::size_t i = 0; i < n; ++i) {
    const Position &p = positions[i];
    AttributionRow &r = out.rows[i];
    r.id = frame.id[i];
    r.uid = frame.uid[i];
    r.status = frame.status[i];

    // The regrouped PnlFrame columns (bit-exact 1:1). For a failed lane every column
    // is NaN, so these axes are NaN too and the row is gated out of the totals.
    r.pnl_total = frame.pnl_total[i];
    r.spot = frame.pnl_delta[i] + frame.pnl_gamma[i];
    r.vol_second = frame.pnl_volga[i] + frame.pnl_vanna[i];
    r.rates = frame.pnl_rho[i];
    r.time = frame.pnl_theta[i] + frame.pnl_charm[i];
    r.unexplained = frame.pnl_unexplained[i];

    const double pnl_vega = frame.pnl_vega[i];
    const double dvol = frame.d_vol[i];

    // Contract's log-moneyness against the BASE forward at its maturity.
    const SurfaceRef sb = base.find(p.contract.uid);
    const double F = (sb != nullptr) ? sb->forward_at(p.contract.T) : 0.0;
    const double k_i =
        (std::isfinite(F) && F > 0.0) ? std::log(p.contract.K / F) : 0.0;

    const GroupCoef &c = coef_for(p.contract.uid, p.contract.T);

    // Split the frame's vega P&L as fractions of dvol. A zero dvol (no smile move at
    // this strike => pnl_vega is also zero) or an edge group (a0=a1=a2=0) leaves the
    // level/skew/curv pieces at 0; vol_resid is always the exact remainder, so the
    // four sum to pnl_vega bit-for-bit. Forming the ratio (a0/dvol) FIRST makes a
    // pure move exact: a pure level move has a0 == dvol => a0/dvol == 1.0 exactly.
    double v_atf = 0.0;
    double v_skew = 0.0;
    double v_curv = 0.0;
    if (dvol != 0.0 && !c.edge) {
      v_atf = pnl_vega * (c.a0 / dvol);
      v_skew = pnl_vega * ((c.a1 * k_i) / dvol);
      v_curv = pnl_vega * ((c.a2 * k_i * k_i) / dvol);
    }
    r.vol_atf = v_atf;
    r.vol_skew = v_skew;
    r.vol_curv = v_curv;
    r.vol_resid = pnl_vega - v_atf - v_skew - v_curv;
  }

  out.n_pivot_edge_fallback = n_edge;

  // ── Totals: serial fixed-input-order reduction over Ok rows. ─────────────────
  AttributionTotals t{};
  for (std::size_t i = 0; i < n; ++i) {
    const AttributionRow &r = out.rows[i];
    if (r.status != PriceStatus::Ok) {
      continue;
    }
    t.pnl_total += r.pnl_total;
    t.spot += r.spot;
    t.vol_atf += r.vol_atf;
    t.vol_skew += r.vol_skew;
    t.vol_curv += r.vol_curv;
    t.vol_resid += r.vol_resid;
    t.vol_second += r.vol_second;
    t.rates += r.rates;
    t.time += r.time;
    t.unexplained += r.unexplained;
    ++t.n_ok;
  }
  out.total = t;

  return out;
}

} // namespace atx::vol
