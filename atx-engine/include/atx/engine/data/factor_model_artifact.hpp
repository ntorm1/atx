#pragma once

// atx::engine::data — FactorModelArtifact: a BYO (bring-your-own) factored
// covariance block (S6.6; on-disk seam added p8-S1-0).
//
// Lives in data:: (NOT risk::) to avoid a risk→data include edge. All matrices
// are owned by value. Validation is fully delegated to risk::FactorModel::create
// (via artifact_to_factor_model in adapt_factor.hpp) — the single source of
// truth for shape / SPD / fit-window checks.
//
// ===========================================================================
//  p8-S1-0: the on-disk / cross-stage seam
// ===========================================================================
//  stage_riskmodel (atx-impl, p8-S1-1) is a SEPARATE deploy stage from
//  stage_optimize / the combine path: it builds a FactorModelArtifact per
//  fit-window and must hand it to later stages/processes. serialize_artifact /
//  deserialize_artifact are the byte-level round-trip (a flat little-endian
//  layout: shape header, then X, F, D column-major f64 payloads, then the two
//  usize fit-window bounds); digest_artifact is the content digest used by the
//  determinism contract's "twice-run -> same artifact bytes" test class. The
//  round-trip contract is BYTE-IDENTICAL: serialize -> deserialize reproduces
//  every f64 cell exactly (memcpy of the IEEE-754 bit pattern, not a
//  text/lossy encoding), and re-serializing a deserialized artifact reproduces
//  the same bytes (idempotent).
//
//  NO RNG, no clock, no map iteration anywhere in this seam — same artifact ->
//  same bytes -> same digest, run to run, process to process (portable only to
//  other little-endian hosts, matching serialize_panel.hpp's documented scope).

#include <cmath>   // std::sqrt (correlation<->covariance rescale)
#include <cstring> // std::memcpy (bit-exact f64 round-trip)
#include <vector>

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/hash.hpp"          // hash_bytes (wyhash; the content digest)
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // usize, u64, u8

#include "atx/engine/risk/psd_repair.hpp" // eigenvalue_clip (strict-PD floor)
#include "atx/engine/risk/shrinkage.hpp"  // constant_correlation_shrinkage, mp_clip

namespace atx::engine::data {

// A BYO factor model block. Owns its matrices by value.
//
//   X  — M×K exposure matrix
//   F  — K×K factor covariance (must be SPD; validated on lowering)
//   D  — M specific variances (must be > 0; validated on lowering)
//   fit_begin / fit_end — the fit window [fit_begin, fit_end) forwarded to
//                         risk::FactorModel::create (require fit_begin < fit_end)
//
// Construct this struct directly (aggregate init) and pass to
// artifact_to_factor_model to produce a risk::FactorModel.
struct FactorModelArtifact {
  atx::core::linalg::MatX X;    // M×K exposures
  atx::core::linalg::MatX F;    // K×K factor covariance (must be SPD)
  atx::core::linalg::VecX D;    // M specific variances (must be > 0)
  atx::usize fit_begin = 0;
  atx::usize fit_end   = 0;
};

// ---------------------------------------------------------------------------
//  serialize_artifact / deserialize_artifact — the byte-level round-trip.
// ---------------------------------------------------------------------------
//
// Layout (all integers little-endian, all floats raw IEEE-754 bit patterns):
//   u32 kSchemaVersion
//   u64 M (X.rows() == D.size())
//   u64 K (X.cols() == F.rows() == F.cols())
//   u64 fit_begin
//   u64 fit_end
//   f64[M*K]  X, COLUMN-MAJOR (matches atx::core::linalg::MatX storage)
//   f64[K*K]  F, COLUMN-MAJOR
//   f64[M]    D
//
// A schema version bump is append-only (old readers reject a newer version via
// the explicit check in deserialize_artifact; this sprint ships version 1 only).
inline constexpr atx::u32 kFactorModelArtifactSchemaVersion = 1U;

[[nodiscard]] inline std::vector<atx::u8> serialize_artifact(const FactorModelArtifact &a) {
  const atx::u64 m = static_cast<atx::u64>(a.X.rows());
  const atx::u64 k = static_cast<atx::u64>(a.X.cols());
  const atx::usize header_bytes = sizeof(atx::u32) + 4U * sizeof(atx::u64);
  const atx::usize payload_cells =
      static_cast<atx::usize>(m * k) + static_cast<atx::usize>(k * k) + static_cast<atx::usize>(m);
  std::vector<atx::u8> out;
  out.reserve(header_bytes + payload_cells * sizeof(atx::f64));

  auto put_u32 = [&out](atx::u32 v) {
    for (int b = 0; b < 4; ++b) {
      out.push_back(static_cast<atx::u8>((v >> (8 * b)) & 0xFFU));
    }
  };
  auto put_u64 = [&out](atx::u64 v) {
    for (int b = 0; b < 8; ++b) {
      out.push_back(static_cast<atx::u8>((v >> (8 * b)) & 0xFFU));
    }
  };
  auto put_f64 = [&out](atx::f64 v) {
    atx::u8 raw[sizeof(atx::f64)];
    std::memcpy(raw, &v, sizeof(atx::f64));
    out.insert(out.end(), raw, raw + sizeof(atx::f64)); // host is little-endian (x86_64/ARM64)
  };

  put_u32(kFactorModelArtifactSchemaVersion);
  put_u64(m);
  put_u64(k);
  put_u64(static_cast<atx::u64>(a.fit_begin));
  put_u64(static_cast<atx::u64>(a.fit_end));

  // Column-major traversal matches MatX's native storage order (Eigen default).
  for (Eigen::Index c = 0; c < a.X.cols(); ++c) {
    for (Eigen::Index r = 0; r < a.X.rows(); ++r) {
      put_f64(a.X(r, c));
    }
  }
  for (Eigen::Index c = 0; c < a.F.cols(); ++c) {
    for (Eigen::Index r = 0; r < a.F.rows(); ++r) {
      put_f64(a.F(r, c));
    }
  }
  for (Eigen::Index i = 0; i < a.D.size(); ++i) {
    put_f64(a.D[i]);
  }
  return out;
}

// Deserialize bytes produced by serialize_artifact. Err(ParseError) on a short
// buffer or a shape whose declared cell count does not fit the remaining bytes;
// Err(InvalidArgument) on an unrecognized schema version (forward-compat guard —
// this sprint ships version 1 only, so any other value is rejected rather than
// silently misread).
[[nodiscard]] inline atx::core::Result<FactorModelArtifact>
deserialize_artifact(const std::vector<atx::u8> &bytes) {
  const atx::usize header_bytes = sizeof(atx::u32) + 4U * sizeof(atx::u64);
  if (bytes.size() < header_bytes) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "deserialize_artifact: buffer shorter than the fixed header");
  }
  atx::usize off = 0;
  auto get_u32 = [&bytes, &off]() -> atx::u32 {
    atx::u32 v = 0;
    for (int b = 0; b < 4; ++b) {
      v |= static_cast<atx::u32>(bytes[off + static_cast<atx::usize>(b)]) << (8 * b);
    }
    off += 4U;
    return v;
  };
  auto get_u64 = [&bytes, &off]() -> atx::u64 {
    atx::u64 v = 0;
    for (int b = 0; b < 8; ++b) {
      v |= static_cast<atx::u64>(bytes[off + static_cast<atx::usize>(b)]) << (8 * b);
    }
    off += 8U;
    return v;
  };

  const atx::u32 version = get_u32();
  if (version != kFactorModelArtifactSchemaVersion) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "deserialize_artifact: unrecognized schema version");
  }
  const atx::u64 m = get_u64();
  const atx::u64 k = get_u64();
  const atx::u64 fit_begin = get_u64();
  const atx::u64 fit_end = get_u64();

  const atx::usize payload_cells =
      static_cast<atx::usize>(m * k) + static_cast<atx::usize>(k * k) + static_cast<atx::usize>(m);
  const atx::usize expected_total = header_bytes + payload_cells * sizeof(atx::f64);
  if (bytes.size() != expected_total) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "deserialize_artifact: declared shape does not match buffer length");
  }

  auto get_f64 = [&bytes, &off]() -> atx::f64 {
    atx::f64 v = 0.0;
    std::memcpy(&v, bytes.data() + off, sizeof(atx::f64));
    off += sizeof(atx::f64);
    return v;
  };

  FactorModelArtifact a;
  a.X = atx::core::linalg::MatX(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(k));
  for (Eigen::Index c = 0; c < a.X.cols(); ++c) {
    for (Eigen::Index r = 0; r < a.X.rows(); ++r) {
      a.X(r, c) = get_f64();
    }
  }
  a.F = atx::core::linalg::MatX(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(k));
  for (Eigen::Index c = 0; c < a.F.cols(); ++c) {
    for (Eigen::Index r = 0; r < a.F.rows(); ++r) {
      a.F(r, c) = get_f64();
    }
  }
  a.D = atx::core::linalg::VecX(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < a.D.size(); ++i) {
    a.D[i] = get_f64();
  }
  a.fit_begin = static_cast<atx::usize>(fit_begin);
  a.fit_end = static_cast<atx::usize>(fit_end);
  return atx::core::Ok(std::move(a));
}

// Content digest of the artifact's serialized bytes (wyhash via
// atx::core::hash_bytes — process-stable, not cross-process/platform-portable;
// matches the scope of every other digest primitive in this codebase). Used by
// the S1 determinism contract's "twice-run -> same artifact bytes" test class:
// same artifact -> same serialized bytes -> same digest, deterministically.
[[nodiscard]] inline atx::u64 digest_artifact(const FactorModelArtifact &a) {
  const std::vector<atx::u8> bytes = serialize_artifact(a);
  return atx::core::hash_bytes(bytes.data(), bytes.size());
}

// ===========================================================================
//  p8-S1-3: cleaned_alpha_cov — the combine-visible shrunk/denoised covariance.
// ===========================================================================
//  SEAM, NOT A stage_combine.cpp EDIT (binding — see the sprint-1 ledger's
//  "Sprint-3 seam handoff" note). stage_combine.cpp:755 today fits blend
//  weights on `combine::detail::mle_covariance(centered, na)` — an UNSHRUNK
//  MLE covariance that is noise-dominated exactly when N ≈ T (the combiner's
//  own regime: N alphas over a T-period fit window). cleaned_alpha_cov is the
//  PURE accessor Sprint 3 (which owns stage_combine.cpp) calls behind
//  RiskModelConfig.kind==Factor to replace that raw sample covariance — S1
//  ships the accessor + its own unit tests only; the call site threading is
//  Sprint 3's edit, keeping file ownership disjoint (S1 never touches
//  stage_combine.cpp).
//
//  Pipeline (REUSES the existing S8.7 toolkit verbatim — no new estimator
//  math, per the sprint's frozen-math contract):
//    1. constant_correlation_shrinkage(centered) -> a Ledoit-Wolf-shrunk
//       covariance S_shrunk (shrinkage.hpp). PSD by construction (a convex
//       blend of the PSD sample covariance and a PD-ish constant-correlation
//       target); positive-definite even at N>T where the raw sample
//       covariance is rank-deficient.
//    2. Convert S_shrunk to a correlation matrix (divide by the per-name
//       stddevs) and Marchenko-Pastur eigen-clip it (mp_clip, q=N/T — the
//       exact aspect ratio of `centered`) to flatten the noise-bulk
//       eigenvalues toward their average, trace-preserving.
//    3. Rescale the cleaned correlation back to a covariance (multiply by the
//       ORIGINAL per-name stddevs — the shrinkage step already set the scale;
//       only the correlation structure is MP-cleaned) and run it through
//       eigenvalue_clip (psd_repair.hpp) as the FINAL strict-PD floor, so the
//       result is always Cholesky-factorizable even if the two prior steps'
//       floating-point reassembly left a borderline-singular direction.
//
//  Contract: `centered` is a T×N COLUMN-DEMEANED alpha-return window (each
//  column's own time-mean already subtracted — same contract as
//  constant_correlation_shrinkage / combine::detail::mle_covariance, so a
//  caller migrating off mle_covariance passes the SAME centered matrix
//  unchanged). Degenerate inputs (T<2, N<1, a degenerate stddev column) fall
//  back gracefully exactly as constant_correlation_shrinkage / mp_clip /
//  eigenvalue_clip already do (documented in shrinkage.hpp / psd_repair.hpp);
//  this wrapper adds no additional fallible path, so it returns MatX directly
//  (non-fallible), matching mle_covariance's own non-fallible signature — a
//  drop-in replacement shape for the Sprint-3 call site.
//
//  Determinism: pure function of `centered`. NO RNG, no clock, no map
//  iteration (every composed step is itself order-fixed deterministic). Same
//  input -> byte-identical output, run to run and process to process.
[[nodiscard]] inline atx::core::linalg::MatX
cleaned_alpha_cov(const atx::core::linalg::MatX &centered) {
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;

  const Eigen::Index t = centered.rows();
  const Eigen::Index n = centered.cols();

  // Step 1: Ledoit-Wolf constant-correlation shrinkage (PSD by construction).
  const MatX shrunk = atx::engine::risk::constant_correlation_shrinkage(centered);

  if (t < 2 || n < 1) {
    return shrunk; // degenerate window: shrinkage already returned S verbatim
  }

  // Step 2: shrunk covariance -> correlation, MP-clip, -> covariance.
  VecX sd(n);
  bool degenerate_scale = false;
  for (Eigen::Index i = 0; i < n; ++i) {
    const atx::f64 v = shrunk(i, i);
    if (v <= 0.0) {
      degenerate_scale = true; // a non-positive diagonal -> correlation form is undefined
      break;
    }
    sd[i] = std::sqrt(v);
  }
  if (degenerate_scale) {
    // Skip the correlation round-trip (would divide by a non-positive
    // variance); still pass through the final strict-PD floor below.
    return atx::engine::risk::eigenvalue_clip(shrunk, 1e-12);
  }

  MatX corr(n, n);
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      corr(i, j) = shrunk(i, j) / (sd[i] * sd[j]);
    }
  }

  const atx::f64 q = static_cast<atx::f64>(n) / static_cast<atx::f64>(t);
  const auto clipped_r = atx::engine::risk::mp_clip(corr, q);
  const MatX clipped_corr = clipped_r.has_value() ? *clipped_r : corr; // graceful fallback

  MatX cleaned_cov(n, n);
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      cleaned_cov(i, j) = clipped_corr(i, j) * sd[i] * sd[j];
    }
  }

  // Step 3: final strict-PD floor (Cholesky-safe for FactorModel::create /
  // any downstream min-variance solve).
  return atx::engine::risk::eigenvalue_clip(cleaned_cov, 1e-12);
}

} // namespace atx::engine::data
