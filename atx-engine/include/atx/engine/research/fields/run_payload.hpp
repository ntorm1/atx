#pragma once

// atx::engine::research::fields -- a payload of the same fields run that a field reads (its
// registry row's `requires`; P9 lane A4): today shares_out.f64, which ftd_shares_ratio21 and the
// 13F ownership fields divide by (research_fields_holdings.py Ctx.so, "this run's shares_out").
// The Python reads the output directory's own file, pinned by the manifest being published; the
// engine reads the file the spec names, pinned by its SHA-256 (build_spec.hpp FileRequest): a
// date-major little-endian f64 matrix of the role's shape, read whole and hashed from the same
// bytes, with the file's stamp unchanged across the read.

#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

class RunPayload {
public:
  // Err(InvalidArgument) for bytes whose SHA-256 is not the pin (compared lower-case) or whose
  // size is not dates x instruments x 8 of `role`; Err(IoError) when it cannot be read or changed
  // while it was read.
  [[nodiscard]] static core::Result<RunPayload> load(const FileRequest &request,
                                                     const RoleAxes &role);

  // Row t (instruments values); precondition t < dates (asserted).
  [[nodiscard]] std::span<const f64> row(usize t) const noexcept;
  [[nodiscard]] usize dates() const noexcept { return dates_; }
  [[nodiscard]] usize columns() const noexcept { return columns_; }
  // {path, bytes, sha256} as a manifest entry's source records it.
  [[nodiscard]] const SourceRecord &source() const noexcept { return source_; }

private:
  RunPayload() = default;

  std::vector<f64> values_;
  usize dates_{};
  usize columns_{};
  SourceRecord source_;
};

} // namespace atx::engine::research::fields
