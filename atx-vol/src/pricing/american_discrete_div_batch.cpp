#include "atx/vol/api/pricing/american_discrete_div_batch.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "pricing/american_discrete_div_detail.hpp"

// The schedule-grouped batch (header: why, contract). Every number here is the
// kernel TU's: a route, its prebuilt step maps, and the section 4 rollbacks /
// inversion / bundle on them. This file owns the per-group state and the
// outcome codes; it adds no lattice arithmetic.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct GroupState {
  DiscreteDivBatchGroup inputs{};
  bool ok = false;
  DiscreteDivLatticeRoute route{};
  DiscreteDivLatticeRoute screen{};
  bool has_screen = false;
  ddiv_detail::RouteMaps maps{};
  ddiv_detail::RouteMaps screen_maps{};
  std::size_t map_offset = 0; // into the batch's step-map arena
};

[[nodiscard]] DiscreteDivBatchOutcome outcome_of(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::InvalidArgument:
    return DiscreteDivBatchOutcome::RefusedInput;
  case ErrorCode::Unavailable:
    return DiscreteDivBatchOutcome::RefusedBudget;
  default:
    return DiscreteDivBatchOutcome::RefusedRange;
  }
}

[[nodiscard]] std::uint16_t all_greek_bits() noexcept {
  std::uint16_t bits = 0;
  for (unsigned g = 0; g <= static_cast<unsigned>(DiscreteDivGreek::Roll); ++g) {
    bits = static_cast<std::uint16_t>(bits | (1U << g));
  }
  return bits;
}

[[nodiscard]] DiscreteDivProductionGreeks refused_bundle() noexcept {
  DiscreteDivProductionGreeks g;
  g.price = kNaN;
  g.delta = kNaN;
  g.gamma = kNaN;
  g.vega = kNaN;
  g.rho = kNaN;
  g.phi = kNaN;
  g.theta = kNaN;
  g.charm = kNaN;
  g.roll.rolled_price = kNaN;
  g.roll.roll = kNaN;
  g.unsupported = all_greek_bits();
  return g;
}

} // namespace

const char *to_string(DiscreteDivBatchOutcome value) noexcept {
  switch (value) {
  case DiscreteDivBatchOutcome::Priced:
    return "priced";
  case DiscreteDivBatchOutcome::PriceOnly:
    return "price_only";
  case DiscreteDivBatchOutcome::RefusedInput:
    return "refused_input";
  case DiscreteDivBatchOutcome::RefusedRange:
    return "refused_range";
  case DiscreteDivBatchOutcome::RefusedBudget:
    return "refused_budget";
  case DiscreteDivBatchOutcome::RefusedGroup:
    return "refused_group";
  }
  return "unknown";
}

struct DiscreteDivBatch::Impl {
  DiscreteDivProductionOpts opts{};
  int screen_steps = 0;
  std::vector<GroupState> groups;
  std::vector<double> arena; // every group's step maps, one allocation
  DiscreteDivWorkspace workspace{4, 0};
  DiscreteDivLatticeRoute empty_route{};

  [[nodiscard]] const GroupState *group(std::uint32_t index) const noexcept {
    if (index >= groups.size() || !groups[index].ok) {
      return nullptr;
    }
    return &groups[index];
  }
};

DiscreteDivBatch::DiscreteDivBatch(const DiscreteDivProductionOpts &opts, int screen_steps)
    : impl_(std::make_unique<Impl>()) {
  impl_->opts = opts;
  impl_->screen_steps = screen_steps;
}

DiscreteDivBatch::~DiscreteDivBatch() = default;
DiscreteDivBatch::DiscreteDivBatch(DiscreteDivBatch &&) noexcept = default;
DiscreteDivBatch &DiscreteDivBatch::operator=(DiscreteDivBatch &&) noexcept = default;

std::size_t DiscreteDivBatch::prepare(std::span<const DiscreteDivBatchGroup> groups) {
  Impl &im = *impl_;
  im.groups.assign(groups.size(), GroupState{});
  std::size_t refused = 0;
  std::size_t doubles = 0;
  int max_steps = 4;
  // Pass 1: routes (the rule is a pure function of (S, T, schedule, opts)) and the
  // arena each group's maps need.
  for (std::size_t i = 0; i < groups.size(); ++i) {
    GroupState &g = im.groups[i];
    g.inputs = groups[i];
    const DiscreteDivBatchGroup &in = groups[i];
    // The price entry's validation of everything but the strike and sigma (a
    // placeholder K = S, sigma = 1 inside the admissible range).
    const Result<DiscreteDivLatticeRoute> route =
        discrete_div_lattice_route(in.S, in.T, in.dividends, im.opts);
    const Status valid = route ? ddiv_detail::validate(in.S, in.S, in.T, 1.0, in.r, in.q,
                                                       in.dividends, route->steps)
                               : Status{};
    if (!route || !valid) {
      ++refused;
      continue;
    }
    g.route = *route;
    g.ok = true;
    if (im.screen_steps >= 4 && g.route.method != DiscreteDivMethod::HistoricalSpliced) {
      const Result<DiscreteDivLatticeRoute> screen =
          ddiv_detail::screen_route(in.S, in.T, in.dividends, im.screen_steps);
      if (screen && screen->steps < g.route.steps) {
        g.screen = *screen;
        g.has_screen = true;
      }
    }
    g.map_offset = doubles;
    doubles += ddiv_detail::route_map_doubles(g.route);
    if (g.has_screen) {
      doubles += ddiv_detail::route_map_doubles(g.screen);
    }
    max_steps = std::max({max_steps, g.route.steps, g.has_screen ? g.screen.steps : 0,
                          im.screen_steps});
  }
  // Pass 2: the maps, into one arena sized once (no reallocation under the views).
  im.arena.assign(doubles, 0.0);
  const std::span<double> arena(im.arena);
  for (GroupState &g : im.groups) {
    if (!g.ok) {
      continue;
    }
    const DiscreteDivBatchGroup &in = g.inputs;
    const std::size_t fine = ddiv_detail::route_map_doubles(g.route);
    g.maps = ddiv_detail::build_route_maps(g.route, in.T, in.r, in.q, in.dividends,
                                           arena.subspan(g.map_offset, fine));
    if (g.has_screen) {
      const std::size_t scr = ddiv_detail::route_map_doubles(g.screen);
      g.screen_maps = ddiv_detail::build_route_maps(g.screen, in.T, in.r, in.q, in.dividends,
                                                    arena.subspan(g.map_offset + fine, scr));
    }
  }
  // One workspace for every route of the batch: the largest step count, and the
  // pre-step ceiling (the Greek bundle's time roll re-routes the shortened life).
  im.workspace = DiscreteDivWorkspace(max_steps, kDiscreteDivMaxPreSteps);
  return refused;
}

std::size_t DiscreteDivBatch::groups() const noexcept { return impl_->groups.size(); }

bool DiscreteDivBatch::group_ok(std::size_t group) const noexcept {
  return group < impl_->groups.size() && impl_->groups[group].ok;
}

const DiscreteDivLatticeRoute &DiscreteDivBatch::route(std::size_t group) const noexcept {
  return group_ok(group) ? impl_->groups[group].route : impl_->empty_route;
}

Status DiscreteDivBatch::price(std::span<const DiscreteDivBatchContract> contracts,
                               ExerciseStyle exercise, std::span<DiscreteDivBatchResult> out) {
  if (out.size() != contracts.size()) {
    return Err(ErrorCode::InvalidArgument, "discrete-dividend batch: out.size() != contracts");
  }
  Impl &im = *impl_;
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    const DiscreteDivBatchContract &c = contracts[i];
    DiscreteDivBatchResult &res = out[i];
    res = DiscreteDivBatchResult{};
    const GroupState *g = im.group(c.group);
    if (g == nullptr) {
      res.value = kNaN;
      res.outcome = DiscreteDivBatchOutcome::RefusedGroup;
      continue;
    }
    const DiscreteDivBatchGroup &in = g->inputs;
    const Result<double> p =
        ddiv_detail::route_price_prebuilt(g->route, &g->maps, in.S, c.K, in.T, c.sigma, in.r,
                                          in.q, c.side, in.dividends, exercise, im.workspace);
    if (p) {
      res.value = *p;
    } else {
      res.value = kNaN;
      res.outcome = outcome_of(p.error().code());
    }
  }
  return Ok();
}

Status DiscreteDivBatch::implied_vol(std::span<const DiscreteDivBatchContract> contracts,
                                     const DiscreteDivIvOpts &iv_opts,
                                     std::span<DiscreteDivBatchResult> out) {
  if (out.size() != contracts.size()) {
    return Err(ErrorCode::InvalidArgument, "discrete-dividend batch: out.size() != contracts");
  }
  Impl &im = *impl_;
  // The kill switch's historical inversion takes its Screen count from the batch.
  DiscreteDivIvOpts historical_opts = iv_opts;
  historical_opts.screen_steps = std::max(im.screen_steps, 2);
  if (im.screen_steps < 2) {
    historical_opts.tier = DiscreteDivTier::Accurate;
  }
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    const DiscreteDivBatchContract &c = contracts[i];
    DiscreteDivBatchResult &res = out[i];
    res = DiscreteDivBatchResult{};
    const GroupState *g = im.group(c.group);
    if (g == nullptr) {
      res.value = kNaN;
      res.outcome = DiscreteDivBatchOutcome::RefusedGroup;
      continue;
    }
    const DiscreteDivBatchGroup &in = g->inputs;
    const bool historical = (g->route.method == DiscreteDivMethod::HistoricalSpliced);
    const Result<DiscreteDivIvResult> iv =
        historical ? american_discrete_div_implied_vol(c.quote, in.S, c.K, in.T, in.r, in.q,
                                                       c.side, in.dividends, g->route.steps,
                                                       DiscreteDivScheme::SplicedCrr,
                                                       historical_opts, im.workspace)
                   : ddiv_detail::route_iv_prebuilt(
                         c.quote, g->route, &g->maps, g->has_screen ? &g->screen : nullptr,
                         g->has_screen ? &g->screen_maps : nullptr, in.S, c.K, in.T, in.r, in.q,
                         c.side, in.dividends, iv_opts, im.workspace);
    if (iv) {
      res.value = iv->sigma;
      res.fallback = iv->fallback;
      res.screen_lattices = iv->screen_lattices;
      res.accurate_lattices = iv->accurate_lattices;
      continue;
    }
    res.value = kNaN;
    const ErrorCode code = iv.error().code();
    if (!(c.quote > 0.0) || !std::isfinite(c.quote)) {
      res.outcome = DiscreteDivBatchOutcome::RefusedInput;
      continue;
    }
    res.outcome = outcome_of(code);
    if (code != ErrorCode::OutOfRange) {
      continue;
    }
    // Price-only or a range refusal: the quote against the lattice's price at the
    // lowest volatility the inversion can reach (one rollback, refusals only).
    const double floor_sigma = ddiv_detail::route_sigma_floor(g->route, in.T, in.r, in.q);
    const Result<double> lowest = ddiv_detail::route_price_prebuilt(
        g->route, &g->maps, in.S, c.K, in.T, floor_sigma, in.r, in.q, c.side, in.dividends,
        ExerciseStyle::American, im.workspace);
    if (lowest && std::isfinite(*lowest) && c.quote <= *lowest) {
      res.outcome = DiscreteDivBatchOutcome::PriceOnly;
    }
  }
  return Ok();
}

Status DiscreteDivBatch::greeks(std::span<const DiscreteDivBatchContract> contracts,
                                ExerciseStyle exercise, double roll_horizon,
                                std::span<DiscreteDivProductionGreeks> out) {
  if (out.size() != contracts.size()) {
    return Err(ErrorCode::InvalidArgument, "discrete-dividend batch: out.size() != contracts");
  }
  Impl &im = *impl_;
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    const DiscreteDivBatchContract &c = contracts[i];
    const GroupState *g = im.group(c.group);
    if (g == nullptr) {
      out[i] = refused_bundle();
      continue;
    }
    const DiscreteDivBatchGroup &in = g->inputs;
    const Result<DiscreteDivProductionGreeks> bundle = ddiv_detail::route_greeks_prebuilt(
        g->route, &g->maps, in.S, c.K, in.T, c.sigma, in.r, in.q, c.side, in.dividends,
        exercise, std::span<double>{}, roll_horizon, im.opts, im.workspace);
    if (bundle) {
      out[i] = *bundle;
    } else {
      out[i] = refused_bundle();
      out[i].route = g->route;
    }
  }
  return Ok();
}

} // namespace atx::vol
