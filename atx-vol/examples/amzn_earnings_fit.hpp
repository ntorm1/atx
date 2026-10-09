#pragma once

// AMZN-around-earnings CStar surface FIT (WS-1) — the two-pass calibration shared
// by examples/amzn_earnings_report.cpp (the CSV/JSON emitter) and
// tests/amzn_earnings_test.cpp (the correctness gate), so the report and its
// regression test can never diverge. Header-only, public-API-only.
//
// Pipeline (existing calibrator only; no new curve math):
//   1. data_install the (PM-close-stamped) frame; a Fast VolaSession supplies the
//      per-expiry term forward and is returned for the earnings decomposition.
//   2. Two-pass fit at tier C8. Pass 1: each WELL-QUOTED expiry (>= 50 quotes) gets
//      an independent, theta-monotone-floored eSSVI seed + full price-domain
//      cstar_calibrate_slice. Pass 2: each thin (< 50-quote) expiry is seeded from
//      a calendar-consistent eSSVI interpolated linear-in-T between its well-quoted
//      neighbors and NOT price-refit (its noisy quotes would re-inject an off skew
//      that crosses calendar).
//   3. A light cstar_project_calendar makes total variance non-crossing near-money
//      (damps the later slice's modes then theta; never the front / earliest slice).
//
// Every function here is a pure transform of its inputs (the returned session is
// the only owned state) — safe to call concurrently on distinct panels.

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/fitting/calib.hpp"        // CalibOpts, FitObs, build_observations
#include "fitting/cstar.hpp"        // CStarParams, cstar_slice_iv/_w/_w_derivs
#include "fitting/cstar_calib.hpp"  // cstar_calibrate_slice, cstar_seed_from_essvi
#include "atx/vol/api/marketdata/data.hpp"         // data_install, QuoteFrame
#include "fitting/essvi_calib.hpp"  // essvi_fit_slice
#include "atx/vol/api/marketdata/opra_panel.hpp"   // OpraPanel
#include "atx/vol/api/fitting/session.hpp"      // VolaSession, make_session_inputs, FitPreset
#include "atx/vol/api/core/types.hpp"        // Result, Status
#include "atx/vol/api/marketdata/universe.hpp"     // Universe, Underlying, Chain
#include "atx/vol/api/fitting/vol_surface.hpp"  // EssviParams

namespace atx::vol::amzn {

// ── Config constants ─────────────────────────────────────────────────────────

inline constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;

// VolaDynamics fits this board at C8 (base + shoulder/ATM modes {2,5,8}).
inline constexpr CStarTier kReportTier = CStarTier::C8;

// Below this quote count an expiry is regularized toward its neighbors (thin).
inline constexpr int kThinQuoteFloor = 50;

// Theta-bump cap for the residual near-money calendar projection.
inline constexpr double kCalendarThetaBump = 1.5;

// Near-money log-moneyness knots for the calendar (non-crossing) check.
inline constexpr std::array<double, 5> kCalK = {-0.6, -0.3, 0.0, 0.3, 0.6};

// ── Per-slice fit result ─────────────────────────────────────────────────────

struct SliceFit {
  int expiry_ymd{};
  std::int64_t expiry_ns{};
  double dte{};
  double T{};
  double F{};
  int n_quotes{};
  double atm_vol{};       // sigma0 = sqrt(theta / T)
  double theta{};
  double s2{};
  double c2{};            // atx base c2 (f = 1 + 2*s2*z + c2*z^2)
  double c2_eff{};        // f''(0) = wpp(0) incl. modes == VolaDynamics c2
  double c2_seed{};       // eSSVI-seeded base c2 before the price LM
  double C_left{};
  double C_right{};
  std::array<double, kCStarNModes> beta{};
  double rmse_price{};
  double vol_rmse{};
  double min_roper_g{};
  double arb_damping{};
  CStarTier tier{CStarTier::C16};
  bool reverted{false};
  bool ok{false};
  std::string note{};
  CStarParams params{};
  std::vector<FitObs> obs{};  // market points (for smiles.csv)
};

// Calendar (non-crossing) statistics over the fitted slices.
struct CalStats {
  std::array<double, kCalK.size()> min_dw{};
  int n_viol{};
  std::string detail{};
};

struct FitResult {
  double implied_spot{};
  std::int64_t snapshot_ns{};
  std::string snapshot_iso{};
  double r{};
  std::size_t n_expiries_total{};
  double fit_wall_ms{};
  std::vector<SliceFit> slices{};
  CalStats cal_before{};
  CalStats cal_after{};
  bool calendar_projected_ok{false};
  std::optional<VolaSession> session{};  // Fast session (forwards + earnings)
  bool session_ok{false};
};

// ── Utilities ────────────────────────────────────────────────────────────────

// UTC calendar date (YYYYMMDD) of an epoch-ns instant, via Howard Hinnant's
// civil-from-days (days floored toward -inf). Pure integer arithmetic.
[[nodiscard]] inline int yyyymmdd_from_ns(std::int64_t ns) noexcept {
  std::int64_t days = ns / kNsPerDay;
  if (ns < 0 && (ns % kNsPerDay) != 0) {
    --days;
  }
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = y + (m <= 2 ? 1 : 0);
  return static_cast<int>(year * 10000 + m * 100 + d);
}

[[nodiscard]] inline const char* tier_name(CStarTier t) noexcept {
  switch (t) {
  case CStarTier::C5:
    return "C5";
  case CStarTier::C8:
    return "C8";
  case CStarTier::C12:
    return "C12";
  case CStarTier::C16:
    return "C16";
  }
  return "?";
}

// Diagnostic vol-space RMSE against the calibrator's own observation set.
[[nodiscard]] inline double vol_rmse_of(const CStarParams& s,
                                        std::span<const FitObs> obs) noexcept {
  double sse = 0.0;
  int n = 0;
  for (const FitObs& o : obs) {
    const double iv = cstar_slice_iv(s, o.k);
    if (!std::isfinite(iv) || iv <= 0.0) {
      continue;
    }
    const double dv = iv - o.sigma_mkt;
    sse += dv * dv;
    ++n;
  }
  return n > 0 ? std::sqrt(sse / static_cast<double>(n)) : 0.0;
}

// Recompute every derived field of a slice from its (possibly re-projected) params.
inline void fill_derived(SliceFit& row) {
  const CStarParams& s = row.params;
  row.theta = s.theta;
  row.s2 = s.s2;
  row.c2 = s.c2;
  row.c2_eff = cstar_slice_w_derivs(s, 0.0).wpp;
  row.C_left = s.C_left;
  row.C_right = s.C_right;
  row.beta = s.beta;
  row.tier = s.fit_tier;
  row.reverted = s.reverted_to_seed;
  row.arb_damping = s.arb_damping;
  row.rmse_price = s.rmse_price;
  row.atm_vol = (row.T > 0.0 && s.theta > 0.0)
                    ? std::sqrt(s.theta / row.T)
                    : std::numeric_limits<double>::quiet_NaN();
  row.min_roper_g = cstar_min_roper_g(s);
  row.vol_rmse = vol_rmse_of(s, row.obs);
}

[[nodiscard]] inline double forward_for(
    const std::vector<std::pair<double, double>>& fwds, double T,
    double implied_spot, double r) {
  for (const auto& [t, f] : fwds) {
    if (std::fabs(t - T) < 1.0e-9) {
      return f;
    }
  }
  return implied_spot * std::exp(r * T);  // q = 0 fallback
}

// Interpolate a backbone eSSVI slice at maturity T from the two bracketing
// well-quoted slices (ascending T), linear-in-T on theta/phi/rho. theta is
// calendar-monotone because the source is, so the interpolant is a calendar-
// consistent, cross-expiry-regularized seed. Residual layer stripped (backbone).
//
// The asymmetric-rho blend is RETIRED (T9) and `essvi_backbone_w` returns NaN
// for an armed slice. This routine used to copy the left endpoint wholesale
// (`EssviParams e = a`), inheriting its `rho_scale`, and then interpolate rho
// and rho_R INDEPENDENTLY — so two endpoints that were each unarmed could
// produce an armed interpolant the moment one carried rho_R != rho, and the
// seed would evaluate to NaN at every strike with no diagnostic. Both endpoints
// coming from the calibrator makes that unreachable today, which is an argument
// about the callers rather than about this function. Refuse an armed endpoint
// outright, and stamp the result unarmed by construction rather than by
// inheritance.
[[nodiscard]] inline std::optional<EssviParams> interp_surface_essvi(
    const std::vector<EssviParams>& surf, double T, double F) {
  if (surf.size() < 2) {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < surf.size(); ++i) {
    const EssviParams& a = surf[i - 1];
    const EssviParams& b = surf[i];
    if (atx::vol::essvi_rho_blend_armed(a) || atx::vol::essvi_rho_blend_armed(b)) {
      continue;  // not evaluatable: interpolating it would launder a NaN slice
    }
    if (a.T <= T && T <= b.T && b.T > a.T && a.theta > 0.0 && b.theta > 0.0) {
      const double v = (T - a.T) / (b.T - a.T);
      EssviParams e = a;  // inherit structure only; every blend field is reset
      e.theta = a.theta + v * (b.theta - a.theta);
      e.phi = a.phi + v * (b.phi - a.phi);
      e.rho = a.rho + v * (b.rho - a.rho);
      // Reserved-zero wire vocabulary (vol_surface.hpp): rho_scale == 0 with
      // rho_R == rho is the ONLY unarmed encoding, so write it explicitly.
      e.rho_scale = 0.0;
      e.rho_R = e.rho;
      e.T = T;
      e.F = F;
      e.resid_scale = 0.0;
      e.resid_coef = {};
      e.resid_basis_kind = ResidualBasisKind::None;
      e.resid_n_basis = 0;
      return e;
    }
  }
  return std::nullopt;
}

// Min Δw over consecutive fitted slice pairs at each kCalK knot (a negative value
// flags a calendar crossing). `detail` lists each crossing pair.
[[nodiscard]] inline CalStats calendar_stats(const std::vector<SliceFit>& rows) {
  CalStats cs;
  cs.min_dw.fill(std::numeric_limits<double>::infinity());
  const SliceFit* prev = nullptr;
  for (const SliceFit& row : rows) {
    if (!row.ok) {
      continue;
    }
    if (prev != nullptr) {
      for (std::size_t j = 0; j < kCalK.size(); ++j) {
        const double dw = cstar_slice_w(row.params, kCalK[j]) -
                          cstar_slice_w(prev->params, kCalK[j]);
        cs.min_dw[j] = std::min(cs.min_dw[j], dw);
        if (dw < 0.0) {
          ++cs.n_viol;
          char buf[128];
          std::snprintf(buf, sizeof buf, "    k=%+.1f  %d->%d  dw=%+.3e\n",
                        kCalK[j], prev->expiry_ymd, row.expiry_ymd, dw);
          cs.detail.append(buf);
        }
      }
    }
    prev = &row;
  }
  return cs;
}

// ── The fit ──────────────────────────────────────────────────────────────────

// Fit the AMZN-earnings CStar surface from an already-loaded OPRA panel (loaded
// with OpraLoadSpec::expiry_close = UsEquityPmClose for the true near-dated DTE).
// Returns one SliceFit per chain (ok=false with a note for any that failed), the
// before/after calendar diagnostics, and the Fast session (for the caller's
// earnings decomposition). AMZN is non-div (q=0) so each forward is S*exp(rT)
// when the session drops an expiry.
[[nodiscard]] inline FitResult amzn_earnings_fit(const OpraPanel& panel, double r) {
  FitResult res;
  res.implied_spot = panel.implied_spot;
  res.snapshot_ns = panel.frame.snapshot_ts_ns;
  res.snapshot_iso = panel.snapshot_iso;
  res.r = r;

  Universe u;
  auto uid = data_install(u, panel.frame);
  if (!uid.has_value()) {
    return res;  // no slices; caller sees an empty fit
  }
  auto under = u.get_underlying(*uid);
  if (!under.has_value()) {
    return res;
  }
  const Underlying& U = *under.value();
  res.n_expiries_total = U.chains.size();

  // Fast session: per-expiry forward + the earnings-decomposition input.
  const SessionInputs sin = make_session_inputs(FitPreset::Fast, panel.implied_spot,
                                                r, panel.frame.snapshot_ts_ns);
  auto sess = VolaSession::from_frame(panel.frame, sin);
  std::vector<std::pair<double, double>> fwds;
  if (sess.has_value()) {
    for (const SliceContext& c : sess->expiries()) {
      if (c.T > 0.0 && c.forward > 0.0) {
        fwds.emplace_back(c.T, c.forward);
      }
    }
    res.session.emplace(std::move(sess.value()));
    res.session_ok = true;
  }

  const CalibOpts opts{};
  double wall_ms = 0.0;

  // Pass 1: fit well-quoted slices independently (theta-floored), collect their
  // eSSVI backbones; defer thin expiries.
  std::vector<EssviParams> good_essvi;
  std::vector<std::size_t> thin_idx;
  double prev_essvi_theta = 0.0;

  for (const Chain& chain : U.chains) {
    SliceFit row;
    row.T = chain.T;
    row.expiry_ns = chain.expiry_ns;
    row.expiry_ymd = yyyymmdd_from_ns(chain.expiry_ns);
    row.dte = static_cast<double>(chain.expiry_ns - panel.frame.snapshot_ts_ns) /
              static_cast<double>(kNsPerDay);
    if (!(chain.T > 0.0)) {
      row.note = "skip: non-positive T";
      res.slices.push_back(std::move(row));
      continue;
    }
    const double T = chain.T;
    const double F = forward_for(fwds, T, panel.implied_spot, r);
    const double df = std::exp(-r * T);
    row.F = F;

    auto obs_res = build_observations(chain, F, T, df, opts);
    if (!obs_res.has_value()) {
      row.note = "build_observations: " + obs_res.error().to_string();
      res.slices.push_back(std::move(row));
      continue;
    }
    row.obs = std::move(obs_res.value().obs);
    row.n_quotes = static_cast<int>(row.obs.size());
    const std::span<const FitObs> obs{row.obs};

    if (row.n_quotes < kThinQuoteFloor) {
      thin_idx.push_back(res.slices.size());
      res.slices.push_back(std::move(row));
      continue;
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto essvi_res = essvi_fit_slice(obs, T, F, opts, nullptr, prev_essvi_theta);
    if (!essvi_res.has_value()) {
      const auto t1 = std::chrono::steady_clock::now();
      wall_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
      row.note = "essvi_fit_slice: " + essvi_res.error().to_string();
      res.slices.push_back(std::move(row));
      continue;
    }
    EssviParams essvi = essvi_res.value();
    prev_essvi_theta = essvi.theta;

    auto seed = cstar_seed_from_essvi(essvi, kReportTier);
    row.c2_seed = seed.has_value() ? seed.value().c2
                                   : std::numeric_limits<double>::quiet_NaN();

    auto cstar_res = cstar_calibrate_slice(essvi, chain, df, opts, kReportTier);
    const auto t1 = std::chrono::steady_clock::now();
    wall_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!cstar_res.has_value()) {
      row.note = "cstar_calibrate_slice: " + cstar_res.error().to_string();
      res.slices.push_back(std::move(row));
      continue;
    }
    row.params = cstar_res.value();
    row.ok = true;
    fill_derived(row);
    essvi.T = T;
    essvi.F = F;
    good_essvi.push_back(essvi);
    res.slices.push_back(std::move(row));
  }

  // Pass 2: fill each thin slice from a calendar-consistent interpolated eSSVI
  // seed (no price refit).
  for (const std::size_t idx : thin_idx) {
    SliceFit& row = res.slices[idx];
    auto interp = interp_surface_essvi(good_essvi, row.T, row.F);
    if (!interp.has_value()) {
      row.note = "thin slice: no bracketing well-quoted neighbors to interpolate";
      continue;
    }
    auto seed = cstar_seed_from_essvi(interp.value(), kReportTier);
    if (!seed.has_value()) {
      row.note = "thin slice seed: " + seed.error().to_string();
      continue;
    }
    row.c2_seed = seed.value().c2;
    row.params = seed.value();
    row.ok = true;
    fill_derived(row);
  }
  res.fit_wall_ms = wall_ms;

  // Residual near-money calendar projection (never touches the front slice).
  res.cal_before = calendar_stats(res.slices);
  std::vector<CStarParams> proj_slices;
  std::vector<SliceFit*> ok_rows;
  for (SliceFit& row : res.slices) {
    if (row.ok) {
      proj_slices.push_back(row.params);
      ok_rows.push_back(&row);
    }
  }
  const Status proj =
      cstar_project_calendar(std::span<CStarParams>{proj_slices}, 25u, kCalendarThetaBump);
  res.calendar_projected_ok = proj.has_value();
  for (std::size_t i = 0; i < ok_rows.size(); ++i) {
    ok_rows[i]->params = proj_slices[i];
    fill_derived(*ok_rows[i]);
  }
  res.cal_after = calendar_stats(res.slices);
  return res;
}

}  // namespace atx::vol::amzn
