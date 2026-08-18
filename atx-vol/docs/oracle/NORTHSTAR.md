# Oracle north star — SpiderRock parity dashboard

Loop state for the RSI loop (`vol-oracle-iter`). MUTABLE — the Ratchet stage rewrites
sections each iteration. Append-only history lives in `../LEDGER.md`; design in
`docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md` and the v2 design of
record in `docs/superpowers/specs/2026-08-17-oracle-rsi-v2-design.md`.

## Status

| | |
|---|---|
| Capability state | `missing_mode_b` — bootstrap stages 1, 2 and 3 COMPLETE. `data_receipt_valid=true mode_a_receipt_valid=true conventions_receipt_valid=true mode_b_receipt_valid=false` |
| Canonical | `refs/heads/oracle/canonical` @ `a1c984e5`, merged to `main` @ `11155df9`. Pre-move backup at `backup/oracle-canonical-20260817` @ `e232a118` |
| Last verdict | BOOTSTRAP stage 3 PASS (2026-08-18; holdout untouched by design) |
| Consecutive rejects | 0 (ESCALATE to user at 3) |
| Data | INGESTED — `C:\atx-cache\oracle\spiderrock\date=2026-08-14`: 31,771,788 rows (post-0930-drop), 19 `bucket_et` partitions, 3.10 GB zstd |
| Bench tool | BUILT — `atx-vol-oracle-bench`; 49/49 `OracleBench*` cases pass. The six frozen `vol-oracle-iter` gate command strings now parse and run |
| Conventions | **RESOLVED** — `discrete_forward_pv__rate__sdiv_yield`. Full map + rationale in `atx-vol/bench/oracle/CONVENTIONS.md` |
| Ratchet baseline | PINNED — `atx-vol/bench/oracle/scorecards/iter-000.json`, aggregate smoke+tune, 277,952 rows, 0 engine errors, 100% selection coverage |
| Mode B | NOT IMPLEMENTED. `--mode B` parses then refuses at run time with `ErrorCode::NotImplemented` before any store read. Deliberately not stubbed |

## Targets (from spec)

| Metric | Target | Current (symmetric) | Current (standard rel) | Met? |
|---|---|---:|---:|:--:|
| Mode A price MAE | <= 1 tick | 376.06 | 376.06 | NO |
| Mode A vol | <= 5 bp | 0 (identity) | 0 (identity) | n/a |
| Mode A delta | <= 1% rel | 0.0123 | 0.0133 | NO |
| Mode A gamma | <= 1% rel | 0.0617 | 0.0788 | NO |
| Mode A theta | <= 1% rel | 0.1295 | 12.684 | NO |
| Mode A vega | <= 1% rel | 0.0815 | 0.2134 | NO |
| Mode A rho | <= 1% rel | 0.1144 | 0.8489 | NO |
| Mode A phi | <= 1% rel | 0.1188 | 1.1989 | NO |
| Mode A volga | <= 1% rel | 0.1091 | 0.5228 | NO |
| Mode A vanna | <= 1% rel | 0.0989 | 0.1573 | NO |
| Mode A deDecay | <= 1% rel | 0.1507 | 1.3189 | NO |
| Mode B | <= 2x Mode A floor | — | — | not implemented |
| Speed | >= pinned baseline | 3857.54 rows/s vs pin 3122 | | YES |

**Not one accuracy target is met.** Price MAE is 376x its target. The loop has
resolved conventions and built machinery; it has not yet moved the numbers.

`mode_a_vol_mae = 0` is an IDENTITY, not an achievement: Mode A prices AT `srVol`,
so the vol it reports back is the vol it was handed. It becomes a real measurement
only under Mode B. Never cite it as vol accuracy.

## Two error conventions — never unify

- **symmetric** `|m-o| / max(|m|,|o|,floor)` — the RATCHET BASELINE and the gated
  no-regression criterion, because it is the loss the scale selection minimises and
  it is bounded with no smallest-scale gradient.
- **standard relative** `|m-o| / max(|o|,floor)` — published beside it only so the
  floor stays comparable to the charter's "greeks within 1% rel" target. NEVER gated.

Bounded rule: a symmetric metric may regress only while `candidate <= baseline * 1.01`,
and every permitted regression is PUBLISHED in `accepted_regressions`. Cross-checked in
BOTH directions by five layers in three languages.

Iter-000 accepted regressions: `mode_a_vega_rel` 0.081233446188804986 ->
0.081468501930500911, `pct_of_baseline` 0.002893583280335071.

## Speed

Pinned `rel_avx2_rows_per_second`: baseline 3469.4698564618907, pin 3122 =
`floor(baseline * 0.90)`. The pin is DERIVED, never copied — a pin equal to the
baseline makes re-measurement a coin flip on run-to-run noise, and the validator
rejects any pin above `baseline * 0.95`. Re-measured 3857.54 rows/s on a quiet host.

Sweep `diagnostic_speed` (dev preset, ~770 rows/s) carries `citable: false` and is
NOT performance evidence.

## Open leads, ranked

1. **theta and deDecay have a basis error, not a residual.** Both moved the WRONG
   way under standard-relative (theta 8.97 -> 12.68, deDecay 1.14 -> 1.32) while
   IMPROVING under symmetric (0.324 -> 0.130, 0.348 -> 0.151). A sign divergence
   between the two conventions on exactly the two `per_day` + `BUS_252` metrics is
   the fingerprint of a basis/scale error the symmetric loss partly absorbs. Highest
   information-per-unit-work lead on the board.
2. **Price MAE 376 ticks is structural, not tuning.** The staged sweep's best
   candidate scored 93.65 ticks on smoke but 375.51 on tune, so the map that wins
   on smoke does not generalise. Suspect the American exercise treatment or the
   hybrid volatility clock, not the forward.
3. **Mode B blocked on a schema gap.** `OracleRow` has no date/bucket field and
   `run_oracle_bench_core` flattens all partitions, so per `underlier x expiry x
   bucket` fitting cannot group correctly until partition identity reaches the row.
4. **`calcEngine` is absent from our data.** SpiderRock's two-tier
   `{FastHybrid, NumericLow, NumericStd, NumericMax}` selection is unobservable
   here, which is an irreducible reproduction floor of unknown size.

## Oracle suspects

`oracle_suspect_candidates` is EMPTY and `market_evidence_status` is
`not_evaluated_no_nbbo_gate`. No cell has been excluded from the ratchet, because no
NBBO gate has run. The oracle is the north star, not truth; that list stays honest by
staying empty until evidence fills it.

## Next

Stage 4 — implement Mode B (fit vol from raw NBBO per underlier x expiry x bucket),
then `bootstrap/mode-b.json` and the state advances to `ready`. The ready-state
Measure/Attribute/Improve/Ratchet path is still RETIRED behind
`READY_BROKER_MIGRATION_REQUIRED` and the `vol-sprint` workflow is a fail-closed stub;
both must be migrated to the broker capability protocol before any iteration can run.
