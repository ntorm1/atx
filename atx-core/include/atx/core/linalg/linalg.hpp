#pragma once

// atx::core::linalg — thin typed bridge to Eigen.
//
// Purpose:
//   This module is the *only* place the rest of atx depends on Eigen. Code
//   elsewhere uses these aliases (Vec2, MatX, VecMap, …) and the span bridges,
//   never `Eigen::…` directly. That keeps the dependency on a single seam: the
//   numeric backend could be swapped, or the aliases re-tuned (scalar type,
//   storage order), without touching call sites.
//
// Conventions:
//   - Scalar type is `double` throughout. Quant math is f64; we do not template
//     the aliases on scalar to keep the surface small and the ABI predictable.
//   - Storage is column-major (Eigen's default), so a flat buffer {a,b,c,d}
//     viewed as a 2x2 fills column 0 first: [[a,c],[b,d]].
//   - Dimensions use `Eigen::Index` (signed). std::span sizes are std::size_t;
//     we cast explicitly at the boundary (no implicit narrowing, /Wconversion
//     clean).
//
// Zero-copy bridges:
//   as_vector / as_matrix wrap an existing buffer in an Eigen::Map. They copy
//   nothing — the Map aliases the caller's storage (see // SAFETY: notes).

#include <span>

#include <Eigen/Dense>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core::linalg {

// ---- Fixed-size vectors (double, column-major) ----
using Vec2 = Eigen::Vector2d;
using Vec3 = Eigen::Vector3d;
using Vec4 = Eigen::Vector4d;

// ---- Dynamic-size vector ----
using VecX = Eigen::VectorXd;

// ---- Fixed-size matrices ----
using Mat2 = Eigen::Matrix2d;
using Mat3 = Eigen::Matrix3d;
using Mat4 = Eigen::Matrix4d;

// ---- Dynamic-size matrix ----
using MatX = Eigen::MatrixXd;

// ---- Zero-copy views over external memory ----
// A Map borrows a contiguous buffer and presents it as an Eigen expression.
// Mutable variants permit write-through; const variants are read-only views.
using VecMap = Eigen::Map<VecX>;
using MatMap = Eigen::Map<MatX>;
using VecMapConst = Eigen::Map<const VecX>;
using MatMapConst = Eigen::Map<const MatX>;

// View a span as a dynamic Eigen vector of length s.size().
//
// SAFETY: the returned Map aliases `s.data()`; it owns nothing and must not
// outlive the span's backing storage. Writes through the Map mutate that
// storage. The static_cast is a widening of std::size_t -> Eigen::Index for a
// length that fits comfortably in a signed index on any realistic buffer.
[[nodiscard]] inline VecMap as_vector(std::span<double> s) noexcept {
    return VecMap(s.data(), static_cast<Eigen::Index>(s.size()));
}

// Const overload: a read-only vector view over an immutable span.
//
// SAFETY: as above — non-owning view; must not outlive `s`.
[[nodiscard]] inline VecMapConst as_vector(std::span<const double> s) noexcept {
    return VecMapConst(s.data(), static_cast<Eigen::Index>(s.size()));
}

// View a span as a rows x cols column-major matrix.
//
// Precondition: rows * cols == s.size(). Checked with ATX_ASSERT (debug).
//
// SAFETY: the returned Map aliases `s.data()`; non-owning, must not outlive the
// span's backing storage. Writes through the Map mutate that storage.
[[nodiscard]] inline MatMap as_matrix(std::span<double> s, Eigen::Index rows, Eigen::Index cols) {
    ATX_ASSERT(rows >= 0 && cols >= 0);
    ATX_ASSERT(static_cast<usize>(rows) * static_cast<usize>(cols) == s.size());
    return MatMap(s.data(), rows, cols);
}

// Const overload: a read-only rows x cols column-major matrix view.
//
// Precondition: rows * cols == s.size(). Checked with ATX_ASSERT (debug).
//
// SAFETY: as above — non-owning view; must not outlive `s`.
[[nodiscard]] inline MatMapConst
as_matrix(std::span<const double> s, Eigen::Index rows, Eigen::Index cols) {
    ATX_ASSERT(rows >= 0 && cols >= 0);
    ATX_ASSERT(static_cast<usize>(rows) * static_cast<usize>(cols) == s.size());
    return MatMapConst(s.data(), rows, cols);
}

} // namespace atx::core::linalg
