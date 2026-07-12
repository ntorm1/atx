#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "atx/vol/data.hpp"      // data_install, iso_to_ns, year_fraction
#include "atx/vol/event_vol.hpp"
#include "atx/vol/panel.hpp"     // SynthPanelSpec, make_synthetic_american_panel
#include "atx/vol/session.hpp"   // VolaSession, SessionInputs (production seam)
#include "atx/vol/universe.hpp"  // Universe, Underlying
#include "atx/vol/vol_time.hpp"  // kCalendarYearNs

// Coverage for the SpiderRock-style earnings event-variance model: censored
// total variance, FLEX recombination, implied per-event move (eMove), and
// event-aware time interpolation. Numbers are hand-derived in comments next
// to each expectation (flat censored-vol fixtures make the arithmetic
// checkable by hand).

namespace {

using atx::vol::censored_total_variance;
using atx::vol::ErrorCode;
using atx::vol::event_aware_w;
using atx::vol::event_recombined_vol;
using atx::vol::EventSchedule;
using atx::vol::implied_emove;
using atx::vol::kEmoveSqClampEps;
using atx::vol::kWCenFloor;

// ── EventSchedule ────────────────────────────────────────────────────────

TEST(EventVol, ScheduleSortsUnorderedInput) {
  const EventSchedule sched(std::vector<std::int64_t>{300, 100, 200});
  const auto ev = sched.events();
  ASSERT_EQ(ev.size(), std::size_t{3});
  EXPECT_EQ(ev[0], 100);
  EXPECT_EQ(ev[1], 200);
  EXPECT_EQ(ev[2], 300);
}

TEST(EventVol, ScheduleBoundarySemantics_ExpiryCountsNowDoesNot) {
  // Events at 100 and 200. (now=100, expiry=200] -> only 200 counts (100 is
  // "now" and is excluded; 200 is "expiry" and is included).
  const EventSchedule sched(std::vector<std::int64_t>{100, 200});
  EXPECT_EQ(sched.count_between(100, 200), std::size_t{1});
  // (now=99, expiry=200] -> both 100 and 200 count.
  EXPECT_EQ(sched.count_between(99, 200), std::size_t{2});
  // (now=100, expiry=199] -> neither counts (100 excluded as "now", 200 is
  // beyond expiry).
  EXPECT_EQ(sched.count_between(100, 199), std::size_t{0});
  // (now=200, expiry=200] -> 200 excluded (equals "now").
  EXPECT_EQ(sched.count_between(200, 200), std::size_t{0});
}

TEST(EventVol, ScheduleEmpty_CountIsZero) {
  const EventSchedule sched(std::vector<std::int64_t>{});
  EXPECT_EQ(sched.count_between(0, 1000), std::size_t{0});
  EXPECT_TRUE(sched.events().empty());
}

TEST(EventVol, ScheduleExpiryBeforeNow_ReturnsZero) {
  const EventSchedule sched(std::vector<std::int64_t>{50});
  EXPECT_EQ(sched.count_between(200, 100), std::size_t{0});
}

// ── censored_total_variance ──────────────────────────────────────────────

TEST(EventVol, CensoredSubtractsEventVariance) {
  // w_total = 0.04*0.25 + 2*0.0025 = 0.01+0.005 = 0.015; n=2, emove=0.05
  // (e^2=0.0025) -> w_cen = 0.015 - 2*0.0025 = 0.01.
  EXPECT_NEAR(censored_total_variance(0.015, 2, 0.05), 0.01, 1e-12);
}

TEST(EventVol, CensoredFlooredNonNegative) {
  // n=3 events at emove=0.10 (e^2=0.01): n*e^2 = 0.03 >> w_total = 0.001, so
  // the raw censored variance (0.001 - 0.03 = -0.029) floors at kWCenFloor.
  const double result = censored_total_variance(/*w_total=*/0.001, /*n_events=*/3,
                                                /*emove=*/0.10);
  EXPECT_DOUBLE_EQ(result, kWCenFloor);
  EXPECT_DOUBLE_EQ(kWCenFloor, 1e-10);
}

TEST(EventVol, CensoredNaNInNaNOut) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(std::isnan(censored_total_variance(nan, 1, 0.05)));
  EXPECT_TRUE(std::isnan(censored_total_variance(0.01, 1, nan)));
}

// ── event_recombined_vol ─────────────────────────────────────────────────

TEST(EventVol, RecombinedVolMatchesSpiderRockFormula) {
  // atmCen=0.18, T=0.5, n=2, emove=0.06:
  // atmVol = sqrt(0.18^2 + 2*0.06^2/0.5) = sqrt(0.0324 + 0.0144) = sqrt(0.0468)
  const double atm_cen = 0.18, T = 0.5, emove = 0.06;
  const std::size_t n = 2;
  const double expect = std::sqrt(atm_cen * atm_cen + static_cast<double>(n) * emove * emove / T);
  EXPECT_NEAR(event_recombined_vol(atm_cen, T, n, emove), expect, 1e-12);
  EXPECT_NEAR(event_recombined_vol(atm_cen, T, n, emove), 0.2163330766, 1e-9);
}

TEST(EventVol, RecombinedVolZeroEventsIsPlainAtm) {
  // n=0 -> atmVol == atmCen exactly.
  EXPECT_NEAR(event_recombined_vol(0.22, 0.75, 0, 0.05), 0.22, 1e-12);
}

TEST(EventVol, RecombinedVolNonPositiveTIsNaN) {
  EXPECT_TRUE(std::isnan(event_recombined_vol(0.20, 0.0, 1, 0.05)));
  EXPECT_TRUE(std::isnan(event_recombined_vol(0.20, -1.0, 1, 0.05)));
}

// ── implied_emove ────────────────────────────────────────────────────────

TEST(EventVol, RoundTripKnownEmove) {
  // flat censored vol 20%, emove 5%: w(T,n) = 0.04*T + n*0.0025
  const double w1 = 0.04 * 0.10 + 1 * 0.0025, w2 = 0.04 * 0.25 + 2 * 0.0025;
  auto e = implied_emove(w1, 0.10, 1, w2, 0.25, 2);
  ASSERT_TRUE(e.has_value());
  EXPECT_NEAR(*e, 0.05, 1e-12);
}

TEST(EventVol, NoIdentificationWhenProportional) {
  // n1/T1 == n2/T2 -> denominator (n1*T2 - n2*T1) == 0
  const auto res = implied_emove(0.01, 0.1, 1, 0.02, 0.2, 2);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(EventVol, ImpliedEmove_T1EqualsT2_ReturnsInvalidArgument) {
  const auto res = implied_emove(0.01, 0.2, 1, 0.02, 0.2, 2);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(EventVol, ImpliedEmove_NonPositiveT_ReturnsInvalidArgument) {
  EXPECT_EQ(implied_emove(0.01, 0.0, 1, 0.02, 0.2, 2).error().code(),
           ErrorCode::InvalidArgument);
  EXPECT_EQ(implied_emove(0.01, -0.1, 1, 0.02, 0.2, 2).error().code(),
           ErrorCode::InvalidArgument);
  EXPECT_EQ(implied_emove(0.01, 0.1, 1, 0.02, 0.0, 2).error().code(),
           ErrorCode::InvalidArgument);
}

TEST(EventVol, ImpliedEmove_NonFiniteInput_ReturnsInvalidArgument) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(implied_emove(nan, 0.1, 1, 0.02, 0.2, 2).error().code(),
           ErrorCode::InvalidArgument);
  EXPECT_EQ(implied_emove(0.01, inf, 1, 0.02, 0.2, 2).error().code(),
           ErrorCode::InvalidArgument);
}

TEST(EventVol, ImpliedEmove_NegativeESquaredBeyondEps_ReturnsOutOfRange) {
  // T1=0.1,n1=2; T2=0.2,n2=1: denom = n1*T2-n2*T1 = 2*0.2-1*0.1 = 0.3.
  // w1=0.001,w2=0.05: numer = w1*T2-w2*T1 = 0.001*0.2-0.05*0.1 = -0.0048.
  // e^2 = -0.0048/0.3 = -0.016, far below -kEmoveSqClampEps -> not physical.
  const auto res = implied_emove(0.001, 0.1, 2, 0.05, 0.2, 1);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(EventVol, ImpliedEmove_ESquaredExactlyZero_ClampsToZero) {
  // e == 0 (no event move): w(T,n) == 0.04*T regardless of n. T1=0.1,n1=1;
  // T2=0.3,n2=2 (not proportional: n1*T2=0.3 != n2*T1=0.2, so identified).
  const double w1 = 0.04 * 0.1, w2 = 0.04 * 0.3;
  auto e = implied_emove(w1, 0.1, 1, w2, 0.3, 2);
  ASSERT_TRUE(e.has_value());
  EXPECT_NEAR(*e, 0.0, 1e-9);
}

TEST(EventVol, ImpliedEmove_ESquaredWithinEpsWindow_ClampsToZero) {
  // Construct e^2 = -kEmoveSqClampEps/2 exactly via denom=1, numer=-eps/2.
  // T1=1,n1=0 ; T2=2,n2=... choose n1*T2-n2*T1=1 => n2=0,n1=... simplest:
  // n1=1,T2=2 => n1*T2=2; need n2*T1=1 => n2=1,T1=1. denom=2-1=1.
  // numer = w1*T2 - w2*T1 = w1*2 - w2*1; pick w2=0, w1 = -eps/4 so that
  // numer = -eps/2. Then e^2 = numer/denom = -eps/2, inside [-eps,0].
  const double eps = kEmoveSqClampEps;
  const double w1 = -eps / 4.0, T1 = 1.0;
  const double w2 = 0.0, T2 = 2.0;
  auto e = implied_emove(w1, T1, /*n1=*/1, w2, T2, /*n2=*/1);
  ASSERT_TRUE(e.has_value());
  EXPECT_DOUBLE_EQ(*e, 0.0);
}

// ── event_aware_w ────────────────────────────────────────────────────────

TEST(EventVol, EventAwareInterpExactAtSlices) {
  // at T_query == T_lo with n_query == n_lo, returns w_lo exactly (censoring
  // then re-adding n_lo*emove^2 is a round trip).
  const double w_lo = 0.04 * 0.10 + 1 * 0.0025;  // T=0.10, n=1
  const double w_hi = 0.04 * 0.25 + 2 * 0.0025;  // T=0.25, n=2
  const double w = event_aware_w(w_lo, 0.10, 1, w_hi, 0.25, 2, /*T_query=*/0.10,
                                 /*n_query=*/1, /*emove=*/0.05);
  EXPECT_NEAR(w, w_lo, 1e-12);
}

TEST(EventVol, EventAwareInterpExactAtHiSlice) {
  const double w_lo = 0.04 * 0.10 + 1 * 0.0025;
  const double w_hi = 0.04 * 0.25 + 2 * 0.0025;
  const double w = event_aware_w(w_lo, 0.10, 1, w_hi, 0.25, 2, /*T_query=*/0.25,
                                 /*n_query=*/2, /*emove=*/0.05);
  EXPECT_NEAR(w, w_hi, 1e-12);
}

TEST(EventVol, EventAwareInterpJumpAcrossEvent) {
  // censored vol flat 20%, emove 5%. Slices at T=0.1 (n=0), T=0.3 (n=1).
  // Query T=0.19 (n=0) vs T=0.21 (n=1): censored parts nearly equal, w jumps by ~emove².
  const double e2 = 0.0025;
  const double w_lo = 0.04 * 0.1, w_hi = 0.04 * 0.3 + e2;
  const double w_a = event_aware_w(w_lo, 0.1, 0, w_hi, 0.3, 1, 0.19, 0, 0.05);
  const double w_b = event_aware_w(w_lo, 0.1, 0, w_hi, 0.3, 1, 0.21, 1, 0.05);
  EXPECT_NEAR(w_a, 0.04 * 0.19, 1e-12);
  EXPECT_NEAR(w_b, 0.04 * 0.21 + e2, 1e-12);
}

TEST(EventVol, ZeroEventsIsPlainLinearW) {
  // emove == 0: falls back to plain linear-in-w regardless of n's (n's are
  // deliberately nonzero here to prove the emove<=0 branch, not the
  // all-n-zero branch, is the one firing).
  const double w_lo = 0.03, T_lo = 0.2, w_hi = 0.09, T_hi = 0.5;
  const double T_query = 0.35;
  const double w = event_aware_w(w_lo, T_lo, /*n_lo=*/2, w_hi, T_hi, /*n_hi=*/5, T_query,
                                 /*n_query=*/3, /*emove=*/0.0);
  const double weight_hi = (T_query - T_lo) / (T_hi - T_lo);  // (0.15)/(0.3) = 0.5
  const double expect = w_lo + weight_hi * (w_hi - w_lo);      // 0.03+0.5*0.06 = 0.06
  EXPECT_NEAR(w, expect, 1e-12);
  EXPECT_NEAR(w, 0.06, 1e-12);
}

TEST(EventVol, AllEventCountsZeroIsPlainLinearW_EvenWithPositiveEmove) {
  // n_lo == n_hi == n_query == 0 bypasses censoring even though emove > 0.
  const double w_lo = 0.05, T_lo = 0.1, w_hi = 0.20, T_hi = 0.4;
  const double T_query = 0.2;
  const double w = event_aware_w(w_lo, T_lo, 0, w_hi, T_hi, 0, T_query, 0, /*emove=*/0.05);
  const double weight_hi = (T_query - T_lo) / (T_hi - T_lo);  // 0.1/0.3
  const double expect = w_lo + weight_hi * (w_hi - w_lo);
  EXPECT_NEAR(w, expect, 1e-12);
}

// ── Session e2e (production-seam acceptance: event_vol.hpp through
//    VolaSession's build/serve path, not just the module's own header) ────

using atx::vol::data_install;
using atx::vol::iso_to_ns;
using atx::vol::make_synthetic_american_panel;
using atx::vol::S3Params;
using atx::vol::SessionInputs;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::TimeConvention;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::VolaSession;
using atx::vol::year_fraction;

// A 2-expiry, FLAT-smile (s2 = c2 = 0, so the true ATM vol IS the whole
// smile) synthetic panel, with expiry2's ATM vol constructed so its total
// variance embeds exactly ONE earnings event of a KNOWN eMove on top of a
// shared censored vol level -- the exact w_total(T) = sigma_C^2*T + n*eMove^2
// relation this module's header models (see the RoundTripKnownEmove /
// EventAwareInterpJumpAcrossEvent tests above for the same identity in
// isolation). `event_iso` places the schedule's one event relative to the
// two fitted expiries (2026-07-19 / 2026-12-19); the caller picks it to
// either bracket cleanly or fall outside the fitted range.
struct EventPanelFixture {
  SynthPanelSpec spec;
  std::int64_t now_ns = 0;
  std::int64_t event_ns = 0;
  double sigma_c = 0.20;
  double emove = 0.05;
};

[[nodiscard]] EventPanelFixture make_event_panel(const std::string& event_iso) {
  EventPanelFixture fx;
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {"2026-07-19", "2026-12-19"};  // ~0.10y, ~0.50y

  fx.spec.uid = "SYNTHEV";
  fx.spec.snapshot_iso = snapshot;
  fx.spec.spot = 100.0;
  fx.spec.r = 0.03;
  fx.spec.borrow = 0.0;

  const double T1 = year_fraction(snapshot, isos[0]);
  const double T2 = year_fraction(snapshot, isos[1]);
  // n1 = 0 (no event before expiry1), n2 = 1 (the schedule's one event
  // falls between expiry1 and expiry2 in the bracketing test) -- built
  // straight into the true ATM vols: sigma1 = sigma_c;
  // sigma2 = sqrt(sigma_c^2 + emove^2/T2).
  const double sigma2 =
      std::sqrt(fx.sigma_c * fx.sigma_c + (fx.emove * fx.emove) / T2);
  fx.spec.expiries.push_back(SynthExpiry{isos[0], T1, S3Params{fx.sigma_c, 0.0, 0.0}});
  fx.spec.expiries.push_back(SynthExpiry{isos[1], T2, S3Params{sigma2, 0.0, 0.0}});

  for (double K = 60.0; K <= 140.0 + 1e-9; K += 5.0) {
    fx.spec.strikes.push_back(K);
  }
  fx.spec.half_spread_frac = 0.02;

  fx.now_ns = iso_to_ns(snapshot);
  fx.event_ns = iso_to_ns(event_iso);
  return fx;
}

[[nodiscard]] const Underlying* install_event_panel(const SynthPanelSpec& spec, Universe& u) {
  const auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value());
  if (!panel) return nullptr;
  const auto uid = data_install(u, panel->frame);
  EXPECT_TRUE(uid.has_value());
  if (!uid) return nullptr;
  const auto under = u.get_underlying(*uid);
  EXPECT_TRUE(under.has_value());
  return under ? *under : nullptr;
}

[[nodiscard]] SessionInputs make_event_inputs(const EventPanelFixture& fx) {
  SessionInputs in;
  in.S = fx.spec.spot;
  in.r = fx.spec.r;
  in.now_ts_ns = fx.now_ns;
  in.deam.n_atm = 3;
  return in;
}

TEST(Session, ImpliedEmoveSolvedAndServed) {
  const EventPanelFixture fx = make_event_panel("2026-09-01");  // between the two expiries
  Universe u;
  const Underlying* under = install_event_panel(fx.spec, u);
  ASSERT_NE(under, nullptr);
  ASSERT_EQ(under->chains.size(), std::size_t{2});

  SessionInputs in_events = make_event_inputs(fx);
  in_events.events =
      std::make_shared<EventSchedule>(std::vector<std::int64_t>{fx.event_ns});
  const auto sess = VolaSession::build(*under, in_events);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double got_emove = sess->diagnostics().implied_emove;
  ASSERT_TRUE(std::isfinite(got_emove)) << "implied_emove was NaN";
  // Tol from de-Am + eSSVI LM fit noise (S3-truth ATM vols are only
  // approximately recovered by the fit, not exact recombination -- see the
  // module's own RoundTripKnownEmove test for the noiseless closed form).
  // Empirically the flat (no-skew) fixture recovers eMove to well under 1%
  // (nothing else competes for the ATM level), so 10% is a real regression
  // check, not a rubber stamp.
  EXPECT_NEAR(got_emove, fx.emove, fx.emove * 0.10) << "got=" << got_emove;

  // Plain (no-event) session off the SAME underlying for comparison. Note
  // `events` plays no part in the fit itself (SurfaceParityInputs has no
  // such field) -- only in post-fit diagnostics + serving -- so the two
  // sessions' fitted surfaces are otherwise identical.
  SessionInputs in_plain = make_event_inputs(fx);
  const auto sess_plain = VolaSession::build(*under, in_plain);
  ASSERT_TRUE(sess_plain.has_value()) << sess_plain.error().to_string();

  // A query strictly between expiry1 and the event date: event-aware
  // censoring must DEFER (not smoothly leak) expiry2's extra event variance
  // across the whole T1->T2 span the way a plain linear-in-w blend does, so
  // it reports LESS total variance here than the plain blend.
  const auto exps = sess->expiries();
  ASSERT_EQ(exps.size(), std::size_t{2});
  const double T1 = exps[0].T;
  const double T_event =
      static_cast<double>(fx.event_ns - fx.now_ns) / atx::vol::kCalendarYearNs;
  const double T_mid = T1 + 0.3 * (T_event - T1);
  ASSERT_GT(T_mid, T1);
  ASSERT_LT(T_mid, T_event);

  const double w_events = sess->total_variance(100.0, T_mid);
  const double w_plain = sess_plain->total_variance(100.0, T_mid);
  ASSERT_TRUE(std::isfinite(w_events));
  ASSERT_TRUE(std::isfinite(w_plain));
  EXPECT_LT(w_events, w_plain) << "w_events=" << w_events << " w_plain=" << w_plain;
}

TEST(Session, NoBracketingExpiriesLeavesEmoveNaN) {
  // Event scheduled AFTER the last fitted expiry: nothing to bracket.
  const EventPanelFixture fx = make_event_panel("2027-03-01");
  Universe u;
  const Underlying* under = install_event_panel(fx.spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in_events = make_event_inputs(fx);
  in_events.events =
      std::make_shared<EventSchedule>(std::vector<std::int64_t>{fx.event_ns});
  const auto sess = VolaSession::build(*under, in_events);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_TRUE(std::isnan(sess->diagnostics().implied_emove));

  SessionInputs in_plain = make_event_inputs(fx);
  const auto sess_plain = VolaSession::build(*under, in_plain);
  ASSERT_TRUE(sess_plain.has_value()) << sess_plain.error().to_string();

  // No error, and serving is BIT-IDENTICAL to the plain (non-event) session:
  // a failed solve must fall back exactly, never propagate NaN or a
  // fabricated event contribution into an otherwise-normal query.
  const auto exps = sess->expiries();
  const double T_mid = 0.5 * (exps[0].T + exps[1].T);
  EXPECT_EQ(sess->iv(100.0, T_mid), sess_plain->iv(100.0, T_mid));
  EXPECT_EQ(sess->total_variance(100.0, T_mid), sess_plain->total_variance(100.0, T_mid));
}

TEST(Session, VolTimeConventionDisablesEmoveSolve) {
  // eMove policy v1 is Calendar365-only (see SessionInputs::time / ::events
  // docs). `solve_implied_emove` synthesizes each fitted slice's absolute
  // expiry instant from its own T via `ns_from_year_fraction` -- the
  // Calendar365 INVERSE of time_to_expiry_years. Under VolTime a fitted T is
  // vol-time-shaped, so that synthesized instant is not the real listed
  // expiry and could mis-bucket a nearby event by days with no error. Reuse
  // the EXACT bracketing fixture from ImpliedEmoveSolvedAndServed (which
  // solves a finite eMove under the default Calendar365) to prove the
  // VolTime guard -- not some other fixture difference -- is what disables
  // the solve here.
  const EventPanelFixture fx = make_event_panel("2026-09-01");  // between the two expiries
  Universe u;
  const Underlying* under = install_event_panel(fx.spec, u);
  ASSERT_NE(under, nullptr);
  ASSERT_EQ(under->chains.size(), std::size_t{2});

  SessionInputs in_events = make_event_inputs(fx);
  in_events.events =
      std::make_shared<EventSchedule>(std::vector<std::int64_t>{fx.event_ns});
  in_events.time.convention = TimeConvention::VolTime;
  const auto sess = VolaSession::build(*under, in_events);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_TRUE(std::isnan(sess->diagnostics().implied_emove))
      << "got=" << sess->diagnostics().implied_emove;

  // A NaN emove must make serving inert, bit-identical to events == nullptr
  // (event_aware_active() gates on isfinite(implied_emove)) -- same
  // no-fabricated-fallback contract as NoBracketingExpiriesLeavesEmoveNaN.
  SessionInputs in_plain = make_event_inputs(fx);
  in_plain.time.convention = TimeConvention::VolTime;
  const auto sess_plain = VolaSession::build(*under, in_plain);
  ASSERT_TRUE(sess_plain.has_value()) << sess_plain.error().to_string();

  const auto exps = sess->expiries();
  ASSERT_EQ(exps.size(), std::size_t{2});
  const double T_mid = 0.5 * (exps[0].T + exps[1].T);
  EXPECT_EQ(sess->iv(100.0, T_mid), sess_plain->iv(100.0, T_mid));
  EXPECT_EQ(sess->total_variance(100.0, T_mid), sess_plain->total_variance(100.0, T_mid));
}

}  // namespace
