// European-only outputs on a declared common-model surface (lane A1, r6 2026-10-23):
// api/analytics/common_model_analytics.hpp is the contract.
#include "atx/vol/api/analytics/common_model_analytics.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/american_markov.hpp"

namespace atx::vol {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void declare_record(const SliceServeRecord &record, AnalyticsDeclaration &d) noexcept {
  const ServeRecordVersion &v = record.version();
  d.record_fit_sha256 = v.fit_sha256;
  d.record_policy_sha256 = v.policy_sha256;
  d.record_input_sha256 = v.input_sha256;
  d.clock.as_of_ns = v.as_of_ns;
  d.clock.tau = record.tau();
}

[[nodiscard]] EuropeanOutputs refuse(EuropeanOutputRefusal why) {
  EuropeanOutputs out;
  out.status = AnalyticsStatus::Refused;
  out.refusal = why;
  return out;
}

// F13: the American quoting surface is not a European input.
[[nodiscard]] EuropeanOutputs refuse_quoting(const EuropeanSurfaceRef &surface) {
  EuropeanOutputs out = refuse(EuropeanOutputRefusal::AmericanQuotingSurface);
  out.declaration.model = AnalyticsModel::QuotingRecord;
  if (surface.kind == EuropeanSurfaceKind::QuotingRecord && surface.record != nullptr) {
    declare_record(*surface.record, out.declaration);
  }
  if (surface.kind == EuropeanSurfaceKind::QuotingBookExpiry && surface.book != nullptr) {
    const std::shared_ptr<const ServeBookVersion> v = surface.book->current();
    out.declaration.book_version = v->number;
    out.declaration.expiry_id = surface.expiry_id;
    if (const ServeBookEntry *e = v->find(surface.expiry_id); e != nullptr) {
      out.declaration.entry_tag = e->tag;
      out.declaration.clock.expiry_ns = e->expiry_ns;
      out.declaration.clock.next_event_ns = e->next_event_ns;
      if (e->record) declare_record(*e->record, out.declaration);
    }
  }
  return out;
}

// The unit mass at the initial state pushed through transitions [0, layer) (the transpose
// action). False when an action refuses.
[[nodiscard]] bool push_forward(const AmericanMarkovModel &model, std::size_t layer,
                                std::vector<double> &mass, double &discount, bool &cash,
                                std::size_t &applied) {
  mass.assign(model.layers().front().states().size(), 0.0);
  mass[model.initial_state_index()] = 1.0;
  std::vector<double> next;
  discount = 1.0;
  cash = false;
  applied = 0;
  for (std::size_t n = 0; n < layer; ++n) {
    const AmericanMarkovTransition &t = model.transitions()[n];
    next.assign(t.target_state_count(), 0.0);
    const Status ok = apply_american_markov_transition(t, mass, next,
                                                       AmericanMarkovTransitionAction::Forward);
    if (!ok) return false;
    mass.swap(next);
    discount *= t.discount;
    if (t.kind == AmericanMarkovStepKind::CashDividend && t.cash_dividend > 0.0) cash = true;
    ++applied;
  }
  return true;
}

void fill_density(const AmericanMarkovModel &model, const AmericanMarkovLayer &layer,
                  std::span<const double> mass, bool keep, bool cash, CommonModelDensity &d) {
  const std::span<const double> x = layer.states();
  NeumaierSum total;
  NeumaierSum mean;
  double zero = 0.0;
  if (keep) {
    d.spot.resize(x.size());
    d.mass.assign(mass.begin(), mass.end());
    d.cdf.resize(x.size());
  }
  for (std::size_t i = 0; i < x.size(); ++i) {
    total.add(mass[i]);
    mean.add(mass[i] * layer.growth * x[i]);
    if (x[i] == 0.0) zero += mass[i];
    if (keep) {
      d.spot[i] = layer.growth * x[i];
      d.cdf[i] = total.value();
    }
  }
  d.total_mass = total.value();
  d.mean = mean.value();
  d.zero_mass = zero;
  // Without cash, X = S / G is the chain's martingale: the forward is G x X_0. With cash
  // the stock pays out of the grid, and the model's forward is its own mean.
  d.forward = cash ? d.mean : layer.growth * model.spot();
}

void fill_strip(const AmericanMarkovModel &model, const AmericanMarkovLayer &layer,
                std::span<const double> mass, double zero_mass, EuropeanOutputs &out) {
  if (zero_mass > kCommonModelMaxZeroMass) {
    out.strip_refusal = EuropeanOutputRefusal::AtomAtZero;
    return;
  }
  const std::span<const double> x = layer.states();
  const double x0 = model.spot();
  NeumaierSum log_moment;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (x[i] > 0.0) log_moment.add(mass[i] * std::log(x[i] / x0));
  }
  CommonModelVarianceStrip &s = out.strip;
  s.total_variance = -2.0 * log_moment.value();
  s.variance_years = layer.variance_time;
  s.calendar_years = layer.calendar_time;
  s.vol_variance_clock =
      layer.variance_time > 0.0 ? std::sqrt(s.total_variance / layer.variance_time) : kNaN;
  s.vol_calendar_clock =
      layer.calendar_time > 0.0 ? std::sqrt(s.total_variance / layer.calendar_time) : kNaN;
  s.excluded_zero_mass = zero_mass;
}

void fill_local_vol(const AmericanMarkovModel &model, std::size_t layer, CommonModelLocalVol &lv) {
  for (const AmericanMarkovIntervalData &data : model.interval_data()) {
    if (data.first_layer >= layer) continue;
    const AmericanMarkovLayer &first = model.layers()[data.first_layer];
    const std::span<const double> x = first.states();
    for (std::size_t i = 0; i < data.state_volatilities.size() && i < x.size(); ++i) {
      lv.calendar_time.push_back(first.calendar_time);
      lv.spot.push_back(first.growth * x[i]);
      lv.volatility.push_back(data.state_volatilities[i]);
    }
  }
}

[[nodiscard]] EuropeanOutputs common_model_outputs(const EuropeanSurfaceRef &surface,
                                                   const EuropeanOutputsRequest &request) {
  const AmericanMarkovModel &model = *surface.model;
  if (surface.expiry_index >= model.expiry_layers().size()) {
    return refuse(EuropeanOutputRefusal::NoExpiryLayer);
  }
  const std::size_t L = model.expiry_layers()[surface.expiry_index];
  const AmericanMarkovLayer &layer = model.layers()[L];
  EuropeanOutputs out;
  out.declaration.model = AnalyticsModel::CommonModelEuropean;
  out.declaration.entry_tag = surface.surface_version;
  out.declaration.clock.tau = layer.variance_time;
  std::vector<double> mass;
  bool cash = false;
  double discount = 1.0;
  std::size_t applied = 0;
  if (!push_forward(model, L, mass, discount, cash, applied)) {
    EuropeanOutputs bad = refuse(EuropeanOutputRefusal::NumericalFailure);
    bad.declaration = out.declaration;
    return bad;
  }
  fill_density(model, layer, mass, request.density, cash, out.density);
  out.density.discount = discount;
  if (request.variance_strip) {
    if (cash) {
      out.strip_refusal = EuropeanOutputRefusal::CashDividendJump;
    } else {
      fill_strip(model, layer, mass, out.density.zero_mass, out);
    }
  }
  if (request.local_vol) fill_local_vol(model, L, out.local_vol);
  CommonModelBudgets &b = out.budgets;
  b.mass_error = std::abs(out.density.total_mass - 1.0);
  b.mean_error = cash ? kNaN : std::abs(out.density.mean / out.density.forward - 1.0);
  b.model_max_forward_mass_error = model.diagnostics().max_forward_mass_error;
  b.model_max_relative_mean_error = model.diagnostics().max_relative_mean_error;
  b.transitions_applied = applied;
  b.stored_rows = model.diagnostics().stored_rows;
  b.max_states = model.diagnostics().max_states;
  b.stored_volatility_values = model.diagnostics().stored_volatility_values;
  b.output_bytes = sizeof(double) * (out.density.spot.size() + out.density.mass.size() +
                                     out.density.cdf.size() + out.local_vol.spot.size() +
                                     out.local_vol.volatility.size() +
                                     out.local_vol.calendar_time.size());
  out.status = AnalyticsStatus::Ok;
  out.refusal = EuropeanOutputRefusal::None;
  return out;
}

} // namespace

EuropeanSurfaceRef EuropeanSurfaceRef::common_model(const AmericanMarkovModel &model,
                                                    std::size_t expiry_index,
                                                    std::uint64_t surface_version) {
  EuropeanSurfaceRef r;
  r.kind = EuropeanSurfaceKind::CommonModel;
  r.model = &model;
  r.expiry_index = expiry_index;
  r.surface_version = surface_version;
  return r;
}

EuropeanSurfaceRef EuropeanSurfaceRef::quoting_record(const SliceServeRecord &record) {
  EuropeanSurfaceRef r;
  r.kind = EuropeanSurfaceKind::QuotingRecord;
  r.record = &record;
  return r;
}

EuropeanSurfaceRef EuropeanSurfaceRef::quoting_book_expiry(const ServeBook &book,
                                                           std::uint64_t expiry_id) {
  EuropeanSurfaceRef r;
  r.kind = EuropeanSurfaceKind::QuotingBookExpiry;
  r.book = &book;
  r.expiry_id = expiry_id;
  return r;
}

EuropeanOutputs european_outputs(const EuropeanSurfaceRef &surface,
                                 const EuropeanOutputsRequest &request) {
  switch (surface.kind) {
  case EuropeanSurfaceKind::None: return refuse(EuropeanOutputRefusal::NoSurface);
  case EuropeanSurfaceKind::QuotingRecord:
  case EuropeanSurfaceKind::QuotingBookExpiry: return refuse_quoting(surface);
  case EuropeanSurfaceKind::CommonModel:
    if (surface.model == nullptr) return refuse(EuropeanOutputRefusal::NoSurface);
    return common_model_outputs(surface, request);
  }
  return refuse(EuropeanOutputRefusal::NoSurface);
}

const char *to_string(EuropeanOutputRefusal value) noexcept {
  switch (value) {
  case EuropeanOutputRefusal::None: return "none";
  case EuropeanOutputRefusal::AmericanQuotingSurface: return "american_quoting_surface";
  case EuropeanOutputRefusal::NoSurface: return "no_surface";
  case EuropeanOutputRefusal::NoExpiryLayer: return "no_expiry_layer";
  case EuropeanOutputRefusal::CashDividendJump: return "cash_dividend_jump";
  case EuropeanOutputRefusal::AtomAtZero: return "atom_at_zero";
  case EuropeanOutputRefusal::NumericalFailure: return "numerical_failure";
  }
  return "unknown";
}

} // namespace atx::vol
