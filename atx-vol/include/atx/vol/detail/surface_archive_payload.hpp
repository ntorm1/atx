#pragma once

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "atx/vol/c8.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol::detail {

// The native parameter structs are the historical archive ABI, including their
// sizes and member offsets. Their padding is not state, however: copying the
// whole object representation leaks indeterminate bytes into CRCs and makes an
// otherwise identical archive depend on stack/thread history. Keep the wire
// geometry, clear every payload byte, then write only named fields.
static_assert(std::is_standard_layout_v<EssviParams>);
static_assert(std::is_standard_layout_v<SviParams>);
static_assert(std::is_standard_layout_v<C8Params>);

static_assert(sizeof(EssviParams) == 248);
static_assert(offsetof(EssviParams, theta) == 0);
static_assert(offsetof(EssviParams, phi) == 8);
static_assert(offsetof(EssviParams, rho) == 16);
static_assert(offsetof(EssviParams, rho_R) == 24);
static_assert(offsetof(EssviParams, rho_scale) == 32);
static_assert(offsetof(EssviParams, psi) == 40);
static_assert(offsetof(EssviParams, p) == 48);
static_assert(offsetof(EssviParams, lambda) == 56);
static_assert(offsetof(EssviParams, lambda_R) == 64);
static_assert(offsetof(EssviParams, T) == 72);
static_assert(offsetof(EssviParams, F) == 80);
static_assert(offsetof(EssviParams, expiry_ns) == 88);
static_assert(offsetof(EssviParams, expiry_id) == 96);
static_assert(offsetof(EssviParams, resid_coef) == 104);
static_assert(offsetof(EssviParams, resid_scale) == 232);
static_assert(offsetof(EssviParams, resid_basis_kind) == 240);
static_assert(offsetof(EssviParams, resid_n_basis) == 241);

static_assert(sizeof(SviParams) == 72);
static_assert(offsetof(SviParams, a) == 0);
static_assert(offsetof(SviParams, b) == 8);
static_assert(offsetof(SviParams, rho) == 16);
static_assert(offsetof(SviParams, m) == 24);
static_assert(offsetof(SviParams, sigma) == 32);
static_assert(offsetof(SviParams, T) == 40);
static_assert(offsetof(SviParams, F) == 48);
static_assert(offsetof(SviParams, expiry_ns) == 56);
static_assert(offsetof(SviParams, expiry_id) == 64);

static_assert(sizeof(C8Params) == 176);
static_assert(offsetof(C8Params, T) == 0);
static_assert(offsetof(C8Params, F) == 8);
static_assert(offsetof(C8Params, expiry_ns) == 16);
static_assert(offsetof(C8Params, expiry_id) == 24);
static_assert(offsetof(C8Params, v) == 32);
static_assert(offsetof(C8Params, psi) == 40);
static_assert(offsetof(C8Params, p) == 48);
static_assert(offsetof(C8Params, c) == 56);
static_assert(offsetof(C8Params, v_min) == 64);
static_assert(offsetof(C8Params, kappa) == 72);
static_assert(offsetof(C8Params, q_L) == 80);
static_assert(offsetof(C8Params, q_R) == 88);
static_assert(offsetof(C8Params, h_atm) == 96);
static_assert(offsetof(C8Params, k_L) == 104);
static_assert(offsetof(C8Params, h_L) == 112);
static_assert(offsetof(C8Params, k_R) == 120);
static_assert(offsetof(C8Params, h_R) == 128);
static_assert(offsetof(C8Params, arb_damping_factor) == 136);
static_assert(offsetof(C8Params, rmse_price) == 144);
static_assert(offsetof(C8Params, rmse_vol) == 152);
static_assert(offsetof(C8Params, n_lm_iters) == 160);
static_assert(offsetof(C8Params, n_irls_iters) == 164);
static_assert(offsetof(C8Params, bumps_active) == 168);

template <class T>
void write_archive_field(std::byte *dst, std::size_t offset, const T &value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(dst + offset, &value, sizeof value);
}

inline void write_archive_payload(std::byte *dst, const EssviParams &src) noexcept {
  std::memset(dst, 0, sizeof src);
  write_archive_field(dst, offsetof(EssviParams, theta), src.theta);
  write_archive_field(dst, offsetof(EssviParams, phi), src.phi);
  write_archive_field(dst, offsetof(EssviParams, rho), src.rho);
  write_archive_field(dst, offsetof(EssviParams, rho_R), src.rho_R);
  write_archive_field(dst, offsetof(EssviParams, rho_scale), src.rho_scale);
  write_archive_field(dst, offsetof(EssviParams, psi), src.psi);
  write_archive_field(dst, offsetof(EssviParams, p), src.p);
  write_archive_field(dst, offsetof(EssviParams, lambda), src.lambda);
  write_archive_field(dst, offsetof(EssviParams, lambda_R), src.lambda_R);
  write_archive_field(dst, offsetof(EssviParams, T), src.T);
  write_archive_field(dst, offsetof(EssviParams, F), src.F);
  write_archive_field(dst, offsetof(EssviParams, expiry_ns), src.expiry_ns);
  write_archive_field(dst, offsetof(EssviParams, expiry_id), src.expiry_id);
  write_archive_field(dst, offsetof(EssviParams, resid_coef), src.resid_coef);
  write_archive_field(dst, offsetof(EssviParams, resid_scale), src.resid_scale);
  write_archive_field(dst, offsetof(EssviParams, resid_basis_kind), src.resid_basis_kind);
  write_archive_field(dst, offsetof(EssviParams, resid_n_basis), src.resid_n_basis);
}

inline void write_archive_payload(std::byte *dst, const SviParams &src) noexcept {
  std::memset(dst, 0, sizeof src);
  write_archive_field(dst, offsetof(SviParams, a), src.a);
  write_archive_field(dst, offsetof(SviParams, b), src.b);
  write_archive_field(dst, offsetof(SviParams, rho), src.rho);
  write_archive_field(dst, offsetof(SviParams, m), src.m);
  write_archive_field(dst, offsetof(SviParams, sigma), src.sigma);
  write_archive_field(dst, offsetof(SviParams, T), src.T);
  write_archive_field(dst, offsetof(SviParams, F), src.F);
  write_archive_field(dst, offsetof(SviParams, expiry_ns), src.expiry_ns);
  write_archive_field(dst, offsetof(SviParams, expiry_id), src.expiry_id);
}

inline void write_archive_payload(std::byte *dst, const C8Params &src) noexcept {
  std::memset(dst, 0, sizeof src);
  write_archive_field(dst, offsetof(C8Params, T), src.T);
  write_archive_field(dst, offsetof(C8Params, F), src.F);
  write_archive_field(dst, offsetof(C8Params, expiry_ns), src.expiry_ns);
  write_archive_field(dst, offsetof(C8Params, expiry_id), src.expiry_id);
  write_archive_field(dst, offsetof(C8Params, v), src.v);
  write_archive_field(dst, offsetof(C8Params, psi), src.psi);
  write_archive_field(dst, offsetof(C8Params, p), src.p);
  write_archive_field(dst, offsetof(C8Params, c), src.c);
  write_archive_field(dst, offsetof(C8Params, v_min), src.v_min);
  write_archive_field(dst, offsetof(C8Params, kappa), src.kappa);
  write_archive_field(dst, offsetof(C8Params, q_L), src.q_L);
  write_archive_field(dst, offsetof(C8Params, q_R), src.q_R);
  write_archive_field(dst, offsetof(C8Params, h_atm), src.h_atm);
  write_archive_field(dst, offsetof(C8Params, k_L), src.k_L);
  write_archive_field(dst, offsetof(C8Params, h_L), src.h_L);
  write_archive_field(dst, offsetof(C8Params, k_R), src.k_R);
  write_archive_field(dst, offsetof(C8Params, h_R), src.h_R);
  write_archive_field(dst, offsetof(C8Params, arb_damping_factor), src.arb_damping_factor);
  write_archive_field(dst, offsetof(C8Params, rmse_price), src.rmse_price);
  write_archive_field(dst, offsetof(C8Params, rmse_vol), src.rmse_vol);
  write_archive_field(dst, offsetof(C8Params, n_lm_iters), src.n_lm_iters);
  write_archive_field(dst, offsetof(C8Params, n_irls_iters), src.n_irls_iters);
  write_archive_field(dst, offsetof(C8Params, bumps_active), src.bumps_active);
}

} // namespace atx::vol::detail
