# Independent W1-R1 source review

Root reviewed pool-4 source `96aadfb8`, `42f781a0`, `fdc49642`,
`b37283f2`, and correction `454f8f97` on 2026-09-26. Approved for bounded
integration/qualification; no runtime, byte parity, speed or 5000-name solve claim.

The CSR recipe keeps logical row order, column traversal and bounds aligned with
the explicit dense legacy branch. Per-name identity rows are implicit. Sparse
assembly, discretization pins and elasticity address logical rows; the factor
solver materializes only its declared general-row workspace, with a resource cap.
It is not an arbitrary O(nnz) solver and no solver route was silently switched.
Checked nnz/dimension/byte admission and separately capped LDL factors replace
the dense-square rejection only in explicit V2. Oversized problems fail instead
of dropping holdings.

New participation and liquidation descriptors use explicit dollar ADV/NAV and
intersect hard boxes. Trade limits reference prior weights; missing/nonfinite
liquidity fails, zero ADV binds to zero, contradictory bounds fail, and hard
liquidity cannot be loosened through shared elastic box rows. Actual decision-ADV
acquisition/wiring remains R3; this review does not claim that consumer exists.

RelativeEconomicV2 checks actual-weight linear, gross and turnover limits, so
per-row auxiliary slack cannot accumulate into M times an economic allowance.
Finite weights, totals and tolerances are required. Cone feasibility remains the
solver's separate responsibility. The allocator retains its independent post-fee
and represented-execution economic certificate. The book CLI selects the explicit
sparse rule; saved recipes bind storage/factor budgets and both tolerance formulas.
Library/API defaults and explicit application V1 retain the old dense/absolute
recipe and omit V2-only saved metadata.

One blocker was found: NaN gross metadata could skip the direct budget check via
`gross_l1_budget >= 0`. Correction `454f8f97` adds V2 active-metadata validation,
including turnover/cone shapes/finiteness and elastic refusal. The direct helper
now documents that it is a linear/L1 certificate. This resolves the finding.
The same correction preserves the static `check_problem` API and bounds sparse
relaxed/prebuilt augmented forms plus their factor payloads.

Owning postimplementation fixtures, existing V1/V2 small-book parity, native build
and actual-stage recipe checks remain pending. The M5000 materialization timing,
RSS and solve acceptance remain unrun under the owner's build-first direction.

Final follow-ups reviewed: `760f28f7` charges copied robust metadata before sparse
pin allocation; fixtures `2308d27f` cover CSR/dense bit parity, real economic
budgets, malformed metadata and application recipe selection. `fb4702b1` makes
MPC/reference solver rejection of nonlegacy storage explicit, including empty
logical-row cases. Their existing geometry checks already rejected ordinary
nonempty CSR, so this is an explicit unsupported-consumer guard, not evidence of
a demonstrated dropped-row runtime failure. Source approved; fixtures unrun.
