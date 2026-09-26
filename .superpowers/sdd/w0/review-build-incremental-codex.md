# Independent incremental-build review

## Wrapper, provenance, hygiene and caller compatibility

**APPROVE for integration** the subset through
`0c5f87a0daa67f11f1952fc76be81a954ba9c7f7`, including
`440116f360b5edd972c0f2d75bdb1b59a9f6cdec` and
`464d04836bfc6eb0cb76a76ecb3dac3ff328de24`. Root retains the actual changed-target
compile/no-op qualification; this review does not claim that future build passed.

The wrapper forwards one bounded worker count to both CMake build and Ninja
check, including dependency work. Explicit `-Jobs` overrides the build environment;
otherwise a valid environment value or one worker is selected. Invalid counts
fail. CTest keeps its separate serial default. Native worker overrides are
rejected, and builds must name targets. The separate equity-hygiene preset keeps
PCH-off checks and dependencies out of the warm equity-dev tree.

Two review findings were fixed before approval:

- Attached `-j+2` escaped the first guard although installed Ninja accepts it.
  The final guard rejects the whole `-j` prefix; owning checks cover signed and
  equals forms as well as ordinary compact/long forms.
- Seven oracle preparation callers still passed the newly rejected raw
  `--parallel 2`. All now use `-Jobs 2`, preserving presets and targets. Four
  existing assertions were updated; seven focused pure-argv checks passed.
  Those checks use fixture identity and actual wrapper DryRun, without opening
  retired oracle sources, cohorts or market data.

Independently executed `atx-build-dryrun-test.ps1 -CheckPreset equity-dev`:
**PASS, exit 0**, including multi-TU check limits. Receipt:
`C:/atx-wt/pool-5/build-equity-bench/w0-build-tools-independent.log`.
Inspected the frozen caller migration, all focused cases and owner result:
**7/7 passed, zero skipped, 23.01 seconds**, in pool4's
`build-equity/w0-oracle-build-args-tests.log`. The legacy full oracle suite reads
absent retired sources at load and was not represented as runnable/passed.

The provenance macro has one production consumer, `stage_discover.cpp`.
Independently inspected the generated compile database and the owner's complete
385-command before/after configure receipt: only that consumer's Git-SHA
definition changed, with zero other command differences. This removes repeated
SHA-only invalidation of the other pipeline TUs without changing provenance
semantics. First migration still changes their old target-wide definitions once.

## Stable PCH carriers: separate gate

Code `3f2c25fbdf608fb08270316e424333d08d4a0ab1` has no static blocker, but
**compiler qualification remains OPEN** at this report revision. It is separate
from the approved subset above. The carriers are minimal OBJECT targets with a
tracked empty TU, created only for configured consumers when PCH is enabled.
The data carrier preserves its extra miniz include environment. Scratch runtime
objects stay on executable consumers. Neither carrier depends on a test suite
or worker executable.

Independently compared actual command prefixes: all 18 configured book and 37
data consumers match their respective PCH producer. Inspected two-command carrier
closures and group-reorder receipts preserving generated-file timestamps/hashes.
Evidence: `C:/atx-wt/pool-5/build-equity-bench/w0-build-tools-configured-review.json`
and pool4's `w0-pch-closure.json`, `w0-pch-reorder-stability.json`.
The required remaining proof is the two carriers plus one consumer from each
environment, followed by an unchanged no-op. No broad second correctness-suite
recompile is required merely to prove these build-system changes.
