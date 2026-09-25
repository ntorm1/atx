# W0 D-12 production membership integration

Branch: `feat/w0-membership-integration-codex-20260925` in `C:\atx-wt\pool-5`.
Base: `bc5cc646b46f6a7c23a60e87d28dfa9b972671ec`.
Run: `aes-w0-membership-codex-20260925`.

## Problem and implementation

The earlier W0-I0b implementation excluded nonmembers only when publishing family
signals. Inside a family's DSL, rank, normalize and group neutralization still saw
the context's year-union instrument set. Changing a not-yet-admitted security's
prices could therefore change an incumbent's published signal.

`alpha::Engine::set_cross_section_mask` now accepts an owned, validated date-major
mask. Each CS opcode filters its valid set before computing statistics; excluded
outputs are NaN. The mask also applies through fused execution and date-parallel
CS execution. Field loads continue to use the observation mask, preserving raw
price history for trailing time-series features. A rolling CS feature receives
NaN for dates the security was not a member; it does not invent pre-entry ranks.

`evaluate_equity_families` derives eligibility from dated membership over the
entire required-lookback feature window. Memory preflight includes this mask and
temporary parsed IDs. The existing baseline admission logic remains the final
gate. `stage_equity_ic` already forwards the validated membership configuration;
its manifest now labels the DSL cross-sectional mask explicitly.

With an empty mask, VM semantics are unchanged. `ContextYearUnionV1` still selects
the old family behavior. Active CS masks bypass the subtree cache because its key
does not identify eligibility. This avoids reuse or publication across different
member sets without changing the shared cache format.

## Verification status

Implementation preceded tests, as requested. No real market inputs were read.
Builds are held until the root grants the shared host's build slot.

Added verification:

- All 16 CS opcodes against a reference universe physically lacking the excluded
  instrument, including fused evaluation with two CS workers.
- Owned-mask validation, failed-setter state preservation, cache isolation,
  restored unmasked-cache behavior and retained nonmember raw-price history.
- Midyear-joiner perturbation invariance through rank, normalize, neutralization,
  CS-inside-TS and TS-inside-CS. The V1 control must actually change.
- Immediate raw-history readiness at entry versus truthful CS-history warmup.
- Constant-membership bit parity and mask-memory preflight rejection.

Commands/results and review will be appended after the build slot is available.

## Scope

This closes the family-evaluation D-12 residual recorded in W0-I0b. It does not
claim every production VM caller already supplies a tradability mask; W4-A6 may
reuse this API when wiring additional callers. No replay, config or inference
files were changed.
