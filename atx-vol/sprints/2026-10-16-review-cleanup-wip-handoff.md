# Review cleanup: WIP handoff at user stop

Lane `feat/vol-review-cleanup-20261016`, pool-1 (released), RunId `vol-20261016-clean`.
The frozen base was `be85fd21`; the lane was rebased onto the moved root `2378605e`. The
full finding-to-fix table is in `2026-10-16-review-cleanup-sprint.md` (committed with
this note).

## Status per finding

| Group | Finding | Status |
|---|---|---|
| A (Markov fit `88c77c5d`) | MINOR-1 one-sided interval seed / prior | fixed, `811282cf` |
| A | MINOR-2 failed Jacobian column (JacobianFailure + retry) | fixed, `811282cf` |
| A | MINOR-3 DampingLimit / Cholesky tests | fixed, `811282cf` |
| A | MINOR-4 VarianceTimeKnots LM reuse bit test | fixed, `811282cf` |
| A | MINOR-5 expanded-definition texts | fixed, `811282cf` |
| A | NIT-1, NIT-2, NIT-3, NIT-5 | fixed, `811282cf` (NIT-4: disclosure only, no change) |
| B (implied forward `a5d2018a`) | Parity-American unmeasured pair | fixed, `3ed89f9b` |
| B | inert `implied_forward` policy block | fixed, `3ed89f9b` |
| B | `estimate_implied_forward` split | fixed, `3ed89f9b` |
| B | NITs: typed fallback cause, aligned-new counting, `max_outlier_fraction`, ledger correction | fixed, `3ed89f9b` |
| B | `quote_slice.cpp` dispatch before `validate()` | deferred (owned by another lane) |
| C (time knots `f4416a74`) | header-inline helpers moved to `.cpp` | fixed, `abf15ab0` |
| C | report `schema_version` 3 to 4 | fixed, `abf15ab0` |
| C | NITs: nodiscard, "Markov policy:" prefix, defensive-invariant comment | fixed, `abf15ab0` |
| C | NIT: log/exp hoist (optional) | not started (deliberately skipped) |

## Gates run (rebased head `abf15ab0`, vol-rel)

- gtest: fit 41/41, seed 25/25, policy 17/17, implied-forward 26/26.
- Anchored ctest (fit, seed, policy, implied-forward, quote-benchmark-native): 5/5.
- Native pytest: 149/149 (Python312).
- PCH-off `vol-hygiene`: 16 TUs, 23 objects, clean.
- Legacy e0/r104/r416 expanded definitions: equal to the 88c77c5d hashes (bd946410 /
  33ef8af7 / de236acf). This was measured with the group A binary before the rebase.
- R104 dev200 parity with binary `c349d99f`: predictions sha256
  `6b129f6478c86b10b7a8dc3a5b20beecc5f35218fb21471a919d15fe178ad8e2`, identical to the
  frozen R104 run (exit 0, 460 s).
- Quote-slice default parity (`c89d0739`): NOT run.

Receipts: `C:/atx-cache/quote-benchmark/cleanup-20261016-{bin,gates,defs,r104}/`.

## Next steps

1. Run the quote-slice default parity with the archived binary
   (`--policy auto --dividends funding-pv` on dev200 `quotes.csv`). It must hash
   `c89d0739...`.
2. Re-run the e0/r104/r416 definition probe with the rebased binary.
3. Fresh exact-SHA review of `811282cf..abf15ab0`, then integrate into the root.
4. Every m5/m6 `prior: seed` result predates the one-sided refusal. Rerun them before
   any held-out use.
