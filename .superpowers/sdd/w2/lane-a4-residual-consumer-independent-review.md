# Independent A4 residual fitness/search consumer source review

Reviewer: pool4, independent of pool5 implementation. Source-only approval; compilation and runtime remain pending.

Reviewed exact commits:

- Consumer: `9d01987b6e8a49d2d378fcd04a47780e74fd8e1b` (11 files), against root `4816bb85` / `f4c6ff38` context/numerical boundary.
- Resource correction: `112a95062e025671ce0f525d2079fb0e3ebdaf08`.
- Postimplementation fixtures: `9cf04ec5441271b6cb77de54685d28b7281bdfc4`, existing `tests/factory/factory_objective_ic_test.cpp`, filter `FactoryResidualConsumer.*` (3).

No dependency merge was imported. No implementation edits, configure, build, tests, data or benchmark ran in this review. `git diff --check` passed on the scoped source/fixture delta.

## Finding and resolution

R-A4-C1 (blocking, resolved): `SearchDriver::run` originally constructed `DetPool` before checking residual worker count and memory budget. `DetPool` immediately reserves state and creates every requested thread. A context prepared for one worker with a huge requested count could therefore exhaust resources before typed refusal. `112a9506` moves explicit nonzero count/budget admission and residual scratch preparation before `DetPool`. The bounded route refuses auto-sizing (`n_workers=0`). Fixtures cover 0 and 100000 without launching those workers. Legacy/execution worker behavior is unchanged by this correction.

No further confirmed blocker in the bounded final diff.

## Checked production boundaries

- `fitness.cpp`: explicit objective dispatch precedes execution/legacy streams. Residual score is the signed equal mean of all three defined finite HAC IRs. Missing any required inference yields negative infinity with an explicit unavailable flag; it never becomes a zero score. P&L, WQ, DSR, turnover and costs are not fabricated. The direct VM route and the already-evaluated SignalSet route use the same kernel.
- `objective_ic.cpp` / `ResidualFitnessBinding`: binding compares actual price-field and Panel-presence bytes over the captured window with the prepared context. A same-shape changed payload is refused. Additional VM fields may differ because they are candidate inputs rather than label inputs. Context retains immutable exposure and captured label/support ownership. Per-candidate checks are O(1), relying on the documented requirement that the exact bound Panel object and borrowed backing remain alive, immutable and unmoved. This is not a runtime detector for callers violating that lifetime contract.
- `search_driver.cpp`: residual mode explicitly requires raw IC screening disabled; rejects fidelity, DSR selection, output-fingerprint reuse, checkpoints/resume, weak panels, nonempty P&L pools and execution/cost overlays. Therefore no raw screen can discard a residual alpha, no legacy rescore is reached, and no stale checkpoint can silently reuse another recipe. Digest binds residual objective/context identity.
- Parallel scoring: each worker owns distinct scratch; all fresh-score writes have their existing unique representative slot. The corrected admission checks context plus declared scratch bytes before thread creation. Existing VM/search storage is outside the residual kernel budget and is not represented as an RSS cap.
- Trial bookkeeping: every distinct attempted representative merges into canonical/all-scored accounting. Insufficient evidence and VM/compile failure keep the residual-unavailable sentinel; diagnostic status distinguishes insufficient evidence from failed evaluation. Such origins are not emitted or credited as successful operator gains. Hard configuration failures return an invalid run, not a partial admission. Durable residual trial/registry support remains intentionally unsupported.
- `factory.cpp`: all five admission entry points refuse residual-only mode before legacy P&L/library admission. `pool_view.cpp` also refuses the unbound residual route. Direct empty-AlphaStore fitness is supported; a nonempty P&L pool is refused.
- Default reproduction: objective rule defaults to LegacyV1; new branches/identity additions are gated. Legacy score arithmetic and default codec bytes are unchanged by inspection. This is a source parity conclusion, not a runtime golden-test result.

## Fixture review and scope limits

The three new fixtures exercise actual VM fitness on exposure proxy plus residual/inverse signals, negative meaningful scores versus unavailable, explicit absence of invented P&L/DSR, actual one/two-worker SearchDriver equivalence and trial preservation, unavailable-origin codec, payload and Panel-object binding, raw-screen/fidelity/DSR/resume refusal, oversize/auto-worker refusal, and Factory admission refusal.

These are bounded constructed integration checks. They do not establish empirical alpha retention, out-of-sample performance, executable profitability, full A4 completion, or a calibrated half-life/net-cost objective. Source is approved for the planned focused qualification; runtime approval depends on the owning target results.
