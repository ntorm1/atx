// corpus_board_fit — see corpus_board_fit.hpp. Moved verbatim out of
// corpus.cpp (T5 extraction) so `populate_surface_db` can reuse the EXACT
// same per-board fit pipeline `build_corpus` runs, instead of duplicating it.

#include "corpus_board_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception> // std::exception (the fit's what()-bearing catch clause)
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "al_probe.hpp"               // env-gated AL hot-path zone timers (Perf 2b step 1)
#include "atx/vol/arb.hpp"            // arb_check_calendar
#include "atx/vol/chain.hpp"          // OptionChain
#include "atx/vol/curve_selector.hpp" // SelectorResult, CandidateScore, score_curve_oos

namespace atx::vol {

namespace {

// Local copy of corpus.cpp's admission-reason bit helper: `terminal_decision`
// below needs it, and it is a five-line pure constexpr, not "the fit block"
// the T5 extraction is about — duplicating it here avoids exporting an
// admission-internal helper from the shared header.
static_assert(static_cast<unsigned>(CorpusAdmissionReason::Count) < 32u,
              "CorpusAdmissionReason no longer fits its uint32 failure mask");

[[nodiscard]] constexpr CorpusAdmissionFailureMask
admission_reason_mask(CorpusAdmissionReason reason) noexcept {
  const unsigned bit = static_cast<unsigned>(reason);
  return (reason == CorpusAdmissionReason::None || reason == CorpusAdmissionReason::Count)
             ? 0u
             : (CorpusAdmissionFailureMask{1u} << bit);
}

[[nodiscard]] std::uint32_t count_two_sided_quotes(const QuoteFrame &frame) noexcept {
  std::uint32_t count = 0u;
  for (const QuoteRow &row : frame.rows) {
    if (std::isfinite(row.bid) && std::isfinite(row.ask) && row.bid > 0.0 && row.ask > 0.0 &&
        row.bid <= row.ask && count < std::numeric_limits<std::uint32_t>::max()) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] CorpusAdmissionDecision terminal_decision(CorpusDisposition disposition,
                                                        CorpusAdmissionReason reason) noexcept {
  return CorpusAdmissionDecision{disposition, reason, admission_reason_mask(reason)};
}

// SurfaceHealth -> SurfaceProvenance for the ONE health record matching the
// surface `fitter.surface()` actually served (fail-closed: risk_health when
// the config requested/admitted risk, market_mark_health for a mark-only
// request — see PricerFitter::surface()). Preserves the independently
// admitted state/digest so it can reach the archive instead of being dropped
// (unwired C-2): every produced record satisfies the writer's
// healthy-implies-clean-digest invariant because decide_risk_surface_admission
// / the market-mark build path already only mark Healthy with a clean digest.
[[nodiscard]] SurfaceProvenance provenance_from_health(const SurfaceHealth &health) noexcept {
  SurfaceProvenance provenance;
  provenance.purpose = health.purpose;
  provenance.quality_mode = health.quality_mode;
  provenance.state = health.state;
  provenance.validation = health.validation;
  provenance.source_generation = health.candidate_generation;
  provenance.served_generation = health.served_generation;
  provenance.legacy_format = false;
  return provenance;
}

[[nodiscard]] CorpusQualityMetrics
collect_quality(const CorpusBoard &board, const OptionChain &chain, const PricerConfig &cfg,
                const PricerFitter &fitter, const PricedSurface &surface,
                const CorpusAdmissionPolicy *admission,
                const std::optional<FitDecision> &precomputed_classification) {
  CorpusQualityMetrics quality;
  quality.n_raw_quotes = saturated_u32(board.frame.rows.size());
  quality.n_two_sided = count_two_sided_quotes(board.frame);
  quality.n_slices = saturated_u32(surface.n_slices());
  quality.provenance_complete = board.source_provenance_complete;
  quality.source_schema_version = board.source_schema_version;
  quality.source_fingerprint = board.source_fingerprint;
  quality.market_input_fingerprint = board.market_input_fingerprint;
  quality.n_cash_dividends = saturated_u32(board.frame.divs.size());

  const std::optional<FitDecision> &actual = fitter.decision();
  if (actual.has_value()) {
    quality.profile = actual->profile.kind;
    quality.decision_source = actual->source;
    quality.preset = actual->preset;
    quality.primary_kind = actual->primary_curve.kind;
    quality.final_kind = actual->curve.kind;
    quality.used_fallback = actual->used_fallback;
  } else {
    // L1: reuse the pre-fit classification `retain_consumed_fit_parity` already
    // computed for this board when it is available (a caller-pinned curve leaves
    // the fitter's `decision()` empty, so this else-branch runs). select_fit_policy
    // is a pure function of (underlying, ticker, context, policy) — none mutated by
    // the fit — so the retained decision is byte-identical to a recomputation. Only
    // fall back to a fresh scan when no pre-fit decision was carried (e.g. admission
    // disabled but quality still collected).
    const FitDecision classified =
        precomputed_classification.has_value()
            ? *precomputed_classification
            : select_fit_policy(chain.underlying(), chain.underlying().ticker, cfg.context,
                                cfg.policy);
    quality.profile = classified.profile.kind;
    quality.decision_source = classified.source;
    quality.preset = cfg.preset;
    quality.primary_kind = surface.kind_at(0);
    quality.final_kind = surface.kind_at(0);
    quality.curve_pinned = true;
  }

  quality.final_kind_consistent = surface.n_slices() > 0u;
  for (std::size_t i = 0; i < surface.n_slices(); ++i) {
    if (surface.kind_at(i) != quality.final_kind) {
      quality.final_kind_consistent = false;
      break;
    }
  }

  const VolaSession &session = fitter.surface()->session();
  const SessionDiagnostics &diagnostics = session.diagnostics();
  std::size_t fit_scorable = 0u;
  std::size_t fit_in_band = 0u;
  for (const ParityReport &parity : session.parity()) {
    fit_scorable += parity.n;
    fit_in_band += parity.n_within;
  }
  quality.n_fit_scorable = saturated_u32(fit_scorable);
  quality.n_fit_in_band = saturated_u32(fit_in_band);
  if (fit_scorable > 0u) {
    quality.fit_in_band = static_cast<double>(fit_in_band) / static_cast<double>(fit_scorable);
    quality.mean_vol_rmse = diagnostics.mean_rmse_vol;
    quality.mean_reduced_chi2 = diagnostics.mean_chi2_reduced;
  }

  std::optional<CandidateScore> oos_score;
  if (!quality.used_fallback && fitter.selection().has_value()) {
    const SelectorResult &selection = *fitter.selection();
    if (selection.chosen_index < selection.scores.size()) {
      const CandidateScore &score = selection.scores[selection.chosen_index];
      if (score.kind == quality.final_kind) {
        oos_score = score;
      }
    }
  }

  const std::size_t profile_index = static_cast<std::size_t>(quality.profile);
  const CorpusAdmissionRule *rule =
      admission != nullptr && admission->enabled && profile_index < admission->by_profile.size()
          ? &admission->by_profile[profile_index]
          : nullptr;
  const bool require_oos =
      rule != nullptr && (rule->min_holdout > 0u || rule->min_oos_in_band.has_value() ||
                          rule->min_oos_vega_weighted.has_value());
  if (!oos_score.has_value() && require_oos) {
    const SessionInputs &resolved = session.inputs();
    SurfaceParityInputs scoring;
    scoring.S = resolved.S;
    scoring.r = resolved.r;
    scoring.expiry_rate_T = resolved.expiry_rate_T;
    scoring.expiry_rates = resolved.expiry_rates;
    scoring.cash_divs = resolved.cash_divs;
    scoring.now_ts_ns = resolved.now_ts_ns;
    scoring.deam = resolved.deam;
    scoring.calib = resolved.calib;
    scoring.band_k = resolved.band_k;
    scoring.repair = resolved.calendar_repair;
    scoring.fit_workers = resolved.fit_workers;
    scoring.score_parity = resolved.score_parity;
    scoring.enforce_calendar_floor = resolved.enforce_calendar_floor;
    scoring.use_deam_cache_for_fit = resolved.use_deam_cache_for_fit;
    Result<CandidateScore> scored =
        score_curve_oos(chain.underlying(), scoring, resolved.curve, cfg.selector);
    if (scored.has_value()) {
      oos_score = std::move(*scored);
    }
  }
  if (oos_score.has_value()) {
    quality.n_holdout = saturated_u32(oos_score->n_holdout);
    quality.n_oos_in_band = saturated_u32(oos_score->n_in_band);
    if (oos_score->n_holdout > 0u) {
      quality.oos_in_band = oos_score->oos_in_band;
    }
    quality.oos_vega_weight_in_band = oos_score->vega_weight_in_band;
    quality.oos_vega_weight_total = oos_score->vega_weight_total;
    if (oos_score->vega_weight_total > 0.0) {
      quality.oos_vega_weighted = oos_score->oos_vw;
    }
  }

  const double calendar_abs_k = rule != nullptr ? rule->calendar_abs_k : 3.0;
  const auto violations =
      arb_check_calendar(surface.surface(), -calendar_abs_k, calendar_abs_k, 25u);
  if (violations.has_value()) {
    quality.calendar_violations = saturated_u32(violations->size());
  }
  return quality;
}

[[nodiscard]] bool consumes_fit_parity(const CorpusAdmissionRule &rule) noexcept {
  return rule.min_fit_in_band.has_value() || rule.max_mean_vol_rmse.has_value() ||
         rule.max_mean_reduced_chi2.has_value();
}

// L1: computes the board classification ONCE (pre-fit) and RETURNS it so the
// post-fit `collect_quality` can reuse it instead of re-running the O(quotes)
// select_fit_policy scan for a pinned-curve board. Returns nullopt only when no
// classification was needed (admission absent/disabled) — the caller then lets
// collect_quality classify lazily on the rare path that still needs it.
[[nodiscard]] std::optional<FitDecision>
retain_consumed_fit_parity(const OptionChain &chain, const CorpusAdmissionPolicy *admission,
                           PricerConfig &cfg) {
  if (admission == nullptr || !admission->enabled) {
    return std::nullopt;
  }
  const FitDecision decision =
      select_fit_policy(chain.underlying(), chain.underlying().ticker, cfg.context, cfg.policy);
  const std::size_t profile_index = static_cast<std::size_t>(decision.profile.kind);
  if (profile_index < admission->by_profile.size() &&
      consumes_fit_parity(admission->by_profile[profile_index])) {
    // The fitter's Mark admission does not consume parity, but qualified-corpus
    // admission can do so immediately afterward. Make that outer dependency
    // explicit before the Mark fast-path default elides the diagnostic pass.
    // R-25: only DEFAULT parity on when the caller left it unset. An explicit
    // `score_parity == false` is documented to fail closed (parity-consuming
    // admission then rejects the board for want of the diagnostic), so it must
    // not be silently overridden here.
    if (!cfg.score_parity.has_value()) {
      cfg.score_parity = true;
    }
  }
  return decision;
}

} // namespace

std::uint32_t saturated_u32(std::size_t value) noexcept {
  return value > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
             ? std::numeric_limits<std::uint32_t>::max()
             : static_cast<std::uint32_t>(value);
}

FitSlot fit_board(const CorpusBoard &board, const PricerConfig &tmpl,
                  const CorpusAdmissionPolicy *admission,
                  const std::function<void(SessionInputs &)> &session_overlay,
                  WarmCacheExport *out_caches) {
  const alprobe::Scope probe_zone(alprobe::Zone::BoardFit);
  FitSlot slot{};
  slot.quality.n_raw_quotes = saturated_u32(board.frame.rows.size());
  slot.quality.n_two_sided = count_two_sided_quotes(board.frame);
  slot.quality.provenance_complete = board.source_provenance_complete;
  slot.quality.source_schema_version = board.source_schema_version;
  slot.quality.source_fingerprint = board.source_fingerprint;
  slot.quality.market_input_fingerprint = board.market_input_fingerprint;
  slot.quality.n_cash_dividends = saturated_u32(board.frame.divs.size());

  if (board.frame.rows.empty()) {
    slot.status = CorpusFitStatus::Skipped; // nothing fittable
    // No Error is produced on this path (nothing was attempted), but a caller that
    // counts Skipped as a failure -- populate_surface_db does -- still needs a
    // reason to show. `error_code` deliberately stays Unknown so corpus.cpp's
    // manifest column is byte-unchanged.
    slot.error_message = "empty board: no quote rows to fit";
    slot.admission = terminal_decision(CorpusDisposition::Empty, CorpusAdmissionReason::EmptyBoard);
    return slot;
  }

  try {
    auto chain = OptionChain::from_frame(board.frame, board.env);
    if (!chain) {
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = chain.error().code();
      slot.error_message = chain.error().message();
      slot.admission =
          terminal_decision(CorpusDisposition::FitFailed, CorpusAdmissionReason::FitError);
      return slot;
    }

    PricerConfig cfg = tmpl;
    cfg.n_threads = 1; // each board fits single-threaded; fan-out is ACROSS boards
    cfg.context = board.fit_context;
    if (board.curve.has_value()) {
      cfg.curve = *board.curve; // per-board pin overrides the template policy
    }
    const std::optional<FitDecision> board_classification =
        retain_consumed_fit_parity(*chain, admission, cfg);
    PricerFitter fitter{cfg};
    const Status st = fitter.fit(*chain, session_overlay);
    if (!st) {
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = st.error().code();
      // THE seam this whole diagnostic depends on: PricerFitter's rejection text
      // is built here and nowhere else, and until now it died on this line.
      slot.error_message = st.error().message();
      slot.admission =
          terminal_decision(CorpusDisposition::FitFailed, CorpusAdmissionReason::FitError);
      return slot;
    }

    const FittedSurface *fitted = fitter.surface();
    if (fitted == nullptr) { // defensive: a successful fit always stores a surface
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = ErrorCode::Internal;
      slot.error_message = "fit reported success but stored no surface";
      slot.admission =
          terminal_decision(CorpusDisposition::FitFailed, CorpusAdmissionReason::FitError);
      return slot;
    }

    auto ps = fitted->session().to_priced_surface();
    if (!ps) {
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = ps.error().code();
      slot.error_message = ps.error().message();
      slot.admission =
          terminal_decision(CorpusDisposition::FitFailed, CorpusAdmissionReason::FitError);
      return slot;
    }

    slot.n_slices = saturated_u32(ps->n_slices());
    const std::optional<SelectorResult> &sel = fitter.selection();
    if (sel.has_value()) {
      slot.chosen_kind = sel->chosen.kind;
      if (sel->chosen_index < sel->scores.size()) {
        slot.oos_in_band = sel->scores[sel->chosen_index].oos_in_band;
        slot.oos_in_band_available = true;
      }
    } else if (ps->n_slices() > 0) {
      slot.chosen_kind = ps->kind_at(0); // curve was pinned; no OOS score
    }
    if (admission != nullptr) {
      slot.quality =
          collect_quality(board, *chain, cfg, fitter, *ps, admission, board_classification);
      if (admission->enabled) {
        const std::size_t profile_index = static_cast<std::size_t>(slot.quality.profile);
        if (profile_index >= admission->by_profile.size()) {
          slot.admission =
              terminal_decision(CorpusDisposition::Quarantined, CorpusAdmissionReason::InvalidRule);
        } else {
          slot.admission =
              evaluate_corpus_admission(slot.quality, admission->by_profile[profile_index]);
        }
      }
    }
    // Capture the fitter's own admitted provenance for the served surface
    // BEFORE it goes out of scope — `fitter` is a stack-local per board, so
    // this is the one seam where the health computed inside PricerFitter::fit
    // can reach the archive write later in build_corpus_core (unwired C-2).
    // fitted->purpose() names which of bundle()'s two healths matches `ps`.
    const SurfaceBundle bundle = fitter.bundle();
    slot.provenance = provenance_from_health(fitted->purpose() == SurfacePurpose::Risk
                                                 ? bundle.risk_health
                                                 : bundle.market_mark_health);
    // C2 (perf): export the per-side correction caches this fit BUILT so the
    // cross-date chain can carry them forward. correction_caches() returns nulls
    // when the fit REUSED supplied caches (built nothing) — the export then stays
    // empty and the chain keeps its carried caches. Copied while the session (owned
    // by `fitter`, a stack local) is still alive.
    if (out_caches != nullptr) {
      const AmericanCorrectionCaches built = fitted->session().correction_caches();
      if (built.call != nullptr && built.call->populated()) {
        out_caches->call = *built.call;
      }
      if (built.put != nullptr && built.put->populated()) {
        out_caches->put = *built.put;
      }
    }
    slot.surface = std::move(*ps);
    slot.status = CorpusFitStatus::Ok;
    return slot;
  } catch (const std::exception &ex) {
    // SAFETY: a std::jthread worker must not let an exception escape (e.g.
    // std::bad_alloc from fit scratch) — that would std::terminate the process.
    // Record it as a Failed board instead. `what()` is copied into the slot so a
    // throwing board is as legible in the run report as a returned Error; the
    // status/error_code disposition is exactly what the bare `catch (...)` below
    // already produced.
    slot.status = CorpusFitStatus::Failed;
    slot.error_code = ErrorCode::Internal;
    try {
      // This concatenation itself allocates. If `ex` IS the std::bad_alloc this
      // handler exists to catch, the allocator may still be exhausted right here
      // — capturing the message must not be able to throw a SECOND exception,
      // because a throw from inside a catch clause is not caught by this
      // function's own try/catch: it would escape fit_board, be caught by
      // fit_scheduler.cpp's run_next as FailureKind::Exception, and turn this
      // single Failed board (partial coverage preserved) into a hard Err out of
      // populate_surface_db — exit 1 for the whole build. An empty message on a
      // genuine OOM is strictly better than that.
      slot.error_message = std::string("exception during fit: ") + ex.what();
    } catch (...) {
      slot.error_message.clear();
    }
    slot.admission =
        terminal_decision(CorpusDisposition::FitFailed, CorpusAdmissionReason::FitError);
    return slot;
  } catch (...) {
    // Same contract for a non-std exception, which carries no readable text.
    slot.status = CorpusFitStatus::Failed;
    slot.error_code = ErrorCode::Internal;
    try {
      slot.error_message = "unknown exception during fit"; // may allocate (SSO-exceeding); see above
    } catch (...) {
      slot.error_message.clear();
    }
    slot.admission =
        terminal_decision(CorpusDisposition::FitFailed, CorpusAdmissionReason::FitError);
    return slot;
  }
}

} // namespace atx::vol
