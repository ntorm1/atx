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
