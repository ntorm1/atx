#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "pricing/american_family_error.hpp"
#include "pricing/american_iv_premium.hpp"

// Lane K3 (r5, 2026-10-22): the slow independent audit of the (sigma, q) family route's root
// acceptance (registered in ATX_VOL_SLOW_FILTER). Over randomized SPY- and S=100-shaped
// families at seven expiries, both sides, every root the family route accepts - family-native
// (deam_family_native) and B-family band (deam_confirm Band), each with the family's
// validated envelope in scope - is re-solved by bisection of the accurate map to 1e-9 (never
// the family) and must lie within 1e-7 of it. The tool path over the development cohorts is
// quote-bench --deam-audit with ATX_VOL_FAMILY_ROOT_AUDIT (deam_iv_cache.cpp).

namespace {

using atx::vol::AlExpiryGeometry;
using atx::vol::AlProfile;
using atx::vol::AlSigmaQFamily;
using atx::vol::american_implied_vol_premium;
using atx::vol::andersen_lake;
using atx::vol::FamilyEnvelopeScope;
using atx::vol::IvConfirmPolicy;
using atx::vol::IvOpts;
using atx::vol::IvPremiumFallback;
using atx::vol::IvPremiumGate;
using atx::vol::IvPremiumStats;
using atx::vol::IvResult;
using atx::vol::IvStatus;
using atx::vol::IvStrategy;
using atx::vol::kIvRootRequirement;
using atx::vol::kRootAcceptanceKindCount;
using atx::vol::prepare_al_geometry;
using atx::vol::RootAcceptanceKind;
using atx::vol::Side;
using atx::vol::ValidatedErrorEnvelope;

constexpr double kBisectWidth = 1.0e-9;

// The accurate map's root of `price` bracketed by bisection to kBisectWidth near `guess`.
[[nodiscard]] std::optional<std::pair<double, double>>
bisect_root(double price, double S, double K, double T, double r, double q, Side side,
            double guess) {
  const auto at = [&](double s) { return andersen_lake(S, K, T, s, r, q, side); };
  double h = kIvRootRequirement;
  double lo = guess - h;
  double hi = guess + h;
  for (int i = 0; i < 24; ++i) {
    const auto pl = at(lo);
    const auto ph = at(hi);
    if (!pl || !ph) return std::nullopt;
    if (*pl <= price && *ph >= price) break;
    h *= 4.0;
    if (*pl > price) lo = std::max(guess - h, 1.0e-3);
    if (*ph < price) hi = guess + h;
    if (i == 23) return std::nullopt;
  }
  for (int i = 0; i < 64 && hi - lo > kBisectWidth; ++i) {
    const double mid = 0.5 * (lo + hi);
    const auto pm = at(mid);
    if (!pm) return std::nullopt;
    (*pm > price ? hi : lo) = mid;
  }
  return std::pair<double, double>{lo, hi};
}

struct Shape {
  double S;
  double r;
  double q_lo;
  double q_hi;
};

struct Tally {
  std::array<std::size_t, kRootAcceptanceKindCount> kinds{};
  std::size_t audited = 0;
  std::size_t unresolved = 0;
  std::size_t violations = 0;
  double max_skipped = 0.0;
  double max_confirmed = 0.0;
};

} // namespace

TEST(AmericanFamilyRootAudit, EveryAcceptedFamilyRootIsWithinTheRequirementOfTheBisectedRoot) {
  std::mt19937_64 rng(0x4b33a0d17ull);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  const std::array<Shape, 2> shapes{Shape{600.0, 0.045, 0.0120, 0.0138},
                                    Shape{100.0, 0.040, 0.0100, 0.0120}};
  Tally native, band;
  for (const Shape &sh : shapes) {
    for (const double T : {0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0}) {
      const AlExpiryGeometry g = prepare_al_geometry(T, sh.r, 0.5 * (sh.q_lo + sh.q_hi));
      for (const Side side : {Side::Put, Side::Call}) {
        AlSigmaQFamily fam(g, side, sh.S);
        ASSERT_TRUE(fam.prepare(0.05, 1.2, 16, sh.q_lo, sh.q_hi, 4));
        const ValidatedErrorEnvelope env = atx::vol::build_family_error_envelope(fam);
        IvPremiumGate native_gate;
        native_gate.qfamily = &fam;
        native_gate.qfamily_native = true;
        IvPremiumGate band_gate;
        band_gate.qfamily = &fam;
        band_gate.policy = IvConfirmPolicy::Band;
        band_gate.half_spread_usd = 0.05;
        for (int i = 0; i < 150; ++i) {
          const double K = sh.S * std::exp(std::log(0.7) + std::log(1.3 / 0.7) * u01(rng));
          const double sigma = 0.08 * std::exp(std::log(10.0) * u01(rng));
          const double q = sh.q_lo + (sh.q_hi - sh.q_lo) * u01(rng);
          const auto p = andersen_lake(sh.S, K, T, sigma, sh.r, q, side);
          if (!p || !(*p > 1.0e-3)) continue;
          for (const bool is_native : {true, false}) {
            Tally &t = is_native ? native : band;
            IvOpts o;
            o.strategy = IvStrategy::Premium;
            o.seed_profile = AlProfile::Screen;
            IvPremiumStats st;
            IvResult res{};
            {
              const FamilyEnvelopeScope scope(&env);
              res = american_implied_vol_premium(*p, sh.S, K, T, sh.r, q, side, o, nullptr, st,
                                                 is_native ? &native_gate : &band_gate);
            }
            ASSERT_EQ(res.status, IvStatus::Ok);
            ++t.kinds[static_cast<std::size_t>(st.acceptance.kind)];
            if (st.fallback != IvPremiumFallback::None || st.seed_routed) continue;
            if (st.acceptance.kind == RootAcceptanceKind::None) continue;
            ++t.audited;
            const auto ref = bisect_root(*p, sh.S, K, T, sh.r, q, side, res.iv);
            if (!ref) {
              ++t.unresolved;
              continue;
            }
            const double err = std::max(std::abs(res.iv - ref->first),
                                        std::abs(res.iv - ref->second));
            const bool skipped = st.acceptance.kind == RootAcceptanceKind::Skipped;
            double &worst = skipped ? t.max_skipped : t.max_confirmed;
            worst = std::max(worst, err);
            if (!(err <= kIvRootRequirement)) {
              ++t.violations;
              ADD_FAILURE() << (is_native ? "native" : "band") << " S " << sh.S << " T " << T
                            << " side " << static_cast<int>(side) << " K " << K << " sigma "
                            << sigma << " q " << q << " kind "
                            << static_cast<int>(st.acceptance.kind) << " reason "
                            << static_cast<int>(st.acceptance.reason) << " err " << err;
            }
          }
        }
      }
    }
  }
  for (const bool is_native : {true, false}) {
    const Tally &t = is_native ? native : band;
    std::printf("[K3 root audit] %s: kinds none %zu confirmed %zu skipped %zu price-only %zu "
                "interval %zu; %zu audited, %zu unresolved, %zu violations, max |served - "
                "root| skipped %.3g confirmed %.3g\n",
                is_native ? "native" : "band", t.kinds[0], t.kinds[1], t.kinds[2], t.kinds[3],
                t.kinds[4], t.audited, t.unresolved, t.violations, t.max_skipped,
                t.max_confirmed);
    EXPECT_GT(t.audited, 1000u);
    EXPECT_GT(t.kinds[static_cast<std::size_t>(RootAcceptanceKind::Skipped)], 100u);
    EXPECT_EQ(t.unresolved, 0u);
    EXPECT_EQ(t.violations, 0u);
  }
}
