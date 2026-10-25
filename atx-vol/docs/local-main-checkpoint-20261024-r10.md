# r10 research checkpoint and transition to the 100 ms guide

Reviewed combined source is `e286b8b7`, based on local main `05ccc878`.
This checkpoint preserves completed research while the next fitting lanes
implement the [new execution guide](spy-cold-fit-100ms-execution-20261024.md).
It does not promote the unsuccessful working-set mode or claim a speedup.

## Integrated results

| Work | Result | Production implication |
|---|---|---|
| Bounded joint working set | Own SPY 13 financial + 8 lifecycle checks pass; 3 reduced solves plus full fallback, 106 factors total | Default OFF; no performance promotion |
| Retained-policy saved linear systems | Four original saved systems meet the original residual test | Research evidence only |
| Retained-policy fixed twelve PDE requests | 10 computed, 2 finest-space refusals; refinement remains incomplete | No reference or Greek qualification |
| Complete cubic Gaussian expectation | Reviewed affine-tail price and one-sided Delta integration, including moved-owner refusal fix | Research target only |
| Single-cash-event American call builder | 93 focused controls pass; independent six-case outcome not run | Research target only |

The working-set SPY candidate is source `e000b9dd`, with archived executable
SHA `9e9bcedff2725b0e8c2fa682aa441d62796013d7c3a8301fc56206a6619f12a6`.
It serves 33/33 valid expiries and 12,866/12,866 eligible rows, with 12,851
inside the original boxes, 12,850 identified rows and 16 PriceOnly rows.
Its full fallback is real and fully charged. The 3.4171109 s cold time is
unqualified because the host and SMT sibling were busy. The negative algorithm
finding rests on the actual factor/row work, not a ratio to a different host run.

## Validation of the combined source

- Working-set ON before the SPY run: 16 dedicated native controls and 50 affected
  projection/target/publication controls pass; 8 post-fit CLI controls and two
  working-set CLI controls pass. The combined benchmark source passed PCH-off.
- Default working-set OFF at `e286b8b7`: 50 affected native controls pass,
  8 post-fit CLI controls pass, and the benchmark/quote targets build.
- Combined cash research target: 93 controls pass, including all eight new
  single-event builder controls. Both new owning sources pass PCH-off.
- Combined retained-PDE adapter/reader/runner closure: 26 tests and 23 subtests
  pass with the explicit immutable historical archives configured. This runs
  small controls and archive verification, not the twelve-request PDE study again.

The initial combined Python invocation omitted the explicit historical archive
environment and therefore skipped nine controls; the configured run above has
no skips. The initial new cash PCH-off check had no generated object target;
reconfiguring the existing hygiene tree registered the two new sources and both
compiled successfully. These were invocation/configuration issues, not hidden
numerical passes. The initial logs and final results remain retained.

The default full route was not subjected to another full-SPY proof in this
checkpoint. Its preceding r9 own-output proof remains historical evidence; the
r10 working-set run has its own full proof and exercised the original full
fallback. There is no combined release-suite or quiet performance qualification.

## Next implementation

The saved census invalidates adjacent-only projection assumptions: 724 of the
initially violated calendar rows are nonadjacent. A new prototype retains all
authoritative rows, partitions them by latest expiry, and adds sign-aware
future-box look-ahead before small sequential solves. Every failed sequence
discards its partial surface and charges the original full fallback.

Two other lanes implement private short-tenor family preparation and linear
all-pairs validation. They start from `e000b9dd` and are not included in this
checkpoint. The current public three-day family guard, all-pair requirements
and economic acceptance contract remain in force until reviewed changes pass
their own output proofs.

The evidence root is
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.
`integrated-research-r10-receipt.json` binds the source and retained artifacts;
the local-main merge record is separate so the qualification receipt remains
immutable. No remote push is part of this work.
