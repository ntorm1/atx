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
The root granted a single-worker build slot after the frozen G0 Release build
completed. An independent static review by `g0_evidence` found no blockers.
The build and runtime acceptance checks below passed without a source fix pass;
the implementation remains `af40186d23ae6a2ee6fa35f728315e768249fffa`.

Added verification:

- All 16 CS opcodes against a reference universe physically lacking the excluded
  instrument, including fused evaluation with two CS workers.
- Owned-mask validation, failed-setter state preservation, cache isolation,
  restored unmasked-cache behavior and retained nonmember raw-price history.
- Midyear-joiner perturbation invariance through rank, normalize, neutralization,
  CS-inside-TS and TS-inside-CS. The V1 control must actually change.
- Immediate raw-history readiness at entry versus truthful CS-history warmup.
- Constant-membership bit parity and mask-memory preflight rejection.

Configuration completed successfully with isolated dependencies:

```powershell
Set-Location C:\atx-wt\pool-5
$env:CMAKE_BUILD_PARALLEL_LEVEL='1'
& C:\atx-wt\pool-5\scripts\atx-build.ps1 configure -Preset equity-dev -Jobs 1 -Groups 'alpha;eval;combine' -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-5/deps/equity-dev
# exit=0; FETCHCONTENT_BASE_DIR verified in build-equity/CMakeCache.txt
& C:\atx-wt\pool-5\scripts\atx-build.ps1 build -Preset equity-dev -Jobs 1 atx-engine-alpha-tests atx-impl-tests atx-shm-worker
```

Available memory at build startup was 4.205 GiB with one other compiler worker.
The build completed with exit 0, linking both tests and the worker. Logs are
`build-equity/w0-membership-configure.log` and `w0-membership-build.log`.

Before execution, cleared `ATX_DATA_DIR`, `ATX_ORATS_ZIP`, `ATX_ALPHA101_PANEL`,
`ATX_ALPHA101_FIXTURE`, all four L10 input/output variables, `ATX_NIGHTLY` and
`ATX_RISK_NIGHTLY`. The existing date-labelled fixtures are synthetic; no real
market input was used.

```powershell
& C:\atx-wt\pool-5\scripts\atx-build.ps1 -Ctest -Preset equity-dev -Jobs 1 -R '^(AlphaCrossSectionMembership|ImplMembershipCrossSection|EquityBaselineViews|ImplIcAsOfMembership_[^.]+|StageEquityIc)\.' --output-on-failure
# 27/27 passed, 96.01 seconds, exit=0
& C:\atx-wt\pool-5\build-equity\bin\atx-engine-alpha-tests.exe
# 706/706 passed, 274 suites, 33.190 seconds, exit=0
```

Logs: `build-equity/w0-membership-focused.log` and
`build-equity/w0-membership-alpha-all.log`. An earlier pre-link discovery attempt
found zero tests and is not counted as validation. Root explicitly owns the
whole-impl run after integration; this lane did not duplicate that gate.

Runtime acceptance includes all 16 CS operators versus a physically reduced
universe, mask ownership/validation/cache behavior, incumbent invariance under
nonmember-price perturbation, retained raw-history warmup, truthful pre-entry CS
history, constant-membership bit parity, and all existing equity membership and
IC-stage checks. Both new impl and VM test files compiled on the first pass.

Built executable SHA256:

- Alpha: `90FCBC5C23C2F4A7A0749F8AAF3959B87ACE995905B439A8D89B0E82F1E2637B`.
- Impl: `E2E09E774F9B657836FE0922BE9644BB8EC9043D556A31E5E21324BED180ADA7`.

Independent reviewer `g0_evidence` approved runtime acceptance after inspecting
both logs, verifying the alpha binary SHA256 and rerunning the reduced-universe
test (1/1 passed). Receipt:
`C:/atx-wt/g0-data/bc5cc646_20260925/logs/membership-independent.receipt.json`.
Root authorized the corrected G0 run after these results; release was sent.

## Hygiene incident and remaining verification

The separate `hygiene` configure succeeded with `ATX_EQUITY_ONLY=ON`, PCH off,
and isolated `pool-5/deps/hygiene`. A four-TU wrapper `check` unexpectedly pulled
182 dependency actions through the impl-test target's worker dependency.
`atx-build.ps1 check` invokes Ninja directly without forwarding `-Jobs`; unlike
the build verb, `CMAKE_BUILD_PARALLEL_LEVEL=1` does not cap it. This caused an
observed 17 global compiler processes, despite the requested single-worker check.
The owner stopped only its verified pool-5 hygiene Ninja descendant tree. No
other pool was stopped; RAM recovered to 7.84 GiB and one global compiler.

That interrupted command is not a hygiene pass. Logs are
`build-equity/w0-membership-hygiene-{configure,check}.log`. Root asked this lane to
wait for a compiler slot, then verify only the two production TUs individually,
after checking their dependency dry-runs. No source or tested Debug binary was
changed by the interrupted hygiene attempt. Functional acceptance above remains
valid; no include-clean claim is made until the serial checks finish.

## Scope

This closes the family-evaluation D-12 residual recorded in W0-I0b. It does not
claim every production VM caller already supplies a tradability mask; W4-A6 may
reuse this API when wiring additional callers. No replay, config or inference
files were changed.
