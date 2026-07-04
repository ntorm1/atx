#include "atx/engine/eval/robustness_battery.hpp"

#include <algorithm> // std::sort, std::max
#include <cmath>     // std::sqrt, std::abs

#include "atx/core/random.hpp" // atx::core::Xoshiro256pp

namespace atx::engine::eval {

namespace {

// Per-check PRNG salts (arbitrary, fixed, odd-looking-but-meaningless constants —
// distinct so each check's stream never collides with another's, even at the
// same cfg.seed). NOT secrets; just stream separators.
constexpr atx::u64 kNoiseControlSalt = 0x5EED0000A17FAC70ULL;
constexpr atx::u64 kAltNeutralizationSalt = 0x5EED00007A17AB01ULL;
constexpr atx::u64 kParamPerturbationSalt = 0x5EED0000BADDECAFULL;

// Fisher-Yates, seeded, in-place shuffle. A permutation trivially preserves the
// exact empirical marginal distribution of `v` (same multiset of values, new
// order) — the "matched marginal" the noise-control / alt-neutralization
// scenarios need, with zero distribution-fitting machinery.
template <class T> void seeded_shuffle(std::vector<T> &v, atx::u64 seed) {
  if (v.size() < 2U) {
    return;
  }
  atx::core::Xoshiro256pp rng{seed};
  for (atx::usize i = v.size() - 1U; i > 0U; --i) {
    const atx::u64 j = rng.next_u64() % (static_cast<atx::u64>(i) + 1ULL);
    std::swap(v[i], v[static_cast<atx::usize>(j)]);
  }
}

// TOP-N-by-value instrument ranking: descending by `adv`, ascending index
// tie-break (a deterministic total order — std::sort is not stable by default).
[[nodiscard]] std::vector<atx::usize> top_n_indices(std::span<const atx::f64> adv, atx::usize n) {
  std::vector<atx::usize> idx(adv.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&adv](atx::usize a, atx::usize b) {
    if (adv[a] != adv[b]) {
      return adv[a] > adv[b];
    }
    return a < b;
  });
  n = std::min(n, idx.size());
  idx.resize(n);
  std::sort(idx.begin(), idx.end()); // ascending index order for the caller's convenience
  return idx;
}

// Population mean/stddev (N divisor, matching the eval spine's population-moment
// convention elsewhere in this codebase — e.g. combine::detail::pnl_moments).
struct MeanStd {
  atx::f64 mean = 0.0;
  atx::f64 std = 0.0;
};

[[nodiscard]] MeanStd mean_std(std::span<const atx::f64> v) {
  if (v.empty()) {
    return {};
  }
  atx::f64 sum = 0.0;
  for (const atx::f64 x : v) {
    sum += x;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(v.size());
  atx::f64 sq = 0.0;
  for (const atx::f64 x : v) {
    const atx::f64 d = x - mean;
    sq += d * d;
  }
  const atx::f64 var = sq / static_cast<atx::f64>(v.size());
  return {mean, std::sqrt(var)};
}

// A SURVIVAL check (sub_universe / alt_neutralization): PASS iff the perturbed
// edge retains at least `min_survival_ratio` of the base edge. A non-positive
// base_edge has nothing to preserve — trivially PASS (there is no edge to lose).
[[nodiscard]] CheckOutcome survival_outcome(atx::f64 base_edge, atx::f64 scenario_edge,
                                            atx::f64 min_survival_ratio) {
  CheckOutcome out;
  out.applicable = true;
  out.base_edge = base_edge;
  out.scenario_edge = scenario_edge;
  out.survival_ratio = (base_edge > 0.0) ? (scenario_edge / base_edge) : 0.0;
  out.passed = (base_edge <= 0.0) || (scenario_edge >= min_survival_ratio * base_edge);
  return out;
}

} // namespace

BatteryResult RobustnessBattery::run(const BatteryConfig &cfg, const CandidateInputs &inputs,
                                     const Reevaluator &reeval) {
  BatteryResult res;
  if (!cfg.any_enabled()) {
    return res; // ran=false; reeval is NEVER called (off-path byte-identity)
  }
  res.ran = true;
  res.overall_pass = true;

  // ---- sub_universe --------------------------------------------------------
  if (cfg.sub_universe) {
    if (inputs.adv.empty()) {
      res.sub_universe = CheckOutcome{}; // inapplicable: no liquidity proxy supplied
    } else {
      const atx::usize n =
          (cfg.sub_universe_top_n > 0U) ? cfg.sub_universe_top_n
                                       : std::max<atx::usize>(1U, inputs.adv.size() / 2U);
      RobustnessScenario sc;
      sc.kind = ScenarioKind::SubUniverse;
      sc.keep_instruments = top_n_indices(inputs.adv, n);
      auto edge_r = reeval(sc);
      if (edge_r.has_value()) {
        res.sub_universe = survival_outcome(inputs.base_edge, *edge_r, cfg.min_survival_ratio);
      } // else: Err -> stays default (inapplicable) — a genuine eval failure, not a screening verdict
    }
    if (res.sub_universe.applicable && !res.sub_universe.passed) {
      res.overall_pass = false;
    }
  }

  // ---- alt_neutralization ---------------------------------------------------
  if (cfg.alt_neutralization) {
    if (inputs.group_id.empty()) {
      res.alt_neutralization = CheckOutcome{}; // inapplicable: no group_map supplied (S1 absent)
    } else {
      std::vector<atx::u32> alt(inputs.group_id.begin(), inputs.group_id.end());
      seeded_shuffle(alt, cfg.seed ^ kAltNeutralizationSalt);
      RobustnessScenario sc;
      sc.kind = ScenarioKind::AltNeutralization;
      sc.alt_group_id = std::move(alt);
      auto edge_r = reeval(sc);
      if (edge_r.has_value()) {
        res.alt_neutralization = survival_outcome(inputs.base_edge, *edge_r, cfg.min_survival_ratio);
      }
    }
    if (res.alt_neutralization.applicable && !res.alt_neutralization.passed) {
      res.overall_pass = false;
    }
  }

  // ---- noise_control (NEGATIVE control — INVERTED polarity) -----------------
  if (cfg.noise_control) {
    if (inputs.input_values.empty()) {
      res.noise_control = CheckOutcome{}; // inapplicable: no input column supplied
    } else {
      std::vector<atx::f64> noisy(inputs.input_values.begin(), inputs.input_values.end());
      seeded_shuffle(noisy, cfg.seed ^ kNoiseControlSalt);
      RobustnessScenario sc;
      sc.kind = ScenarioKind::NoiseControl;
      sc.noise_input = std::move(noisy);
      auto edge_r = reeval(sc);
      if (edge_r.has_value()) {
        CheckOutcome out;
        out.applicable = true;
        out.base_edge = inputs.base_edge;
        out.scenario_edge = *edge_r;
        out.survival_ratio =
            (inputs.base_edge > 0.0) ? (*edge_r / inputs.base_edge) : 0.0;
        // PASS (not an artifact) iff the edge COLLAPSED on noise. A non-positive
        // base_edge has no edge to collapse from — vacuously PASS (nothing to
        // flag as an artifact when there was no measured edge to begin with).
        out.passed =
            (inputs.base_edge <= 0.0) || (*edge_r < cfg.min_survival_ratio * inputs.base_edge);
        res.noise_control = out;
      }
    }
    if (res.noise_control.applicable && !res.noise_control.passed) {
      res.overall_pass = false;
    }
  }

  // ---- param_perturbation ----------------------------------------------------
  if (cfg.param_perturbation) {
    const atx::usize draws = std::max<atx::usize>(2U, cfg.param_perturbation_draws);
    atx::core::Xoshiro256pp rng{cfg.seed ^ kParamPerturbationSalt};
    std::vector<atx::f64> edges;
    edges.reserve(draws);
    bool any_err = false;
    for (atx::usize d = 0; d < draws; ++d) {
      const atx::f64 scale = 1.0 + cfg.param_perturbation_band * (2.0 * rng.uniform01() - 1.0);
      RobustnessScenario sc;
      sc.kind = ScenarioKind::ParamPerturbation;
      sc.param_scale = scale;
      auto edge_r = reeval(sc);
      if (!edge_r.has_value()) {
        any_err = true;
        break;
      }
      edges.push_back(*edge_r);
    }
    if (any_err || edges.empty()) {
      res.param_perturbation = CheckOutcome{}; // inapplicable: the caller can't score a perturbed draw
    } else {
      const MeanStd ms = mean_std(edges);
      const atx::f64 cv = (std::abs(ms.mean) > 0.0) ? (ms.std / std::abs(ms.mean)) : ms.std;
      CheckOutcome out;
      out.applicable = true;
      out.base_edge = inputs.base_edge;
      out.scenario_edge = cv; // repurposed: the coefficient of variation, not a single edge
      out.survival_ratio = 0.0; // not a survival-ratio check
      out.passed = cv <= cfg.param_perturbation_max_cv;
      res.param_perturbation = out;
    }
    if (res.param_perturbation.applicable && !res.param_perturbation.passed) {
      res.overall_pass = false;
    }
  }

  return res;
}

} // namespace atx::engine::eval
