#include "atx/vol/curve_fit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/calib.hpp"  // build_observations_european, ObsSet, FitObs
#include "atx/vol/deamer.hpp" // resolve_chain_forward, european_equiv_iv, otm_side, DeAmOptions
#include "atx/vol/parallel_for.hpp"     // parallel_for (block-partition fan-out)
#include "atx/vol/parity.hpp"           // chain_parity, ParityInputs, ParityReport
#include "atx/vol/prepared_fitting.hpp" // canonical configured preparation
#include "atx/vol/universe.hpp"         // Chain, chain_index

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Minimum usable strikes to attempt a slice fit (mirrors run_surface_parity).
constexpr std::size_t kMinUsableObs = 5;

using ProfileClock = std::chrono::steady_clock;

[[nodiscard]] bool profile_enabled() noexcept {
#if defined(_WIN32)
  char *raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, "ATX_VOL_PROFILE") != 0 || raw == nullptr) {
    return false;
  }
  const bool enabled = len > 0 && raw[0] != '\0' && raw[0] != '0';
  std::free(raw);
  return enabled;
#else
  const char *v = std::getenv("ATX_VOL_PROFILE");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
#endif
}

[[nodiscard]] double elapsed_ms(ProfileClock::time_point t0, ProfileClock::time_point t1) noexcept {
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Per-chain output slot for the parallel de-Am pre-pass (phase 1). `usable`
// mirrors EXACTLY the set of `continue` gates the old sequential loop applied
// (T<=0, forward resolve failed, F non-finite/non-positive, obs < kMinUsableObs)
// so phase 2 skips precisely the chains the pre-S0-1 code skipped. Written by
// AT MOST one worker (its own chain index) and read only after every worker has
// joined (parallel_for's scope-exit barrier) — no cross-thread reduction, pure
// const reads of `under`/`in`, disjoint writes into `slot[i]`. Bit-identical for
// any worker count (the value_chain / calibrate_pool determinism pattern).
struct ChainPrepass {
  bool usable = false;
  double T = 0.0;
  double rate = 0.0;
  double F = 0.0;
  double borrow = 0.0;
  double q_eff = 0.0;
  double df = 0.0;
  std::optional<PreparedSlice> prepared; // meaningful only when `usable`
  double ms_forward_borrow = 0.0;
  double ms_obs_eu = 0.0;
};

[[nodiscard]] bool valid_expiry_rates(const SurfaceParityInputs &in,
                                      const Underlying &under) noexcept {
  return (in.expiry_rates.empty() && in.expiry_rate_T.empty()) ||
         (in.expiry_rates.size() == under.chains.size() &&
          in.expiry_rate_T.size() == under.chains.size() &&
          std::all_of(in.expiry_rates.begin(), in.expiry_rates.end(),
                      [](double rate) { return std::isfinite(rate); }) &&
          std::equal(in.expiry_rate_T.begin(), in.expiry_rate_T.end(), under.chains.begin(),
                     [](double T, const Chain &chain) {
                       return std::isfinite(T) && T > 0.0 && T == chain.T;
                     }));
}

[[nodiscard]] double expiry_rate(const SurfaceParityInputs &in, std::size_t index) noexcept {
  return in.expiry_rates.empty() ? in.r : in.expiry_rates[index];
}

// Phase 1: the cold, per-chain de-Am (resolve_chain_forward + the European
// observation build) fanned out over `n_threads` workers. Pure per-chain work,
// disjoint output slots — see `ChainPrepass` above.
[[nodiscard]] std::vector<ChainPrepass> run_deam_prepass(const Underlying &under,
                                                         const SurfaceParityInputs &in,
                                                         unsigned n_threads, bool profile) {
  std::vector<ChainPrepass> prepass(under.chains.size());
  // Long/dense expiries have highly variable Andersen-Lake cost. Run the
  // largest boards first and let workers dynamically claim the next chain;
  // output remains deterministic because every task owns prepass[i].
  std::vector<std::size_t> schedule(under.chains.size());
  std::iota(schedule.begin(), schedule.end(), std::size_t{0});
  std::stable_sort(schedule.begin(), schedule.end(), [&](std::size_t a, std::size_t b) {
    return under.chains[a].n_strikes() > under.chains[b].n_strikes();
  });
  parallel_for_dynamic(schedule.size(), n_threads, [&](std::size_t task) {
    const std::size_t i = schedule[task];
    const Chain &chain = under.chains[i];
    ChainPrepass &slot = prepass[i];
    const double T = chain.T;
    const double rate = expiry_rate(in, i);
    if (!(T > 0.0)) {
      return;
    }
    const auto t_forward0 = ProfileClock::now();
    const auto d_res =
        resolve_chain_forward(chain, in.S, rate, in.cash_divs, in.now_ts_ns, in.deam);
    if (profile) {
      slot.ms_forward_borrow = elapsed_ms(t_forward0, ProfileClock::now());
    }
    if (!d_res) {
      return;
    }
    const double F = d_res->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      return;
    }
    const double q_eff = rate - std::log(F / in.S) / T;
    const double df = std::exp(-rate * T);

    // COLD per-strike de-Am (no correction cache) at the caller's Andersen-Lake
    // accuracy: see the HARD CONSTRAINT note on the (now sequential-only)
    // fit/parity walk below — this pre-pass must stay cold for the same reason.
    const AmericanCorrectionCaches fit_caches =
        in.use_deam_cache_for_fit ? in.deam.caches : AmericanCorrectionCaches{};
    PreparedSliceInputs prepare_inputs;
    prepare_inputs.expiry_index = static_cast<std::uint32_t>(i);
    prepare_inputs.S = in.S;
    prepare_inputs.r = rate;
    prepare_inputs.F = F;
    prepare_inputs.q_eff = q_eff;
    prepare_inputs.df = df;
    prepare_inputs.calib = in.calib;
    prepare_inputs.caches = fit_caches;
    prepare_inputs.al_opts = in.deam.al_opts;
    prepare_inputs.iv_tolerance = in.deam.iv_tol;
    prepare_inputs.iv_max_iterations = in.deam.iv_max_iter;
    prepare_inputs.method = in.deam.method;
    prepare_inputs.policy = PreparedObservationPolicy::Configured;
    prepare_inputs.prepare_scoring = in.score_parity;
    const auto t_obs0 = ProfileClock::now();
    Result<PreparedSlice> prepared = PreparedSlice::create(chain, prepare_inputs);
    if (profile) {
      slot.ms_obs_eu = elapsed_ms(t_obs0, ProfileClock::now());
    }
    if (!prepared || prepared->fit_observations().size() < kMinUsableObs) {
      return;
    }

    slot.T = T;
    slot.rate = rate;
    slot.F = F;
    slot.borrow = d_res->borrow;
    slot.q_eff = q_eff;
    slot.df = df;
    slot.prepared.emplace(std::move(*prepared));
    slot.usable = true;
  });
  return prepass;
}

} // namespace

Result<CurveSurfaceReport> fit_curve_surface(const Underlying &under, const SurfaceParityInputs &in,
                                             const CurveConfig &cfg) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument, "fit_curve_surface: non-positive S or non-finite r");
  }
  if (!valid_expiry_rates(in, under)) {
    return Err(ErrorCode::InvalidArgument, "fit_curve_surface: invalid expiry rate vectors");
  }
  if (under.chains.empty()) {
    return Err(ErrorCode::NotFound, "fit_curve_surface: underlying carries no chains");
  }

  CurveSurfaceReport out;
  out.context.reserve(under.chains.size());
  out.per_expiry.reserve(under.chains.size());
  double worst = std::numeric_limits<double>::infinity();
  const bool profile = profile_enabled();
  const auto t_fit0 = ProfileClock::now();

  // Phase 1 (PARALLEL): the cold per-chain de-Am — resolve_chain_forward (term
  // forward/borrow) + build_observations_european (the 99.5% recipe's European
  // fit observations) — is independent per chain, so it fans out over
  // `in.fit_workers` disjoint output slots (0 => hardware_concurrency; 1 =>
  // serial, bit-identical to the pre-S0-1 path). MUST stay COLD (no correction
  // cache): a near-interpolating convex fit propagates any de-Am bias straight
  // into the served IV, so the small carry-bias of the cached-de-Am hot path
  // (fine for the coarse eSSVI backbone) knocks the penny-tight dense fit out
  // of band. Correctness over speed here — cold Andersen-Lake per strike, just
  // run concurrently across chains.
  const auto t_pre0 = ProfileClock::now();
  const std::vector<ChainPrepass> prepass = run_deam_prepass(under, in, in.fit_workers, profile);
  const double ms_prepass = profile ? elapsed_ms(t_pre0, ProfileClock::now()) : 0.0;

  // Phase 2 (SEQUENTIAL): the fit is order-dependent — each fitted slice's w(k)
  // becomes the calendar floor for the next (ascending-T) slice — so this walk
  // stays single-threaded, unchanged from the pre-S0-1 logic. It only reads the
  // phase-1 pre-pass results (skipping EXACTLY the chains phase 1 flagged) and
  // re-derives nothing the pre-pass already computed.
  double ms_fit_slice = 0.0;
  double ms_chain_parity = 0.0;
  for (std::size_t ci = 0; ci < under.chains.size(); ++ci) {
    const ChainPrepass &pre = prepass[ci];
    if (!pre.usable) {
      continue;
    }
    const double T = pre.T;
    const double F = pre.F;
    const double q_eff = pre.q_eff;
    const double df = pre.df;
    const PreparedSlice &prepared = *pre.prepared;
    out.n_score_inversions += prepared.provenance().n_score_inversions;

    // 3. Fit the configured curve kind from the European obs.
    //    Calendar floor: previous fitted slice's total variance (ascending T).
    //    Guard on the prior slice's T so a non-ascending input degrades to
    //    no-enforcement (never an inverted floor). The loader sorts ascending-T
    //    (data.cpp sort_chains_by_T), so on the standard board this is always
    //    the immediately-shorter expiry.
    std::function<double(double)> w_prev;
    std::span<const double> calendar_floor_knots;
    if (in.enforce_calendar_floor && !out.surface.empty() && out.context.back().T < T) {
      const IVolCurve *prev = out.surface.slices().back().get();
      w_prev = [prev](double k) { return prev->w(k); };
      if (const auto *linear = dynamic_cast<const LinearVarianceCurve *>(prev); linear != nullptr) {
        calendar_floor_knots = linear->k_nodes();
      }
    }
    // The calendar floor inside fit_convex_slice enforces w_curr >= w_prev at the
    // fit nodes (STRICT, per the served-surface policy: calendar-arb-free by
    // construction). It adds constraint ROWS to the N-node QP, not slack variables,
    // so enforcement does not materially slow the fit. On boards with genuine
    // calendar structure this trades some price-in-band tightness for no-arb — an
    // explicit product choice (see spy_bidask_regression_test's rebaselined floor).
    const auto t_slice0 = ProfileClock::now();
    auto slice_res =
        fit_slice_curve(cfg, prepared.fit_observations(), F, T, df, w_prev, calendar_floor_knots);
    if (profile) {
      ms_fit_slice += elapsed_ms(t_slice0, ProfileClock::now());
    }
    if (!slice_res) {
      continue;
    }
    const IVolCurve *const slice = slice_res->get();

    // 4. Score re-Americanized parity off the fitted slice's own iv(k). A parity
    //    (diagnostic) failure is non-fatal: keep the slice, push a zeroed report.
    //    `in.score_parity` opts OUT of this block entirely. The prepared score
    //    rows are the same keyed population as the fit rows, so scoring cannot
    //    silently reintroduce a quote rejected by filtering or the cap.
    //    `parity` stays the default-constructed zeroed
    //    ParityReport{} (n == 0), so `worst` below never advances past its
    //    `infinity` init and `worst_frac_within_bidask` resolves to 0.0 -- the
    //    intended "no diagnostic" sentinel.
    ParityReport parity{};
    if (in.score_parity) {
      const auto t_parity0 = ProfileClock::now();
      const PreparedScoreColumns &score = prepared.score_columns();
      if (score.k_log.size() >= 4u) {
        std::vector<double> model_iv;
        model_iv.reserve(score.k_log.size());
        for (const double k_log : score.k_log) {
          model_iv.push_back(slice->iv(k_log));
        }
        ParityInputs pin{};
        pin.S = in.S;
        pin.r = pre.rate;
        pin.q_eff = q_eff;
        pin.T = T;
        pin.method = in.deam.method;
        pin.al_opts = in.deam.al_opts;
        pin.band_k = in.band_k;
        pin.n_curve_params = 3; // nominal chi2 dof (informational for dense fits)
        // Re-Americanize through the same cache policy that produced the one
        // canonical European row. A cold fit must not score against a cached
        // inverse map (or vice versa).
        pin.caches = in.use_deam_cache_for_fit ? in.deam.caches : AmericanCorrectionCaches{};
        auto pr = chain_parity(score.strike, score.bid, score.ask, score.mid, score.side, model_iv,
                               score.market_iv, pin);
        if (pr) {
          parity = *pr;
        }
      }
      if (profile) {
        ms_chain_parity += elapsed_ms(t_parity0, ProfileClock::now());
      }
    }

    // 5. Commit the slice + its context (ascending T by construction).
    out.surface.push(std::move(*slice_res));
    out.context.push_back(SliceContext{T, F, pre.borrow, q_eff, prepared.fit_observations().size(),
                                       static_cast<std::size_t>(prepared.n_dropped())});
    out.per_expiry.push_back(parity);
    if (parity.n > 0) {
      worst = std::min(worst, parity.frac_fv_within_bidask);
    }
  }

  if (out.surface.empty()) {
    return Err(ErrorCode::NotFound, "fit_curve_surface: no expiry produced a usable slice");
  }
  out.n_slices = out.surface.n_slices();
  out.worst_frac_within_bidask = std::isfinite(worst) ? worst : 0.0;
  if (profile) {
    double ms_forward_borrow = 0.0;
    double ms_obs_eu = 0.0;
    std::size_t n_usable = 0;
    std::size_t n_quotes = 0;
    for (const ChainPrepass &pre : prepass) {
      ms_forward_borrow += pre.ms_forward_borrow;
      ms_obs_eu += pre.ms_obs_eu;
      if (pre.usable) {
        ++n_usable;
        n_quotes += pre.prepared->fit_observations().size();
      }
    }
    std::fprintf(stderr,
                 "[ATX_VOL_PROFILE] curve_fit_total=%.3fms prepass_wall=%.3fms "
                 "forward_borrow_sum=%.3fms obs_eu_sum=%.3fms fit_slice_sum=%.3fms "
                 "chain_parity_sum=%.3fms usable=%zu "
                 "slices=%zu quotes=%zu workers=%u\n",
                 elapsed_ms(t_fit0, ProfileClock::now()), ms_prepass, ms_forward_borrow, ms_obs_eu,
                 ms_fit_slice, ms_chain_parity, n_usable, out.n_slices, n_quotes, in.fit_workers);
  }
  return Ok(std::move(out));
}

} // namespace atx::vol
