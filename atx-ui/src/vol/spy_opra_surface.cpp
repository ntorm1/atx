#include "atx/ui/opra_vol_surface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "atx/ui/vol_workspace_model.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/chain.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/universe.hpp"

namespace atx::ui {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

atx::vol::FitQualityMode to_engine_quality(UiFitQualityMode mode) {
  switch (mode) {
  case UiFitQualityMode::Latency:
    return atx::vol::FitQualityMode::Latency;
  case UiFitQualityMode::Accuracy:
    return atx::vol::FitQualityMode::Accuracy;
  case UiFitQualityMode::Balanced:
  default:
    return atx::vol::FitQualityMode::Balanced;
  }
}

} // namespace

struct OpraVolSurface::Impl {
  OpraSourceConfig config;
  SurfaceSourceInfo source_info;
  std::optional<atx::vol::OpraPanel> panel;
  std::unique_ptr<atx::vol::OptionChain> chain;
  std::unique_ptr<atx::vol::PricerFitter> fitter;
  std::vector<ExpiryInfo> expiries;
  VolCurveSlice slice;
  SurfaceDiagnostics diagnostics;
  std::string error;
  std::size_t selected{0};
  double fit_ms{0.0};
};

OpraVolSurface::OpraVolSurface() : impl_(std::make_unique<Impl>()) {}
OpraVolSurface::~OpraVolSurface() = default;
OpraVolSurface::OpraVolSurface(OpraVolSurface &&) noexcept = default;
OpraVolSurface &OpraVolSurface::operator=(OpraVolSurface &&) noexcept = default;

bool OpraVolSurface::load(const OpraSourceConfig &config) {
  impl_ = std::make_unique<Impl>();
  impl_->config = config;
  impl_->source_info = SurfaceSourceInfo{
      .provider = "DATABENTO",
      .feed = "OPRA CBBO-1M",
      .path = config.path,
      .symbol = config.symbol,
      .snapshot_iso = config.snapshot_iso,
  };

  atx::vol::OpraLoadSpec spec;
  spec.path = config.path;
  spec.underlying = config.symbol;
  spec.snapshot_iso = config.snapshot_iso;
  spec.r = config.rate;

  auto panel_result = atx::vol::load_opra_cbbo_parquet(spec);
  if (!panel_result.has_value()) {
    impl_->error = "OPRA load failed: " + panel_result.error().message();
    return false;
  }
  impl_->panel.emplace(std::move(*panel_result));

  auto chain_result = atx::vol::OptionChain::from_frame(impl_->panel->frame, config.rate,
                                                        impl_->panel->implied_spot);
  if (!chain_result.has_value()) {
    impl_->error = "chain build failed: " + chain_result.error().message();
    return false;
  }
  impl_->chain = std::make_unique<atx::vol::OptionChain>(std::move(*chain_result));

  atx::vol::PricerConfig fit_config;
  fit_config.quality_mode = to_engine_quality(config.quality_mode);
  fit_config.outputs = atx::vol::SurfaceOutputs::MarketMarkAndRisk;
  impl_->fitter = std::make_unique<atx::vol::PricerFitter>(std::move(fit_config));
  const Clock::time_point fit_start = Clock::now();
  const atx::vol::Status fit_status = impl_->fitter->fit(*impl_->chain);
  impl_->fit_ms = milliseconds_since(fit_start);
  if (!fit_status.has_value()) {
    impl_->error = "surface fit failed: " + fit_status.error().message();
    return false;
  }

  const atx::vol::SurfaceBundle bundle = impl_->fitter->bundle();
  const atx::vol::FittedSurface *fitted = bundle.risk.get();
  if (fitted == nullptr) {
    impl_->error = "surface fit completed without an admitted risk surface";
    return false;
  }
  const atx::vol::SessionDiagnostics &diag = fitted->diagnostics();
  impl_->diagnostics = SurfaceDiagnostics{
      .worst_in_band = diag.worst_frac_within_bidask,
      .mean_in_band = diag.mean_frac_within_bidask,
      .mean_rmse_vol = diag.mean_rmse_vol,
      .fitted_slices = diag.n_slices,
      .fitted_quotes = diag.n_quotes,
      .calendar_violations = diag.n_calendar_viol_pre,
      .calendar_arb_free = diag.calendar_arb_free,
      .risk_state = std::string(atx::vol::to_string(bundle.risk_health.state)),
      .quality_mode = std::string(atx::vol::to_string(bundle.risk_health.quality_mode)),
      .risk_model = atx::vol::to_string(fitted->session().inputs().curve.kind),
      .mark_model = bundle.market_mark != nullptr
                        ? atx::vol::to_string(bundle.market_mark->session().inputs().curve.kind)
                        : "unavailable",
      .candidate_generation = bundle.candidate_generation,
      .served_generation = bundle.risk_health.served_generation,
      .using_fallback = bundle.risk_health.using_fallback(),
      .carry_confident = diag.carry_confident,
      .inversion_certified = diag.inversion_certified,
      .butterfly_violations = bundle.risk_health.validation.n_butterfly_violations,
      .inversion_fallbacks = diag.n_iv_fallback,
      .carry_dispersion = diag.max_carry_dispersion,
      .carry_leave_one_out = diag.max_carry_leave_one_out,
      .validation_milliseconds = bundle.timings.risk_validation_ms,
  };

  const atx::vol::Underlying &underlying = impl_->chain->underlying();
  const atx::vol::VolaSession &session = fitted->session();
  impl_->expiries.reserve(underlying.chains.size());
  for (const atx::vol::Chain &chain : underlying.chains) {
    const double forward = session.forward_at(chain.T);
    impl_->expiries.push_back(ExpiryInfo{
        .iso_date = atx::vol::ns_to_iso_date(chain.expiry_ns),
        .years = chain.T,
        .forward = forward,
        .atm_vol = session.iv(forward, chain.T),
        .carry = session.q_eff_at(chain.T),
        .total_variance = session.iv(forward, chain.T) * session.iv(forward, chain.T) * chain.T,
        .strike_count = chain.n_strikes(),
    });
  }
  for (std::size_t i = 0; i < impl_->expiries.size(); ++i) {
    ExpiryInfo &expiry = impl_->expiries[i];
    if (i == 0) {
      expiry.forward_variance = expiry.total_variance / expiry.years;
    } else {
      const ExpiryInfo &previous = impl_->expiries[i - 1];
      expiry.forward_variance =
          (expiry.total_variance - previous.total_variance) / (expiry.years - previous.years);
    }
  }
  if (impl_->expiries.empty()) {
    impl_->error = "the fitted " + config.symbol + " board contains no expiries";
    return false;
  }

  const std::optional<std::size_t> initial =
      choose_initial_expiry(impl_->expiries, config.initial_expiry);
  if (!initial.has_value()) {
    impl_->error = "the fitted board has no selectable expiry";
    return false;
  }
  return select_expiry(*initial);
}

bool OpraVolSurface::select_expiry(std::size_t index) {
  if (impl_->chain == nullptr || impl_->fitter == nullptr || impl_->fitter->surface() == nullptr ||
      index >= impl_->expiries.size()) {
    impl_->error = "expiry selection is unavailable before a successful fit";
    return false;
  }

  const atx::vol::Underlying &underlying = impl_->chain->underlying();
  const atx::vol::Chain &chain = underlying.chains[index];
  const atx::vol::FittedSurface *risk_surface = impl_->fitter->risk_surface();
  const atx::vol::FittedSurface *mark_surface = impl_->fitter->market_mark_surface();
  if (risk_surface == nullptr) {
    impl_->error = "selected expiry has no admitted risk surface";
    return false;
  }
  const atx::vol::VolaSession &session = risk_surface->session();
  const double spot = impl_->chain->spot();
  const double rate = impl_->chain->rate();
  const double years = chain.T;
  const double forward = session.forward_at(years);
  const double carry = session.q_eff_at(years);
  const double discount = std::exp(-rate * years);
  if (!(forward > 0.0) || !(years > 0.0)) {
    impl_->error = "selected expiry has a degenerate forward or tenor";
    return false;
  }

  const atx::vol::CalibOpts calib_options{};
  auto market_observations =
      atx::vol::build_observations(chain, forward, years, discount, calib_options);
  if (!market_observations.has_value() || market_observations->obs.size() < 5) {
    impl_->error =
        market_observations.has_value()
            ? "selected expiry has too few market observations"
            : "market observation build failed: " + market_observations.error().message();
    return false;
  }
  const double atm_vol = session.iv(forward, years);
  const double sigma_hat = atm_vol * std::sqrt(years);
  if (!(sigma_hat > 0.0) || !std::isfinite(sigma_hat)) {
    impl_->error = "selected expiry has a degenerate ATM normalization";
    return false;
  }

  VolCurveSlice next;
  next.symbol = impl_->config.symbol;
  next.snapshot_iso = impl_->config.snapshot_iso;
  next.expiry_iso = impl_->expiries[index].iso_date;
  next.model_name = atx::vol::to_string(session.inputs().curve.kind);
  next.spot = spot;
  next.forward = forward;
  next.years = years;
  next.rate = rate;
  next.carry = carry;
  next.atm_vol = atm_vol;
  next.observations = market_observations->obs.size();
  next.curve.reserve(121);
  for (int i = 0; i <= 120; ++i) {
    const double z = -2.4 + 4.8 * static_cast<double>(i) / 120.0;
    const double k_log = z * sigma_hat;
    const double strike = forward * std::exp(k_log);
    const double model_iv = session.iv(strike, years);
    if (std::isfinite(model_iv)) {
      next.curve.push_back(VolCurvePoint{z, strike, model_iv});
    }
  }
  if (mark_surface != nullptr) {
    const atx::vol::VolaSession &mark_session = mark_surface->session();
    next.market_mark_curve.reserve(121);
    for (int i = 0; i <= 120; ++i) {
      const double z = -2.4 + 4.8 * static_cast<double>(i) / 120.0;
      const double k_log = z * sigma_hat;
      const double strike = forward * std::exp(k_log);
      const double mark_iv = mark_session.iv(strike, years);
      if (std::isfinite(mark_iv)) {
        next.market_mark_curve.push_back(VolCurvePoint{z, strike, mark_iv});
      }
    }
  }

  next.quotes.reserve(market_observations->obs.size());
  const atx::vol::AmericanCorrectionCaches caches = session.correction_caches();
  for (const atx::vol::FitObs &observation : market_observations->obs) {
    const double z = observation.k / sigma_hat;
    if (std::fabs(z) > 2.4) {
      continue;
    }
    const double half_spread = 0.5 * observation.spread;
    const double bid = observation.mid - half_spread;
    const double ask = observation.mid + half_spread;
    if (!(bid > 0.0) || !(ask > bid)) {
      continue;
    }
    const atx::vol::CorrectionCache *correction = caches.for_side(observation.side);
    const auto invert = [&](double price) {
      const auto result = atx::vol::american_implied_vol(
          price, spot, observation.K, years, rate, carry, observation.side,
          atx::vol::AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt, correction);
      return result.has_value() ? *result : std::numeric_limits<double>::quiet_NaN();
    };
    const double bid_iv = invert(bid);
    const double ask_iv = invert(ask);
    const double mid_iv = invert(observation.mid);
    const double model_iv = session.iv(observation.K, years);
    const atx::vol::Side side = observation.side;
    const auto greeks = session.greeks(observation.K, years, side);
    if (!std::isfinite(bid_iv) || !std::isfinite(ask_iv) || !std::isfinite(mid_iv) ||
        !std::isfinite(model_iv) || !greeks.has_value()) {
      continue;
    }
    next.quotes.push_back(VolQuotePoint{
        .z = z,
        .strike = observation.K,
        .side = observation.side == atx::vol::Side::Call ? 'C' : 'P',
        .bid_price = bid,
        .mid_price = observation.mid,
        .ask_price = ask,
        .theoretical_price = greeks->price,
        .bid_iv = bid_iv,
        .ask_iv = ask_iv,
        .mid_iv = mid_iv,
        .model_iv = model_iv,
        .delta = greeks->delta,
        .gamma = greeks->gamma,
        .theta = greeks->theta,
        .vega = greeks->vega,
    });
  }
  std::sort(
      next.quotes.begin(), next.quotes.end(),
      [](const VolQuotePoint &lhs, const VolQuotePoint &rhs) { return lhs.strike < rhs.strike; });

  if (!next.quotes.empty()) {
    std::size_t in_band = 0;
    double squared_error = 0.0;
    for (const VolQuotePoint &quote : next.quotes) {
      const double error = quote.model_iv - quote.mid_iv;
      squared_error += error * error;
      next.max_abs_error = std::max(next.max_abs_error, std::fabs(error));
      if (quote.model_iv >= quote.bid_iv && quote.model_iv <= quote.ask_iv) {
        ++in_band;
      }
    }
    next.fraction_in_band = static_cast<double>(in_band) / static_cast<double>(next.quotes.size());
    next.rmse_iv = std::sqrt(squared_error / static_cast<double>(next.quotes.size()));
  }

  impl_->selected = index;
  impl_->slice = std::move(next);
  impl_->error.clear();
  return true;
}

bool OpraVolSurface::ready() const noexcept {
  return impl_->fitter != nullptr && impl_->fitter->fitted() && !impl_->slice.curve.empty();
}

const std::string &OpraVolSurface::error() const noexcept { return impl_->error; }
const OpraSourceConfig &OpraVolSurface::config() const noexcept { return impl_->config; }
const SurfaceSourceInfo &OpraVolSurface::source_info() const noexcept { return impl_->source_info; }
const std::vector<ExpiryInfo> &OpraVolSurface::expiries() const noexcept { return impl_->expiries; }
std::size_t OpraVolSurface::selected_expiry() const noexcept { return impl_->selected; }
const VolCurveSlice &OpraVolSurface::slice() const noexcept { return impl_->slice; }
const SurfaceDiagnostics &OpraVolSurface::diagnostics() const noexcept {
  return impl_->diagnostics;
}
std::size_t OpraVolSurface::contract_count() const noexcept {
  return impl_->panel.has_value() ? impl_->panel->n_contracts : 0;
}
std::size_t OpraVolSurface::dropped_count() const noexcept {
  return impl_->panel.has_value() ? impl_->panel->n_dropped : 0;
}
double OpraVolSurface::fit_milliseconds() const noexcept { return impl_->fit_ms; }
UiFitQualityMode OpraVolSurface::quality_mode() const noexcept {
  return impl_->config.quality_mode;
}
bool OpraVolSurface::set_quality_mode(UiFitQualityMode mode) {
  if (mode == impl_->config.quality_mode) {
    return ready();
  }
  OpraSourceConfig next = impl_->config;
  next.quality_mode = mode;
  if (!impl_->expiries.empty() && impl_->selected < impl_->expiries.size()) {
    next.initial_expiry = impl_->expiries[impl_->selected].iso_date;
  }
  return load(next);
}

} // namespace atx::ui
