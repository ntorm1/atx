# Observation seam ready for source review; replay not executed

The prospective plan was frozen at `ca2e80fa` on reviewed checkpoint `a037b1e4`
before this component. `lcp_observe.py` adds a private observation seam and an
independent row reader. It has no CLI, saved-stage loader, PDE call or automatic
launcher. **None of the four declared saved systems has been replayed.**

The complete frozen primitive and original multiply source are validated before
either private module executes. Their text is compiled into unregistered module
objects. Only those private objects receive wrappers; imported process-global
functions remain unchanged. The wrappers delegate arguments directly and retain
actual call counts. The original algorithm, tolerance, pair-reserved work bound,
finite checks and refusal/result handling execute from their approved source.

The existing resource argument caps observation at128 updates and256 calls, or
the smaller original `n+2` bound. Checked-state storage is preflighted and owned,
with at most129 vectors and Boolean policies. The result exposes readonly views
of the retained prefix; the reported allocation includes the full reservation.
Original finite inputs are copied, marked readonly and rehashed after execution.
A nonfinite residual attempt does not become a fabricated checked vector.

The reader reconstructs each original equation in the same binary64 order,
recomputes all policies and residuals, and joins state indices/counters to the
original actual work. It also evaluates the selected-system residual under the
preceding policy. This is a read-only equation check, not a second linear solve
or an independent verification of every internal LAPACK operation. A complete
runner must additionally bind the execution's source and artifact provenance.

Repeat detection uses hashes only as a filter, then compares all vector bits,
policy bits and residual bits. Signed zero is retained. A confirmed cycle or
stagnation requires two complete consecutive laps, with deterministic earliest
confirmation/shortest-period selection. Policy-only recurrence and one repeated
state remain separate from a confirmed recurrence. The exact historical final
vector join and bounded rational witness analysis belong to the later runner.

Eleven new tiny controls and fifteen existing primitive/original controls pass.
The new controls cover real unobserved-versus-observed bit parity, a real
one-update observation cap, source validation before either module loads,
nonfinite original/policy/correction refusals, two-lap requirements, forced hash
collisions, signed zero, policy/residual distinctions, storage refusal and
malformed-reader evidence. The initial placeholder recorded eight runtime errors;
subsequent real source-preflight and Boolean-counter adversaries each reproduced
an assertion failure before their fixes. A separate Python `-O` adversary exposed
optimizable reader assertions; all reader gates now use mandatory checks, and
the optimized-process control passes. All negative logs remain retained.
The existing primitive controls include the previously approved stage60 fixture;
they are not any of the four newly declared saved stages.

```powershell
python -m unittest -v test_lcp_observe test_lcp_correction test_pde_reference.ObstacleTests
```

Evidence under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/` is pinned by
`lcp-observe-seam-receipt.json`. Python source syntax, source hashes, exact frozen
primitive/PDE text and `git diff --check` are the applicable component checks.
There is no C++ change or owning native build.

The declared24MiB per-stage archive and128MiB complete-output caps, fresh-directory
guard, complete four-input joins, execution provenance and rational witnesses
remain runner work. They are not claimed implemented by this seam. Fresh
independent component review is required before that runner is finalized; a
second exact-source review is required before its fixed four-stage execution.
No PDE result, historical refusal, Greek accuracy rule or production path changes.

## Refusal-phase review correction

Independent review of `2c963fea` found that a genuine zero-solve observation-cap
result could be relabeled as an impossible numerical or algorithmic failure.
All seven alternative refusal labels reproduced runtime failures in the new
control. The reader now joins each reason to its exact original phase using
completed updates, policy/correction calls, selected-matrix multiplies and
nonfinite original-residual observations. An algorithmic iteration-limit label
requires the full original `n+2` updates; an observation-cap label requires a
strictly smaller allowance. Neither can masquerade as the other.

Fourteen observation controls plus the fifteen existing controls pass after
the correction. Positive controls retain failures at the policy solve,
correction-RHS, correction solve, corrected-value and initial/updated original
residual phases; scripted solver results exercise these branches on two-row
inputs. Both kinds of linear-solve exception remain valid. A scripted complete
original iteration-limit exit also remains valid. These controls are separate
from the declared four-stage experiment, which still has not run.

The joins prove phase consistency, not an independent re-execution of LAPACK or
a forensic distinction between different exceptions with identical counters.
The later runner must still bind exact source/execution/artifact provenance.

A second independent review found that a finite initial state could be erased
and relabeled as a nonfinite first residual, with internally consistent counts.
That exact zero-solve mutant reproduced runtime RED. The reader now independently
recomputes the original initial continuation and obstacle gap before accepting
either an absent or a present state prefix. Genuine continuation-overflow and
gap-overflow initial refusals remain valid and retain no invented checked state.
The full focused set is now sixteen observation controls plus fifteen existing
controls; no saved-stage replay or new solver arithmetic is involved.
