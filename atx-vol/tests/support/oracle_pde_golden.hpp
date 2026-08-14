#pragma once

// Golden-value front end for the Crank-Nicolson PDE oracle (test-only).
//
// oracle_pde_american() is a pure function and every test call site passes
// compile-time literals, so its values are constants. Recomputing the
// 2000x4000 (or 4000x8000) CN march at test runtime cost ~5 min of debug
// wall per run. This shim serves the values from a committed TSV instead.
//
// Regenerate (release build, single process) after ANY oracle change:
//   Step 1: Empty the table (regen only fills misses, not cache hits):
//     Clear-Content atx-vol/tests/support/oracle_pde_golden.tsv
//   Step 2: Run with regen enabled:
//     $env:ATX_VOL_ORACLE_REGEN = "1"
//     build-rel/bin/atx-vol-tests.exe --gtest_filter=<affected suites>
//     Remove-Item Env:ATX_VOL_ORACLE_REGEN
// then commit the updated oracle_pde_golden.tsv. Keys are exact %.17g
// round-trips of the inputs; any drift in inputs is a MISS, never a stale
// hit. Duplicate appended keys are ignored (first wins).

#include "atx/vol/api/core/types.hpp"
#include "oracle_pricer_pde.hpp"

namespace atx::vol::test {

[[nodiscard]] double oracle_pde_golden(double S, double K, double T,
                                       double sigma, double r, double q,
                                       Side side,
                                       const OraclePdeOpts& opts = {});

}  // namespace atx::vol::test
