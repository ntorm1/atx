# Remaining monetary IV candidates: attribution before batching

This is a predeclared instrumentation design, not an implemented optimization.
On the complete SPY audit of `701f96de`, 700 calls to
`detail::american_implied_vol_price_candidate` remain after the native family
and prepared-seed routes. Of these, 684 accept and 16 refuse. Every acceptance
comes from the Screen proposal followed by the original scalar cold proof.
The current counters aggregate inverse proposal and proof work, so they cannot
justify whether the next investment should target either one.

The prior roughly 130 ms attribution was a diagnostic on another contended run.
It is motivation to measure the complete stage, not a latency promise or an
independent additive bucket in this audit.

## Exact source path and phase graph

At this source SHA, the repaired-node fallback in `src/fitting/quote_slice.cpp`
enters a `ReimplyWorkScope(ReimplyStage::PriceCandidate)` and calls
`american_implied_vol_price_candidate` in `src/pricing/american_iv.cpp`.
`IvPriceCandidateStats` reports decisions but no exclusive phase timings.
The candidate function contains the following conditional sequence:

| Phase | Existing operation | Condition for visiting |
|---|---|---|
| Screen proposal | `american_implied_vol_impl` with `al_screen_opts()` and early residual control | `ScreenThenFast` selected and outer controls admitted |
| Screen proof | Original `american_implied_vol_price_seed` | Screen returns finite sigma above the floor |
| Fast proposal | `american_implied_vol_impl` with resolved Fast options and early residual control | Screen did not return an accepted answer, or Fast selected |
| Fast proposal proof | Original scalar seed helper | Fast proposal returns finite sigma above the floor |
| Complete Fast retry | Original `search_typed` with Fast options | Early Fast proposal stopped and its proof did not accept |
| Retry proof | Original scalar seed helper | Complete retry returns an identified finite sigma above the floor |

Each invocation records every visited phase and why other phases were skipped.
There are at most six top-level phases, but the capture must not fabricate six
operations for an early return. A full Screen solve can still require a cold
proof and can still fail; a warm residual is only a proposal. Exceptions,
invalid controls, unpriceable proposals and final refusals remain explicit.

## Observation contract

Use a separate default-off research build option and private observer. Do not
add a public policy field, rerun any inverse/price, change branch order, alter
the scalar proof, or substitute the empirical vector seed in this capture.
The observer is compiled out in the normal build. An enabled capture freezes:

- Invocation ID and complete exact request: target, S/K/T/r/q, side, monetary
  budget, proposal-kernel enum, every `IvOpts` field, optional-options presence,
  and resolved Screen/Fast/target option values. No vendor IV or Greek label.
- Entry/exit, acceptance/refusal and returned sigma, plus the original decision
  statistics. Preserve float bits or a lossless encoding, including signed zero.
- Each visited phase's input and result, explicit error/status, early-stop flag,
  and counter deltas for actual boundary solves and IV iterations. Record zero
  work as zero rather than dropping the phase.
- Floor-cache owner/generation, exact model and option key, and preparation/hit
  deltas. A pointer address alone is not an identity and is not portable replay
  state. Report cardinalities both with and without strike and sigma, so reuse
  opportunities are measured without conflating different contracts.

Use a bounded preallocated collector supplied by the tool, with a complete-entry
count and overflow/refusal flag. Capture all calls, not only successes. A lost
entry, exception while recording, overflow, duplicate ID or incomplete phase
invalidates the capture. No file I/O or allocation occurs in an observed phase;
serialize only after fitting. A single-worker capture provides deterministic
ordering. If a concurrent capture is ever supported, its thread/call ownership
and ordering must be explicit rather than assuming a global counter order.

## Time and proof accounting

The outer clock covers the original whole candidate invocation. Each top-level
phase clock covers the actual inverse or scalar helper call, including its local
setup and destruction. Phase clocks are disjoint; report the remainder of the
outer clock rather than attributing all overhead to a chosen numerical phase.
Collector setup, append overhead and serialization are reported separately from
the unchanged-fit comparison. Observation clocks are diagnostic until a quiet
ThinLTO campaign establishes their measurement overhead.

Within an already visited scalar helper, an optional read-only nested record can
observe existing range/zero-vol checks, classified cold target evaluation,
sigma-floor preparation or cache hit, and positive-chord prices. These are
subspans of the helper, not extra work to sum with it. Record the exact existing
short-circuit reason and attempted-solve counts. No extra classification, floor
price or chord endpoint may be computed merely to populate the record.

This decomposition matters because a candidate in the nonmonotone band retains
its scalar positive-chord proof, and a cached floor differs from a new boundary
solve. A price-only replay is not the full original acceptance operation.

## Required controls before using the capture

1. TDD a deterministic fixture visiting Screen acceptance, Screen refusal with
   Fast acceptance, an early Fast refusal requiring full retry, final refusal,
   invalid input and allocation/error short circuit. Assert exact visited phase
   identities and no fabricated work after failure.
2. Compare observer off/on numeric and metadata outputs bit for bit. Confirm no
   added pricing or inverse calls. Test collector exhaustion and incomplete
   phase records; they must invalidate evidence without modifying fit results.
3. On the frozen SPY input and canonical policy, join all 700 calls and all 684
   acceptances to the existing whole-fit counters; retain all 16 refusals.
   Require exact predictions and all 13 scalar actual-API correctness checks.
4. Independently reconstruct phase transitions, resolved option schemes and
   every complete request ID in a verifier. Do not accept only an aggregate
   count, an optional digest, or an unexplained subset.
5. Archive the enabled and control binaries, full compile settings, exact source
   SHA, input/policy hashes and raw capture before any rebuild. Do not overwrite
   an output directory or substitute a successful repetition for a failed one.

## Replay and architecture decision

First replay complete candidate invocations in original order with the same
declared floor-cache lifecycle. The inverse contains warm boundary evolution
within a call; restarting every residual as a cold price is a different task.
Require exact accepted/refused results and sigma bits before timing a replay.
Report all attempted invocations, model-key cardinalities and setup plus
inclusive evaluation costs.

Then isolate proposals and proofs using their captured exact inputs, explicitly
separating that counterfactual microbenchmark from complete-call replay. Preserve
the proof's original floor and positive-chord semantics. If scalar cold target
proofs dominate, evaluate batching eligible proposal outputs under the already
explicit empirical tier, with scalar fallback and unchanged scalar actual-API
audit. If inverse proposals dominate, investigate shared model geometry or a
joint inverse lane algorithm; batching only the final target price cannot remove
that work. Neither choice is selected in advance.

Any batching design must account for delayed dependencies: early Screen
acceptance suppresses later Fast work, and an early Fast refusal can trigger the
original complete retry. Packing/setup/destruction, unused lanes and all scalar
fallbacks belong in the inclusive result. No approximate vector value may be
recorded under an exact scalar proof field. No generic error theorem or new
Greek budget follows from passing this SPY experiment.
