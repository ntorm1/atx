#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>

#include "atx/vol/arb.hpp"             // arb_check_calendar(CurveSurface, ...)
#include "atx/vol/surface_archive.hpp" // SurfaceArchive
#include "support/cached_artifacts.hpp"

// By-construction calendar no-arb gate for the SERVED dense surface.
//
// `fit_curve_surface` walks expiries ascending-T; each fitted slice's total-
// variance curve w(k) becomes the CALENDAR FLOOR for the next (longer) expiry's
// fit (the per-node floor in `fit_convex_slice`). So a longer-dated slice's
// total variance can never dip below a shorter-dated one's AT THE NODES, and the
// served `CurveSurface` should be calendar-arb-free.
//
// Reloads the shared cached SPY ConvexDense archive (see cached_artifacts.hpp)
// instead of re-fitting live: cached_spy_convex_dense() fits with the SAME
// recipe this test used to build inline (Fast preset -> SessionInputs ->
// SurfaceParityInputs 1:1, CalendarRepair::None, al_fast_opts + iv_tol 1e-5 +
// n_atm 1, default ConvexDense node_cap 40 — see session.cpp's
// apply_fit_preset(Fast) and VolaSession::build), and the archive round-trip
// (spy_archive_roundtrip_test) proves the reload's CurveSurface is bit-
// identical to the live fit's, so `arb_check_calendar` below sees the exact
// same calendar residuals — the property under test lives in the artifact.
// The parquet is gitignored; if it is absent the test SKIPS (never triggers a
// Databento pull).

namespace {

using atx::vol::arb_check_calendar;
using atx::vol::SurfaceArchiveV2;

}  // namespace

TEST(CurveSurfaceNoArb, SpyDenseIsCalendarArbFree) {
  const auto archive_path = atx::vol::test::cached_spy_convex_dense();
  if (archive_path.empty()) {
    GTEST_SKIP() << "cached SPY OPRA parquet not found; run the databento pull + "
                    "opra_dbn_to_parquet to materialise the fixture.";
  }

  auto arch = SurfaceArchiveV2::open_file(archive_path.string());
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto recon = arch->reconstruct_symbol("SPY");
  ASSERT_TRUE(recon.has_value()) << recon.error().to_string();

  auto viol = arb_check_calendar(recon->surface(), -0.6, 0.6, 64);
  ASSERT_TRUE(viol.has_value()) << viol.error().to_string();

  // Dump every residual crossing (k, T_prev, T_curr, slack = w_prev - w_curr).
  for (const auto& v : *viol) {
    std::printf(
        "[SPY dense calendar residual] k=%.4f T_prev=%.4f T_curr=%.4f "
        "slack=%.3e\n",
        v.k_log, v.T1, v.T2, v.slack);
  }

  // The previous node-only floor left two real between-node crossings on this
  // board. The production fitter now scans this same shared lattice and promotes
  // every residual breach to an exact QP node, so zero is the admission gate.
  EXPECT_TRUE(viol->empty())
      << "shared-k calendar projection left a served-surface crossing";
}
