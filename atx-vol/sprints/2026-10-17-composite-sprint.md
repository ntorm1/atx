# Composite benchmark on both cohorts, resume 5 (2026-10-17)

Lane `composite`, branch `feat/vol-composite-bench-20261017` off root `24e01644`, pool-33,
run `vol-20261017-composite`. Run-only lane plus one tool fix; no library source change, no
default changed, development cohorts only, holdout closed. Receipts, every number and every
hash: `C:/atx-cache/quote-benchmark/composite-20261017/RESULTS.md` (manifest `sha256.json`).

## What and why

The 2026-10-16 composite lane stopped after dev200: no generalisation verdict existed, its m6
predictions (`dc435595`) predated the one-sided sigma-interval seed fix, and the new opt-in
lanes (slice-repair v3, smooth-div `escrowed-lr`) had never been composed. This lane rebuilt
quote-bench from the integrated root, reran m6 and X1, and scored C1-C5 on dev200 and dev200b
with strata and an unserved census.

## Tool fix: payer-first routing in the status composer

`tools/compose_predictions_by_status.py` gains opt-in `--restrict INDEX=ROWS.csv`: input INDEX
may serve only the listed row_ids; other rows skip it, and an unserved row falls back to the
first input allowed on it (input 0 when nothing is restricted). The receipt records each row
list's path, SHA256 and count. Without the flag the output is byte-identical: re-composing the
2026-10-16 C1 inputs reproduces `77d9cdde`. Tests:
`python/tests/test_compose_predictions_by_status.py` (11 cases), wired into the existing
`atx-vol-quote-benchmark-tools` ctest. C5 needs it: the dividend run's served set includes the
static fits of non-payers, which would otherwise overwrite repair-v3's refits there.

## Parity (binary `bc43944c...`, vol-rel, built at -Jobs 2)

Keys-off defaults reproduce the ledger bytes: dev200 `c89d0739`, dev200b `4b273b08`;
graded-v1 `1348f63d` / `4c70d2bc`; divsched `abc75955` / `747d3e6c`; escrowed-lr `2cfda793` /
`5a9c1f5f`; repair-v3 `14d9dc6d` / `81cc69e0`. X1 is unchanged by the seed fix (`43515c45`,
`60a570c3`). m6 changes as expected: dev200 `dc435595` -> `946e6650`, exact-intersection MAE
0.3390 -> 0.3363 (-0.79%), the whole delta in the 18 one-sided-interval snapshots (2,254 rows,
0.386 -> 0.333); dev200b m6 `ce4fa480` (first run).

## Headline (own population; coverage / MAE $ / MAE per spread)

| Composite | dev200 (63,942 eligible) | dev200b (62,369 eligible) |
|---|---|---|
| graded-v1 alone (primary) | 78.74% / 0.2343 / 0.109 | 87.15% / 0.2393 / 0.116 |
| repair-v3 alone (primary) | 84.89% / 0.2250 / 0.114 | 90.49% / 0.2367 / 0.111 |
| C1 graded -> m6 | 88.63% / 0.2356 / 0.151 | 98.45% / 0.2395 / 0.119 |
| C2 divsched -> graded -> m6 | 88.63% / 0.2332 / 0.150 | 98.45% / 0.2313 / 0.116 |
| C3 graded -> X1 | 88.63% / 0.2359 / 0.152 | 98.45% / 0.2400 / 0.120 |
| C4 repair-v3 -> m6 | 92.15% / 0.2278 / 0.145 | 98.63% / 0.2346 / 0.113 |
| C5 escrowed-lr (payers) -> repair-v3 -> m6 | 92.42% / 0.2197 / 0.144 | 98.63% / 0.2245 / 0.110 |

## What generalises (exact intersections, dev200 / dev200b)

C1 -> C4 -1.41% / -2.03%; C4 -> C5 -3.50% / -4.34% (payer rows only, non-payers identical);
C1 -> C5 -4.91% / -6.28%; m6 vs X1 as fallback a wash (-0.12% / -0.22%). The ranking C5 < C4 <
C2 < C1 ~ C3 holds on both cohorts in dollars and in spread units. Payer routing (C5 vs
dividends-first-for-all) is worth -0.04% on dev200 but -1.27% on dev200b. Weak spots shared by
both cohorts: |z| <= 0.5 is the dearest bucket, payers are still 1.2-1.4x non-payers, and the
common-model rows behind a repair-v3 calendar refusal are the worst routed population
(dev200 0.73 spreads, containment 85%). dev200-only class A (sparse, under-levelled ATM) is
untouched by repair and dividends (C5 0.550 vs mid 0.643).

Unserved: dev200 C5 4,849 rows (7.58%: META/ASML quote cap 3,682, adjusted deliverables 716,
V/TSM parameter cap 409, ARI 30, SRLN 12); dev200b C5 855 (1.37%: deliverables 702, carry
dispersion 153).

## Disclosed side effect

Reading `.py` files in three earlier cache folders with the editor-integrated Read tool made
the IDE type checker write `.mypy_cache` beside them (two new cache folders, 157 rewritten
cache entries). No data artifact changed: composite-20261016 78/78 and markov-fit-20261016 105/105
manifest files verify byte-identical, policy-experiments-20261016 every non-cache file except
the root composer that the 2026-10-16 integration changed. The cache folders appeared within
seconds of each Read; copy an archived script to scratch before reading it (a shell read is
probably also safe, not verified).

Timings are unqualified (loaded host, 3 fit workers): graded 182 / 101 s, repair-v3 181 / 116 s,
m6 108 / 183 s, X1 430 / 700 s, dividend runs 10-13 s.
