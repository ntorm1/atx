#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"
#include "support/oracle_pricer_pde.hpp"

// Sub-Sprint A, Task A5 — negative-rate / double-boundary (Healy) domain map.
//
// Primary sources: J. Healy, "Pricing American options under negative rates" (arXiv
// 2109.15157); "Double sweep LU decomposition for American options under negative
// rates" (arXiv 2203.08794); Battauz-De Donno-Sbuelz, Mgmt Sci 61(5) 2015. Under a
// negative internal-put rate with a strictly smaller yield (yield < rate <= 0) the
// American put exercise region is NOT downward-connected: a SECOND exercise boundary
// appears (a "double continuation region"). The single-boundary Andersen-Lake scheme
// cannot represent two boundaries.
//
// This suite is the checked-in DOMAIN MAP: it verifies, per side, that the engine's
// capability predicate (detail::classify_regime, the single source of truth in
// american.hpp, reached via the McDonald-Schroder internal-put map) exactly matches
// what a Crank-Nicolson FD oracle says is priceable:
//   * American   (put r>0 / call q>0, incl. negative OPPOSITE-carry): AL prices it and
//                matches the FD oracle within the economic bound.
//   * European   (rate<=0 && rate<=yield): early exercise never optimal; AL == European,
//                matches the FD oracle.
//   * Unsupported(yield<rate<=0): the double-continuation region — AL returns an explicit
//                NotImplemented (never a silent wrong price), and the FD oracle confirms a
//                genuine early-exercise value exists there, so the bail is CORRECT, not
//                over-conservative.
//
// Outcome for the A5 gate: the current single-boundary engine already classifies
// correctly and bails cleanly on the double-boundary regime — there is NO regime where
// it silently mis-prices. Implementing a second boundary is therefore NOT warranted by a
// real pricing error; this map + evidence is the valid complete outcome. If a future
// need arises, the double-sweep LU (arXiv 2203.08794) is the reference method and the
// Unsupported predicate below is where it would attach.

namespace atx::vol {
namespace {

using atx::vol::detail::classify_regime;
using atx::vol::detail::ExerciseRegime;
using atx::vol::test::oracle_pde_american;

// Internal-put (rate, yield) for a side: put is (r, q); call maps via
// C(S,K,r,q) = P(K,S,q,r) to (q, r).
[[nodiscard]] ExerciseRegime regime_for(double r, double q, Side side) {
  const double rate = (side == Side::Put) ? r : q;
  const double yield = (side == Side::Put) ? q : r;
  return classify_regime(rate, yield);
}

// European price under (S,K,T,sigma,r,q) via Black-76 (forward + discount).
[[nodiscard]] double european_price(double S, double K, double T, double sigma, double r, double q,
                                    Side side) {
  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  return black76_price(F, K, T, sigma, df, side);
}

struct Cell {
  double S, r, q;
  Side side;
  ExerciseRegime expect;
};

// Map every cell's classification and assert the engine's actual behavior matches.
TEST(NegRateDomainMap, EngineBehaviorMatchesCapabilityPredicatePerSide) {
  const double K = 100.0, T = 1.0, sigma = 0.28;
  const Cell cells[] = {
      // ── American: put r>0 (any q), incl. negative opposite-carry q<0 ──────
      {90.0, 0.05, 0.02, Side::Put, ExerciseRegime::American},
      {90.0, 0.05, -0.03, Side::Put, ExerciseRegime::American}, // negative-carry American
      {80.0, 0.03, 0.00, Side::Put, ExerciseRegime::American},
      // ── American: call q>0 (any r), incl. negative opposite-carry r<0 ─────
      {120.0, 0.02, 0.05, Side::Call, ExerciseRegime::American},
      {120.0, -0.03, 0.05, Side::Call, ExerciseRegime::American}, // negative-carry American
      // ── European: rate<=0 && rate<=yield -> American == European ──────────
      {85.0, -0.02, 0.04, Side::Put, ExerciseRegime::European},
      {90.0, 0.00, 0.05, Side::Put, ExerciseRegime::European},
      {115.0, 0.04, -0.02, Side::Call, ExerciseRegime::European},
      {110.0, 0.05, 0.00, Side::Call, ExerciseRegime::European},
      // ── Unsupported: yield<rate<=0 -> double continuation region ──────────
      {70.0, -0.005, -0.03, Side::Put, ExerciseRegime::Unsupported},
      {75.0, -0.01, -0.04, Side::Put, ExerciseRegime::Unsupported},
      {140.0, -0.03, -0.005, Side::Call, ExerciseRegime::Unsupported},
  };

  int n_american = 0, n_european = 0, n_unsupported = 0;
  for (const Cell &c : cells) {
    // (1) The capability predicate classifies exactly as the map claims.
    ASSERT_EQ(regime_for(c.r, c.q, c.side), c.expect)
        << "S=" << c.S << " r=" << c.r << " q=" << c.q;

    const Result<double> al = andersen_lake(c.S, K, T, sigma, c.r, c.q, c.side);
    const double fd = oracle_pde_american(c.S, K, T, sigma, c.r, c.q, c.side);
    ASSERT_TRUE(std::isfinite(fd)) << "FD oracle failed";

    switch (c.expect) {
    case ExerciseRegime::American:
    case ExerciseRegime::European: {
      // (2) A priceable regime: AL succeeds and matches the FD oracle.
      ASSERT_TRUE(al.has_value())
          << "AL should price a " << (c.expect == ExerciseRegime::American ? "American" : "European")
          << " regime; S=" << c.S << " r=" << c.r << " q=" << c.q;
      if (fd > 0.05) {
        EXPECT_LT(std::abs(*al - fd) / fd, 5.0e-3)
            << "AL vs FD; S=" << c.S << " r=" << c.r << " q=" << c.q;
      }
      (c.expect == ExerciseRegime::American) ? ++n_american : ++n_european;
      break;
    }
    case ExerciseRegime::Unsupported: {
      // (3) The double-continuation region: AL bails EXPLICITLY (no silent wrong price),
      // and the FD oracle confirms a real early-exercise premium exists over the
      // European value -> the bail is correct, not over-conservative.
      ASSERT_FALSE(al.has_value()) << "AL must NOT price the double-continuation regime";
      EXPECT_EQ(al.error().code(), atx::core::ErrorCode::NotImplemented);
      const double euro = european_price(c.S, K, T, sigma, c.r, c.q, c.side);
      EXPECT_GT(fd, euro + 0.005)
          << "double-continuation early-exercise value should be material; FD=" << fd
          << " euro=" << euro;
      ++n_unsupported;
      break;
    }
    }
  }
  // The map covers all three regimes on both sides.
  EXPECT_GE(n_american, 4);
  EXPECT_GE(n_european, 4);
  EXPECT_GE(n_unsupported, 3);
}

} // namespace
} // namespace atx::vol
