# Exposure implementation boundary and bounded header audit

Production `2058f3a579856dc350aab11e3bd87b69145aada6` moves non-template
exposure calculations from `risk/exposures.hpp` into
`atx-engine/src/risk/exposures.cpp`. Root must add exactly that source to the
atx-engine library. No CMake, ledger, other owner's source, compiler settings,
PCH layout or build cache was changed in this lane. No build was launched.

## Implemented boundary

All sixteen detail math helpers and both build_exposures overloads retain their
signatures, namespaces and external availability. This matters because the factor
builder/hybrid/capacity paths use step_return, fundamental factors use market_returns
and sector_groups, and existing diagnostics/tests call beta, beta_cached,
sqrt_cap_weight and apply_industry_sum_to_zero. They were not hidden in an anonymous
namespace or renamed. Constants, enums, config member order/defaults, ExposureMatrix,
PitSideInputs and trivial shape/accessor bodies remain in the header.

The header drops only its now-unused algorithm/cmath includes; the new source has
direct standard and project includes. Existing public type dependency headers are
retained for compatibility. No extra Pimpl or Eigen/layout redesign is part of this
slice. The local header falls from 1008 to 550 lines; root carries two extra unchanged
R2 policy include/member lines. Those root-only config additions are outside the
patch hunks. Future exposure math body edits now affect this translation unit and
normal links instead of invalidating every textual consumer of the header.

After implementation, a bounded source script extracted all 18 original function
signatures/bodies from root `2e3981011478085829596143ebbd905d1283697a` and compared
them with the new source. Every signature matches after removing inline, and every
body matches exactly with LF newlines, including evaluation/reduction order and
comments. See `exposures-extraction-body-receipt.json` for per-body SHA256 values.
The new source LF SHA256 is
`d9f0b0aaa4d438c9cf23bfead18b3261840ee3aec44e7672564c8b39d528c9f3`.
This is source equivalence evidence, not a compiler/runtime byte-parity claim.
`git diff --check` passed. No new implementation-mirroring tests were added for the
mechanical move; existing owning checks already assert meaningful invariants.

Relevant existing qualification includes RiskExposures (size, missing cap, zscore,
windows, sector/order/degenerate and bad-shape cases), RiskFactorModelPit (lagged
clock, future mutation, cap/group dates, winsor/order and missing-side coverage),
RiskFundamentalFactors.HoistedBetaIsBitIdenticalToLegacy and GoldenDeterminismHash,
and robust regression's normalized sqrt-cap and industry-sum-to-zero checks.
Root owns the combined compile/runtime and PCH-off checks; all remain pending here.

## Source-only audit and ownership handoff

Graph tools were unavailable, so discovery used targeted rg and a read-only quoted
include graph over tracked first-party code. At root 2e398101, exposures.hpp had
7 direct header includers and 13 direct C++ includers, with additional fanout through
factor_model.hpp. This is a source relationship count, not an active Ninja action
count or a measured latency result.

factor_model.hpp already has out-of-line math and a private implementation; its
remaining large parse chain comes through exposures.hpp and Eigen-backed public
value types. Repeating its existing Pimpl work would add churn. A later lightweight
risk-config/type split could reduce that chain further, but is outside this move.

qp_solver.hpp still carries roughly 1300 lines of non-template solver bodies plus
kkt_ldl/qp_factor_admm and private formatting dependencies. Root owns its analogous
private implementation extraction and must preserve scheduled/unscheduled solves,
certificates and exact defaults. Pool4 confirmed R3 does not edit qp_solver.hpp or
exposures.hpp; its optimizer change adds declarations and implements the new path
in cost_terms.cpp. No overlap was introduced.

optimizer.hpp still contains non-template legacy projection/dispatch bodies, but
its active R3 declaration work makes a simultaneous move unnecessary in this batch.
Most CPCV arithmetic is already in cpcv.cpp; the dominant remaining risk was config
consumers pulling the public geometry/combinatorics header through config.hpp. Root
owns the lightweight CPCV/PBO/IC configuration-header split. No second CPCV algorithm
rewrite was proposed or performed.

Compilation latency, new dirty-action counts, no-op/cache behavior and numeric
parity are not inferred from file sizes. The one-time header boundary change still
requires dependent recompilation; savings apply to later implementation-only edits.
