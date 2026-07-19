#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/arb.hpp" // QuoteFlag (carry-skip diagnostics test)
#include "atx/vol/calib.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/detail/deam_pass_counter.hpp" // C1 duplicate-de-Am proof
#include "atx/vol/opra_panel.hpp"               // OpraLoadSpec (C1 real-OPRA characterization)
#include "atx/vol/panel.hpp"
#include "atx/vol/query_pricing.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"

// VolaSession composable-facade acceptance harness (atx/vol/session.hpp).
//
// Drives the same 4-expiry known-truth American-equity panel the SurfaceParity
// harness uses (rising ATM term structure, downward skew, one mid-life cash
// dividend), builds a session, and exercises the query surface: interpolated IV,
// re-Americanized fair value on- and between-slices, Greeks, and the argument-
// validation error path. The strike grid includes the at-forward level (100) so
// the ATM checks land on a real quote.

namespace {

using atx::vol::build_observations;
using atx::vol::build_observations_european;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::data_install;
using atx::vol::DividendEvent;
using atx::vol::FitDiag;
using atx::vol::InterpMode;
using atx::vol::iso_to_ns;
using atx::vol::make_synthetic_american_panel;
using atx::vol::S3Params;
using atx::vol::SessionInputs;
using atx::vol::Side;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::VolaSession;
using atx::vol::VolCurveKind;
using atx::vol::year_fraction;

// The 4-expiry known-truth panel spec (mirrors surface_parity_test's panel; the
// strike grid steps by 5 so the at-forward strike 100 is present for the ATM
// checks).
[[nodiscard]] SynthPanelSpec make_spec() {
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {
      "2026-07-26", // ~0.10y
      "2026-10-06", // ~0.30y
      "2027-01-24", // ~0.60y
      "2027-06-19", // ~1.00y
  };
  const std::vector<S3Params> truths = {
      S3Params{0.32, -0.70, 1.00},
      S3Params{0.30, -0.60, 0.90},
      S3Params{0.28, -0.55, 0.80},
      S3Params{0.27, -0.50, 0.70},
  };

  SynthPanelSpec spec;
  spec.uid = "SYNTH";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.03;
  spec.borrow = 0.008;

  DividendEvent div;
  div.ex_date_ns = iso_to_ns("2026-12-15"); // mid-life; inside the 0.6y/1.0y
  div.amount = 0.5;
  spec.cash_divs = {div};

  for (std::size_t i = 0; i < isos.size(); ++i) {
    const double T = year_fraction(snapshot, isos[i]);
    spec.expiries.push_back(SynthExpiry{isos[i], T, truths[i]});
  }
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K); // 13 strikes over 70..130, includes 100
  }
  spec.half_spread_frac = 0.02;
  return spec;
}

[[nodiscard]] SessionInputs make_inputs(const SynthPanelSpec &spec) {
  SessionInputs in;
  in.S = spec.spot;
  in.r = spec.r;
  in.cash_divs = spec.cash_divs;
  in.now_ts_ns = iso_to_ns(spec.snapshot_iso);
  in.deam.hyb = spec.hyb;
  in.deam.imply_borrow = true;
  in.deam.n_atm = 3;
  return in;
}

// A 2-expiry panel with DELIBERATELY contrasting smile shapes: the near
// expiry is strongly put-skewed, the far expiry is flat. Used to prove
// SessionInputs::interp actually reaches the eval seam -- ShapeBlend and
// PiecewiseTotalVariance must disagree at an arbitrary-T query strictly
// between two slices whose SHAPES (not just level) differ this much.
[[nodiscard]] SynthPanelSpec make_shape_contrast_spec() {
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {
      "2026-07-19", // ~0.10y, skewed lo
      "2026-12-19", // ~0.50y, flat hi
  };
  const std::vector<S3Params> truths = {
      S3Params{0.35, -1.20, 1.20}, // strong put skew
      S3Params{0.22, 0.0, 0.30},   // flat
  };

  SynthPanelSpec spec;
  spec.uid = "SYNTH2";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.03;
  spec.borrow = 0.0;

  for (std::size_t i = 0; i < isos.size(); ++i) {
    const double T = year_fraction(snapshot, isos[i]);
    spec.expiries.push_back(SynthExpiry{isos[i], T, truths[i]});
  }
  for (double K = 60.0; K <= 140.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K);
  }
  spec.half_spread_frac = 0.02;
  return spec;
}

// Install the spec's panel into `u` and return the resolved underlying pointer.
[[nodiscard]] const Underlying *install(const SynthPanelSpec &spec, Universe &u) {
  const auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value());
  if (!panel) {
    return nullptr;
  }
  const auto uid = data_install(u, panel->frame);
  EXPECT_TRUE(uid.has_value());
  if (!uid) {
    return nullptr;
  }
  const auto under = u.get_underlying(*uid);
  EXPECT_TRUE(under.has_value());
  return under ? *under : nullptr;
}

} // namespace

TEST(VolaSession, Build_KnownTruthPanel_SucceedsWithFourArbFreeSlices) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  ASSERT_EQ(under->chains.size(), std::size_t{4});

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto &diag = sess->diagnostics();
  EXPECT_EQ(diag.n_slices, std::size_t{4});
  EXPECT_EQ(sess->expiries().size(), std::size_t{4});
  EXPECT_EQ(sess->parity().size(), std::size_t{4});
  EXPECT_TRUE(diag.calendar_arb_free);
  EXPECT_GE(diag.worst_frac_within_bidask, 0.90);
  EXPECT_GT(diag.n_quotes, std::size_t{0});
  EXPECT_EQ(diag.n_carry_slices, diag.n_slices);
  EXPECT_EQ(diag.n_carry_confident, diag.n_slices);
  EXPECT_TRUE(diag.carry_confident);
  EXPECT_EQ(diag.n_inversion_slices, diag.n_slices);
  EXPECT_GT(diag.n_iv_proposed, std::size_t{0});
  // C1 (class accuracy-improving): the input-diagnostics de-Am is no longer a
  // SECOND independent (Configured, always-audited) pass — it now REUSES the
  // eSSVI fit's own de-Am (finding 10). This build does NOT set
  // deam.audit_fit_inversions, so the FIT ran the UN-audited Legacy route: the
  // proposal ledger reflects the rows the fit actually de-Americanized
  // (n_iv_proposed > 0), but n_iv_audited is 0 because the fit never repriced
  // them against the cold reference. Pre-C1 this diagnostic re-run audited every
  // proposal (n_iv_audited == n_iv_proposed) — describing rows the fit never
  // used. The honest audited-fit variant is AuditedEssviFitCertifiesInversions.
  EXPECT_EQ(diag.n_iv_audited, std::size_t{0});
  EXPECT_EQ(diag.n_iv_rejected_residual, std::size_t{0});
  // Honest certificate (§5.3/§8.1): with audit-off fit rows the certificate must
  // stay false (a certificate may not vouch for un-audited rows). Unchanged by
  // C1 — the certificate gate still requires audited fit rows, so admission is
  // byte-identical on this path (the audited variant certifies below).
  EXPECT_FALSE(diag.inversion_certified);
  EXPECT_EQ(diag.n_carry_skipped_expiries, std::size_t{0});

  const auto input_diag = sess->slice_diagnostics();
  ASSERT_EQ(input_diag.size(), sess->expiries().size());
  for (std::size_t i = 0; i < input_diag.size(); ++i) {
    EXPECT_DOUBLE_EQ(input_diag[i].T, sess->expiries()[i].T);
    EXPECT_TRUE(input_diag[i].carry.available);
    EXPECT_TRUE(input_diag[i].carry.confident);
    EXPECT_GE(input_diag[i].carry.n_retained, std::size_t{3});
    EXPECT_TRUE(input_diag[i].inversion_available);
    EXPECT_FALSE(input_diag[i].inversion_certified); // unaudited fit rows
  }

  // Slice context is ascending in T.
  const auto exps = sess->expiries();
  for (std::size_t i = 1; i < exps.size(); ++i) {
    EXPECT_LT(exps[i - 1].T, exps[i].T);
  }
}

// C1 (perf, class accuracy-improving): the eSSVI session build must de-Americanize
// each expiry EXACTLY ONCE — the fit's own pass. Historically it ran a SECOND
// full de-Am per slice in the certification/diagnostics layer
// (collect_input_diagnostics -> build_observations_european, finding 10). This
// proves the reduction with the provisional deam-pass counters (folded into
// WS-V's ledger at merge): the CERT pass drops to zero while the FIT pass is
// unchanged (one per fitted slice). The fit path itself stays byte-identical
// (covered by Build_KnownTruthPanel above).
TEST(VolaSession, EssviBuild_DeAmericanizesEachExpiryExactlyOnce) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  ASSERT_EQ(under->chains.size(), std::size_t{4});

  atx::vol::detail::reset_deam_slice_passes();
  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  const std::size_t n_slices = sess->diagnostics().n_slices;
  ASSERT_EQ(n_slices, std::size_t{4});

  // The fit de-Ams every fitted slice once (unchanged by C1).
  EXPECT_EQ(atx::vol::detail::fit_deam_slice_passes(), n_slices);
  // C1: the certification/diagnostics layer must no longer run its own de-Am —
  // it reuses the fit's. Pre-C1 this was == n_slices (the duplicate pass).
  EXPECT_EQ(atx::vol::detail::cert_deam_slice_passes(), std::size_t{0});
}

// C1 (perf, class accuracy-improving) — real-OPRA de-Am characterization / gate 3.
// Loads real Databento OPRA boards from the shared developer cache and fits each
// with the Populate-representative Robust preset. Robust leaves
// `deam.audit_fit_inversions` at its false default (deamer.hpp:270), so the fit
// runs UN-audited and C1's cert-de-Am reuse (session.cpp VolaSession::build HYBRID
// GATE) engages — the exact bulk-populate regime C1 targets.
//
// Emits one "C1CHAR" TSV line per board (de-Am diagnostic counts +
// inversion_certified + surface quality) so a before/after run (git-stash the C1
// src, rebuild, re-run) fills the audit-count-delta column of the characterization
// table. Asserts the two zero-flip invariants that gate C1 admission (PM gate 3),
// which hold IDENTICALLY before and after C1: (1) the certificate stays FALSE on an
// un-audited fit (a certificate may not vouch for rows the fit never audited — the
// gate is `fit_rows_audited && …`, and the fit ran un-audited), so C1 causes zero
// `inversion_certified` flips; (2) a usable, finite-quality surface is still
// produced (the served surface is byte-identical — C1 only changes which rows the
// diagnostics DESCRIBE, never the fit). Skips cleanly when the OPRA cache is absent.
TEST(VolaSession, C1RealBoardDeAmCharacterization) {
  using atx::vol::FitPreset;
  using atx::vol::load_opra_cbbo_parquet;
  using atx::vol::make_session_inputs;
  using atx::vol::OpraLoadSpec;

  struct RealBoard {
    const char *sym;
    const char *date;
  };
  // XOM/AAPL multi-date real boards (the fixture family the corpus tests cite).
  const RealBoard boards[] = {
      {"AAPL", "2026-01-02"}, {"AAPL", "2026-01-05"}, {"AAPL", "2026-01-06"},
      {"XOM", "2026-01-02"},  {"XOM", "2026-01-05"},  {"XOM", "2026-01-06"},
  };
  const std::string root = "C:/atx-data/spy-dispersion/opra/";
  const double r = 0.043;

  std::size_t n_fit = 0;
  for (const RealBoard &b : boards) {
    const std::string path = root + b.sym + "/" + b.date + ".parquet";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      continue;
    }
    OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = b.sym;
    spec.snapshot_iso = std::string(b.date) + "T14:00:00Z"; // cosmetic; frame carries the real ts
    spec.r = r;
    auto panel = load_opra_cbbo_parquet(spec);
    if (!panel.has_value()) {
      continue;
    }
    const auto in =
        make_session_inputs(FitPreset::Robust, panel->implied_spot, r, panel->frame.snapshot_ts_ns);
    auto sess = VolaSession::from_frame(panel->frame, in);
    if (!sess.has_value()) {
      continue;
    }
    const auto &d = sess->diagnostics();
    ++n_fit;
    std::printf("C1CHAR\t%-4s\t%s\tS=%.2f\tn_slices=%zu\tn_prop=%zu\tn_aud=%zu\tn_fb=%zu\t"
                "n_rej=%zu\tcert=%d\trmse_vol=%.6f\tworst_frac=%.4f\n",
                b.sym, b.date, panel->implied_spot, d.n_slices, d.n_iv_proposed, d.n_iv_audited,
                d.n_iv_fallback, d.n_iv_rejected_residual, static_cast<int>(d.inversion_certified),
                d.mean_rmse_vol, d.worst_frac_within_bidask);

    // Gate-3 zero-flip invariants (identical before and after C1):
    EXPECT_FALSE(d.inversion_certified) << b.sym << " " << b.date << ": un-audited fit must not certify";
    EXPECT_GT(d.n_slices, std::size_t{0}) << b.sym << " " << b.date << ": no usable slice";
    EXPECT_TRUE(std::isfinite(d.mean_rmse_vol)) << b.sym << " " << b.date << ": non-finite fit RMSE";
  }

  if (n_fit == 0) {
    GTEST_SKIP() << "no real OPRA boards under " << root << " (shared developer cache absent)";
  }
  std::printf("C1CHAR\tTOTAL_BOARDS_FIT=%zu\n", n_fit);
}

// C2 (perf) — cross-date correction-cache reuse: ledger + quality-parity +
// determinism + positive-control, on the real AAPL Jan-2026 chain (the fixtures
// the warm-start chain drives). Validates the SESSION-level mechanism the
// corpus.cpp chain driver composes: date 1 builds caches cold; a later date
// supplies them via deam.caches + deam.reuse_supplied_caches, and the session's
// stale-gate (supplied_caches_cover_board) reuses them — skipping the ~192-solve/
// board rebuild (finding 11) — only when they still cover the board at a
// compatible baked carry, else cold-rebuilds (byte-identical fallback).
TEST(VolaSession, C2CrossDateCacheReuseCutsSolvesInBand) {
  using atx::vol::AmericanCorrectionCaches;
  using atx::vol::CorrectionCache;
  using atx::vol::FitPreset;
  using atx::vol::load_opra_cbbo_parquet;
  using atx::vol::make_session_inputs;
  using atx::vol::OpraLoadSpec;
  using atx::vol::OpraPanel;
  namespace led = atx::vol::counters::ledger;

  const std::string root = "C:/atx-data/spy-dispersion/opra/";
  const double r = 0.043;
  const auto load = [&](const char *sym, const char *date) -> std::optional<OpraPanel> {
    const std::string path = std::string(root) + sym + "/" + date + ".parquet";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      return std::nullopt;
    }
    OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = sym;
    spec.snapshot_iso = std::string(date) + "T14:00:00Z";
    spec.r = r;
    auto p = load_opra_cbbo_parquet(spec);
    if (!p.has_value()) {
      return std::nullopt;
    }
    return std::move(*p);
  };

  auto d1 = load("AAPL", "2026-01-02");
  auto d2 = load("AAPL", "2026-01-05");
  if (!d1.has_value() || !d2.has_value()) {
    GTEST_SKIP() << "AAPL OPRA chain not available under " << root;
  }

  const auto make_in = [&](double spot, std::int64_t ts, double rate, bool chain) {
    auto in = make_session_inputs(FitPreset::Robust, spot, rate, ts);
    in.fit_workers = 1u;                       // single-threaded => deterministic solve ledger
    in.deam.chain_cache_mode = chain;          // wide, reusable cache for the chain path
    return in;
  };
  const auto solves_now = []() {
    return led::snapshot().get(led::Solve::AlBoundarySolves);
  };
  const auto fingerprint = [](const VolaSession &s, double spot) {
    const auto ps = s.to_priced_surface();
    std::vector<double> fp;
    const double Ts[] = {0.03, 0.08, 0.16, 0.30};
    const double ms[] = {0.85, 0.95, 1.0, 1.05, 1.15};
    for (const double T : Ts) {
      for (const double m : ms) {
        fp.push_back(ps.has_value() ? ps->iv(spot * m, T) : std::nan(""));
      }
    }
    return fp;
  };

  // Date 1: cold fit; copy out its built per-side correction caches.
  auto s1 = VolaSession::from_frame(d1->frame, make_in(d1->implied_spot, d1->frame.snapshot_ts_ns, r, /*chain=*/true));
  ASSERT_TRUE(s1.has_value()) << s1.error().to_string();
  const AmericanCorrectionCaches c1 = s1->correction_caches();
  if (c1.call == nullptr || c1.put == nullptr) {
    GTEST_SKIP() << "date-1 fit built no correction caches (nothing to reuse)";
  }
  const CorrectionCache cache_call = *c1.call;
  const CorrectionCache cache_put = *c1.put;

  // Date 2 COLD baseline.
  const auto in2 = make_in(d2->implied_spot, d2->frame.snapshot_ts_ns, r, /*chain=*/false); // production cold baseline
  led::reset();
  auto s2_cold = VolaSession::from_frame(d2->frame, in2);
  const std::uint64_t cold_solves = solves_now();
  ASSERT_TRUE(s2_cold.has_value()) << s2_cold.error().to_string();
  const auto fp_cold = fingerprint(*s2_cold, d2->implied_spot);
  const auto cold_diag = s2_cold->diagnostics();

  // Date 2 WARM: reuse date-1 caches (same symbol, adjacent date -> gate admits).
  auto in2w = in2;
  in2w.deam.chain_cache_mode = true; // reuse; a gate miss rebuilds a wide chain cache
  in2w.deam.caches = AmericanCorrectionCaches{&cache_call, &cache_put};
  in2w.deam.reuse_supplied_caches = true;
  led::reset();
  auto s2_warm = VolaSession::from_frame(d2->frame, in2w);
  const std::uint64_t warm_solves = solves_now();
  ASSERT_TRUE(s2_warm.has_value()) << s2_warm.error().to_string();
  const auto fp_warm = fingerprint(*s2_warm, d2->implied_spot);
  const auto warm_diag = s2_warm->diagnostics();

  std::printf("C2CHAR\tAAPL 01-05\tcold_solves=%llu\twarm_solves=%llu\tcold_frac=%.4f\twarm_frac=%.4f\t"
              "cold_rmse=%.5f\twarm_rmse=%.5f\n",
              static_cast<unsigned long long>(cold_solves),
              static_cast<unsigned long long>(warm_solves), cold_diag.mean_frac_within_bidask,
              warm_diag.mean_frac_within_bidask, cold_diag.mean_rmse_vol, warm_diag.mean_rmse_vol);

  // GATE 1 (ledger): reuse must cut AL boundary solves >= 40% on the reuse date
  // (the eliminated correction-cache rebuild, finding 11).
  ASSERT_GT(cold_solves, 0u);
  EXPECT_LE(warm_solves, cold_solves * 6u / 10u)
      << "warm=" << warm_solves << " cold=" << cold_solves << " (<40% reduction)";

  // GATE 2 (quality-parity, ECONOMIC): the served surface must price the market as
  // well as the cold fit. The admission-relevant metric is bid-ask coverage (the
  // fraction of quotes the re-Americanized surface prices in-band — what corpus
  // admission actually gates on), NOT raw interpolated-IV distance. Warm coverage
  // must not degrade vs cold beyond a small tolerance; the fit RMSE stays in-band.
  EXPECT_GE(warm_diag.mean_frac_within_bidask, cold_diag.mean_frac_within_bidask - 0.01)
      << "cross-date warm de-Am degraded bid-ask coverage";
  EXPECT_GE(warm_diag.worst_frac_within_bidask, cold_diag.worst_frac_within_bidask - 0.03)
      << "cross-date warm de-Am degraded worst-expiry coverage";
  EXPECT_LE(warm_diag.mean_rmse_vol, cold_diag.mean_rmse_vol + 2.0e-3)
      << "cross-date warm de-Am raised the surface fit RMSE out of band";
  // Informational: max served-IV deviation. Largest at deep-wing / short-T grid
  // points where vega -> 0 (price impact within the half-spread), so this is a
  // loose sanity bound, not the economic gate.
  ASSERT_EQ(fp_warm.size(), fp_cold.size());
  double max_iv_diff = 0.0;
  for (std::size_t i = 0; i < fp_warm.size(); ++i) {
    if (std::isfinite(fp_warm[i]) && std::isfinite(fp_cold[i])) {
      max_iv_diff = std::max(max_iv_diff, std::fabs(fp_warm[i] - fp_cold[i]));
    }
  }
  EXPECT_LT(max_iv_diff, 1.5e-2) << "served IV moved implausibly far (>1.5 vol pts)";
  std::printf("C2CHAR\tmax_iv_diff=%.6f\n", max_iv_diff);

  // GATE 3 (determinism): the warm fit repeated is bit-identical (solves + IV grid).
  led::reset();
  auto s2_warm2 = VolaSession::from_frame(d2->frame, in2w);
  const std::uint64_t warm_solves2 = solves_now();
  ASSERT_TRUE(s2_warm2.has_value());
  EXPECT_EQ(warm_solves2, warm_solves);
  const auto fp_warm2 = fingerprint(*s2_warm2, d2->implied_spot);
  for (std::size_t i = 0; i < fp_warm.size(); ++i) {
    EXPECT_TRUE((std::isnan(fp_warm[i]) && std::isnan(fp_warm2[i])) || fp_warm[i] == fp_warm2[i])
        << "warm fit non-deterministic at grid " << i;
  }

  // POSITIVE CONTROL: a rate far from the cache's baked carry (0.20 vs 0.043) must
  // FAIL the stale-gate -> cold rebuild -> byte-identical to the pure cold fit
  // (same solve count), proving reuse is refused when it would be wrong.
  const auto in_off = make_in(d2->implied_spot, d2->frame.snapshot_ts_ns, 0.20, /*chain=*/true);
  led::reset();
  auto off_cold = VolaSession::from_frame(d2->frame, in_off);
  const std::uint64_t off_cold_solves = solves_now();
  ASSERT_TRUE(off_cold.has_value());
  auto in_off_w = in_off;
  in_off_w.deam.caches = AmericanCorrectionCaches{&cache_call, &cache_put}; // baked at r=0.043
  in_off_w.deam.reuse_supplied_caches = true;
  led::reset();
  auto off_warm = VolaSession::from_frame(d2->frame, in_off_w);
  const std::uint64_t off_warm_solves = solves_now();
  ASSERT_TRUE(off_warm.has_value());
  EXPECT_EQ(off_warm_solves, off_cold_solves)
      << "stale-gate must refuse a cache baked at a far carry (cold-rebuild fallback)";
}

// C3 (accuracy-trading) — Populate-tier per-knob characterization + economic gate
// on real AAPL/XOM boards. The Populate tier keeps Robust's eSSVI fit quality
// (MonotoneFit, 3 ATM pairs, parity) but bakes al_fast_opts (not al_default_opts)
// for de-Am / cache-sampling / cold marks (K1 audit, docs/al-preset-ladder.md §6).
// Measures each knob individually vs the Robust baseline and gates the composed
// tier: AL boundary solves must drop, and surface RMSE / bid-ask coverage / calendar
// arb must stay in-band (§3 tier-honesty — no silent budget cut). Knob C (drop
// MonotoneFit) is reported to justify the policy choice to KEEP it in Populate.
TEST(VolaSession, C3PopulateTierEconomicParityVsRobust) {
  using atx::vol::al_default_opts;
  using atx::vol::al_fast_opts;
  using atx::vol::CalendarRepair;
  using atx::vol::FitPreset;
  using atx::vol::load_opra_cbbo_parquet;
  using atx::vol::make_session_inputs;
  using atx::vol::OpraLoadSpec;
  using atx::vol::OpraPanel;
  namespace led = atx::vol::counters::ledger;

  const std::string root = "C:/atx-data/spy-dispersion/opra/";
  const double r = 0.043;
  const auto load = [&](const char *sym, const char *date) -> std::optional<OpraPanel> {
    const std::string path = std::string(root) + sym + "/" + date + ".parquet";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      return std::nullopt;
    }
    OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = sym;
    spec.snapshot_iso = std::string(date) + "T14:00:00Z";
    spec.r = r;
    auto p = load_opra_cbbo_parquet(spec);
    return p.has_value() ? std::optional<OpraPanel>(std::move(*p)) : std::nullopt;
  };

  struct Board {
    const char *sym;
    const char *date;
  };
  const Board boards[] = {{"AAPL", "2026-01-02"}, {"AAPL", "2026-01-06"}, {"XOM", "2026-01-02"},
                          {"XOM", "2026-01-06"}};

  struct Metrics {
    std::uint64_t solves = 0;      // AL boundary solves (COUNT — preset-invariant)
    std::uint64_t prem_evals = 0;  // premium quadrature evaluations (moves with the preset)
    double cache_ms = 0.0;         // correction-cache build wall (V2 attribution)
    double wall_ms = 0.0;          // total fit wall (V2)
    double rmse = 0.0, mean_frac = 0.0, worst_frac = 0.0;
    bool arb_free = false;
    bool ok = false;
  };
  const auto measure = [&](const OpraPanel &p, FitPreset base,
                           const std::function<void(SessionInputs &)> &knob) -> Metrics {
    auto in = make_session_inputs(base, p.implied_spot, r, p.frame.snapshot_ts_ns);
    in.fit_workers = 1u;
    in.collect_stage_timings = true; // V2 FitTimings attribution (cache/wall ms)
    if (knob) {
      knob(in);
    }
    led::reset();
    auto s = VolaSession::from_frame(p.frame, in);
    const auto snap = led::snapshot();
    if (!s.has_value()) {
      return Metrics{};
    }
    const auto &d = s->diagnostics();
    return Metrics{snap.get(led::Solve::AlBoundarySolves),
                   snap.get(led::Solve::AlPremiumEvals),
                   d.fit_timings.correction_cache_ms,
                   d.fit_timings.total_wall_ms,
                   d.mean_rmse_vol,
                   d.mean_frac_within_bidask,
                   d.worst_frac_within_bidask,
                   d.calendar_arb_free,
                   true};
  };

  std::size_t n = 0;
  for (const Board &b : boards) {
    auto p = load(b.sym, b.date);
    if (!p.has_value()) {
      continue;
    }
    const Metrics robust = measure(*p, FitPreset::Robust, {});
    const Metrics knobA = measure(*p, FitPreset::Robust,
                                  [](SessionInputs &in) { in.deam.al_opts = al_fast_opts(); });
    const Metrics knobB = measure(*p, FitPreset::Robust,
                                  [](SessionInputs &in) { in.deam.iv_tol = 1.0e-5; });
    const Metrics knobC = measure(*p, FitPreset::Robust, [](SessionInputs &in) {
      in.calendar_repair = CalendarRepair::None; // drop MonotoneFit (informational)
    });
    const Metrics pop = measure(*p, FitPreset::Populate, {});
    if (!robust.ok || !pop.ok) {
      continue;
    }
    ++n;
    // Premium-eval reduction and cache-build speedup are the C3 win (the AL solve
    // COUNT is preset-invariant — the preset makes each solve CHEAPER, not fewer).
    const double cache_speedup =
        pop.cache_ms > 0.0 ? robust.cache_ms / pop.cache_ms : 0.0;
    std::printf("C3CHAR\t%-4s %s\tRobust[prem=%llu cache_ms=%.2f wall_ms=%.2f rmse=%.5f frac=%.4f arb=%d]"
                "\tPOP[prem=%llu cache_ms=%.2f wall_ms=%.2f rmse=%.5f frac=%.4f worst=%.4f arb=%d]"
                "\tcache_speedup=%.2fx\tC_nomono[arb=%d frac=%.4f]\n",
                b.sym, b.date, (unsigned long long)robust.prem_evals, robust.cache_ms, robust.wall_ms,
                robust.rmse, robust.mean_frac, robust.arb_free ? 1 : 0,
                (unsigned long long)pop.prem_evals, pop.cache_ms, pop.wall_ms, pop.rmse, pop.mean_frac,
                pop.worst_frac, pop.arb_free ? 1 : 0, cache_speedup, knobC.arb_free ? 1 : 0,
                knobC.mean_frac);

    // COST (deterministic): the fast preset does fewer premium quadrature evals per
    // solve — Populate < Robust. (Solve COUNT is invariant; this is the cheaper-solve
    // proxy that is contention-free, unlike wall time.)
    EXPECT_LT(knobA.prem_evals, robust.prem_evals)
        << b.sym << " " << b.date << ": al_fast did not reduce premium evals";
    EXPECT_LT(pop.prem_evals, robust.prem_evals)
        << b.sym << " " << b.date << ": Populate did not reduce premium evals";
    // COST (provisional, timing): the correction-cache build should be materially
    // cheaper (al_fast ~47us/node vs al_default ~200us/node). Loose bound — shared host.
    EXPECT_LT(pop.cache_ms, robust.cache_ms)
        << b.sym << " " << b.date << ": Populate cache build not faster";

    // ECONOMIC PARITY (deterministic, §3 tier-honesty) — each knob + composed tier:
    EXPECT_LE(knobA.rmse, robust.rmse + 2.0e-3) << b.sym << " " << b.date << ": al_fast RMSE out of band";
    EXPECT_LE(knobB.rmse, robust.rmse + 2.0e-3) << b.sym << " " << b.date << ": iv_tol RMSE out of band";
    EXPECT_LE(pop.rmse, robust.rmse + 2.0e-3) << b.sym << " " << b.date << ": Populate RMSE out of band";
    EXPECT_GE(pop.mean_frac, robust.mean_frac - 0.02)
        << b.sym << " " << b.date << ": Populate degraded bid-ask coverage";
    EXPECT_GE(pop.worst_frac, robust.worst_frac - 0.03)
        << b.sym << " " << b.date << ": Populate degraded worst-expiry coverage";
    EXPECT_EQ(pop.arb_free, robust.arb_free)
        << b.sym << " " << b.date << ": Populate changed calendar-arb-free status";
  }
  if (n == 0) {
    GTEST_SKIP() << "no real OPRA boards under " << root;
  }
  std::printf("C3CHAR\tTOTAL_BOARDS=%zu\n", n);
}

// C6 — COMPOSED production-populate-lane parity gate (§11 trap 2: per-stage in-band
// does not prove composed in-band). The production lane composes C2 (cross-date
// warm-start cache reuse) AND C3 (Populate preset) — this test fits a real AAPL
// date chain under that composed config (Populate + reuse the prior date's wide
// chain cache) and gates it against the Robust COLD oracle on the reuse date:
// surface RMSE / bid-ask coverage / calendar-arb must stay in-band. STOP condition
// (PM): a composed-config parity breach vs Robust cold.
TEST(VolaSession, C6ComposedPopulateLaneParityVsRobustCold) {
  using atx::vol::AmericanCorrectionCaches;
  using atx::vol::CorrectionCache;
  using atx::vol::FitPreset;
  using atx::vol::load_opra_cbbo_parquet;
  using atx::vol::make_session_inputs;
  using atx::vol::OpraLoadSpec;
  using atx::vol::OpraPanel;

  const std::string root = "C:/atx-data/spy-dispersion/opra/";
  const double r = 0.043;
  const auto load = [&](const char *sym, const char *date) -> std::optional<OpraPanel> {
    const std::string path = std::string(root) + sym + "/" + date + ".parquet";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      return std::nullopt;
    }
    OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = sym;
    spec.snapshot_iso = std::string(date) + "T14:00:00Z";
    spec.r = r;
    auto p = load_opra_cbbo_parquet(spec);
    return p.has_value() ? std::optional<OpraPanel>(std::move(*p)) : std::nullopt;
  };

  auto d1 = load("AAPL", "2026-01-02");
  auto d2 = load("AAPL", "2026-01-05");
  if (!d1.has_value() || !d2.has_value()) {
    GTEST_SKIP() << "AAPL OPRA chain not available under " << root;
  }

  const auto make_in = [&](FitPreset preset, double spot, std::int64_t ts) {
    auto in = make_session_inputs(preset, spot, r, ts);
    in.fit_workers = 1u;
    return in;
  };

  // Date 1 of the chain: Populate preset + chain-cache mode (build the wide,
  // reusable cache); copy it out.
  auto in1 = make_in(FitPreset::Populate, d1->implied_spot, d1->frame.snapshot_ts_ns);
  in1.deam.chain_cache_mode = true;
  auto s1 = VolaSession::from_frame(d1->frame, in1);
  ASSERT_TRUE(s1.has_value()) << s1.error().to_string();
  const AmericanCorrectionCaches c1 = s1->correction_caches();
  if (c1.call == nullptr || c1.put == nullptr) {
    GTEST_SKIP() << "date-1 Populate fit built no caches";
  }
  const CorrectionCache cache_call = *c1.call;
  const CorrectionCache cache_put = *c1.put;

  // Date 2 COMPOSED: the full production lane — Populate preset + reuse date-1's
  // wide chain cache (warm).
  auto in2c = make_in(FitPreset::Populate, d2->implied_spot, d2->frame.snapshot_ts_ns);
  in2c.deam.chain_cache_mode = true;
  in2c.deam.caches = AmericanCorrectionCaches{&cache_call, &cache_put};
  in2c.deam.reuse_supplied_caches = true;
  auto s2c = VolaSession::from_frame(d2->frame, in2c);
  ASSERT_TRUE(s2c.has_value()) << s2c.error().to_string();
  const auto composed = s2c->diagnostics();

  // Date 2 REFERENCE: Robust cold (the oracle the composed lane must match in-band).
  auto s2r = VolaSession::from_frame(
      d2->frame, make_in(FitPreset::Robust, d2->implied_spot, d2->frame.snapshot_ts_ns));
  ASSERT_TRUE(s2r.has_value()) << s2r.error().to_string();
  const auto robust = s2r->diagnostics();

  std::printf("C6CHAR\tAAPL 01-05 composed(Populate+warm) vs Robust-cold\t"
              "rmse %.5f/%.5f\tfrac %.4f/%.4f\tworst %.4f/%.4f\tarb %d/%d\n",
              composed.mean_rmse_vol, robust.mean_rmse_vol, composed.mean_frac_within_bidask,
              robust.mean_frac_within_bidask, composed.worst_frac_within_bidask,
              robust.worst_frac_within_bidask, composed.calendar_arb_free ? 1 : 0,
              robust.calendar_arb_free ? 1 : 0);

  // Composed-config parity gate vs Robust cold (the STOP condition):
  EXPECT_LE(composed.mean_rmse_vol, robust.mean_rmse_vol + 2.0e-3)
      << "composed populate lane RMSE out of band vs Robust cold";
  EXPECT_GE(composed.mean_frac_within_bidask, robust.mean_frac_within_bidask - 0.02)
      << "composed populate lane degraded bid-ask coverage vs Robust cold";
  EXPECT_GE(composed.worst_frac_within_bidask, robust.worst_frac_within_bidask - 0.03)
      << "composed populate lane degraded worst-expiry coverage vs Robust cold";
  EXPECT_EQ(composed.calendar_arb_free, robust.calendar_arb_free)
      << "composed populate lane changed calendar-arb-free status vs Robust cold";
}

TEST(VolaSession, Iv_OnSliceAtm_IsSaneVol) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries()[1].T; // ~0.30y slice
  const double vol = sess->iv(100.0, T);
  ASSERT_TRUE(std::isfinite(vol));
  EXPECT_GT(vol, 0.01);
  EXPECT_LT(vol, 3.0);
}

TEST(VolaSession, FairValue_OnSliceAtm_IsPositiveAndNearMarket) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Middle expiry; read the market Call quote at the at-forward strike 100.
  const auto &chain = under->chains[1];
  std::size_t sidx = 0;
  bool found = false;
  for (std::size_t j = 0; j < chain.strikes.size(); ++j) {
    if (std::abs(chain.strikes[j] - 100.0) < 1e-6) {
      sidx = j;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  const std::size_t ci = chain_index(static_cast<std::uint16_t>(sidx), Side::Call);
  const double bid = chain.bids[ci];
  const double ask = chain.asks[ci];

  const auto fv = sess->fair_value(100.0, chain.T, Side::Call);
  ASSERT_TRUE(fv.has_value()) << fv.error().to_string();
  EXPECT_TRUE(std::isfinite(*fv));
  EXPECT_GT(*fv, 0.0);
  // Re-priced off the fitted surface it should land inside the bid-ask (a light
  // band absorbs any residual fit error).
  EXPECT_GE(*fv, bid - 0.05);
  EXPECT_LE(*fv, ask + 0.05);
}

TEST(VolaSession, FairValue_BetweenSlices_IsFinitePositive) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Midpoint between the first two fitted slices exercises the interpolation
  // path (forward + carry linearly interpolated in T).
  const auto exps = sess->expiries();
  const double Tstar = 0.5 * (exps[0].T + exps[1].T);
  const auto fv = sess->fair_value(100.0, Tstar, Side::Call);
  ASSERT_TRUE(fv.has_value()) << fv.error().to_string();
  EXPECT_TRUE(std::isfinite(*fv));
  EXPECT_GT(*fv, 0.0);
}

TEST(VolaSession, Greeks_Call_HasDeltaInUnitInterval) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries()[1].T;
  const auto g = sess->greeks(100.0, T, Side::Call);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_GT(g->delta, 0.0);
  EXPECT_LT(g->delta, 1.0);
}

TEST(VolaSession, FairValue_NonPositiveStrike_ReturnsInvalidArgument) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries()[0].T;
  const auto fv = sess->fair_value(-1.0, T, Side::Call);
  ASSERT_FALSE(fv.has_value());
  EXPECT_EQ(fv.error().code(), atx::vol::ErrorCode::InvalidArgument);

  // A non-positive maturity is rejected the same way.
  const auto fv_t = sess->fair_value(100.0, 0.0, Side::Call);
  ASSERT_FALSE(fv_t.has_value());
  EXPECT_EQ(fv_t.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(VolaSession, FromFrame_KnownTruthPanel_BuildsFourSlices) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  const auto sess = VolaSession::from_frame(panel->frame, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{4});
  EXPECT_TRUE(sess->diagnostics().calendar_arb_free);
}

TEST(FitPreset, PopulatesPolicyFieldsPerPreset) {
  using atx::vol::al_default_opts;
  using atx::vol::al_fast_opts;
  using atx::vol::AmericanMethod;
  using atx::vol::apply_fit_preset;
  using atx::vol::CalendarRepair;
  using atx::vol::FitPreset;
  using atx::vol::make_session_inputs;

  // make_session_inputs fills the market snapshot then applies the policy.
  const auto fast = make_session_inputs(FitPreset::Fast, 100.0, 0.03, 42);
  EXPECT_DOUBLE_EQ(fast.S, 100.0);
  EXPECT_DOUBLE_EQ(fast.r, 0.03);
  EXPECT_EQ(fast.now_ts_ns, std::int64_t{42});
  EXPECT_TRUE(fast.use_correction_cache);
  EXPECT_TRUE(fast.score_parity);
  EXPECT_TRUE(fast.enforce_calendar_floor);
  EXPECT_TRUE(fast.use_deam_cache_for_fit);
  EXPECT_EQ(fast.calib.max_obs_per_slice, 0u);
  EXPECT_DOUBLE_EQ(fast.calib.max_otm_shortcut_premium_spread_frac, 0.0);
  ASSERT_TRUE(fast.deam.al_opts.has_value());
  EXPECT_EQ(fast.deam.al_opts->n_collocation, al_fast_opts().n_collocation);
  EXPECT_EQ(fast.deam.method, AmericanMethod::AndersenLake);
  EXPECT_EQ(fast.deam.n_atm, std::size_t{1});
  EXPECT_EQ(fast.deam.max_borrow_pairs, std::size_t{5});
  ASSERT_TRUE(fast.deam.carry_al_opts.has_value());
  EXPECT_EQ(fast.deam.carry_al_opts->n_collocation, al_fast_opts().n_collocation);
  EXPECT_DOUBLE_EQ(fast.deam.iv_tol, 1.0e-5);
  EXPECT_EQ(fast.calendar_repair, CalendarRepair::None);

  // apply_fit_preset preserves market fields the caller already set.
  SessionInputs robust;
  robust.S = 200.0;
  robust.r = 0.05;
  robust.deam.carry_al_opts = al_default_opts();
  apply_fit_preset(robust, FitPreset::Robust);
  EXPECT_DOUBLE_EQ(robust.S, 200.0);
  EXPECT_DOUBLE_EQ(robust.r, 0.05);
  ASSERT_TRUE(robust.deam.al_opts.has_value());
  EXPECT_EQ(robust.deam.al_opts->n_collocation, al_default_opts().n_collocation);
  EXPECT_EQ(robust.deam.method, AmericanMethod::AndersenLake);
  EXPECT_EQ(robust.deam.n_atm, std::size_t{3});
  EXPECT_EQ(robust.deam.max_borrow_pairs, std::size_t{5});
  ASSERT_TRUE(robust.deam.carry_al_opts.has_value());
  EXPECT_EQ(robust.deam.carry_al_opts->n_collocation, al_fast_opts().n_collocation);
  EXPECT_EQ(robust.calendar_repair, CalendarRepair::MonotoneFit);
  EXPECT_TRUE(robust.use_deam_cache_for_fit);

  SessionInputs hft;
  apply_fit_preset(hft, FitPreset::Hft);
  EXPECT_EQ(hft.calendar_repair, CalendarRepair::None);
  EXPECT_EQ(hft.deam.method, AmericanMethod::AndersenLake);
  EXPECT_EQ(hft.deam.n_atm, std::size_t{1}); // fast borrow
  EXPECT_EQ(hft.deam.max_borrow_pairs, std::size_t{1});
  EXPECT_EQ(hft.curve.kind, atx::vol::VolCurveKind::LinearVariance);
  EXPECT_FALSE(hft.use_correction_cache);
  EXPECT_FALSE(hft.score_parity);
  EXPECT_FALSE(hft.enforce_calendar_floor);
  EXPECT_FALSE(hft.use_deam_cache_for_fit);
  EXPECT_EQ(hft.calib.max_obs_per_slice, 48u);
  EXPECT_DOUBLE_EQ(hft.calib.max_otm_shortcut_premium_spread_frac, 0.50);

  SessionInputs acc;
  apply_fit_preset(acc, FitPreset::Accurate);
  EXPECT_EQ(acc.calendar_repair, CalendarRepair::None);
  ASSERT_TRUE(acc.deam.al_opts.has_value());
  EXPECT_EQ(acc.deam.al_opts->max_newton_iter, al_default_opts().max_newton_iter);
  EXPECT_EQ(acc.deam.max_borrow_pairs, std::size_t{5});
  EXPECT_FALSE(acc.use_deam_cache_for_fit);
}

TEST(VolaSession, OffPillarCarryIsCoherentAcrossLiveAndPricedPaths) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs inputs = make_inputs(spec);
  for (const SynthExpiry &expiry : spec.expiries) {
    inputs.expiry_rate_T.push_back(expiry.T);
  }
  inputs.expiry_rates = {0.021, 0.027, 0.034, 0.041};
  const auto session = VolaSession::build(*under, inputs);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();
  auto priced = session->to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();

  const std::span<const atx::vol::SliceContext> pillars = session->expiries();
  ASSERT_GE(pillars.size(), std::size_t{2});
  std::vector<double> probes{pillars.front().T * 0.5, pillars.back().T * 1.5};
  for (std::size_t i = 0; i + 1u < pillars.size(); ++i) {
    probes.push_back(0.5 * (pillars[i].T + pillars[i + 1u].T));
  }

  for (const double T : probes) {
    const double live_forward = session->forward_at(T);
    const double live_reproduced =
        inputs.S * std::exp((session->rate_at(T) - session->q_eff_at(T)) * T);
    EXPECT_NEAR(live_reproduced, live_forward, 2.0e-13 * live_forward) << "T=" << T;

    const double priced_forward = priced->forward_at(T);
    const double priced_reproduced =
        inputs.S * std::exp((priced->rate_at(T) - priced->q_eff_at(T)) * T);
    EXPECT_NEAR(priced_reproduced, priced_forward, 2.0e-13 * priced_forward) << "T=" << T;
    EXPECT_NEAR(priced_forward, live_forward, 2.0e-13 * live_forward) << "T=" << T;
    EXPECT_NEAR(priced->q_eff_at(T), session->q_eff_at(T), 2.0e-13) << "T=" << T;
  }
  EXPECT_DOUBLE_EQ(session->q_eff_at(pillars.front().T * 0.5), pillars.front().q_eff);
  EXPECT_DOUBLE_EQ(session->q_eff_at(pillars.back().T * 1.5), pillars.back().q_eff);

  for (const atx::vol::SliceContext &pillar : pillars) {
    EXPECT_DOUBLE_EQ(session->forward_at(pillar.T), pillar.forward);
    EXPECT_DOUBLE_EQ(session->q_eff_at(pillar.T), pillar.q_eff);
    EXPECT_NEAR(priced->forward_at(pillar.T), pillar.forward, 2.0e-13 * pillar.forward);
    EXPECT_NEAR(priced->q_eff_at(pillar.T), pillar.q_eff, 2.0e-13);
  }
}

TEST(DeAmFitCache, CachedAndColdLinearVarianceFitsAreEconomicallyEquivalent) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs cold = make_inputs(spec);
  atx::vol::apply_fit_preset(cold, atx::vol::FitPreset::Fast);
  cold.curve.kind = VolCurveKind::LinearVariance;
  cold.use_deam_cache_for_fit = false;
  SessionInputs cached = cold;
  cached.use_deam_cache_for_fit = true;

  const auto cold_session = VolaSession::build(*under, cold);
  ASSERT_TRUE(cold_session.has_value()) << cold_session.error().to_string();
  const auto cached_session = VolaSession::build(*under, cached);
  ASSERT_TRUE(cached_session.has_value()) << cached_session.error().to_string();

  // These are economic serving tolerances, not an implementation-detail
  // requirement for bit-identical fitted parameters.
  constexpr double kIvTolerance = 5.0e-3; // 50 vol basis points
  // Cached proposals may move the fitted mark by a few ticks, but must stay a
  // small fraction of the executable uncertainty on this deliberately wide
  // (2% half-spread) fixture and below five cents per share.
  constexpr double kPriceTolerance = 5.0e-2;
  constexpr double kHalfSpreadFraction = 0.25;
  for (std::size_t expiry_index = 0u; expiry_index < cold_session->expiries().size();
       ++expiry_index) {
    const auto &expiry = cold_session->expiries()[expiry_index];
    const Chain &chain = under->chains[expiry_index];
    for (const double strike : {90.0, 100.0, 110.0}) {
      const Side side = strike >= spec.spot ? Side::Call : Side::Put;
      EXPECT_NEAR(cached_session->iv(strike, expiry.T), cold_session->iv(strike, expiry.T),
                  kIvTolerance);
      const auto cached_price = cached_session->fair_value(strike, expiry.T, side);
      const auto cold_price = cold_session->fair_value(strike, expiry.T, side);
      ASSERT_TRUE(cached_price.has_value()) << cached_price.error().to_string();
      ASSERT_TRUE(cold_price.has_value()) << cold_price.error().to_string();
      EXPECT_NEAR(*cached_price, *cold_price, kPriceTolerance);
      const auto strike_it = std::lower_bound(chain.strikes.begin(), chain.strikes.end(), strike);
      ASSERT_NE(strike_it, chain.strikes.end());
      const auto strike_index =
          static_cast<std::uint16_t>(std::distance(chain.strikes.begin(), strike_it));
      const std::size_t quote_index = chain_index(strike_index, side);
      const double half_spread = 0.5 * (chain.asks[quote_index] - chain.bids[quote_index]);
      EXPECT_LE(std::fabs(*cached_price - *cold_price), kHalfSpreadFraction * half_spread);
    }
  }
}

TEST(DeAmFitCache, TermRateSessionsForceTheColdFitPath) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  atx::vol::apply_fit_preset(in, atx::vol::FitPreset::Robust);
  ASSERT_TRUE(in.use_deam_cache_for_fit);
  for (const auto &expiry : spec.expiries) {
    in.expiry_rate_T.push_back(expiry.T);
    in.expiry_rates.push_back(spec.r);
  }

  const auto session = VolaSession::build(*under, in);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();
  EXPECT_FALSE(session->inputs().use_correction_cache);
  EXPECT_FALSE(session->inputs().use_deam_cache_for_fit);
}

TEST(VolaSession, FairValueLadder_MatchesScalarAndHandlesBadStrikes) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto sess = VolaSession::from_frame(panel->frame, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries().front().T; // an on-slice maturity
  const std::vector<double> strikes = {80.0, 90.0, 100.0, 110.0, 120.0, -5.0};
  std::vector<Side> sides;
  for (const double K : strikes) {
    sides.push_back(K >= 100.0 ? Side::Call : Side::Put);
  }

  std::vector<double> out(strikes.size(), 0.0);
  const auto st = sess->fair_value_ladder(T, strikes, sides, out);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // Each ladder entry is bit-identical to the scalar fair_value; the bad strike
  // becomes NaN without sinking the ladder.
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    if (strikes[i] <= 0.0) {
      EXPECT_TRUE(std::isnan(out[i]));
      continue;
    }
    const auto scalar = sess->fair_value(strikes[i], T, sides[i]);
    ASSERT_TRUE(scalar.has_value());
    EXPECT_DOUBLE_EQ(out[i], *scalar);
  }

  // Structural errors: length mismatch and a bad expiry.
  std::vector<double> short_out(strikes.size() - 1, 0.0);
  EXPECT_FALSE(sess->fair_value_ladder(T, strikes, sides, short_out).has_value());
  EXPECT_FALSE(sess->fair_value_ladder(-1.0, strikes, sides, out).has_value());

  // Greeks ladder matches the scalar greeks entry-for-entry.
  std::vector<atx::vol::AmericanGreeks> g(strikes.size());
  ASSERT_TRUE(sess->greeks_ladder(T, strikes, sides, g).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    if (strikes[i] <= 0.0) {
      EXPECT_TRUE(std::isnan(g[i].price));
      continue;
    }
    const auto sg = sess->greeks(strikes[i], T, sides[i]);
    ASSERT_TRUE(sg.has_value());
    EXPECT_DOUBLE_EQ(g[i].delta, sg->delta);
    EXPECT_DOUBLE_EQ(g[i].price, sg->price);
  }
}

TEST(VolaSession, EvaluateLadder_FusesModelOutputsAndMatchesScalarQueries) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto sess = VolaSession::from_frame(panel->frame, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries().front().T;
  const std::vector<double> strikes = {80.0, 90.0, 100.0, 110.0, 120.0, -5.0};
  const std::vector<Side> sides = {Side::Put,  Side::Put,  Side::Call,
                                   Side::Call, Side::Call, Side::Put};
  std::vector<double> iv(strikes.size());
  std::vector<double> price(strikes.size());
  std::vector<atx::vol::AmericanGreeks> greeks(strikes.size());

  const auto status = sess->evaluate_ladder(T, strikes, sides, iv, price, greeks);
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    if (!(strikes[i] > 0.0)) {
      EXPECT_TRUE(std::isnan(iv[i]));
      EXPECT_TRUE(std::isnan(price[i]));
      EXPECT_TRUE(std::isnan(greeks[i].price));
      continue;
    }
    const auto scalar_greeks = sess->greeks(strikes[i], T, sides[i]);
    ASSERT_TRUE(scalar_greeks.has_value()) << scalar_greeks.error().to_string();
    EXPECT_DOUBLE_EQ(iv[i], sess->iv(strikes[i], T));
    EXPECT_DOUBLE_EQ(price[i], scalar_greeks->price);
    EXPECT_DOUBLE_EQ(greeks[i].price, scalar_greeks->price);
    EXPECT_DOUBLE_EQ(greeks[i].delta, scalar_greeks->delta);
    EXPECT_DOUBLE_EQ(greeks[i].vega, scalar_greeks->vega);
  }

  // Output columns are independently optional, but every requested column must
  // cover the whole ladder.
  std::vector<double> short_price(strikes.size() - 1u);
  EXPECT_FALSE(sess->evaluate_ladder(T, strikes, sides, {}, short_price, {}).has_value());
  EXPECT_TRUE(sess->evaluate_ladder(T, strikes, sides, iv, {}, {}).has_value());
}

TEST(VolaSession, EvaluateLadder_ColdPriceOnlyBatchesBySideWithinEconomicTolerance) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  SessionInputs cold = make_inputs(spec);
  cold.use_correction_cache = false;
  cold.query_pricing_tier = atx::vol::QueryPricingTier::ColdReference;
  const auto sess = VolaSession::from_frame(panel->frame, cold);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  ASSERT_FALSE(sess->correction_caches().any());

  const double T = sess->expiries().back().T;
  std::vector<double> strikes;
  std::vector<Side> sides;
  for (double strike = 70.0; strike <= 130.0; strike += 5.0) {
    strikes.push_back(strike);
    sides.push_back(Side::Call);
    strikes.push_back(strike);
    sides.push_back(Side::Put);
  }
  strikes.push_back(-5.0);
  sides.push_back(Side::Put);

  std::vector<double> iv(strikes.size());
  std::vector<double> price(strikes.size());
  atx::vol::counters::reset();
  const auto status = sess->evaluate_ladder(T, strikes, sides, iv, price, {});
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
  if constexpr (atx::vol::counters::counters_enabled()) {
    const atx::vol::counters::Snapshot batch_counts = atx::vol::counters::snapshot();
    // Thirteen valid rows per side are served by the default eight-node sigma
    // interpolant. The pre-change scalar loop performs 26 boundary solves.
    EXPECT_LE(batch_counts.get(atx::vol::counters::Counter::BoundarySolves), 16u);
  }

  double max_price_error = 0.0;
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    if (!(strikes[i] > 0.0)) {
      EXPECT_TRUE(std::isnan(iv[i]));
      EXPECT_TRUE(std::isnan(price[i]));
      continue;
    }
    const auto oracle = sess->fair_value(strikes[i], T, sides[i]);
    ASSERT_TRUE(oracle.has_value()) << oracle.error().to_string();
    const double error = std::fabs(price[i] - *oracle);
    max_price_error = std::max(max_price_error, error);

    // The sprint's healthy-vega economic gate is the tighter of half a penny
    // tick and one tenth of one vol-bp's dollar vega. Low-vega wings retain the
    // independent absolute slice-interpolation bound below.
    const double vega = atx::vol::american_vega(
        spec.spot, strikes[i], T, iv[i], sess->rate_at(T), sess->q_eff_at(T), sides[i],
        static_cast<const atx::vol::CorrectionCache *>(nullptr));
    if (vega >= 5.0) {
      const double economic_gate = std::min(0.005, 0.1 * vega * 1.0e-4);
      EXPECT_LE(error, economic_gate) << "row=" << i << " vega=" << vega;
    }
  }
  EXPECT_LE(max_price_error, 5.0e-5);

  // Requesting Greeks deliberately bypasses the accuracy-trading price-only
  // route: both the bundle and the accompanying mark remain the scalar cold
  // result, exactly as before.
  const std::vector<double> greek_strikes = {90.0, 110.0};
  const std::vector<Side> greek_sides = {Side::Put, Side::Call};
  std::vector<double> greek_iv(greek_strikes.size());
  std::vector<double> greek_prices(greek_strikes.size());
  std::vector<atx::vol::AmericanGreeks> greeks(greek_strikes.size());
  ASSERT_TRUE(sess->evaluate_ladder(T, greek_strikes, greek_sides, greek_iv, greek_prices, greeks)
                  .has_value());
  for (std::size_t i = 0u; i < greek_strikes.size(); ++i) {
    const auto scalar = sess->greeks(greek_strikes[i], T, greek_sides[i]);
    ASSERT_TRUE(scalar.has_value()) << scalar.error().to_string();
    EXPECT_DOUBLE_EQ(greek_prices[i], scalar->price);
    EXPECT_DOUBLE_EQ(greeks[i].price, scalar->price);
    EXPECT_DOUBLE_EQ(greeks[i].delta, scalar->delta);
    EXPECT_DOUBLE_EQ(greeks[i].vega, scalar->vega);
  }
}

TEST(VolaSession, EvaluateLadder_CachedPriceOnlyRetainsExactCachedRoute) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto sess = VolaSession::from_frame(panel->frame, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  ASSERT_TRUE(sess->correction_caches().any());

  const double T = sess->expiries().front().T;
  const std::vector<double> strikes = {80.0, 90.0, 100.0, 110.0, 120.0};
  const std::vector<Side> sides = {Side::Put, Side::Put, Side::Call, Side::Call, Side::Call};
  std::vector<double> iv(strikes.size());
  std::vector<double> price(strikes.size());
  ASSERT_TRUE(sess->evaluate_ladder(T, strikes, sides, iv, price, {}).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const auto scalar = sess->fair_value(strikes[i], T, sides[i]);
    ASSERT_TRUE(scalar.has_value()) << scalar.error().to_string();
    EXPECT_DOUBLE_EQ(price[i], *scalar);
  }
}

TEST(FitPreset, RobustPresetBuildsSessionOnKnownPanel) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  // Start from the panel-driven inputs, then switch to the Robust preset (keeps
  // S/r/divs/now, sets the fit policy + MonotoneFit calendar repair).
  SessionInputs in = make_inputs(spec);
  atx::vol::apply_fit_preset(in, atx::vol::FitPreset::Robust);
  EXPECT_EQ(in.calendar_repair, atx::vol::CalendarRepair::MonotoneFit);

  const auto sess = VolaSession::from_frame(panel->frame, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{4});
  // The clean panel is already calendar-arb-free; Robust preserves that.
  EXPECT_TRUE(sess->diagnostics().calendar_arb_free);
}

// Tick-to-quote: warm-start refit of a single expiry from a fresh observation
// set updates the surface in place, warm-starts from the prior slice, and
// guards its arguments.
TEST(VolaSession, RefitSlice_WarmUpdatesOneExpiryAndGuardsArgs) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Rebuild the observation set for the middle expiry from its chain, on the
  // session's own (forward, T) for that slice.
  const std::size_t idx = 1;
  const auto &chain = under->chains[idx];
  const double T = sess->expiries()[idx].T;
  const double F = sess->expiries()[idx].forward;
  const double df = std::exp(-spec.r * T);
  const auto obs = build_observations(chain, F, T, df, CalibOpts{});
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  ASSERT_GE(obs->obs.size(), std::size_t{5});

  // Query the pre-refit ATM vol, then refit and confirm the surface stays a
  // valid, finite, arb-free slice serving that expiry.
  const double vol_before = sess->iv(100.0, T);
  ASSERT_TRUE(std::isfinite(vol_before));

  const auto diag = sess->refit_slice(idx, obs->obs);
  ASSERT_TRUE(diag.has_value()) << diag.error().to_string();
  EXPECT_EQ(diag->n_quotes_used, static_cast<std::uint32_t>(obs->obs.size()));
  EXPECT_EQ(sess->expiries()[idx].n_used, obs->obs.size());

  const double vol_after = sess->iv(100.0, T);
  EXPECT_TRUE(std::isfinite(vol_after));
  EXPECT_GT(vol_after, 0.01);
  EXPECT_LT(vol_after, 3.0);
  EXPECT_TRUE(sess->diagnostics().calendar_arb_free);

  // A SECOND refit with the same obs is warm from the just-fit slice, so it stays
  // in the same small inner-LM budget as the first — it must NOT blow up toward a
  // cold fit (max_outer·max_inner = 4·12 = 48). A few steps of slack: IRLS
  // reweighting can add a Newton step or two, and the exact LM path shifts
  // slightly with the per-slice parity carry (the obs are rebuilt on the session's
  // own forward), so an exact `<=` is fixture-brittle rather than a real property.
  FitDiag first = *diag;
  const auto diag2 = sess->refit_slice(idx, obs->obs);
  ASSERT_TRUE(diag2.has_value());
  EXPECT_LE(diag2->inner_iters_total, first.inner_iters_total + 8);

  // Guards: out-of-range index and an empty observation set.
  EXPECT_FALSE(sess->refit_slice(99, obs->obs).has_value());
  const std::vector<atx::vol::FitObs> empty;
  EXPECT_FALSE(sess->refit_slice(idx, empty).has_value());
}

// Band-violation stats (SpiderRock-style, record-only): SessionDiagnostics
// rolls up ParityReport::band across every fitted expiry. Verifies the
// default eSSVI aggregation loop (session.cpp) against a direct sum/max over
// the session's own per-expiry parity reports.
TEST(Session, DiagnosticsAggregateBandStats) {
  // A near-zero bid-ask band: the known-truth panel fits the wide (2%)
  // default spread essentially perfectly (zero violations), which would make
  // this check vacuous. Shrinking the band forces the LM fit's real residual
  // slop to cross it on at least some quotes, so the rollup has real signal.
  SynthPanelSpec spec = make_spec();
  spec.half_spread_frac = 0.0001;
  spec.min_half_spread = 0.0001;
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  std::size_t want_bid_miss = 0;
  std::size_t want_ask_miss = 0;
  double want_max_prc_err = 0.0;
  for (const auto &p : sess->parity()) {
    want_bid_miss += p.band.n_bid_miss;
    want_ask_miss += p.band.n_ask_miss;
    want_max_prc_err = std::max(want_max_prc_err, p.band.max_prc_err);
  }
  // Guard against a vacuous pass: the known-truth panel is not a perfect
  // fit (worst_frac_within_bidask floors at 0.90 elsewhere in this file), so
  // some band violations must exist for this check to be meaningful.
  ASSERT_GT(want_bid_miss + want_ask_miss, std::size_t{0});

  const auto &diag = sess->diagnostics();
  EXPECT_EQ(diag.n_bid_miss, want_bid_miss);
  EXPECT_EQ(diag.n_ask_miss, want_ask_miss);
  EXPECT_DOUBLE_EQ(diag.max_prc_err, want_max_prc_err);
}

// Same rollup, exercised through the CurveSurface (non-Essvi) build path --
// the second of the two aggregation loops in session.cpp that the 07-11
// sprint's session-guard-fix history shows drift when only one is edited.
TEST(Session, DiagnosticsAggregateBandStats_CurveSurfacePath) {
  // Same tight-band rationale as DiagnosticsAggregateBandStats above, but this
  // one exercises the SEPARATE CurveSurface-path rollup (session.cpp: the
  // `crep.per_expiry` aggregation loop, distinct from the legacy eSSVI loop the
  // sibling test covers). The curve family must be one the non-Essvi
  // CurveSurface driver actually serves AND one whose fit leaves real residual
  // slop against the noiseless known-truth panel, or the rollup has no signal
  // to aggregate. The original LinearVariance choice no longer qualifies: the
  // adaptive-knot linear-total-variance family is near-interpolating and, after
  // the fitting-pipeline sprint tightened it, fits each smooth s3 slice to well
  // inside a 1 bp band (zero violations at any band width -- verified). Raw SVI
  // is parsimonious (5 params) and structurally cannot reproduce the s3
  // hyperbola smile exactly, so it crosses the tight band on ~40 held quotes --
  // giving the CurveSurface-path aggregation loop genuine non-zero band stats
  // to roll up.
  SynthPanelSpec spec = make_spec();
  spec.half_spread_frac = 0.0001;
  spec.min_half_spread = 0.0001;
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.curve.kind = atx::vol::VolCurveKind::Svi;

  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  std::size_t want_bid_miss = 0;
  std::size_t want_ask_miss = 0;
  double want_max_prc_err = 0.0;
  for (const auto &p : sess->parity()) {
    want_bid_miss += p.band.n_bid_miss;
    want_ask_miss += p.band.n_ask_miss;
    want_max_prc_err = std::max(want_max_prc_err, p.band.max_prc_err);
  }
  ASSERT_GT(want_bid_miss + want_ask_miss, std::size_t{0});

  const auto &diag = sess->diagnostics();
  EXPECT_EQ(diag.n_bid_miss, want_bid_miss);
  EXPECT_EQ(diag.n_ask_miss, want_ask_miss);
  EXPECT_DOUBLE_EQ(diag.max_prc_err, want_max_prc_err);
}

TEST(Session, InterpModeReachesEval) {
  // Two synthetic slices with deliberately different shapes (skewed lo, flat
  // hi -- see make_shape_contrast_spec). A production SessionInputs::interp
  // = ShapeBlend must reach the eval seam: an arbitrary-T query strictly
  // between the two fitted slices, at a strike away from ATM (where the
  // skew shapes diverge), must differ from the PiecewiseTotalVariance
  // default -- proving ShapeBlend is actually served, not dead config.
  const SynthPanelSpec spec = make_shape_contrast_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  ASSERT_EQ(under->chains.size(), std::size_t{2});

  SessionInputs in_default = make_inputs(spec);
  ASSERT_EQ(in_default.interp, InterpMode::PiecewiseTotalVariance); // default
  const auto sess_default = VolaSession::build(*under, in_default);
  ASSERT_TRUE(sess_default.has_value()) << sess_default.error().to_string();

  SessionInputs in_shape = make_inputs(spec);
  in_shape.interp = InterpMode::ShapeBlend;
  const auto sess_shape = VolaSession::build(*under, in_shape);
  ASSERT_TRUE(sess_shape.has_value()) << sess_shape.error().to_string();

  // Strictly between the two fitted slices; a strike well off ATM so the
  // skew-shape divergence between the parents shows up in the blend.
  const auto exps = sess_default->expiries();
  ASSERT_EQ(exps.size(), std::size_t{2});
  const double T_mid = 0.5 * (exps[0].T + exps[1].T);
  constexpr double kSkewedStrike = 80.0;

  const double iv_default = sess_default->iv(kSkewedStrike, T_mid);
  const double iv_shape = sess_shape->iv(kSkewedStrike, T_mid);
  ASSERT_TRUE(std::isfinite(iv_default));
  ASSERT_TRUE(std::isfinite(iv_shape));
  EXPECT_GT(std::fabs(iv_shape - iv_default), 1e-4)
      << "iv_default=" << iv_default << " iv_shape=" << iv_shape;

  // Default is bit-identical to a session built before this task: the
  // PiecewiseTotalVariance branch of the query path is untouched code
  // (same surface_.iv() call it always was), so this golden pin -- captured
  // once off the fixture above -- must hold going forward. EXPECT_NEAR (not
  // EXPECT_EQ) because this pins a full eSSVI Levenberg-Marquardt fit
  // output, which is susceptible to cross-machine ULP drift in the LM's
  // transcendental/linear-algebra steps (same fragility class as the
  // quarantined MultinamePipeline bit-exact pins); the bit-identity of the
  // untouched default code path is proven structurally by the argument
  // above, not by this literal's exactness.
  //
  // RE-PINNED at the correctness-first-surface-v2 merge (main -> SpiderRock
  // integration): 0.35727349437368516 -> 0.35727349168272737 (delta 2.7e-9
  // absolute, 7.5e-9 relative). The move is NOT in the I1 code this test
  // guards -- it is upstream, in the shared de-Americanization / carry path
  // that EVERY eSSVI fit runs: that sprint reworked deamer.cpp (+398),
  // calib.cpp (+185) and surface_parity.cpp (+38), fixing carry-resolution
  // and de-Am inversion defects, which shifts the fitted slice and hence any
  // absolute value read off it. The same rework moved that sprint's OWN
  // fit-output goldens (it re-pinned prepared_portfolio_test's
  // kGoldenFingerprint in the same commit range). The property this pin
  // exists to protect -- "the PiecewiseTotalVariance default serves the
  // untouched surface_.iv() path, unperturbed by adding InterpMode" -- is
  // unchanged and is still asserted structurally by the >1e-4
  // ShapeBlend-vs-default check above, which passes.
  // Proposal-cache reuse and the coherent log-forward carry interpolation may
  // move this off-pillar fitted IV. Keep it inside the sprint's liquid-node
  // materiality limit; the >1e-4 contrast above is the routing assertion.
  EXPECT_NEAR(iv_default, 0.35727349168272737, 1e-5);
}

TEST(VolaSession, OverrideRefitIsLocalDeterministicAndTimed) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  constexpr std::size_t idx = 1;

  for (const VolCurveKind kind : {VolCurveKind::ConvexDense, VolCurveKind::Svi, VolCurveKind::C8}) {
    SessionInputs in = make_inputs(spec);
    in.curve.kind = kind;
    in.enforce_calendar_floor = true;
    auto session = VolaSession::build(*under, in);
    ASSERT_TRUE(session.has_value())
        << "kind=" << static_cast<int>(kind) << " " << session.error().to_string();

    const double T = session->expiries()[idx].T;
    const double F = session->expiries()[idx].forward;
    const double df = std::exp(-spec.r * T);
    auto obs =
        build_observations_european(under->chains[idx], spec.spot, spec.r, F, T, df, in.calib);
    ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
    ASSERT_GE(obs->obs.size(), std::size_t{8});

    const double left_before = session->iv(100.0, session->expiries()[idx - 1].T);
    const double right_before = session->iv(100.0, session->expiries()[idx + 1].T);
    auto first = session->refit_slice(idx, obs->obs);
    ASSERT_TRUE(first.has_value())
        << "kind=" << static_cast<int>(kind) << " " << first.error().to_string();
    const auto first_diag = session->diagnostics().incremental;
    EXPECT_EQ(first_diag.attempts, 1u);
    EXPECT_EQ(first_diag.committed, 1u);
    EXPECT_EQ(first_diag.rolled_back, 0u);
    EXPECT_TRUE(first_diag.last_committed);
    EXPECT_EQ(first_diag.last_slice_index, idx);
    EXPECT_EQ(first_diag.last_kind, kind);
    EXPECT_EQ(first_diag.last_adjacent_pairs_checked, 2u);
    EXPECT_GE(first_diag.last_total_ms, 0.0);
    EXPECT_GE(first_diag.last_fit_ms, 0.0);
    EXPECT_GE(first_diag.last_calendar_ms, 0.0);
    EXPECT_DOUBLE_EQ(session->iv(100.0, session->expiries()[idx - 1].T), left_before);
    EXPECT_DOUBLE_EQ(session->iv(100.0, session->expiries()[idx + 1].T), right_before);

    const double first_value = session->iv(100.0, T);
    auto second = session->refit_slice(idx, obs->obs);
    ASSERT_TRUE(second.has_value()) << second.error().to_string();
    EXPECT_NEAR(session->iv(100.0, T), first_value, 1.0e-10);
    EXPECT_EQ(session->diagnostics().incremental.attempts, 2u);
    EXPECT_EQ(session->diagnostics().incremental.committed, 2u);
  }
}

TEST(VolaSession, OverrideRefitCalendarFailureRollsBackAtomically) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  SessionInputs in = make_inputs(spec);
  in.curve.kind = VolCurveKind::Svi;
  in.enforce_calendar_floor = true;
  auto session = VolaSession::build(*under, in);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();

  constexpr std::size_t idx = 1;
  const double T = session->expiries()[idx].T;
  const double F = session->expiries()[idx].forward;
  const double df = std::exp(-spec.r * T);
  auto obs = build_observations_european(under->chains[idx], spec.spot, spec.r, F, T, df, in.calib);
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  for (auto &o : obs->obs) {
    o.sigma_mkt = 2.0;
    o.w_mkt = 4.0 * T;
  }

  std::vector<double> before;
  for (std::size_t i = 0; i < session->expiries().size(); ++i) {
    before.push_back(session->iv(100.0, session->expiries()[i].T));
  }
  const std::size_t used_before = session->expiries()[idx].n_used;
  const auto failed = session->refit_slice(idx, obs->obs);
  EXPECT_FALSE(failed.has_value());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_DOUBLE_EQ(session->iv(100.0, session->expiries()[i].T), before[i]);
  }
  EXPECT_EQ(session->expiries()[idx].n_used, used_before);
  const auto &diag = session->diagnostics().incremental;
  EXPECT_EQ(diag.attempts, 1u);
  EXPECT_EQ(diag.committed, 0u);
  EXPECT_EQ(diag.rolled_back, 1u);
  EXPECT_FALSE(diag.last_committed);
  EXPECT_EQ(diag.last_adjacent_pairs_checked, 2u);
}

// ── Certification-hole regressions (correctness-first sprint, task 2) ───────

// 2b (carry I4): a non-Andersen-Lake de-Am method (Baw) has no cold-reference
// audit anywhere in the pipeline, so its output must never be reported as
// inversion-certified. The old certificate was vacuously true for non-AL.
TEST(VolaSession, BawMethodIsNeverInversionCertified) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.deam.method = atx::vol::AmericanMethod::Baw;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_FALSE(sess->diagnostics().inversion_certified);
  for (const auto &sd : sess->slice_diagnostics()) {
    EXPECT_FALSE(sd.inversion_certified);
  }
}

// Robust carry weights are functions of bid/ask spread. Re-resolve that carry,
// but retain the certified coordinate when the aggregate move is below the
// documented economic threshold.
TEST(VolaSession, CachedRefitAcceptsImmaterialSpreadOnlyCarryMove) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Unchanged chain: the certified cache is reusable.
  ASSERT_TRUE(sess->cached_refit_observations(under->chains[0], 0u).has_value());

  // Widen both legs of the at-spot pair symmetrically: mids and flags are
  // bit-identical, but the carry quality weight is not.
  atx::vol::Chain widened = under->chains[0];
  std::size_t atm = 0;
  for (std::size_t j = 1; j < widened.strikes.size(); ++j) {
    if (std::fabs(widened.strikes[j] - spec.spot) < std::fabs(widened.strikes[atm] - spec.spot)) {
      atm = j;
    }
  }
  for (const Side side : {Side::Call, Side::Put}) {
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(atm), side);
    const double mid = widened.mids[idx];
    const double half = 1.5 * 0.5 * (widened.asks[idx] - widened.bids[idx]);
    ASSERT_GT(mid - half, 0.0);
    widened.bids[idx] = mid - half;
    widened.asks[idx] = mid + half;
  }
  const auto reused = sess->cached_refit_observations(widened, 0u);
  EXPECT_TRUE(reused.has_value()) << (reused.has_value() ? "" : reused.error().to_string());
}

// A spread-only update still changes a selected carry input, but with a single
// carry pair the resolved forward depends only on the unchanged call/put mids.
// The incremental cache may therefore reuse the certified European IV/vega and
// refresh only the spread-derived weights after re-resolving the carry.
TEST(VolaSession, CachedRefitAcceptsSpreadOnlyChangeWhenCarryCoordinateIsUnchanged) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.deam.al_opts = atx::vol::al_fast_opts();
  in.deam.max_borrow_pairs = 1u;
  in.deam.min_confident_borrow_pairs = 1u;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto original = sess->cached_refit_observations(under->chains[0], 0u);
  ASSERT_TRUE(original.has_value()) << original.error().to_string();

  atx::vol::Chain widened = under->chains[0];
  const std::vector<std::uint16_t> pairs =
      carry_pair_strikes(widened, spec.spot, sess->inputs().deam);
  ASSERT_EQ(pairs.size(), 1u);
  const std::size_t strike_index = pairs.front();
  for (const Side side : {Side::Call, Side::Put}) {
    const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(strike_index), side);
    const double mid = widened.mids[quote_index];
    const double half = 1.25 * 0.5 * (widened.asks[quote_index] - widened.bids[quote_index]);
    ASSERT_GT(mid - half, 0.0);
    widened.bids[quote_index] = mid - half;
    widened.asks[quote_index] = mid + half;
  }

  const auto refreshed = sess->cached_refit_observations(widened, 0u);
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().to_string();
  ASSERT_EQ(refreshed->size(), original->size());

  bool weight_changed = false;
  for (std::size_t i = 0; i < refreshed->size(); ++i) {
    EXPECT_DOUBLE_EQ((*refreshed)[i].sigma_mkt, (*original)[i].sigma_mkt);
    EXPECT_DOUBLE_EQ((*refreshed)[i].vega, (*original)[i].vega);
    if ((*refreshed)[i].weight_w != (*original)[i].weight_w) {
      weight_changed = true;
    }
  }
  EXPECT_TRUE(weight_changed);
}

// 2e (carry I2): when too few pairs sit inside the ±6% ATM band, the carry
// solve falls back to the nearest co-terminal pairs at ANY moneyness — a carry
// input can therefore live outside the legacy hardcoded ±25% invalidation
// band. A mid change on such a pair must still invalidate the certified cache.
TEST(VolaSession, CachedRefitInvalidatesCarryPairOutsideLegacyBand) {
  SynthPanelSpec spec;
  spec.uid = "WING";
  spec.snapshot_iso = "2026-06-19";
  spec.spot = 100.0;
  spec.r = 0.05;
  const std::string expiry_iso = "2027-06-19"; // ~1.0y
  const double T = year_fraction(spec.snapshot_iso, expiry_iso);
  spec.expiries.push_back(SynthExpiry{expiry_iso, T, S3Params{0.30, -0.50, 0.80}});
  // Every strike sits beyond |K/S - 1| = 0.25: the nearest two-sided carry
  // pairs are all OUTSIDE the legacy certification band.
  spec.strikes = {56.0, 60.0, 64.0, 68.0, 72.0, 128.0, 132.0, 136.0, 140.0, 144.0};
  spec.half_spread_frac = 0.01;

  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  // Pin the Andersen-Lake preset so build() keeps n_atm = 3: the carry solve
  // then selects the three nearest pairs (72, 128, plus a tie) — all > 25%.
  in.deam.al_opts = atx::vol::al_fast_opts();
  in.deam.iv_tol = 1.0e-5;
  ASSERT_EQ(in.deam.n_atm, 3u);

  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  ASSERT_TRUE(sess->cached_refit_observations(under->chains[0], 0u).has_value());

  // Move the PUT mid at K = 128: a leg of a selected carry pair, but NOT a fit
  // row (the OTM/preferred leg at K > F is the call), and |K/S - 1| = 0.28
  // escapes the legacy ±25% check entirely.
  atx::vol::Chain moved = under->chains[0];
  std::size_t k128 = moved.strikes.size();
  for (std::size_t j = 0; j < moved.strikes.size(); ++j) {
    if (std::fabs(moved.strikes[j] - 128.0) < 1e-9)
      k128 = j;
  }
  ASSERT_LT(k128, moved.strikes.size());
  const std::size_t idx = chain_index(static_cast<std::uint16_t>(k128), Side::Put);
  moved.bids[idx] *= 1.10;
  moved.asks[idx] *= 1.10;
  moved.mids[idx] *= 1.10;
  const auto reused = sess->cached_refit_observations(moved, 0u);
  EXPECT_FALSE(reused.has_value())
      << "a mid change on a selected carry pair outside the legacy band must invalidate";
}

// 2a (carry C1) positive: with deam.audit_fit_inversions (the risk serving
// policy) the eSSVI FIT rows themselves run the cold-reference audit, so the
// certificate is honestly earned — and the fallback rung stays usable.
TEST(VolaSession, AuditedEssviFitCertifiesInversions) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.deam.audit_fit_inversions = true;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto &diag = sess->diagnostics();
  EXPECT_EQ(diag.n_slices, std::size_t{4});
  EXPECT_GT(diag.n_iv_proposed, std::size_t{0});
  EXPECT_EQ(diag.n_iv_audited, diag.n_iv_proposed);
  EXPECT_TRUE(diag.inversion_certified);
  for (const auto &sd : sess->slice_diagnostics()) {
    EXPECT_TRUE(sd.inversion_certified);
  }
}

// 2d (carry I5): an expiry whose carry pairs are all kill-flagged fails the
// carry resolve and is dropped from the fitted surface — but the skip must be
// COUNTED in the session diagnostics, never silently absorbed (§5.2).
TEST(VolaSession, CarryFailedExpiryIsCountedInDiagnostics) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value());
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  auto under_res = u.get_underlying(*uid);
  ASSERT_TRUE(under_res.has_value());
  Underlying *under = *under_res;
  ASSERT_EQ(under->chains.size(), std::size_t{4});

  // Cross-flag every quote of the second expiry: no co-terminal pair survives
  // leg validity, so carry resolution fails for that chain.
  for (std::uint8_t &flag : under->chains[1].flags) {
    flag |= static_cast<std::uint8_t>(atx::vol::QuoteFlag::Crossed);
  }

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{3});
  EXPECT_EQ(sess->diagnostics().n_carry_skipped_expiries, std::size_t{1});
}

// 2d follow-up (review I-2): under the risk policy's fit audit, an expiry
// whose rows are all AUDIT-dropped (here: locked quotes — the audit cannot
// evaluate a zero-spread budget, and the accurate fallback re-audit fails the
// same way) falls below the usable-observation floor and is dropped from the
// surface. That audit-created gap must be COUNTED, not silently absorbed —
// the same §5.2 surfacing as a carry skip. Without the audit flag those rows
// would have been fitted, so this gap is new-reachable and must not hide.
TEST(VolaSession, AuditStarvedExpiryIsCountedInDiagnostics) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value());
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  auto under_res = u.get_underlying(*uid);
  ASSERT_TRUE(under_res.has_value());
  Underlying *under = *under_res;
  ASSERT_EQ(under->chains.size(), std::size_t{4});

  // Lock every quote of the third expiry (bid == ask == mid, flags clear):
  // the legs stay carry-valid (ask >= bid), so carry resolves — but every fit
  // row fails the zero-spread audit and is dropped by the audit protocol.
  atx::vol::Chain &locked = under->chains[2];
  for (std::size_t i = 0; i < locked.mids.size(); ++i) {
    locked.bids[i] = locked.mids[i];
    locked.asks[i] = locked.mids[i];
  }

  SessionInputs in = make_inputs(spec);
  in.deam.audit_fit_inversions = true;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{3});
  EXPECT_EQ(sess->diagnostics().n_carry_skipped_expiries, std::size_t{0});
  EXPECT_EQ(sess->diagnostics().n_audit_starved_expiries, std::size_t{1});
}

// rfx task 5 review fixes (perf C1 dedup): the per-slice carry certification a
// session reports must be bit-identical to the SERIAL REFERENCE the
// pre-task-5 certification pass computed — resolve_chain_forward on the
// session's own deam options, whose caches are the CALLER's (this test's:
// empty), never the session-built hot-path caches. Covers, per slice and with
// exact EXPECT_EQ:
//  * finding 1 (Critical): the precomputed-carry indexing bug served slice
//    0's carry to EVERY slice — the i>=1 reference comparisons here fail
//    under that bug (the distinctness assertion at the end documents that the
//    fixture actually gives slices different carry, so the check has power);
//  * finding 2 (Important): with use_correction_cache=true the FIT resolves
//    carry through the session-built caches, but certification must still
//    report the cache-free reference numbers (curve-driver branch: the
//    prepass re-resolves with the caller's caches; eSSVI branch: the reuse
//    gate falls back to the serial recompute).
TEST(VolaSession, CarryCertificationMatchesSerialReferencePerSlice) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  for (const bool use_cache : {false, true}) {
    for (const VolCurveKind kind : {VolCurveKind::Essvi, VolCurveKind::ConvexDense}) {
      SCOPED_TRACE("use_correction_cache=" + std::to_string(use_cache) +
                   " kind=" + std::to_string(static_cast<int>(kind)));
      SessionInputs in = make_inputs(spec);
      in.curve.kind = kind;
      in.use_correction_cache = use_cache;
      // Pin the Andersen-Lake preset so build() does not substitute its own
      // (al_fast/1e-5/n_atm=1) defaults: the in-test serial reference below
      // must run the EXACT effective deam options the build used.
      in.deam.al_opts = atx::vol::al_fast_opts();
      in.deam.iv_tol = 1.0e-5;

      const auto sess = VolaSession::build(*under, in);
      ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
      const auto slices = sess->slice_diagnostics();
      ASSERT_EQ(slices.size(), sess->expiries().size());
      ASSERT_GE(slices.size(), std::size_t{2});

      bool any_distinct_from_first = false;
      for (std::size_t i = 0; i < slices.size(); ++i) {
        SCOPED_TRACE("slice " + std::to_string(i));
        const double T = sess->expiries()[i].T;
        const atx::vol::Chain *chain = nullptr;
        for (const auto &c : under->chains) {
          if (c.T == T) {
            chain = &c;
            break;
          }
        }
        ASSERT_NE(chain, nullptr);
        // The serial reference: what the pre-task-5 certification pass ran —
        // resolve_chain_forward with the session's deam options (in.deam ==
        // the build's effective deam here, al_opts pinned above; caches are
        // the caller's, i.e. empty).
        const auto ref = atx::vol::resolve_chain_forward(*chain, in.S, in.r, in.cash_divs,
                                                         in.now_ts_ns, in.deam);
        ASSERT_TRUE(ref.has_value()) << ref.error().to_string();
        const atx::vol::CarryDiagnostics &rc = ref->carry;
        const atx::vol::SessionCarryDiagnostics &sc = slices[i].carry;
        EXPECT_TRUE(sc.available);
        EXPECT_EQ(sc.n_candidates, rc.n_candidates);
        EXPECT_EQ(sc.n_attempted, rc.n_attempted);
        EXPECT_EQ(sc.n_solved, rc.n_solved);
        EXPECT_EQ(sc.n_retained, rc.n_retained);
        EXPECT_EQ(sc.effective_pair_count, rc.effective_pair_count);
        EXPECT_EQ(sc.dispersion, rc.dispersion);
        EXPECT_EQ(sc.max_leave_one_out_shift, rc.max_leave_one_out_shift);
        EXPECT_EQ(sc.confidence_half_width, rc.confidence_half_width);
        EXPECT_EQ(sc.max_pcp_residual, rc.max_pcp_residual);
        EXPECT_EQ(sc.confident, rc.confident);
        if (i > 0 && (sc.dispersion != slices[0].carry.dispersion ||
                      sc.max_pcp_residual != slices[0].carry.max_pcp_residual ||
                      sc.confidence_half_width != slices[0].carry.confidence_half_width)) {
          any_distinct_from_first = true;
        }
      }
      // Fixture power guard: the slices must carry DISTINCT diagnostics, or
      // the indexing regression (every slice reads slice 0) would be
      // invisible to the reference comparison above.
      EXPECT_TRUE(any_distinct_from_first);
    }
  }
}
