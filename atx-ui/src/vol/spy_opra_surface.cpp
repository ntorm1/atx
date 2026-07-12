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
  fit_config.preset = atx::vol::FitPreset::Hft;
  fit_config.curve = atx::vol::CurveConfig{.kind = atx::vol::VolCurveKind::LinearVariance};
  impl_->fitter = std::make_unique<atx::vol::PricerFitter>(std::move(fit_config));
  const Clock::time_point fit_start = Clock::now();
  const atx::vol::Status fit_status = impl_->fitter->fit(*impl_->chain);
  impl_->fit_ms = milliseconds_since(fit_start);
  if (!fit_status.has_value()) {
    impl_->error = "surface fit failed: " + fit_status.error().message();
    return false;
  }

  const atx::vol::FittedSurface *fitted = impl_->fitter->surface();
  if (fitted == nullptr) {
    impl_->error = "surface fit completed without a fitted surface";
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
        .strike_count = chain.n_strikes(),
    });
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
  const atx::vol::VolaSession &session = impl_->fitter->surface()->session();
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
  next.model_name = "HFT LINEAR VARIANCE";
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

} // namespace atx::ui
