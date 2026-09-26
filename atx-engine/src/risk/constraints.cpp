#include "atx/engine/risk/constraints.hpp"

#include <cmath>
#include <limits>

namespace atx::engine::risk {
namespace {
using atx::usize;
namespace co = atx::core;

[[nodiscard]] co::Result<usize> bounded_add(usize a, usize b, usize limit) {
  if (a > limit || b > limit - a)
    return co::Err(co::ErrorCode::OutOfRange, "constraints: dimension/resource bound exceeded");
  return co::Ok(a + b);
}
[[nodiscard]] co::Result<usize> bounded_product(usize a, usize b, usize limit) {
  if (b != 0 && a > limit / b)
    return co::Err(co::ErrorCode::OutOfRange, "constraints: dimension/resource product exceeded");
  return co::Ok(a * b);
}
} // namespace

atx::core::Status MaterializedConstraints::validate_layout(atx::usize instruments) const {
  if (storage.rule != ConstraintStorageRule::LegacyDenseV1 &&
      storage.rule != ConstraintStorageRule::SparseCsrV2)
    return co::Err(co::ErrorCode::InvalidArgument, "constraints: unknown storage rule");
  if (!sparse()) {
    if ((A.rows() > 0 && static_cast<usize>(A.cols()) != instruments) ||
        l.size() != A.rows() || u.size() != A.rows())
      return co::Err(co::ErrorCode::InvalidArgument, "constraints: dense geometry mismatch");
    return co::Ok(); // preserve the V1 valid-domain numerical recipe
  }
  const auto rows = static_cast<usize>(l.size());
  constexpr auto index_limit = static_cast<usize>(std::numeric_limits<int>::max()) / 4;
  if (rows > index_limit || instruments > index_limit || storage.max_solver_bytes == 0 ||
      csr.columns != instruments || u.size() != l.size() || A.size() != 0 ||
      rows == std::numeric_limits<usize>::max() || csr.row_offsets.size() != rows + 1 ||
      csr.column_indices.size() != csr.values.size() || csr.row_offsets.front() != 0 ||
      csr.row_offsets.back() != csr.values.size() || csr.box_begin > rows ||
      csr.box_count > rows - csr.box_begin ||
      (csr.box_count != 0 && csr.box_count != instruments) ||
      csr.values.size() > storage.max_nnz || csr.box_count > storage.max_nnz - csr.values.size())
    return co::Err(co::ErrorCode::InvalidArgument, "constraints: invalid CSR geometry/resource bounds");
  const auto byte_limit = static_cast<usize>(std::min<atx::u64>(
      storage.max_materialization_bytes, std::numeric_limits<usize>::max()));
  ATX_TRY(auto bytes, bounded_add(rows, instruments, byte_limit));
  ATX_TRY(bytes, bounded_product(bytes, 256, byte_limit));
  ATX_TRY(const auto coefficient_bytes, bounded_product(csr.values.size(), 32, byte_limit));
  ATX_TRY(bytes, bounded_add(bytes, coefficient_bytes, byte_limit));
  ATX_TRY(bytes, bounded_add(bytes, sizeof(MaterializedConstraints), byte_limit));
  (void)bytes;
  for (usize row = 0; row < rows; ++row) {
    const auto begin = csr.row_offsets[row], end = csr.row_offsets[row + 1];
    if (begin > end || end > csr.values.size() || !std::isfinite(l[static_cast<Eigen::Index>(row)]) ||
        !std::isfinite(u[static_cast<Eigen::Index>(row)]) ||
        l[static_cast<Eigen::Index>(row)] > u[static_cast<Eigen::Index>(row)] ||
        (row >= csr.box_begin && row - csr.box_begin < csr.box_count && begin != end))
      return co::Err(co::ErrorCode::InvalidArgument, "constraints: invalid CSR row/bounds");
    for (auto entry = begin; entry < end; ++entry) {
      if (csr.column_indices[entry] >= instruments || !std::isfinite(csr.values[entry]) ||
          csr.values[entry] == 0.0 ||
          (entry > begin && csr.column_indices[entry - 1] >= csr.column_indices[entry]))
        return co::Err(co::ErrorCode::InvalidArgument, "constraints: unsorted/nonfinite CSR coefficient");
    }
  }
  return co::Ok();
}

atx::usize MaterializedConstraints::stored_nonzeros() const noexcept {
  if (sparse()) return csr.values.size() + csr.box_count;
  usize count = 0;
  for (Eigen::Index j = 0; j < A.cols(); ++j)
    for (Eigen::Index i = 0; i < A.rows(); ++i) if (A(i, j) != 0.0) ++count;
  return count;
}

atx::core::Status MaterializedConstraints::validate_factor_workspace(
    usize instruments, usize factors, usize general_rows) const {
  if (!sparse()) return co::Ok();
  const auto limit = static_cast<usize>(std::min<atx::u64>(
      storage.max_solver_bytes, std::numeric_limits<usize>::max()));
  ATX_TRY(auto width, bounded_add(factors, general_rows, limit));
  ATX_TRY(width, bounded_add(width, 2, limit)); // gross/turnover polish equalities
  ATX_TRY(auto cells, bounded_product(instruments, width, limit));
  ATX_TRY(const auto capacitance, bounded_product(width, width, limit));
  ATX_TRY(cells, bounded_add(cells, capacitance, limit));
  ATX_TRY(cells, bounded_add(cells, instruments, limit));
  ATX_TRY(cells, bounded_add(cells, row_count(), limit));
  // Sixteen f64 working copies cover ad/xl/W, capacitances, LLT/Schur,
  // active-set polish and vectors. This bounds the remaining dense fallback;
  // it is not a claim that arbitrary general constraints solve in O(nnz).
  ATX_TRY(const auto bytes, bounded_product(cells, 128, limit));
  (void)bytes;
  return co::Ok();
}

atx::core::Status MaterializedConstraints::validate_augmented_workspace(
    usize instruments, usize factors) const {
  if (!sparse()) return co::Ok();
  constexpr auto limit = static_cast<usize>(std::numeric_limits<int>::max()) / 4;
  const bool gross = gross_l1_budget >= 0.0;
  const bool turn = has_turnover || turnover_penalty > 0.0;
  const bool robust_on = robust.active && robust.kappa > 0.0;
  ATX_TRY(auto columns, bounded_add(instruments, factors, limit));
  ATX_TRY(columns, bounded_add(columns, gross ? instruments : 0U, limit));
  ATX_TRY(columns, bounded_add(columns, turn ? instruments : 0U, limit));
  ATX_TRY(columns, bounded_add(columns, robust_on ? 1U : 0U, limit));
  ATX_TRY(auto rows, bounded_add(factors, row_count(), limit));
  ATX_TRY(const auto split_rows, bounded_product(instruments, 3, limit));
  if (gross) { ATX_TRY(rows, bounded_add(rows, split_rows + 1, limit)); }
  if (turn) { ATX_TRY(rows, bounded_add(rows, split_rows + (has_turnover ? 1U : 0U), limit)); }
  ATX_TRY(const auto cone_rows, bounded_add(instruments, factors, limit));
  if (tracking.active) { ATX_TRY(rows, bounded_add(rows, cone_rows, limit)); }
  usize sector_cones = 0;
  if (sector_risk.active)
    for (const auto sigma : sector_risk.sigma) if (sigma > 0.0) ++sector_cones;
  ATX_TRY(const auto sector_rows, bounded_product(sector_cones, cone_rows, limit));
  ATX_TRY(rows, bounded_add(rows, sector_rows, limit));
  if (robust_on) { ATX_TRY(rows, bounded_add(rows, factors + 1, limit)); }

  ATX_TRY(const auto factor_square, bounded_product(factors, factors, limit));
  ATX_TRY(auto nnz, bounded_product(factors, instruments + 1, limit));
  ATX_TRY(nnz, bounded_add(nnz, stored_nonzeros(), limit));
  ATX_TRY(const auto split_nnz, bounded_product(instruments, 6, limit));
  if (gross) { ATX_TRY(nnz, bounded_add(nnz, split_nnz, limit)); }
  if (turn) { ATX_TRY(nnz, bounded_add(nnz, split_nnz, limit)); }
  if (tracking.active) {
    ATX_TRY(nnz, bounded_add(nnz, factor_square, limit));
    ATX_TRY(nnz, bounded_add(nnz, instruments, limit));
  }
  // Each sector cone currently emits K+M rows; no hidden simplification of its
  // existing solver recipe. Its coefficient bound is deliberately conservative.
  ATX_TRY(const auto sector_width, bounded_product(factors + 1, instruments, limit));
  ATX_TRY(const auto sector_nnz, bounded_product(sector_cones, sector_width, limit));
  ATX_TRY(nnz, bounded_add(nnz, sector_nnz, limit));
  if (robust_on) { ATX_TRY(nnz, bounded_add(nnz, factor_square + 1, limit)); }
  ATX_TRY(nnz, bounded_product(nnz, 2, limit)); // symmetric off-diagonal KKT blocks
  ATX_TRY(nnz, bounded_add(nnz, instruments, limit));
  ATX_TRY(nnz, bounded_add(nnz, factor_square, limit));
  ATX_TRY(const auto dimension, bounded_add(rows, columns, limit));
  ATX_TRY(nnz, bounded_add(nnz, dimension, limit));
  ATX_TRY(const auto amd_extra, bounded_product(dimension, 2, limit));
  ATX_TRY(auto amd, bounded_add(nnz, nnz / 5, limit));
  ATX_TRY(amd, bounded_add(amd, amd_extra, limit));
  ATX_TRY(const auto amd_work, bounded_product(dimension + 1, 8, limit));
  (void)amd; (void)amd_work;
  // Half is reserved for sparse/AMD/iterate copies; the other half permits
  // two separately capped LDL factors. No (rows+columns)^2 index assumption.
  const auto bytes_limit = static_cast<usize>(std::min<atx::u64>(
      storage.max_solver_bytes / 2, std::numeric_limits<usize>::max()));
  ATX_TRY(auto bytes, bounded_product(nnz, 256, bytes_limit));
  ATX_TRY(const auto vectors, bounded_product(dimension, 2048, bytes_limit));
  ATX_TRY(bytes, bounded_add(bytes, vectors, bytes_limit));
  (void)bytes;
  return co::Ok();
}

void MaterializedConstraints::put_coefficient(usize row, usize column, atx::f64 value) {
  if (!sparse()) {
    A(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(column)) = value;
  } else if (!(row >= csr.box_begin && row - csr.box_begin < csr.box_count) && value != 0.0) {
    // The materializer reserved an upper bound before entering any emitter.
    csr.column_indices.push_back(column);
    csr.values.push_back(value);
    ++csr.row_offsets[row + 1];
  }
}

void MaterializedConstraints::finish_sparse_rows() noexcept {
  if (sparse())
    for (usize i = 1; i < csr.row_offsets.size(); ++i) csr.row_offsets[i] += csr.row_offsets[i - 1];
}

atx::core::Result<MaterializedConstraints> ConstraintSet::materialize(
    const atx::core::linalg::MatX& X, std::span<const atx::f64> w_prev, usize M,
    const CapacityRef& ref) const {
  ATX_TRY_VOID(validate(X, M));
  if (storage.rule != ConstraintStorageRule::LegacyDenseV1 &&
      storage.rule != ConstraintStorageRule::SparseCsrV2)
    return co::Err(co::ErrorCode::InvalidArgument, "constraints: unknown storage rule");
  if (storage.rule == ConstraintStorageRule::SparseCsrV2) {
    const auto all_finite = [](const auto& values) {
      return std::all_of(values.begin(), values.end(), [](auto value) { return std::isfinite(value); });
    };
    if (!std::isfinite(gross.gross_leverage) || (pos && !std::isfinite(pos->name_cap)) ||
        (part && !std::isfinite(part->adv_frac)) || (own && !std::isfinite(own->shares_frac)) ||
        (turn && !std::isfinite(turn->max_turnover)) ||
        (fexp && !all_finite(fexp->bound)) || (grp && !all_finite(grp->cap)) ||
        (beta && (!std::isfinite(beta->tol) || !all_finite(beta->beta))) ||
        (sector && (!all_finite(sector->cap) || !all_finite(sector->sigma))) ||
        (!w_prev.empty() && w_prev.size() != M) || !all_finite(w_prev))
      return co::Err(co::ErrorCode::InvalidArgument, "constraints: nonfinite or misaligned sparse descriptor");
    if (grp)
      for (const auto id : grp->group_id)
        if (id >= grp->cap.size())
          return co::Err(co::ErrorCode::InvalidArgument, "constraints: invalid group id");
    if (sector)
      for (const auto id : sector->sector_id)
        if (id >= (sector->soc ? sector->sigma.size() : sector->cap.size()))
          return co::Err(co::ErrorCode::InvalidArgument, "constraints: invalid sector id");
  }
  constexpr auto index_limit = static_cast<usize>(std::numeric_limits<int>::max()) / 4;
  if (M > index_limit)
    return co::Err(co::ErrorCode::OutOfRange, "constraints: instrument index bound exceeded");
  usize rows = gross.dollar_neutral ? 1U : 0U;
  ATX_TRY(rows, bounded_add(rows, has_box() ? M : 0U, index_limit));
  ATX_TRY(rows, bounded_add(rows, fexp ? fexp->factor_cols.size() : 0U, index_limit));
  ATX_TRY(rows, bounded_add(rows, grp ? group_count() : 0U, index_limit));
  ATX_TRY(rows, bounded_add(rows, beta ? 1U : 0U, index_limit));
  ATX_TRY(rows, bounded_add(rows, sector && !sector->soc ? sector_count() : 0U, index_limit));

  MaterializedConstraints mc;
  mc.storage = storage;
  const auto er = static_cast<Eigen::Index>(rows), em = static_cast<Eigen::Index>(M);
  if (mc.sparse()) {
    if (storage.max_materialization_bytes == 0 || storage.max_solver_bytes == 0)
      return co::Err(co::ErrorCode::InvalidArgument, "constraints: sparse byte budgets must be positive");
    // Group/sector partitions contribute at most one coefficient per name,
    // independent of their group count. Factor/beta/net rows may be dense.
    usize row_equivalents = (gross.dollar_neutral ? 1U : 0U) + (beta ? 1U : 0U) +
        (grp ? 1U : 0U) + (sector && !sector->soc ? 1U : 0U);
    ATX_TRY(row_equivalents, bounded_add(row_equivalents,
        fexp ? fexp->factor_cols.size() : 0U, index_limit));
    ATX_TRY(const auto stored, bounded_product(M, row_equivalents, storage.max_nnz));
    ATX_TRY(const auto logical, bounded_add(stored, has_box() ? M : 0U, storage.max_nnz));
    (void)logical;
    const auto byte_limit = static_cast<usize>(std::min<atx::u64>(
        storage.max_materialization_bytes, std::numeric_limits<usize>::max()));
    // Includes result metadata/capacity, bounds, CSR, turnover/cone snapshots,
    // elastic row lists and container overhead; no R*M allocation is hidden here.
    ATX_TRY(auto work, bounded_add(rows, M, byte_limit));
    ATX_TRY(work, bounded_product(work, 256, byte_limit));
    ATX_TRY(const auto coefficient_bytes, bounded_product(stored, 32, byte_limit));
    ATX_TRY(work, bounded_add(work, coefficient_bytes, byte_limit));
    if (robust) {
      ATX_TRY(const auto omega_bytes, bounded_product(
          static_cast<usize>(robust->omega_f.size()), 16, byte_limit));
      ATX_TRY(work, bounded_add(work, omega_bytes, byte_limit));
    }
    ATX_TRY(work, bounded_add(work, sizeof(MaterializedConstraints), byte_limit));
    (void)work;
    mc.csr.columns = M;
    mc.csr.box_begin = gross.dollar_neutral ? 1U : 0U;
    mc.csr.box_count = has_box() ? M : 0U;
    mc.csr.row_offsets.assign(rows + 1, 0);
    mc.csr.column_indices.reserve(stored);
    mc.csr.values.reserve(stored);
  } else {
    mc.A = co::linalg::MatX::Zero(er, em);
  }
  mc.l = co::linalg::VecX::Zero(er);
  mc.u = co::linalg::VecX::Zero(er);
  Eigen::Index next = 0;
  emit_dollar_neutral(mc, M, next);
  emit_position_box(mc, M, ref, next);
  emit_factor_exposure(mc, X, M, next);
  emit_group(mc, M, next);
  emit_beta(mc, M, next);
  emit_sector(mc, M, next);
  mc.finish_sparse_rows();
  mc.gross_l1_budget = gross.gross_leverage;
  fill_turnover(mc, w_prev, M);
  fill_tracking(mc, M);
  fill_sector_soc(mc, M);
  fill_robust(mc);
  fill_elastic(mc, M);
  ATX_TRY_VOID(mc.validate_layout(M));
  return co::Ok(std::move(mc));
}
} // namespace atx::engine::risk
