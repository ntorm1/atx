#pragma once

// slice_param_padding — test-only helpers for building slice params whose
// PADDING bytes are a chosen filler while every VALUE member is preserved.
//
// `EssviParams`/`SviParams`/`C8Params` are serialized by both surface-archive
// writers as their OBJECT representation, which is WIDER than their members
// (`SviParams` has 6 tail bytes after `expiry_id`; eSSVI and C8 have an interior
// gap AND a tail gap). Nothing in the fitters ever writes those pad bytes, so a
// live struct carries whatever the producing thread's stack left there, and a
// verbatim blit used to put that residue in the record — see
// `src/slice_payload_padding.hpp` for the defect and the fix.
//
// `repad(src, pad)` memsets a whole object representation to `pad` and then
// assigns every value member back, so the returned object differs from `src` in
// its padding and NOWHERE else. Two surfaces built with different `pad` values
// must therefore serialize to identical bytes. `pad == 0x00` is the canonical
// (already-normalized) representation.
//
// A member added to one of these structs and not assigned here makes the
// padding-blindness tests fail loudly rather than silently — the same property
// the arity tripwires in `src/slice_payload_padding.hpp` enforce at compile time.
//
// Test-only header, included by `surface_archive_test.cpp` (v1 writer) and
// `surface_archive_v2_test.cpp` (v2 writer) so the two padding-blindness tests
// share one definition of "differs only in padding".

#include <cstring>

#include "atx/vol/c8.hpp"          // C8Params
#include "atx/vol/vol_surface.hpp" // EssviParams, SviParams

namespace atx::vol::test {

[[nodiscard]] inline SviParams repad(const SviParams &src, unsigned char pad) noexcept {
  SviParams out;
  std::memset(&out, pad, sizeof out);
  out.a = src.a;
  out.b = src.b;
  out.rho = src.rho;
  out.m = src.m;
  out.sigma = src.sigma;
  out.T = src.T;
  out.F = src.F;
  out.expiry_ns = src.expiry_ns;
  out.expiry_id = src.expiry_id;
  return out;
}

[[nodiscard]] inline EssviParams repad(const EssviParams &src, unsigned char pad) noexcept {
  EssviParams out;
  std::memset(&out, pad, sizeof out);
  out.theta = src.theta;
  out.phi = src.phi;
  out.rho = src.rho;
  out.rho_R = src.rho_R;
  out.rho_scale = src.rho_scale;
  out.psi = src.psi;
  out.p = src.p;
  out.lambda = src.lambda;
  out.lambda_R = src.lambda_R;
  out.T = src.T;
  out.F = src.F;
  out.expiry_ns = src.expiry_ns;
  out.expiry_id = src.expiry_id;
  out.resid_coef = src.resid_coef;
  out.resid_scale = src.resid_scale;
  out.resid_basis_kind = src.resid_basis_kind;
  out.resid_n_basis = src.resid_n_basis;
  return out;
}

[[nodiscard]] inline C8Params repad(const C8Params &src, unsigned char pad) noexcept {
  C8Params out;
  std::memset(&out, pad, sizeof out);
  out.T = src.T;
  out.F = src.F;
  out.expiry_ns = src.expiry_ns;
  out.expiry_id = src.expiry_id;
  out.v = src.v;
  out.psi = src.psi;
  out.p = src.p;
  out.c = src.c;
  out.v_min = src.v_min;
  out.kappa = src.kappa;
  out.q_L = src.q_L;
  out.q_R = src.q_R;
  out.h_atm = src.h_atm;
  out.k_L = src.k_L;
  out.h_L = src.h_L;
  out.k_R = src.k_R;
  out.h_R = src.h_R;
  out.arb_damping_factor = src.arb_damping_factor;
  out.rmse_price = src.rmse_price;
  out.rmse_vol = src.rmse_vol;
  out.n_lm_iters = src.n_lm_iters;
  out.n_irls_iters = src.n_irls_iters;
  out.bumps_active = src.bumps_active;
  return out;
}

} // namespace atx::vol::test
