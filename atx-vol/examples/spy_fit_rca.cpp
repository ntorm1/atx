// spy_fit_rca.cpp — DIAGNOSTIC (bt-spyfit-rca): reproduce the SPY/XOM populate
// fit failure with full rejection detail. Loads real OPRA boards and fits each
// through the EXACT config path the F-c universe populate driver uses
// (symbol_config_from_preset(preset) + index pin -> pricer_config_for_symbol ->
// fit_board), but keeps the PricerFitter in scope so last_attempt_report() and
// the risk-rejection error string are printed. NOT a gate; OFF by default.
//
//   spy_fit_rca [--opra-root DIR] [--date YYYY-MM-DD] [--symbols A,B,...]
//               [--index-symbol SPY] [--preset fast] [--r 0.043]

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/chain.hpp"
#include "atx/vol/api/marketdata/corpus.hpp"
#include "atx/vol/api/fitting/fit_policy.hpp"
#include "atx/vol/api/marketdata/opra_batch.hpp"
#include "atx/vol/api/fitting/pricer_fitter.hpp"
#include "atx/vol/api/fitting/profile.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/storage/surface_db.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"

using namespace atx::vol;

namespace {

std::vector<std::string> split_csv(std::string_view csv) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t end = csv.find(',', start);
    const std::string_view f =
        csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start);
    if (!f.empty()) out.emplace_back(f);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return out;
}

FitPreset parse_preset(std::string_view n) {
  if (n == "accurate") return FitPreset::Accurate;
  if (n == "robust") return FitPreset::Robust;
  if (n == "hft") return FitPreset::Hft;
  if (n == "populate") return FitPreset::Populate;
  if (n == "bulk") return FitPreset::Bulk;
  return FitPreset::Fast;
}

// Mirror surface_db_populate.cpp's TU-private pricer_config_for_symbol.
PricerConfig pricer_config_for_symbol(const SymbolFitConfig &cfg) {
  PricerConfig out;
  out.preset = cfg.preset;
  out.quality_mode = cfg.surface_policy.quality_mode;
  out.outputs = cfg.surface_policy.outputs;
  out.risk_admission = cfg.surface_policy.risk_admission;
  out.fallback = cfg.surface_policy.fallback;
  if (cfg.pin_curve) out.curve = cfg.curve;
  out.use_correction_cache = cfg.use_correction_cache;
  out.score_parity = cfg.score_parity;
  out.enforce_calendar_floor = cfg.enforce_calendar_floor;
  out.use_deam_cache_for_fit = cfg.use_deam_cache_for_fit;
  return out;
}

const char *reason_name(SurfaceAdmissionReason r) {
  switch (r) {
  case SurfaceAdmissionReason::None: return "None";
  case SurfaceAdmissionReason::BuildFailed: return "BuildFailed";
  case SurfaceAdmissionReason::InsufficientFittedExpiries: return "InsufficientFittedExpiries";
  case SurfaceAdmissionReason::InsufficientExpiryCoverage: return "InsufficientExpiryCoverage";
  case SurfaceAdmissionReason::InsufficientQuoteCoverage: return "InsufficientQuoteCoverage";
  case SurfaceAdmissionReason::FrontExpiryMissing: return "FrontExpiryMissing";
  case SurfaceAdmissionReason::ConsecutiveExpiryGap: return "ConsecutiveExpiryGap";
  case SurfaceAdmissionReason::NonFiniteDiagnostics: return "NonFiniteDiagnostics";
  case SurfaceAdmissionReason::CalendarArbitrage: return "CalendarArbitrage";
  case SurfaceAdmissionReason::QualityBelowFloor: return "QualityBelowFloor";
  case SurfaceAdmissionReason::ImpossibleEvidence: return "ImpossibleEvidence";
  case SurfaceAdmissionReason::DuplicateMaturity: return "DuplicateMaturity";
  case SurfaceAdmissionReason::FiniteIvDomain: return "FiniteIvDomain";
  case SurfaceAdmissionReason::EuropeanPriceBounds: return "EuropeanPriceBounds";
  case SurfaceAdmissionReason::StrikeMonotonicity: return "StrikeMonotonicity";
  case SurfaceAdmissionReason::StrikeConvexity: return "StrikeConvexity";
  case SurfaceAdmissionReason::CalendarTotalVariance: return "CalendarTotalVariance";
  case SurfaceAdmissionReason::ForwardVariance: return "ForwardVariance";
  case SurfaceAdmissionReason::RequiredTenorBucket: return "RequiredTenorBucket";
  case SurfaceAdmissionReason::DiagnosticsUnavailable: return "DiagnosticsUnavailable";
  }
  return "?";
}

void diagnose(const CorpusBoard &board, const std::string &index_symbol, FitPreset preset) {
  std::printf("\n===== %s  %s =====\n", board.symbol.c_str(), board.date.c_str());

  // Board stats.
  std::size_t n_two = 0;
  for (const QuoteRow &r : board.frame.rows)
    if (std::isfinite(r.bid) && std::isfinite(r.ask) && r.bid > 0 && r.ask > 0 && r.bid <= r.ask)
      ++n_two;
  auto chain = OptionChain::from_frame(board.frame, board.env);
  if (!chain) {
    std::printf("  OptionChain::from_frame FAILED: %s\n", chain.error().to_string().c_str());
    return;
  }
  std::size_t n_expiries = chain->underlying().chains.size();
  std::printf("  rows=%zu two_sided=%zu divs=%zu  spot=%.4f rate=%.4f  expiries=%zu\n",
              board.frame.rows.size(), n_two, board.frame.divs.size(), chain->spot(),
              chain->rate(), n_expiries);
  // Expiry T ladder (years) — index dailies/weeklies show as a dense front.
  std::printf("  T(yr):");
  std::size_t shown = 0;
  for (const Chain &c : chain->underlying().chains) {
    if (shown++ < 16) std::printf(" %.4f", c.T);
  }
  if (n_expiries > 16) std::printf(" ...(%zu total)", n_expiries);
  std::printf("\n");

  // Driver config replication.
  SymbolFitConfig resolved = symbol_config_from_preset(preset);
  if (!index_symbol.empty() && board.symbol == index_symbol) {
    resolved.pin_curve = true;
    resolved.curve = CurveConfig{};
  }
  std::printf("  cfg: preset=%d pin_curve=%d outputs=%d quality_mode=%s risk_admission=%d\n",
              static_cast<int>(resolved.preset), resolved.pin_curve ? 1 : 0,
              static_cast<int>(resolved.surface_policy.outputs),
              to_string(resolved.surface_policy.quality_mode).data(),
              static_cast<int>(resolved.surface_policy.risk_admission));

  // Comparison A: pure MARK (Hft legacy, is_v2_request=false) — the SpyFitCorpus
  // config. Shows whether the board fits cleanly as a mark surface.
  {
    PricerConfig mk;
    mk.preset = FitPreset::Hft;
    mk.n_threads = 1;
    mk.fit_workers = 1;
    mk.context = board.fit_context;
    PricerFitter mfit{mk};
    const Status mst = mfit.fit(*chain);
    std::size_t exp_att = 0, exp_fit = 0, q_att = 0, q_fit = 0;
    if (mfit.last_attempt_report().has_value() &&
        !mfit.last_attempt_report()->attempts.empty()) {
      const auto &ev = mfit.last_attempt_report()->attempts.back().evidence;
      exp_att = ev.attempted_expiries; exp_fit = ev.fitted_expiries;
      q_att = ev.attempted_quotes; q_fit = ev.fitted_quotes;
    }
    std::printf("  MARK(hft): %s  exp fit/att=%zu/%zu quote fit/att=%zu/%zu cov=%.1f%%\n",
                mst ? "OK" : "FAIL", exp_fit, exp_att, q_fit, q_att,
                q_att ? 100.0 * static_cast<double>(q_fit) / q_att : 0.0);
  }

  // Comparison B: pinned ConvexDense on the v2 RISK path (latency). Shows whether
  // the dense model — the family the risk fallback should reach — fits + admits
  // this board when the auto-routed parametric primary does not.
  {
    PricerConfig cd;
    cd.preset = FitPreset::Fast;
    cd.quality_mode = FitQualityMode::Latency;
    cd.outputs = SurfaceOutputs::Risk;
    cd.curve = CurveConfig{VolCurveKind::ConvexDense};
    cd.n_threads = 1;
    cd.fit_workers = 1;
    cd.context = board.fit_context;
    if (board.curve.has_value()) cd.curve->convex = board.curve->convex;
    PricerFitter cdfit{cd};
    const Status cst = cdfit.fit(*chain);
    std::size_t exp_att = 0, exp_fit = 0, q_att = 0, q_fit = 0;
    bool admitted = false;
    if (cdfit.last_attempt_report().has_value() &&
        !cdfit.last_attempt_report()->attempts.empty()) {
      const auto &a = cdfit.last_attempt_report()->attempts.back();
      exp_att = a.evidence.attempted_expiries; exp_fit = a.evidence.fitted_expiries;
      q_att = a.evidence.attempted_quotes; q_fit = a.evidence.fitted_quotes;
      admitted = a.admission.admitted;
    }
    std::printf("  RISK(convex): %s admit=%d  exp fit/att=%zu/%zu quote fit/att=%zu/%zu cov=%.1f%%\n",
                cst ? "OK" : "FAIL", admitted ? 1 : 0, exp_fit, exp_att, q_fit, q_att,
                q_att ? 100.0 * static_cast<double>(q_fit) / q_att : 0.0);
  }

  PricerConfig pc = pricer_config_for_symbol(resolved);
  pc.fit_workers = 1;
  pc.n_threads = 1;
  pc.context = board.fit_context;
  if (board.curve.has_value()) pc.curve = *board.curve;

  PricerFitter fitter{pc};
  const Status st = fitter.fit(*chain, [&resolved](SessionInputs &in) {
    apply_symbol_config(resolved, in);
  });

  if (fitter.decision().has_value()) {
    const FitDecision &d = *fitter.decision();
    std::printf("  decision: profile=%d curve=%s preset=%d used_fallback=%d src=%d\n",
                static_cast<int>(d.profile.kind), to_string(d.curve.kind),
                static_cast<int>(d.preset), d.used_fallback ? 1 : 0,
                static_cast<int>(d.source));
  }
  const SurfaceBundle b = fitter.bundle();
  std::printf("  health: mark=%d risk=%d\n", static_cast<int>(b.market_mark_health.state),
              static_cast<int>(b.risk_health.state));

  if (!st) {
    std::printf("  FIT FAILED: %s\n", st.error().to_string().c_str());
  } else {
    std::size_t slices = 0;
    if (fitter.surface() != nullptr) {
      auto ps = fitter.surface()->session().to_priced_surface();
      if (ps.has_value()) slices = ps->n_slices();
    }
    std::printf("  FIT OK. surface slices=%zu\n", slices);
  }

  // Attempt-by-attempt admission detail.
  if (fitter.last_attempt_report().has_value()) {
    const SurfaceBuildReport &rep = *fitter.last_attempt_report();
    std::printf("  attempts=%zu published=%d used_fallback=%d\n", rep.attempts.size(),
                rep.published ? 1 : 0, rep.used_fallback ? 1 : 0);
    for (std::size_t i = 0; i < rep.attempts.size(); ++i) {
      const SurfaceBuildAttemptReport &a = rep.attempts[i];
      const SurfaceAdmissionEvidence &e = a.evidence;
      std::printf("   [%zu] curve=%-12s stage=%d built=%d admitted=%d reason=%s mask=0x%x\n", i,
                  to_string(a.curve.kind), static_cast<int>(a.stage),
                  a.build_succeeded ? 1 : 0, a.admission.admitted ? 1 : 0,
                  reason_name(a.admission.primary_reason), a.admission.failed_checks);
      const double cov = e.attempted_quotes > 0
                             ? static_cast<double>(e.fitted_quotes) / e.attempted_quotes
                             : 0.0;
      std::printf("       exp fit/att=%zu/%zu quote fit/att=%zu/%zu cov=%.1f%% front=%d "
                  "cal_arb_free=%d worst_bidask=%.4f cal_tv=%d fwd_var=%d convex=%d monotone=%d\n",
                  e.fitted_expiries, e.attempted_expiries, e.fitted_quotes, e.attempted_quotes,
                  cov * 100.0, e.front_expiry_fitted ? 1 : 0, e.calendar_arb_free ? 1 : 0,
                  e.worst_frac_within_bidask, e.calendar_total_variance ? 1 : 0,
                  e.forward_variance_nonnegative ? 1 : 0, e.strike_convex ? 1 : 0,
                  e.strike_monotone ? 1 : 0);
      // Per-expiry outcome + n_used (F=fitted, M=missing/dropped) — node-cap vs drop.
      std::printf("       exp[outcome:n_used]:");
      for (std::size_t k = 0; k < a.expiries.size() && k < 40; ++k) {
        const ExpiryBuildReport &er = a.expiries[k];
        const char oc = er.outcome == ExpiryBuildOutcome::Fitted      ? 'F'
                        : er.outcome == ExpiryBuildOutcome::Missing    ? 'M'
                                                                       : 'D';
        std::printf(" %c%zu", oc, er.n_used);
      }
      std::printf("\n");
      if (a.failure.has_value())
        std::printf("       build failure: %s\n", a.failure->to_string().c_str());
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  std::string opra_root = "C:/atx-data/spy-dispersion/opra";
  std::string date = "2026-01-02";
  std::string symbols_csv = "SPY,AAPL,XOM";
  std::string index_symbol = "SPY";
  std::string preset_name = "fast";
  std::string path_template; // empty = OpraBatchSpec default "{symbol}/{date}.parquet"
  std::string snapshot_suffix; // empty = OpraBatchSpec default "T19:55:00Z"
  double r = 0.043;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--opra-root") opra_root = nv();
    else if (a == "--date") date = nv();
    else if (a == "--symbols") symbols_csv = nv();
    else if (a == "--index-symbol") index_symbol = nv();
    else if (a == "--preset") preset_name = nv();
    else if (a == "--path-template") path_template = nv();
    else if (a == "--snapshot-suffix") snapshot_suffix = nv();
    else if (a == "--r") r = std::strtod(nv(), nullptr);
  }

  const std::vector<std::string> symbols = split_csv(symbols_csv);
  const FitPreset preset = parse_preset(preset_name);

  OpraBatchSpec spec;
  spec.symbols = symbols;
  spec.date_lo = date;
  spec.date_hi = date;
  spec.root_dir = opra_root;
  if (!path_template.empty()) spec.path_template = path_template;
  if (!snapshot_suffix.empty()) spec.snapshot_suffix = snapshot_suffix;
  spec.r = r;
  const Result<OpraBatchResult> batch = load_opra_daterange(spec);
  if (!batch) {
    std::fprintf(stderr, "load_opra_daterange: %s\n", batch.error().to_string().c_str());
    return 1;
  }
  std::printf("[opra] date=%s preset=%s loaded=%zu missing=%zu error=%zu of %zu\n", date.c_str(),
              preset_name.c_str(), batch->n_loaded, batch->n_missing, batch->n_error,
              batch->n_total);
  for (const OpraBatchEntry &e : batch->entries) {
    if (!e.panel) {
      std::printf("  [skip] %s %s (no panel)\n", e.symbol.c_str(), e.date.c_str());
      continue;
    }
    diagnose(corpus_board_from_opra(e.date, e.symbol, *e.panel), index_symbol, preset);
  }
  return 0;
}
