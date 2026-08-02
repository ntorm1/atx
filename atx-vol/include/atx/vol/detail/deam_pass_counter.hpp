#pragma once

// Provisional C1 instrumentation — de-Americanization *pass* counters.
//
// The eSSVI session build historically de-Americanized every expiry TWICE: once
// in the fit (`prepare_legacy` / `run_surface_parity`) and once more in the
// certification/diagnostics pass (`collect_input_diagnostics` ->
// `build_observations_european`). C1 removes the second pass by reusing the
// fit's own de-Am. This tiny facility lets the C1 proof test COUNT the two
// passes and prove the reduction (fit + cert = 2 per slice -> fit only = 1),
// without waiting on WS-V's richer solve ledger (`atx/vol/detail/counters.hpp`, a
// sibling worktree, not on this branch). Both counters are thread-safe relaxed
// atomics incremented once per per-slice de-Am pass — negligible next to a de-Am
// — and are namespaced apart from WS-V so the PM can fold this into V1's ledger
// at merge with no collision. Not on any hot inner loop.

#include <atomic>
#include <cstdint>

namespace atx::vol::detail {

// Distinct tallies so a test can assert the CERT pass specifically drops to
// zero (the C1 win) while the FIT pass is unchanged (fit path byte-identical).
inline std::atomic<std::uint64_t> g_fit_deam_slice_passes{0};
inline std::atomic<std::uint64_t> g_cert_deam_slice_passes{0};

inline void note_fit_deam_slice_pass() noexcept {
  g_fit_deam_slice_passes.fetch_add(1u, std::memory_order_relaxed);
}
inline void note_cert_deam_slice_pass() noexcept {
  g_cert_deam_slice_passes.fetch_add(1u, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t fit_deam_slice_passes() noexcept {
  return g_fit_deam_slice_passes.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t cert_deam_slice_passes() noexcept {
  return g_cert_deam_slice_passes.load(std::memory_order_relaxed);
}

inline void reset_deam_slice_passes() noexcept {
  g_fit_deam_slice_passes.store(0u, std::memory_order_relaxed);
  g_cert_deam_slice_passes.store(0u, std::memory_order_relaxed);
}

} // namespace atx::vol::detail
