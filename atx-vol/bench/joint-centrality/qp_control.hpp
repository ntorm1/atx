#pragma once

#include "corrector.hpp"

#include <cstddef>
#include <span>

namespace atx::vol::research {

// Private, bounded control of the actual production Newton factor/direction.
// A is row-major, with s.size() rows and variables columns; 1<=n<=4, 1<=m<=32.
struct JointHomogeneousInput {
  std::size_t variables = 0;
  std::span<const double> a;
  std::span<const double> s, z, rc;
};

struct JointHomogeneousWork {
  // Actual calls attempted, including numerical refusal. Validation resets
  // both counts before inspecting input and makes no factor/direction attempt.
  std::size_t factors = 0, directions = 0;
};

// All borrowed spans remain live through the call. Writable spans must be
// mutually disjoint and disjoint from every input span. Input aliasing is valid.
// Finite A/rc and strictly positive finite s/z are required. Shapes/domain are
// validated; aliasing is a caller precondition. Success publishes all outputs.
// False leaves every output byte unchanged; allocation failure is contained.
// Available only in an ATX_VOL_JOINT_CENTRALITY_RESEARCH build, never public API.
[[nodiscard]] bool solve_joint_homogeneous_control(const JointHomogeneousInput &input,
    CentralityDirection output, JointHomogeneousWork &work);

} // namespace atx::vol::research
