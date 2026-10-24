# Independent reference review and durable continuation

## Exact prior-commit verdict

**APPROVE `8f73464d4647b5574915508492b675ddd2b66005` as an offline numerical
diagnostic.** This does not approve a rigorous accuracy or production admission
gate. Review was performed before implementing followups, from frozen base
`9717c4769fb93ff8e40e9159bcc6b741aa46bf7e`. The prior commit was reproduced as
`66b5115a` in this lane. Its original 19 tests passed freshly in 1.19 seconds.

The review checked integrated funding/yield/variance on the synthetic date axis,
exact decimal endpoint ratios and stopped variance, dividend/event ordering,
unsupported local volatility/deterministic limits, source-price exclusion, and
the distinction between observed refinement stability and a proven error bound.
No mathematical convention discrepancy was found within the documented scope.
At a cash jump QuantLib uses a positive finite mesh floor, so agreement of wider
domains remains an empirical diagnostic, not an exact limited-liability proof.

Official QuantLib v1.44 sources were reread on 2026-10-16:

- [PDE operator](https://github.com/lballabio/QuantLib/blob/v1.44/ql/methods/finitedifferences/operators/fdmblackscholesop.cpp)
  uses interval forward rates and forward variance divided by elapsed time.
- [Step conditions](https://github.com/lballabio/QuantLib/blob/v1.44/ql/methods/finitedifferences/stepconditions/fdmstepconditioncomposite.cpp)
  apply the cash jump before the American exercise maximum during rollback,
  including a terminal cash event.
- [Cash handler](https://github.com/lballabio/QuantLib/blob/v1.44/ql/methods/finitedifferences/utilities/fdmdividendhandler.cpp)
  interpolates the shifted value at a finite lower stock floor.
- [Vanilla engine](https://github.com/lballabio/QuantLib/blob/v1.44/ql/pricingengines/vanilla/fdblackscholesvanillaengine.cpp)
  provides the assembly reproduced by the standalone diagnostic.

The saved refined ATM report and its source match the previous handoff hashes:

- `american-reference-20261016-atm-refined-v1.json`:
  `074d33d1b7d217efcc03ab043b85bc3f917fdde8b9e5ab7f818901c7dc7db5e2`.
- `markov-audit-20261016-atm-only-reference-input.json`:
  `22ef7dd12d72e3352fccd7da2a212da7a62b8cdeb1c1a8ed0a1234f88e81465a`.

Both live under `C:/atx-cache/quote-benchmark`. The 24-row historical evidence
remains 23 observed stable and one unresolved; this review does not relabel it.

## Operational followups

The source SHA256 is captured during module import, before numerical work. The
runtime and installed distribution must both be QuantLib **1.44**. Native module
hashes, Python, and platform are captured before solves. Other versions fail
explicitly; this qualification is scoped to the tested diagnostic and does not
claim all possible v1.44 builds are numerically identical.

Every CLI run now creates a SQLite checkpoint, by default
`OUTPUT.checkpoint.sqlite3`. `--checkpoint PATH` chooses another new journal.
Each unique completed reference is committed with `synchronous=FULL` before its
progress notification. An exclusive connection prevents concurrent writers for
the journal's lifetime. `--resume` reuses it only if the exact input hash, tool
hash, solver settings, exercise/case selection, and dependency receipt match.
Results are separately checksummed and checked against the requested contracts.
An interrupted current contract is recomputed; completed contracts are reused.
Unsupported and solver-failure outcomes are also durable and remain visible.

This guarantees persistence across a terminated process on the tested SQLite
environment; it is not a power-loss guarantee for hardware ignoring fsync.
The optional direct Python API remains in-memory unless given a checkpoint path.
No production library dependency, pricing kernel, stability threshold, fit input,
vendor label, or held-out data changed.

An existing output remains immutable. If the final output write itself was
interrupted, select a new output and explicitly reuse the existing checkpoint:

```powershell
C:/atx-cache/american-reference-venv/Scripts/python.exe atx-vol/tools/american_reference_audit.py `
  --input C:/atx-cache/quote-benchmark/<source>.json `
  --output C:/atx-cache/quote-benchmark/<fresh-output>.json `
  --checkpoint C:/atx-cache/quote-benchmark/<previous-output>.json.checkpoint.sqlite3 `
  --resume <the exact original numerical and case-selection options>
```

There is no import path for the old report into a new checkpoint. Historical
results are reused as evidence, not silently promoted to a new tool identity.
The interrupted earlier full panel still has no saved numerical partial result.

## Postimplementation verification and ownership

Implementation preceded new tests, per the user's no-TDD instruction.
**30 tests passed in 2.87 seconds**, zero skips. Tests include abrupt child-process
`os._exit` after the first contract, exact replay without recomputation, input/
settings/selection/source/dependency mismatches, checksum corruption, concurrent
writer refusal, existing-path refusal, version qualification, source-hash capture,
CLI default journal/resume, and alias protection. The original numerical tests
remain unchanged. Ruff and `git diff --check` passed.

Receipt: `C:/atx-cache/quote-benchmark/reference-checkpoint-20261016-tests.xml`.
Commands used the existing isolated
`C:/atx-cache/american-reference-venv/Scripts/python.exe` with
`-m pytest --noconftest atx-vol/python/tests/test_american_reference_audit.py -q`
and the two-file Ruff check. No C++ target was changed or rebuilt.

Lane: `C:/atx-wt/pool-35`, branch `feat/vol-reference-checkpoint-20261016`,
RunId/HeartbeatId `vol-20261016-reference2`, frozen base above. Acquisition's
ordinary-dev configure failed only on the already documented unrelated missing
modules after lease publication. Graph/broker tools were absent; user-authorized
native leased workflow was used. Shared CMake and ledger were not edited.

Final exact commits and release receipts are supplied in the lane delivery.
