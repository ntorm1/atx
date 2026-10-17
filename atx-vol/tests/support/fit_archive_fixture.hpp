#pragma once

// Shared fixtures for the ATXVFA archive tests (atx-vol-fit-archive-tests only):
// synthetic fitted surfaces, hand-built fits for every curve kind, bit-exact
// deep comparison, and an image "surgeon" that re-frames an archive after a test
// mutates its sections (fresh checksums), so each refusal is reached by the one
// defect a test plants rather than by a checksum.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/c8.hpp"
#include "atx/vol/api/fitting/dense_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/spline_curve.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "atx/vol/api/storage/fit_archive_series.hpp"

namespace atx::vol::testkit {

[[nodiscard]] inline bool bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] inline std::filesystem::path scratch_dir(std::string_view tag) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-fit-archive-" + std::string(tag);
  if (info != nullptr) name += std::string("-") + info->test_suite_name() + "-" + info->name();
  const auto dir = std::filesystem::temp_directory_path() / name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);
  return dir;
}

// ── Synthetic market ─────────────────────────────────────────────────────────

struct SyntheticChain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// American quotes on sigma(k) = atm + skew * ln(K/S), priced with Andersen-Lake.
[[nodiscard]] inline SyntheticChain synthetic_chain(double years, double atm, double skew,
                                                    double half_spread = 0.02) {
  SyntheticChain out;
  out.market = QuoteSliceMarket{100.0, years, years, 0.03, 0.01, 0.0};
  for (int strike = 75; strike <= 125; strike += 5) {
    const double k = std::log(static_cast<double>(strike) / 100.0);
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(100.0, static_cast<double>(strike), years, atm + skew * k,
                                       0.03, 0.01, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * half_spread);
      out.quotes.push_back({static_cast<double>(strike), side, std::max(*price - spread, 0.0),
                            *price + spread});
    }
  }
  return out;
}

struct SyntheticSurface {
  std::vector<SyntheticChain> chains;
  std::vector<std::int32_t> expiry_days;
  QuoteSurfaceConfig config;
  std::vector<QuoteSurfaceSlice> slices;
};

// Three expiries through the library's graded admission (fit_quote_surface).
[[nodiscard]] inline SyntheticSurface synthetic_surface(double atm = 0.22) {
  SyntheticSurface s;
  s.chains = {synthetic_chain(0.08, atm + 0.02, -0.12), synthetic_chain(0.25, atm, -0.10),
              synthetic_chain(0.60, atm - 0.01, -0.08)};
  s.expiry_days = {20770, 20833, 20959};
  s.config.calendar_repair_half_spreads = 0.5;
  std::vector<QuoteSurfaceExpiry> expiries;
  for (const auto &c : s.chains) expiries.push_back({c.market, c.quotes, {}});
  auto fitted = fit_quote_surface(expiries, QuoteSliceConfig{}, s.config);
  if (!fitted) {
    ADD_FAILURE() << fitted.error().to_string();
    return s;
  }
  s.slices = std::move(*fitted);
  return s;
}

// ── Hand-built fits: every curve kind, every optional field engaged ─────────

[[nodiscard]] inline std::unique_ptr<IVolCurve> curve_of(VolCurveKind kind) {
  constexpr double T = 0.25, F = 101.5, df = 0.9925;
  switch (kind) {
  case VolCurveKind::ConvexDense: {
    ConvexSliceFit f;
    f.T = T;
    f.F = F;
    f.df = df;
    f.u = {80.0, 90.0, 100.0, 110.0, 120.0};
    f.C = {21.7, 12.9, 5.8, 1.9, 0.45};
    f.rmse_price = 0.31;
    f.n_obs = 22;
    f.n_active = 3;
    f.qp_iterations = 17;
    f.qp_stationarity = 1.0e-9;
    f.qp_primal_violation = 2.0e-12;
    f.qp_complementarity = 3.0e-11;
    f.qp_dual_violation = 0.0;
    f.effective_lambda = 4.2e-4;
    f.noise_scale = 1.7;
    return std::make_unique<ConvexDenseCurve>(std::move(f));
  }
  case VolCurveKind::Essvi: {
    EssviParams p{};
    p.theta = 0.012;
    p.phi = 0.9;
    p.rho = -0.35;
    p.psi = 0.4;
    p.p = 0.3;
    p.lambda = 0.6;
    p.T = T;
    p.F = F;
    p.expiry_ns = 1791988200000000000LL;
    p.expiry_id = 3;
    p.resid_coef[1] = 0.01;
    p.resid_coef[4] = -0.002;
    p.resid_scale = 0.15;
    p.resid_basis_kind = ResidualBasisKind::HingeQuad;
    p.resid_n_basis = 4;
    return std::make_unique<EssviCurve>(p, df);
  }
  case VolCurveKind::Svi: {
    SviParams p{};
    p.a = 0.004;
    p.b = 0.08;
    p.rho = -0.42;
    p.m = 0.015;
    p.sigma = 0.11;
    p.T = T;
    p.F = F;
    p.expiry_ns = 1791988200000000001LL;
    p.expiry_id = 1;
    return std::make_unique<SviCurve>(p, df);
  }
  case VolCurveKind::LinearVariance:
    return std::make_unique<LinearVarianceCurve>(T, F, df, std::vector<double>{-0.2, -0.05, 0.0, 0.07, 0.2},
                                                 std::vector<double>{0.019, 0.0135, 0.0121, 0.0118, 0.0142});
  case VolCurveKind::C8: {
    C8Params p{};
    p.T = T;
    p.F = F;
    p.expiry_ns = 1791988200000000002LL;
    p.expiry_id = 5;
    p.v = 0.05;
    p.psi = 0.2;
    p.p = 0.6;
    p.c = 0.4;
    p.v_min = 0.03;
    p.kappa = 0.7;
    p.h_atm = 0.002;
    p.k_L = -0.1;
    p.h_L = 0.001;
    p.k_R = 0.12;
    p.h_R = 0.0015;
    p.rmse_vol = 0.004;
    p.n_lm_iters = 11;
    p.n_irls_iters = 2;
    p.bumps_active = false;
    return std::make_unique<C8Curve>(p, df);
  }
  case VolCurveKind::SplineVol: {
    SplineVolParams p;
    p.atm_vol = 0.21;
    p.z = {-2.0, -1.0, 0.0, 1.0, 2.0};
    p.mult = {1.22, 1.07, 1.0, 0.97, 1.02};
    p.z_lo_valid = -1.8;
    p.z_hi_valid = 1.9;
    p.mult_cap = 3.0;
    p.w_offset = 1.0e-4;
    p.n_butterfly_viol = 2;
    return std::make_unique<SplineVolCurve>(std::move(p), T, F, df);
  }
  }
  return nullptr;
}

// A QuoteSurfaceSlice whose every field differs from its default: the real
// slice's certified prices and rows, plus a hand-built curve of `kind`.
[[nodiscard]] inline QuoteSurfaceSlice exotic_slice(const QuoteSurfaceSlice &real, VolCurveKind kind) {
  QuoteSurfaceSlice s;
  QuoteSliceFit &f = s.fit;
  f.fit_status = QuoteSliceFitStatus::Success;
  f.fit_error = "hand-built " + std::string(to_string(kind));
  f.diagnostics = real.fit.diagnostics;
  f.diagnostics.curve_fit.final_grad_norm = 3.5e-7;
  f.diagnostics.curve_fit.termination = FitTermination::IterationCap;
  f.diagnostics.curve_fit.projection = SliceProjection::Moved;
  f.diagnostics.curve_fit.reverted_to_seed = true;
  f.diagnostics.curve_fit.warm_started = true;
  f.diagnostics.channel.used = true;
  f.diagnostics.channel.term_prior = true;
  f.diagnostics.channel.converged = true;
  f.diagnostics.channel.repair_max_half_spreads = std::numeric_limits<double>::infinity();
  f.diagnostics.channel.solve_seconds = 0.0123; // wall clock: archived only with record_timings
  f.diagnostics.repair = {QuoteSliceRepairOutcome::Applied, 2, 1, 0.012, 0.004, 0.0, 1};
  f.curve = curve_of(kind);
  f.prices = real.fit.prices;
  if (f.prices) {
    f.moved_nodes.resize(f.prices->nodes().size());
    for (std::size_t i = 0; i < f.moved_nodes.size(); ++i) f.moved_nodes[i] = static_cast<std::uint8_t>(i % 4);
  }
  f.rows = real.fit.rows;
  f.market = real.fit.market;
  f.dividend_model = QuoteSliceDividendModel::DatedCash;
  f.al_opts = AlOpts{.n_collocation = 9, .n_quadrature = 18, .n_quad_price = 32, .max_newton_iter = 5,
                     .tol = 1.0e-9};
  f.discrete_div_steps = 401;
  f.discrete_div_scheme = DiscreteDivScheme::EscrowedLeisenReimer;
  f.variance_clock_dividends = {{0.05, 0.42}, {0.21, 0.43}};
  f.carry_curve = {{80.0, 0.011}, {100.0, 0.012}, {120.0, 0.0135}};
  f.carry_resolution.mode = QuoteSliceCarryMode::ImpliedForward;
  f.carry_resolution.applied = true;
  f.carry_resolution.status = ImpliedForwardStatus::Ok;
  f.carry_resolution.reason = "implied forward applied";
  f.carry_resolution.implied_forward = 101.25;
  f.carry_resolution.pairs_valid = 9;
  f.carry_resolution.pairs_retained = 7;
  s.grade = QuoteSliceGrade::B;
  s.refusal = "";
  s.error = "detail text";
  s.point_status = QuoteSliceFitStatus::ArbitrageFailure;
  s.channel_refit = true;
  s.calendar_retry = true;
  s.calendar_repaired = true;
  s.calendar_gap = 0.031;
  s.calendar_gap_strike = 105.0;
  s.calendar_repair_required = std::numeric_limits<double>::infinity();
  s.point_error = "InvalidArgument: AmericanPriceSlice: node 3";
  s.repair_rung = true;
  s.rung_repair = {QuoteSliceRepairOutcome::NodeBudget, 17, 17, 0.2, 0.03, 0.01, 0};
  s.calendar_channel_repaired = true;
  s.calendar_channel_repair = {QuoteSliceRepairOutcome::NotCertified, 1, 0, 1.0e-5, 0.0, 0.0, 0};
  s.point_outside_fraction = 0.41;
  s.point_outside_quotes = 12;
  s.point_gated = true;
  s.point_fallback = true;
  s.calendar_readmitted = true;
  return s;
}

// ── Bit-exact deep comparison (first difference as text, empty when equal) ──

class Diff {
public:
  void check(bool ok, std::string_view what) {
    if (!ok && first_.empty()) first_ = what;
  }
  void num(double a, double b, std::string_view what) { check(bits(a, b), what); }
  [[nodiscard]] const std::string &first() const noexcept { return first_; }

private:
  std::string first_;
};

inline void diff_repair(Diff &d, const QuoteSliceRepairRecord &a, const QuoteSliceRepairRecord &b,
                        std::string_view where) {
  d.check(a.outcome == b.outcome && a.nodes == b.nodes && a.material_nodes == b.material_nodes &&
              bits(a.max_price_move, b.max_price_move) && bits(a.max_vol_move, b.max_vol_move) &&
              bits(a.max_unquoted_vol_move, b.max_unquoted_vol_move) && a.unidentified == b.unidentified,
          where);
}

inline void diff_curve(Diff &d, const IVolCurve *a, const IVolCurve *b) {
  d.check((a == nullptr) == (b == nullptr), "curve presence");
  if (a == nullptr || b == nullptr) return;
  d.check(a->kind() == b->kind(), "curve kind");
  if (a->kind() != b->kind()) return;
  d.num(a->T(), b->T(), "curve T");
  d.num(a->F(), b->F(), "curve F");
  d.num(a->df(), b->df(), "curve df");
  for (double k = -0.6; k <= 0.6; k += 0.05) d.num(a->w(k), b->w(k), "curve w(k)");
  d.check(a->dof() == b->dof(), "curve dof");
  const auto same_vec = [](const std::vector<double> &x, const std::vector<double> &y) {
    return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), bits);
  };
  switch (a->kind()) {
  case VolCurveKind::ConvexDense: {
    const auto &x = static_cast<const ConvexDenseCurve *>(a)->fit();
    const auto &y = static_cast<const ConvexDenseCurve *>(b)->fit();
    d.check(bits(x.T, y.T) && bits(x.F, y.F) && bits(x.df, y.df) && same_vec(x.u, y.u) &&
                same_vec(x.C, y.C) && bits(x.rmse_price, y.rmse_price) && x.n_obs == y.n_obs &&
                x.n_active == y.n_active && x.qp_iterations == y.qp_iterations &&
                bits(x.qp_stationarity, y.qp_stationarity) &&
                bits(x.qp_primal_violation, y.qp_primal_violation) &&
                bits(x.qp_complementarity, y.qp_complementarity) &&
                bits(x.qp_dual_violation, y.qp_dual_violation) &&
                bits(x.effective_lambda, y.effective_lambda) && bits(x.noise_scale, y.noise_scale),
            "convex-dense params");
    break;
  }
  case VolCurveKind::Essvi: {
    const auto &x = static_cast<const EssviCurve *>(a)->slice();
    const auto &y = static_cast<const EssviCurve *>(b)->slice();
    d.check(bits(x.theta, y.theta) && bits(x.phi, y.phi) && bits(x.rho, y.rho) && bits(x.rho_R, y.rho_R) &&
                bits(x.rho_scale, y.rho_scale) && bits(x.psi, y.psi) && bits(x.p, y.p) &&
                bits(x.lambda, y.lambda) && bits(x.lambda_R, y.lambda_R) && bits(x.T, y.T) &&
                bits(x.F, y.F) && x.expiry_ns == y.expiry_ns && x.expiry_id == y.expiry_id &&
                std::equal(x.resid_coef.begin(), x.resid_coef.end(), y.resid_coef.begin(), bits) &&
                bits(x.resid_scale, y.resid_scale) && x.resid_basis_kind == y.resid_basis_kind &&
                x.resid_n_basis == y.resid_n_basis,
            "essvi params");
    break;
  }
  case VolCurveKind::Svi: {
    const auto &x = static_cast<const SviCurve *>(a)->slice();
    const auto &y = static_cast<const SviCurve *>(b)->slice();
    d.check(bits(x.a, y.a) && bits(x.b, y.b) && bits(x.rho, y.rho) && bits(x.m, y.m) &&
                bits(x.sigma, y.sigma) && bits(x.T, y.T) && bits(x.F, y.F) && x.expiry_ns == y.expiry_ns &&
                x.expiry_id == y.expiry_id,
            "svi params");
    break;
  }
  case VolCurveKind::LinearVariance: {
    const auto *x = static_cast<const LinearVarianceCurve *>(a);
    const auto *y = static_cast<const LinearVarianceCurve *>(b);
    d.check(std::equal(x->k_nodes().begin(), x->k_nodes().end(), y->k_nodes().begin(), y->k_nodes().end(), bits) &&
                std::equal(x->w_nodes().begin(), x->w_nodes().end(), y->w_nodes().begin(), y->w_nodes().end(), bits),
            "linear-variance nodes");
    break;
  }
  case VolCurveKind::C8: {
    const auto &x = static_cast<const C8Curve *>(a)->slice();
    const auto &y = static_cast<const C8Curve *>(b)->slice();
    d.check(bits(x.T, y.T) && bits(x.F, y.F) && x.expiry_ns == y.expiry_ns && x.expiry_id == y.expiry_id &&
                bits(x.v, y.v) && bits(x.psi, y.psi) && bits(x.p, y.p) && bits(x.c, y.c) &&
                bits(x.v_min, y.v_min) && bits(x.kappa, y.kappa) && bits(x.q_L, y.q_L) &&
                bits(x.q_R, y.q_R) && bits(x.h_atm, y.h_atm) && bits(x.k_L, y.k_L) && bits(x.h_L, y.h_L) &&
                bits(x.k_R, y.k_R) && bits(x.h_R, y.h_R) && bits(x.arb_damping_factor, y.arb_damping_factor) &&
                bits(x.rmse_price, y.rmse_price) && bits(x.rmse_vol, y.rmse_vol) &&
                x.n_lm_iters == y.n_lm_iters && x.n_irls_iters == y.n_irls_iters &&
                x.bumps_active == y.bumps_active,
            "c8 params");
    break;
  }
  case VolCurveKind::SplineVol: {
    const auto &x = static_cast<const SplineVolCurve *>(a)->params();
    const auto &y = static_cast<const SplineVolCurve *>(b)->params();
    d.check(bits(x.atm_vol, y.atm_vol) && same_vec(x.z, y.z) && same_vec(x.mult, y.mult) &&
                bits(x.z_lo_valid, y.z_lo_valid) && bits(x.z_hi_valid, y.z_hi_valid) &&
                bits(x.mult_cap, y.mult_cap) && bits(x.w_offset, y.w_offset) &&
                x.n_butterfly_viol == y.n_butterfly_viol,
            "spline-vol params");
    break;
  }
  }
}

inline void diff_fit(Diff &d, const QuoteSliceFit &a, const QuoteSliceFit &b) {
  d.check(a.fit_status == b.fit_status, "fit_status");
  d.check(a.fit_error == b.fit_error, "fit_error");
  const auto &x = a.diagnostics;
  const auto &y = b.diagnostics;
  d.check(x.quotes_seen == y.quotes_seen && x.iv_attempted == y.iv_attempted &&
              x.quotes_admitted == y.quotes_admitted && x.distinct_strikes == y.distinct_strikes &&
              x.prices_produced == y.prices_produced && x.weights_capped == y.weights_capped,
          "diagnostics counts");
  d.check(bits(x.forward, y.forward) && bits(x.discount, y.discount) &&
              bits(x.effective_rate, y.effective_rate) && bits(x.effective_yield, y.effective_yield),
          "diagnostics carry");
  const auto &f = x.curve_fit;
  const auto &g = y.curve_fit;
  d.check(bits(f.rmse_vol_vega_weighted, g.rmse_vol_vega_weighted) && bits(f.max_residual_vol, g.max_residual_vol) &&
              f.outer_iters == g.outer_iters && f.inner_iters_total == g.inner_iters_total &&
              f.n_quotes_used == g.n_quotes_used && f.n_butterfly_viol == g.n_butterfly_viol &&
              f.n_calendar_viol == g.n_calendar_viol && f.termination == g.termination &&
              f.projection == g.projection && f.reverted_to_seed == g.reverted_to_seed &&
              f.warm_started == g.warm_started && f.final_grad_norm.has_value() == g.final_grad_norm.has_value() &&
              (!f.final_grad_norm || bits(*f.final_grad_norm, *g.final_grad_norm)),
          "curve_fit diag");
  const auto &c = x.channel;
  const auto &e = y.channel;
  d.check(c.used == e.used && c.channel_quotes == e.channel_quotes && c.mid_quotes == e.mid_quotes &&
              c.mid_strikes == e.mid_strikes && c.violations == e.violations &&
              bits(c.max_violation_half_widths, e.max_violation_half_widths) &&
              bits(c.prior_strength, e.prior_strength) && c.term_prior == e.term_prior &&
              c.converged == e.converged && c.iterations == e.iterations &&
              bits(c.solve_seconds, e.solve_seconds) && c.repaired_nodes == e.repaired_nodes &&
              bits(c.repair_max_price, e.repair_max_price) &&
              bits(c.repair_max_half_spreads, e.repair_max_half_spreads),
          "channel diag");
  diff_repair(d, x.repair, y.repair, "diagnostics repair");
  diff_curve(d, a.curve.get(), b.curve.get());
  d.check(a.prices.has_value() == b.prices.has_value(), "prices presence");
  if (a.prices && b.prices) {
    const auto n = a.prices->nodes();
    const auto m = b.prices->nodes();
    d.check(n.size() == m.size() && std::equal(n.begin(), n.end(), m.begin(), [](const auto &p, const auto &q) {
              return bits(p.strike, q.strike) && bits(p.call, q.call) && bits(p.put, q.put);
            }),
            "price nodes");
    const auto &u = a.prices->bounds();
    const auto &v = b.prices->bounds();
    d.check(bits(u.spot, v.spot) && bits(u.discount_min, v.discount_min) && bits(u.discount_max, v.discount_max) &&
                bits(u.prepaid_min, v.prepaid_min) && bits(u.prepaid_max, v.prepaid_max),
            "price bounds");
  }
  d.check(a.moved_nodes == b.moved_nodes, "moved_nodes");
  d.check(a.rows.size() == b.rows.size() &&
              std::equal(a.rows.begin(), a.rows.end(), b.rows.begin(), [](const auto &p, const auto &q) {
                return p.quote_status == q.quote_status && p.evaluation_status == q.evaluation_status &&
                       p.admitted == q.admitted && bits(p.market_iv, q.market_iv) &&
                       bits(p.fitted_iv, q.fitted_iv) && bits(p.fitted_price, q.fitted_price) &&
                       bits(p.spread_vol, q.spread_vol) && bits(p.weight_w, q.weight_w) &&
                       p.weight_capped == q.weight_capped;
              }),
          "rows");
  d.check(bits(a.market.spot, b.market.spot) && bits(a.market.variance_years, b.market.variance_years) &&
              bits(a.market.calendar_years, b.market.calendar_years) && bits(a.market.rate, b.market.rate) &&
              bits(a.market.continuous_yield, b.market.continuous_yield) &&
              bits(a.market.dividend_pv, b.market.dividend_pv),
          "market");
  d.check(a.dividend_model == b.dividend_model, "dividend_model");
  d.check(a.al_opts.has_value() == b.al_opts.has_value() &&
              (!a.al_opts || (a.al_opts->n_collocation == b.al_opts->n_collocation &&
                              a.al_opts->n_quadrature == b.al_opts->n_quadrature &&
                              a.al_opts->n_quad_price == b.al_opts->n_quad_price &&
                              a.al_opts->max_newton_iter == b.al_opts->max_newton_iter &&
                              bits(a.al_opts->tol, b.al_opts->tol))),
          "al_opts");
  d.check(a.discrete_div_steps == b.discrete_div_steps && a.discrete_div_scheme == b.discrete_div_scheme,
          "discrete dividend scheme");
  d.check(a.variance_clock_dividends.size() == b.variance_clock_dividends.size() &&
              std::equal(a.variance_clock_dividends.begin(), a.variance_clock_dividends.end(),
                         b.variance_clock_dividends.begin(),
                         [](const auto &p, const auto &q) { return bits(p.tau, q.tau) && bits(p.amount, q.amount); }),
          "dividends");
  d.check(a.carry_curve.size() == b.carry_curve.size() &&
              std::equal(a.carry_curve.begin(), a.carry_curve.end(), b.carry_curve.begin(),
                         [](const auto &p, const auto &q) {
                           return bits(p.strike, q.strike) && bits(p.continuous_yield, q.continuous_yield);
                         }),
          "carry curve");
  const auto &r = a.carry_resolution;
  const auto &t = b.carry_resolution;
  d.check(r.mode == t.mode && r.applied == t.applied && r.fallback == t.fallback && r.status == t.status &&
              std::string_view(r.reason ? r.reason : "") == std::string_view(t.reason ? t.reason : "") &&
              bits(r.supplied_forward, t.supplied_forward) && bits(r.supplied_yield, t.supplied_yield) &&
              bits(r.implied_forward, t.implied_forward) && bits(r.implied_yield, t.implied_yield) &&
              bits(r.implied_total_yield, t.implied_total_yield) && bits(r.parity_forward, t.parity_forward) &&
              bits(r.forward_lower, t.forward_lower) && bits(r.forward_upper, t.forward_upper) &&
              bits(r.dispersion_sd, t.dispersion_sd) && bits(r.exercise_shift_sd, t.exercise_shift_sd) &&
              bits(r.atm_total_vol, t.atm_total_vol) && r.pairs_valid == t.pairs_valid &&
              r.pairs_selected == t.pairs_selected && r.pairs_retained == t.pairs_retained &&
              r.pairs_contaminated == t.pairs_contaminated && r.pairs_unidentified == t.pairs_unidentified,
          "carry resolution");
}

[[nodiscard]] inline std::string diff_slice(const QuoteSurfaceSlice &a, const QuoteSurfaceSlice &b) {
  Diff d;
  diff_fit(d, a.fit, b.fit);
  d.check(a.grade == b.grade && a.refusal == b.refusal && a.error == b.error && a.point_status == b.point_status,
          "grade / refusal");
  d.check(a.channel_refit == b.channel_refit && a.calendar_retry == b.calendar_retry &&
              a.calendar_repaired == b.calendar_repaired && a.repair_rung == b.repair_rung &&
              a.calendar_channel_repaired == b.calendar_channel_repaired && a.point_gated == b.point_gated &&
              a.point_fallback == b.point_fallback && a.calendar_readmitted == b.calendar_readmitted,
          "slice flags");
  d.check(bits(a.calendar_gap, b.calendar_gap) && bits(a.calendar_gap_strike, b.calendar_gap_strike) &&
              bits(a.calendar_repair_required, b.calendar_repair_required) &&
              bits(a.point_outside_fraction, b.point_outside_fraction) &&
              a.point_outside_quotes == b.point_outside_quotes && a.point_error == b.point_error,
          "slice scalars");
  diff_repair(d, a.rung_repair, b.rung_repair, "rung_repair");
  diff_repair(d, a.calendar_channel_repair, b.calendar_channel_repair, "calendar_channel_repair");
  return d.first();
}

// ── Image surgeon ────────────────────────────────────────────────────────────

struct ImageSection {
  FitArchiveSectionInput meta;
  std::vector<std::byte> bytes;
};

// Split a framed image into its sections (no validation: tests feed it images
// the writer produced).
[[nodiscard]] inline std::vector<ImageSection> split_image(std::span<const std::byte> image) {
  FitArchiveHeader h{};
  std::memcpy(&h, image.data(), sizeof h);
  std::vector<ImageSection> out;
  for (std::uint32_t i = 0; i < h.section_count; ++i) {
    FitArchiveSectionEntry e{};
    std::memcpy(&e, image.data() + sizeof h + i * sizeof e, sizeof e);
    ImageSection s;
    s.meta = FitArchiveSectionInput{static_cast<FitArchiveSectionType>(e.type), e.type_major, e.type_minor,
                                    e.element_stride, e.element_align, e.flags, {}};
    s.bytes.assign(image.begin() + static_cast<std::ptrdiff_t>(e.offset),
                   image.begin() + static_cast<std::ptrdiff_t>(e.offset + e.length));
    out.push_back(std::move(s));
  }
  return out;
}

[[nodiscard]] inline std::vector<std::byte> join_image(const std::vector<ImageSection> &sections,
                                                       const FitArchiveFileInfo &info = {}) {
  std::vector<FitArchiveSectionInput> inputs;
  for (const auto &s : sections) {
    FitArchiveSectionInput in = s.meta;
    in.bytes = s.bytes;
    inputs.push_back(in);
  }
  auto image = build_fit_archive(inputs, info);
  EXPECT_TRUE(image.has_value());
  return image ? std::move(*image) : std::vector<std::byte>{};
}

[[nodiscard]] inline ImageSection *find_section(std::vector<ImageSection> &s, FitArchiveSectionType t) {
  for (auto &x : s) {
    if (x.meta.type == t) return &x;
  }
  return nullptr;
}

template <class T> [[nodiscard]] T read_at(const std::vector<std::byte> &b, std::size_t at) {
  T v{};
  std::memcpy(&v, b.data() + at, sizeof v);
  return v;
}

template <class T> void write_at(std::vector<std::byte> &b, std::size_t at, const T &v) {
  std::memcpy(b.data() + at, &v, sizeof v);
}

// Recompute the header CRC (after a deliberate header edit), then the footer's
// copies and CRCs, so only the planted defect remains.
inline void reseal(std::vector<std::byte> &image) {
  FitArchiveHeader h = read_at<FitArchiveHeader>(image, 0);
  h.header_crc32c = 0;
  h.header_crc32c = fit_archive_crc32c(std::as_bytes(std::span{&h, 1}));
  write_at(image, 0, h);
  const std::size_t footer_at = image.size() - sizeof(FitArchiveFooter);
  FitArchiveFooter f = read_at<FitArchiveFooter>(image, footer_at);
  f.header_crc32c = h.header_crc32c;
  f.file_crc32c = fit_archive_crc32c(std::span<const std::byte>{image.data(), footer_at});
  f.footer_crc32c = 0;
  f.footer_crc32c = fit_archive_crc32c(std::as_bytes(std::span{&f, 1}));
  write_at(image, footer_at, f);
}

[[nodiscard]] inline ArchiveStatus open_status(std::span<const std::byte> image,
                                               FitArchiveVerify verify = FitArchiveVerify::Full) {
  auto a = FitArchive::open_copy(image, verify);
  return a ? ArchiveStatus::Ok : a.error().status;
}

[[nodiscard]] inline std::vector<std::byte> read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::vector<std::byte> out(raw.size());
  if (!raw.empty()) std::memcpy(out.data(), raw.data(), raw.size());
  return out;
}

// `image` with fit `fit`'s curve node_count replaced and every checksum
// re-signed, so only the planted count can cause a refusal. A count of
// n + 2^31 doubles to 2n + 2^32: in u32 arithmetic exactly the stored payload.
[[nodiscard]] inline std::vector<std::byte> forge_node_count(std::span<const std::byte> image, std::size_t fit,
                                                             std::uint32_t node_count) {
  FitArchiveHeader h{};
  EXPECT_GE(image.size(), sizeof h);
  if (image.size() < sizeof h) return {};
  std::memcpy(&h, image.data(), sizeof h);
  auto sections = split_image(image);
  ImageSection *fits = find_section(sections, FitArchiveSectionType::FitRecords);
  EXPECT_NE(fits, nullptr);
  if (fits == nullptr) return {};
  const std::size_t at = fit * sizeof(FitArchiveFitRecord) + offsetof(FitArchiveFitRecord, curve) +
                         offsetof(FitArchiveCurve, node_count);
  EXPECT_LE(at + sizeof node_count, fits->bytes.size());
  if (at + sizeof node_count > fits->bytes.size()) return {};
  write_at(fits->bytes, at, node_count);
  return join_image(sections, {h.created_unix_ns, h.archive_id, h.segment_sequence});
}

// Rewrite manifest entry `index` of the series at `series_dir` with `edit` and
// re-sign every checksum, so only the planted change can cause a refusal.
template <class Edit>
void forge_segment(const std::filesystem::path &series_dir, std::size_t index, Edit edit) {
  const auto path = series_dir / std::string(kFitArchiveSeriesManifest);
  const auto image = read_file(path);
  ASSERT_GE(image.size(), sizeof(FitArchiveHeader));
  const auto h = read_at<FitArchiveHeader>(image, 0);
  auto sections = split_image(image);
  ImageSection *segments = find_section(sections, FitArchiveSectionType::SeriesSegments);
  ASSERT_NE(segments, nullptr);
  const std::size_t at = index * sizeof(FitArchiveSeriesSegment);
  ASSERT_LE(at + sizeof(FitArchiveSeriesSegment), segments->bytes.size());
  auto entry = read_at<FitArchiveSeriesSegment>(segments->bytes, at);
  edit(entry);
  write_at(segments->bytes, at, entry);
  const auto forged = join_image(sections, {h.created_unix_ns, h.archive_id, h.segment_sequence});
  ASSERT_TRUE(publish_fit_archive(path.string(), forged).has_value());
}

// Rewrite the file name of manifest entry `index` (NUL padded; `name` may hold
// embedded NULs) and re-sign every checksum.
inline void forge_segment_name(const std::filesystem::path &series_dir, std::size_t index,
                               std::string_view name) {
  ASSERT_LT(name.size(), FitArchiveSeriesSegment{}.file_name.size());
  forge_segment(series_dir, index, [&](FitArchiveSeriesSegment &entry) {
    entry.file_name.fill('\0');
    std::copy(name.begin(), name.end(), entry.file_name.begin());
  });
}

// Names a manifest entry for sequence 1 must never carry: the review's escape
// forms (parent directory both ways, absolute, drive-relative, embedded NUL)
// plus a subdirectory, another sequence and an unpadded sequence. Each fits the
// 40-byte field with a NUL to spare.
[[nodiscard]] inline std::vector<std::string> foreign_segment_names() {
  const std::string own = "seg-000000000001.atxvfa";
  const std::string nul(1, '\0');
  return {"..\\" + own,
          "../" + own,
          "C:\\" + own,
          "C:" + own,
          own + nul + "..\\x",
          "seg-0000" + nul + "00000001.atxvfa",
          "sub\\" + own,
          "seg-000000000002.atxvfa",
          "seg-1.atxvfa"};
}

// A forged name for test messages, with each NUL shown as \0.
[[nodiscard]] inline std::string printable(std::string_view name) {
  std::string out;
  for (const char c : name) out += c == '\0' ? std::string("\\0") : std::string(1, c);
  return out;
}

} // namespace atx::vol::testkit
