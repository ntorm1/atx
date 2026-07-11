#pragma once

// Process-local + on-disk cache for the fitted SPY ConvexDense (Fast preset,
// node_cap=40) surface. Five consumer tests independently re-fit the IDENTICAL
// 14k-contract SPY board every run; this helper fits it once per build tree
// (or reuses an already-published archive from a prior run) and hands back the
// archive file path, which every consumer reloads via
// `SurfaceArchive::open_file` -> `map_symbol("SPY")` exactly as
// spy_archive_roundtrip_test.cpp does. That round-trip is itself the proof the
// reload prices bit-identically to a live session (see that test), so this
// helper introduces no accuracy gap for consumers that only need the fitted
// surface (fv/iv/greeks) rather than live `VolaSession` state.
//
// Fit gates (which exercise the FITTER itself — cold-start timing, auto-select
// policy, archive serialize/reload correctness, worker-count parity) must NOT
// use this helper; they still fit live by design.

#include <filesystem>

namespace atx::vol::test {

// Path to a fitted+serialized SPY ConvexDense (Fast preset, node_cap=40)
// surface for the 2026-06-05 OPRA fixture. Fits once per build tree, then
// reloads. Empty path if the parquet fixture is unavailable (caller should
// GTEST_SKIP, same as every direct-fit consumer does today).
[[nodiscard]] std::filesystem::path cached_spy_convex_dense();

}  // namespace atx::vol::test
