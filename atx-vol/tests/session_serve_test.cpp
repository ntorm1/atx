#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/api/backtest/panel.hpp"
#include "atx/vol/api/backtest/query_pricing.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/calib.hpp"
#include "atx/vol/api/fitting/s3.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/marketdata/data.hpp"
#include "atx/vol/api/marketdata/universe.hpp"
#include "atx/vol/api/pricing/american.hpp"

// VolaSession serve policy (serve-path lane, 2026-10-16; session.hpp
// SessionServePolicy): the records serve mode and the correction-tensor
// exercise-edge gate.
//
// Records mode contract under test: a query at a fitted expiry inside its
// record box is served from a certified ExpiryRecord (route Record) and carries
// a dollar certificate that bounds its error against the cold ACCURATE
// Andersen-Lake price; every other query is priced by the session's own cold
// route, bit-for-bit, and never by the correction tensor.

namespace {

using atx::vol::AmericanGreeks;
using atx::vol::AmericanMethod;
using atx::vol::american_price;
using atx::vol::build_observations;
using atx::vol::CalibOpts;
using atx::vol::data_install;
using atx::vol::FitObs;
using atx::vol::iso_to_ns;
using atx::vol::make_synthetic_american_panel;
using atx::vol::QueryPricingRoute;
using atx::vol::S3Params;
using atx::vol::SessionInputs;
using atx::vol::SessionServeMode;
using atx::vol::Side;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::VolaSession;
using atx::vol::year_fraction;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Four expiries, downward skew, no cash dividend (records carry continuous q).
[[nodiscard]] SynthPanelSpec make_spec() {
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {"2026-07-26", "2026-10-06", "2027-01-24", "2027-06-19"};
  const std::vector<S3Params> truths = {
      S3Params{0.32, -0.70, 1.00},
      S3Params{0.30, -0.60, 0.90},
      S3Params{0.28, -0.55, 0.80},
      S3Params{0.27, -0.50, 0.70},
  };
  SynthPanelSpec spec;
  spec.uid = "SERVE";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.04;
  spec.borrow = 0.008;
  for (std::size_t i = 0; i < isos.size(); ++i) {
    spec.expiries.push_back(SynthExpiry{isos[i], year_fraction(snapshot, isos[i]), truths[i]});
  }
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K);
  }
  spec.half_spread_frac = 0.02;
  return spec;
}

[[nodiscard]] SessionInputs make_inputs(const SynthPanelSpec &spec) {
  SessionInputs in;
  in.S = spec.spot;
  in.r = spec.r;
  in.now_ts_ns = iso_to_ns(spec.snapshot_iso);
  in.deam.hyb = spec.hyb;
  in.deam.imply_borrow = true;
  in.deam.n_atm = 3;
  return in;
}

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

[[nodiscard]] SessionInputs records_inputs(const SynthPanelSpec &spec) {
  SessionInputs in = make_inputs(spec);
  in.serve.mode = SessionServeMode::Records;
  return in;
}

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// The session's own cold price route (what a non-record query must reproduce).
[[nodiscard]] double session_cold(const VolaSession &s, double K, double T, Side side) {
  const double sigma = s.iv(K, T);
  const auto p = american_price(s.inputs().S, K, T, sigma, s.rate_at(T), s.q_eff_at(T), side,
                                s.inputs().deam.method, s.inputs().deam.al_opts);
  return p ? *p : kNaN;
}

// The certification reference: cold ACCURATE Andersen-Lake at the session's vol.
[[nodiscard]] double cold_accurate(const VolaSession &s, double K, double T, Side side) {
  const double sigma = s.iv(K, T);
  const auto p = american_price(s.inputs().S, K, T, sigma, s.rate_at(T), s.q_eff_at(T), side,
                                AmericanMethod::AndersenLake, std::nullopt);
  return p ? *p : kNaN;
}

} // namespace

TEST(SessionServeRecords, DefaultPolicyBuildsNoRecordsAndNeverReportsTheNewRoutes) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->record_count(), 0u);
  for (const auto &ctx : sess->expiries()) {
    for (const double K : spec.strikes) {
      for (const Side side : {Side::Call, Side::Put}) {
        const QueryPricingRoute route = sess->query_route(K, ctx.T, side);
        EXPECT_NE(route, QueryPricingRoute::Record);
        EXPECT_NE(route, QueryPricingRoute::ExerciseEdgeFallback);
        EXPECT_TRUE(std::isnan(sess->served_certificate_usd(K, ctx.T, side)));
      }
    }
  }
}

TEST(SessionServeRecords, FittedExpiryQueriesAreServedWithinTheirCertificate) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  const auto sess = VolaSession::build(*under, records_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  ASSERT_EQ(sess->record_count(), 2u * sess->expiries().size());

  std::size_t served = 0;
  double worst_slack = -std::numeric_limits<double>::infinity();
  for (const auto &ctx : sess->expiries()) {
    const double T = ctx.T;
    for (const double K : spec.strikes) {
      for (const Side side : {Side::Call, Side::Put}) {
        ASSERT_EQ(sess->query_route(K, T, side), QueryPricingRoute::Record)
            << "T=" << T << " K=" << K;
        const double cert = sess->served_certificate_usd(K, T, side);
        ASSERT_TRUE(std::isfinite(cert) && cert > 0.0);
        const double F = spec.spot * std::exp((sess->rate_at(T) - sess->q_eff_at(T)) * T);
        EXPECT_LE(cert, std::max(5.0e-4, 5.0e-7 * F)) << "policy limit";
        const auto fv = sess->fair_value(K, T, side);
        ASSERT_TRUE(fv.has_value()) << fv.error().to_string();
        const double ref = cold_accurate(*sess, K, T, side);
        ASSERT_TRUE(std::isfinite(ref));
        // The certificate is a bound against the cold accurate solver.
        EXPECT_LE(std::abs(*fv - ref), cert) << "T=" << T << " K=" << K << " cert=" << cert;
        worst_slack = std::max(worst_slack, std::abs(*fv - ref) - cert);
        const auto g = sess->greeks(K, T, side);
        ASSERT_TRUE(g.has_value());
        EXPECT_TRUE(same_bits(g->price, *fv)) << "greeks().price must equal fair_value()";
        EXPECT_TRUE(std::isfinite(g->delta) && std::isfinite(g->gamma) &&
                    std::isfinite(g->vega) && std::isfinite(g->theta));
        EXPECT_TRUE(std::isnan(g->rho) && std::isnan(g->vanna) && std::isnan(g->volga) &&
                    std::isnan(g->charm))
            << "the record route does not serve rho/vanna/volga/charm";
        ++served;
      }
    }
  }
  EXPECT_EQ(served, sess->expiries().size() * spec.strikes.size() * 2u);
  std::printf("[serve-records] %zu record-served contracts, worst |err| - cert = %.3e\n", served,
              worst_slack);
}

TEST(SessionServeRecords, OffExpiryAndOutOfBoxQueriesArePricedByTheColdRoute) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  const auto sess = VolaSession::build(*under, records_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  const auto ex = sess->expiries();
  ASSERT_GE(ex.size(), 2u);

  // Between two fitted expiries there is no record: ColdReference, cold bits.
  const double T_mid = 0.5 * (ex[0].T + ex[1].T);
  for (const double K : {80.0, 100.0, 120.0}) {
    for (const Side side : {Side::Call, Side::Put}) {
      EXPECT_EQ(sess->query_route(K, T_mid, side), QueryPricingRoute::ColdReference);
      EXPECT_TRUE(std::isnan(sess->served_certificate_usd(K, T_mid, side)));
      const auto fv = sess->fair_value(K, T_mid, side);
      ASSERT_TRUE(fv.has_value());
      EXPECT_TRUE(same_bits(*fv, session_cold(*sess, K, T_mid, side)));
    }
  }
  // Far beyond the listed strikes (and the record's spot-move pad): ColdFallback.
  const double T = ex[1].T;
  for (const double K : {20.0, 400.0}) {
    for (const Side side : {Side::Call, Side::Put}) {
      EXPECT_EQ(sess->query_route(K, T, side), QueryPricingRoute::ColdFallback) << "K=" << K;
      const auto fv = sess->fair_value(K, T, side);
      ASSERT_TRUE(fv.has_value());
      EXPECT_TRUE(same_bits(*fv, session_cold(*sess, K, T, side))) << "K=" << K;
    }
  }
}

TEST(SessionServeRecords, OverPolicyCertificatesAreRefusedToTheColdRoute) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  SessionInputs in = records_inputs(spec);
  in.serve.record_max_cert_usd = 1.0e-300; // no real certificate is this small
  in.serve.record_max_cert_rel = 0.0;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  ASSERT_GT(sess->record_count(), 0u);
  for (const auto &ctx : sess->expiries()) {
    for (const double K : spec.strikes) {
      for (const Side side : {Side::Call, Side::Put}) {
        EXPECT_EQ(sess->query_route(K, ctx.T, side), QueryPricingRoute::ColdFallback);
        EXPECT_TRUE(std::isnan(sess->served_certificate_usd(K, ctx.T, side)));
        const auto fv = sess->fair_value(K, ctx.T, side);
        ASSERT_TRUE(fv.has_value());
        EXPECT_TRUE(same_bits(*fv, session_cold(*sess, K, ctx.T, side)));
      }
    }
  }
}

// Serve-path review MINOR-2: a record whose certification REFUSED a row (no cold
// reference at a check sigma) is uncertified, and the session refuses it. A
// record_sigma_lo_mult of 1e-6 starts every record's sigma box near zero, where
// the CALL side at this carry (internal put rate q_eff ~ 0.008, yield r = 0.04) is
// the cold solver's frozen corner: each call record builds but refuses its
// sigma_min row (ExpiryRecord.ACertificationRowWithoutAColdReference... pins the
// record half). The put records certify with a finite (large) error, and the
// dollar limit is opened so far that any finite certificate is within policy, so
// a call can only be refused for its NaN certificate.
TEST(SessionServeRecords, RecordsWithARefusedCertificationRowAreNeverServed) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  SessionInputs in = records_inputs(spec);
  in.serve.record_sigma_lo_mult = 1.0e-6;
  in.serve.record_max_cert_usd = 1.0e6;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  // Both sides of every fitted expiry built a record: a refused row is not a
  // build failure (that would route ColdReference, not ColdFallback).
  ASSERT_EQ(sess->record_count(), 2u * sess->expiries().size());
  std::size_t puts_served = 0;
  for (const auto &ctx : sess->expiries()) {
    for (const double K : spec.strikes) {
      EXPECT_EQ(sess->query_route(K, ctx.T, Side::Call), QueryPricingRoute::ColdFallback)
          << "T=" << ctx.T << " K=" << K;
      EXPECT_TRUE(std::isnan(sess->served_certificate_usd(K, ctx.T, Side::Call)));
      const auto fv = sess->fair_value(K, ctx.T, Side::Call);
      ASSERT_TRUE(fv.has_value());
      EXPECT_TRUE(same_bits(*fv, session_cold(*sess, K, ctx.T, Side::Call)));
      if (sess->query_route(K, ctx.T, Side::Put) == QueryPricingRoute::Record) {
        ++puts_served;
      }
    }
  }
  EXPECT_GT(puts_served, 0u) << "finite certificates are within this policy";
}

TEST(SessionServeRecords, LaddersAndClonesServeTheSameBitsAsScalarQueries) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  const auto sess = VolaSession::build(*under, records_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  const VolaSession copy = sess->clone();
  EXPECT_EQ(copy.record_count(), sess->record_count());

  // Listed strikes plus two far ones (cold) so the ladders mix routes.
  std::vector<double> strikes = spec.strikes;
  strikes.push_back(20.0);
  strikes.push_back(400.0);
  std::vector<double> ks;
  std::vector<Side> sides;
  for (const double K : strikes) {
    for (const Side side : {Side::Call, Side::Put}) {
      ks.push_back(K);
      sides.push_back(side);
    }
  }
  for (const auto &ctx : sess->expiries()) {
    const double T = ctx.T;
    std::vector<double> fv(ks.size(), kNaN);
    std::vector<AmericanGreeks> gk(ks.size());
    std::vector<double> iv(ks.size(), kNaN);
    std::vector<double> px(ks.size(), kNaN);
    std::vector<AmericanGreeks> eg(ks.size());
    std::vector<double> px_only(ks.size(), kNaN);
    ASSERT_TRUE(sess->fair_value_ladder(T, ks, sides, fv).has_value());
    ASSERT_TRUE(sess->greeks_ladder(T, ks, sides, gk).has_value());
    ASSERT_TRUE(sess->evaluate_ladder(T, ks, sides, iv, px, eg).has_value());
    ASSERT_TRUE(sess->evaluate_ladder(T, ks, sides, {}, px_only, {}).has_value());
    for (std::size_t i = 0; i < ks.size(); ++i) {
      const auto scalar = sess->fair_value(ks[i], T, sides[i]);
      const auto g = sess->greeks(ks[i], T, sides[i]);
      const auto cloned = copy.fair_value(ks[i], T, sides[i]);
      ASSERT_TRUE(scalar.has_value() && g.has_value() && cloned.has_value());
      EXPECT_TRUE(same_bits(fv[i], *scalar)) << "fair_value_ladder K=" << ks[i];
      EXPECT_TRUE(same_bits(gk[i].price, g->price)) << "greeks_ladder K=" << ks[i];
      EXPECT_TRUE(same_bits(gk[i].delta, g->delta));
      EXPECT_TRUE(same_bits(eg[i].price, g->price)) << "evaluate_ladder greeks K=" << ks[i];
      EXPECT_TRUE(same_bits(px[i], g->price)) << "evaluate_ladder price K=" << ks[i];
      EXPECT_TRUE(same_bits(*cloned, *scalar)) << "clone K=" << ks[i];
      if (sess->query_route(ks[i], T, sides[i]) == QueryPricingRoute::Record) {
        // The price-only ladder serves the record price, which greeks() matches.
        EXPECT_TRUE(same_bits(px_only[i], *scalar)) << "price-only ladder K=" << ks[i];
      }
    }
  }
}

TEST(SessionServeRecords, SliceRefitRebuildsThatSlicesRecords) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  auto sess = VolaSession::build(*under, records_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  const std::size_t idx = 1;
  const double T = sess->expiries()[idx].T;
  const double F = sess->expiries()[idx].forward;
  const auto obs = build_observations(under->chains[idx], F, T, std::exp(-spec.r * T), CalibOpts{});
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  // Triple the slice's vols: the refit smile leaves the original record's sigma
  // box ([0.6 min, 1.6 max] of the build-time smile), so serving it as a Record
  // proves the slice's record was rebuilt.
  std::vector<FitObs> bumped = obs->obs;
  for (FitObs &o : bumped) {
    o.sigma_mkt *= 3.0;
    o.w_mkt *= 9.0;
  }
  const double vol_before = sess->iv(100.0, T);
  ASSERT_TRUE(sess->refit_slice(idx, bumped).has_value());
  const double vol_after = sess->iv(100.0, T);
  ASSERT_GT(vol_after, 1.6 * vol_before) << "the refit must move ATM vol out of the old box";
  EXPECT_EQ(sess->record_count(), 2u * sess->expiries().size());
  for (const Side side : {Side::Call, Side::Put}) {
    EXPECT_EQ(sess->query_route(100.0, T, side), QueryPricingRoute::Record);
    const double cert = sess->served_certificate_usd(100.0, T, side);
    const auto fv = sess->fair_value(100.0, T, side);
    ASSERT_TRUE(fv.has_value() && std::isfinite(cert));
    EXPECT_LE(std::abs(*fv - cold_accurate(*sess, 100.0, T, side)), cert);
  }
}

// ── Tier mode: the correction-tensor exercise-edge gate ─────────────────────

TEST(SessionServeGate, AnUnclearableBandRoutesEveryInBoxQueryCold) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  SessionInputs in = make_inputs(spec);
  in.serve.exercise_edge_gate = true;
  in.serve.exercise_edge_band = 1.0e6;
  const auto gated = VolaSession::build(*under, in);
  const auto plain = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(gated.has_value() && plain.has_value());
  std::size_t gated_in_box = 0;
  for (const auto &ctx : gated->expiries()) {
    for (const double K : spec.strikes) {
      for (const Side side : {Side::Call, Side::Put}) {
        const QueryPricingRoute before = plain->query_route(K, ctx.T, side);
        const QueryPricingRoute after = gated->query_route(K, ctx.T, side);
        const auto fv = gated->fair_value(K, ctx.T, side);
        ASSERT_TRUE(fv.has_value());
        if (before == QueryPricingRoute::RepresentativeFast ||
            before == QueryPricingRoute::CarryBank) {
          EXPECT_EQ(after, QueryPricingRoute::ExerciseEdgeFallback) << "K=" << K;
          EXPECT_TRUE(same_bits(*fv, session_cold(*gated, K, ctx.T, side))) << "K=" << K;
          ++gated_in_box;
        } else {
          EXPECT_EQ(after, before) << "only in-box tensor queries are gated";
        }
      }
    }
  }
  EXPECT_GT(gated_in_box, 0u) << "fixture must serve some queries from the tensor";
}

TEST(SessionServeGate, AZeroBandGatesOnlyTheExerciseSide) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);
  SessionInputs in = make_inputs(spec);
  in.serve.exercise_edge_gate = true;
  in.serve.exercise_edge_band = 0.0;
  const auto gated = VolaSession::build(*under, in);
  const auto plain = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(gated.has_value() && plain.has_value());
  const double T = gated->expiries().back().T;
  // A far-OTM put sits deep in the continuation region: still tensor-served.
  ASSERT_EQ(plain->query_route(70.0, T, Side::Put), gated->query_route(70.0, T, Side::Put));
  EXPECT_NE(gated->query_route(70.0, T, Side::Put), QueryPricingRoute::ExerciseEdgeFallback);
  // The tensor-served bits are unchanged where the gate passes.
  const auto a = plain->fair_value(70.0, T, Side::Put);
  const auto b = gated->fair_value(70.0, T, Side::Put);
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_TRUE(same_bits(*a, *b));
}
