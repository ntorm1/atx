#include "atx/vol/curve_fit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
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

[[nodiscard]] bool slice_debug_enabled() noexcept {
#if defined(_WIN32)
  char *raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, "ATX_SLICE_DEBUG") != 0 || raw == nullptr) {
    return false;
  }
  const bool enabled = len > 0 && raw[0] != '\0' && raw[0] != '0';
  std::free(raw);
  return enabled;
#else
  const char *v = std::getenv("ATX_SLICE_DEBUG");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
#endif
}

[[nodiscard]] double elapsed_ms(ProfileClock::time_point t0, ProfileClock::time_point t1) noexcept {
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Per-chain output slot for the parallel de-Am pre-pass (phase 1). `usable`
// mirrors EXACTLY the set of `continue` gates the old sequential loop applied
// (T<=0, forward resolve failed, F non-finite/non-positive, obs below the
// shared kMinPreparedFitRows contract)
// so phase 2 skips precisely the chains the pre-S0-1 code skipped. Written by
// AT MOST one worker (its own chain index) and read only after every worker has
// joined (parallel_for's scope-exit barrier) — no cross-thread reduction, pure
// const reads of `under`/`in`, disjoint writes into `slot[i]`. Bit-identical for
// any worker count (the value_chain / calibrate_pool determinism pattern).
struct ChainPrepass {
  bool usable = false;
  // Carry resolution failed (or produced a degenerate forward): the chain is
  // unusable AND the skip must be surfaced in the report, never hidden (§5.2).
  bool carry_failed = false;
  double T = 0.0;
  double rate = 0.0;
  double F = 0.0;
  double borrow = 0.0;
  double q_eff = 0.0;
  double df = 0.0;
  std::optional<PreparedSlice> prepared; // meaningful only when `usable`
  double ms_forward_borrow = 0.0;
  double ms_obs_eu = 0.0;
  // Perf C1: the certification-layer's derived data, captured HERE (same
  // per-chain task) instead of a second serial pass in VolaSession::build's
  // (now-removed, for this driver) collect_input_diagnostics. Meaningful only
  // when `usable`. `carry` is the CERTIFICATION resolve — re-run with
  // `in.deam_cert_caches` substituted when those differ from the fit's own
  // caches (review fix; see run_deam_prepass) — and `carry_available` false
  // means that certification resolve failed (the historical serial pass's
  // carry-unavailable case; a cached fit resolve can succeed where the
  // certification resolve does not).
  CarryDiagnostics carry;
  bool carry_available = false;
  std::vector<double> source_mids;         // ‖ prepared fit rows; raw chain.mids at (K, side)
  std::vector<std::uint8_t> source_flags;  // ‖ prepared fit rows; raw chain.flags at (K, side)
  std::vector<double> chain_mids;          // full-chain snapshot
  std::vector<std::uint8_t> chain_flags;
  std::vector<double> chain_bids;
  std::vector<double> chain_asks;
  std::vector<std::int64_t> chain_ts;
};

// Recover each fit observation's SOURCE chain quote (raw American mid + kill-
// mask flags) by strike, so the incremental-cache certification data is
// captured in the SAME parallel per-chain task that built `obs` instead of a
// second serial pass over the fitted rows (perf finding C1). Mirrors the
// lookup `VolaSession::build`'s (session.cpp) `collect_input_diagnostics`
// used to perform after the fact: binary search `chain.strikes` for
// `fit_obs.K`, then read `chain.mids`/`chain.flags` at
// `chain_index(strike_idx, fit_obs.side)`. Clears BOTH outputs (defensive) if
// any row fails to map back onto the chain.
void source_quote_lookup(const Chain &chain, std::span<const FitObs> obs,
                         std::vector<double> &out_mids,
                         std::vector<std::uint8_t> &out_flags) {
  out_mids.clear();
  out_flags.clear();
  out_mids.reserve(obs.size());
  out_flags.reserve(obs.size());
  for (const FitObs &fit_obs : obs) {
    const auto strike_it = std::lower_bound(chain.strikes.begin(), chain.strikes.end(), fit_obs.K);
    if (strike_it == chain.strikes.end() || *strike_it != fit_obs.K) {
      out_mids.clear();
      out_flags.clear();
      return;
    }
    const auto strike_idx =
        static_cast<std::uint16_t>(std::distance(chain.strikes.begin(), strike_it));
    const std::size_t quote_idx = chain_index(strike_idx, fit_obs.side);
    if (quote_idx >= chain.mids.size() || quote_idx >= chain.flags.size()) {
      out_mids.clear();
      out_flags.clear();
      return;
    }
    out_mids.push_back(chain.mids[quote_idx]);
    out_flags.push_back(chain.flags[quote_idx]);
  }
}

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
      slot.carry_failed = true;
      return;
    }
    const double F = d_res->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      slot.carry_failed = true;
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
    if (!prepared || prepared->fit_observations().size() < kMinPreparedFitRows) {
      return;
    }

    slot.T = T;
    slot.rate = rate;
    slot.F = F;
    slot.borrow = d_res->borrow;
    slot.q_eff = q_eff;
    slot.df = df;
    slot.prepared.emplace(std::move(*prepared));
    // Perf C1 + review fix: the CERTIFICATION carry must be bit-identical to
    // what the historical serial certification pass produced — a resolve with
    // the CALLER's caches (in.deam_cert_caches), never the session-built
    // hot-path caches this prepass's own resolve may have consulted. When the
    // two cache sets are the same pointers, this resolve IS that resolve
    // (pure function of identical arguments): reuse it. When they differ,
    // re-resolve with the certification caches substituted — same per-chain
    // parallel task, so the work the old pass did serially is fanned out, and
    // a certification-resolve failure only marks this slice's carry
    // unavailable (the old pass's behavior), never drops the chain.
    const AmericanCorrectionCaches cert_caches =
        in.deam_cert_caches.has_value() ? *in.deam_cert_caches : in.deam.caches;
    if (cert_caches.call == in.deam.caches.call && cert_caches.put == in.deam.caches.put) {
      slot.carry = d_res->carry;
      slot.carry_available = true;
    } else {
      DeAmOptions cert_deam = in.deam;
      cert_deam.caches = cert_caches;
      const auto t_cert0 = ProfileClock::now();
      const auto cert_res =
          resolve_chain_forward(chain, in.S, rate, in.cash_divs, in.now_ts_ns, cert_deam);
      if (profile) {
        slot.ms_forward_borrow += elapsed_ms(t_cert0, ProfileClock::now());
      }
      if (cert_res) {
        slot.carry = cert_res->carry;
        slot.carry_available = true;
      }
    }
    // Perf C1: capture the certification layer's derived inputs HERE (same
    // task, same `chain`) instead of a second serial pass in
    // VolaSession::build. Source-quote lookup reads the prepared fit rows, so
    // it must run after the emplace above settles the rows into the slot.
    // (The old S0-3 second cold de-Am — `build_parity_data` — is superseded by
    // the PreparedSlice score columns, which score the SAME keyed population
    // as the fit rows; no separate market-side board re-inversion remains.)
    slot.chain_mids = chain.mids;
    slot.chain_flags = chain.flags;
    slot.chain_bids = chain.bids;
    slot.chain_asks = chain.asks;
    slot.chain_ts = chain.ts_ns;
    source_quote_lookup(chain, slot.prepared->fit_observations(), slot.source_mids,
                        slot.source_flags);
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
  out.input_certification.reserve(under.chains.size());
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
  // The prepass fans out over jthreads; parallel_for_dynamic now rethrows the
  // first worker exception on this thread (e.g. std::bad_alloc from an
  // allocating de-Am body). Convert it to an Err so this Result-returning API
  // stays exception-transparent to its callers rather than unwinding past them.
  // Non-const: phase 2 below MOVES the larger per-chain certification vectors
  // (perf C1) out of each committed slot instead of copying them again.
  std::vector<ChainPrepass> prepass;
  try {
    prepass = run_deam_prepass(under, in, in.fit_workers, profile);
  } catch (const std::exception &e) {
    return Err(ErrorCode::Internal,
               std::string("fit_curve_surface: de-Am prepass failed: ") + e.what());
  } catch (...) {
    return Err(ErrorCode::Internal, "fit_curve_surface: de-Am prepass failed (unknown exception)");
  }
  const double ms_prepass = profile ? elapsed_ms(t_pre0, ProfileClock::now()) : 0.0;
  for (const ChainPrepass &pre : prepass) {
    if (pre.carry_failed) {
      ++out.n_carry_skipped; // §5.2: carry-dropped expiries are surfaced
    }
  }

  // Phase 2 (SEQUENTIAL): the fit is order-dependent — each fitted slice's w(k)
  // becomes the calendar floor for the next (ascending-T) slice — so this walk
  // stays single-threaded, unchanged from the pre-S0-1 logic. It only reads the
  // phase-1 pre-pass results (skipping EXACTLY the chains phase 1 flagged) and
  // re-derives nothing the pre-pass already computed.
  double ms_fit_slice = 0.0;
  double ms_chain_parity = 0.0;
  for (std::size_t ci = 0; ci < under.chains.size(); ++ci) {
    ChainPrepass &pre = prepass[ci];
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
      // Diagnostic-only (env-gated, failure path): surface WHY a slice was
      // dropped so a caller can tell a genuinely-thin expiry from a curve-family
      // fit defect (e.g. SplineVol ill-conditioning on boards eSSVI fits). No
      // behavioural change; getenv runs only on the already-failed branch.
      if (slice_debug_enabled()) {
        std::fprintf(stderr, "[slice-drop] kind=%d T=%.4f F=%.2f obs=%zu err=%s\n",
                     static_cast<int>(cfg.kind), T, F, prepared.fit_observations().size(),
                     slice_res.error().to_string().c_str());
      }
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

    // Perf C1: carry the prepass's already-computed input certification data
    // straight into the report -- VolaSession::build's certification layer
    // consumes this instead of a second serial resolve_chain_forward +
    // build_observations_european pass. `obs`/`inversion` are COPIED (not
    // moved) so the end-of-function ATX_VOL_PROFILE summary below -- which
    // still reads `prepass[*].prepared->fit_observations().size()` for every
    // usable chain, including fit failures never pushed here -- stays intact;
    // the larger per-chain snapshot vectors are moved (nothing downstream
    // reads them again).
    SliceInputCertification cert;
    cert.carry = pre.carry;
    cert.carry_available = pre.carry_available;
    cert.inversion = prepared.deam_audit();
    cert.obs.assign(prepared.fit_observations().begin(), prepared.fit_observations().end());
    cert.source_mids = std::move(pre.source_mids);
    cert.source_flags = std::move(pre.source_flags);
    cert.chain_mids = std::move(pre.chain_mids);
    cert.chain_flags = std::move(pre.chain_flags);
    cert.chain_bids = std::move(pre.chain_bids);
    cert.chain_asks = std::move(pre.chain_asks);
    cert.chain_ts = std::move(pre.chain_ts);
    out.input_certification.push_back(std::move(cert));
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
