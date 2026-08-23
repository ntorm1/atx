# Oracle north star — SpiderRock parity dashboard

Loop state for the RSI loop (`vol-oracle-iter`). MUTABLE — the Ratchet stage rewrites
sections each iteration. Append-only history lives in `../LEDGER.md`; design in
`docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md` and the v2 design of
record in `docs/superpowers/specs/2026-08-17-oracle-rsi-v2-design.md`.

## Status

| | |
|---|---|
| Capability state | **`ready`** — bootstrap stages 1-4 ALL COMPLETE, all four receipts valid. `next_iter = iter-001` |
| Canonical | `refs/heads/oracle/canonical` @ `abed0a9c`, merged to `main` @ `087fc8a1`. Backups: `backup/oracle-canonical-20260817` @ `e232a118`, `backup/oracle-canonical-stage4-20260818` @ `a1c984e5` |
| Last verdict | BOOTSTRAP stage 4 PASS (2026-08-18; holdout untouched by design). No ratchet iteration has run |
| Consecutive rejects | 0 (ESCALATE to user at 3) |
| Data | INGESTED — `C:\atx-cache\oracle\spiderrock\date=2026-08-14`: 31,771,788 rows (post-0930-drop), 19 `bucket_et` partitions, 3.10 GB zstd |
| Bench tool | BUILT — `atx-vol-oracle-bench`; 53/53 `OracleBench*` cases pass. The six frozen `vol-oracle-iter` gate command strings now parse and run |
| Conventions | **RESOLVED** — `discrete_dividend_tree__rate__sdiv_yield`, by the closed staged sweep at `54024add`; all 33 keys explicit, exercise style resolved on evidence rather than tie-break. Full map + rationale in `atx-vol/bench/oracle/CONVENTIONS.md` |
| Ratchet baseline | PINNED — `atx-vol/bench/oracle/scorecards/iter-000.json`, aggregate smoke+tune, 277,952 rows, 0 engine errors, 100% selection coverage. **RESET at `54024add`**: the regenerated floors in `CONVENTIONS.md` replace the escrow-era ones as the numbers a later iteration must not be worse than |
| Mode B | IMPLEMENTED — vol MEASURED from raw NBBO per underlier x expiry x bucket via `american_implied_vol`. 241,052 of 299,798 rows fitted; 36,900 refused and counted |

## Targets (from spec)

Every Mode A row below is COPIED from the residual-floor table in
`atx-vol/bench/oracle/CONVENTIONS.md` (resolved at `54024add`, aggregate smoke+tune,
277,952 rows). That file is the source of record; this table is a view of it and
nothing here may be edited without editing it there first. The two error-convention
columns are the two conventions defined in the section below — the **symmetric**
column is the gated one, the **standard rel** column is published for charter
comparability and is NEVER gated, so "Met?" is judged on symmetric.

| Metric | Target | Current (symmetric) | Current (standard rel) | Met? |
|---|---|---:|---:|:--:|
| Mode A price MAE | <= 1 tick | 8.6633 | 8.6633 | NO |
| Mode A vol | <= 5 bp | 0 (identity) | 0 (identity) | n/a |
| Mode A delta | <= 1% rel | 0.0022 | 0.0022 | **YES** |
| Mode A gamma | <= 1% rel | 0.0179 | 0.0318 | NO |
| Mode A theta | <= 1% rel | 0.0639 | 7.3116 | NO |
| Mode A vega | <= 1% rel | 0.0240 | 0.0739 | NO |
| Mode A rho | <= 1% rel | 0.0217 | 0.6926 | NO |
| Mode A phi | <= 1% rel | 0.0245 | 0.9976 | NO |
| Mode A volga | <= 1% rel | 0.0831 | 0.1875 | NO |
| Mode A vanna | <= 1% rel | 0.0277 | 0.0341 | NO |
| Mode A deDecay | <= 1% rel | 0.0903 | 0.8085 | NO |
| Mode B vol | <= 10 bp | **442.79 bp** | | NO |
| Mode B price | <= 2 ticks | **35.52 ticks** ‡ | | NO |
| Speed | >= pinned baseline | 9958.75 rows/s = the baseline itself † | | n/a |

† **NOT A PASSING GATE, and it is marked `n/a` rather than YES for exactly that
reason.** `speed.baseline` and `speed.pin` are the only two speed numbers in
`iter-000.json`; there is NO measured run beside them, and the pin is derived as
`floor(baseline * 0.90)`. So printing the baseline in the Current column and
comparing it to the pin evaluates `baseline >= 0.9 * baseline`, which cannot read
NO no matter what the engine does. The cell had a YES in it, and that YES was a
verdict this dashboard never computed. It goes back to YES the moment a
re-measurement is taken and recorded; until then the honest reading is that the
pin is SET and unverified. (The row is not deleted, because a target with no
measurement is itself a fact about loop state.)

‡ Not an accuracy result either, though for a different reason and the verdict
stands: Mode B re-prices the mid it inverted, so `mode_b_price_mae` is near an
identity for `|mid - srPrc|` — a property of SpiderRock's smoothing rather than of
our pricing. See the Mode B section. Marked here so the table carries its own
caveat instead of relying on a reader reaching the prose.

**Delta is the first and so far only accuracy target met** (0.0022 against 0.01).
Price MAE is 8.7x its target — down from 376x, on the strength of a resolved input
model rather than of tuning: pricing on the discrete-dividend lattice with the
reconstructed cash schedule moved pooled price MAE 40.54 -> 8.66 ticks (4.7x) against
the prior committed map, and every greek except volga improved with it.

The two-convention gap is the live signal. Theta, rho, phi and delta-decay are
materially wrong under the standard convention (7.3x, 0.69x, 1.00x, 0.81x) while
sitting at a few percent under symmetric. Per `CONVENTIONS.md` that spread is the
signature of a scale or basis error the symmetric loss partly absorbs, not of a small
numerical residual.

`mode_a_vol_mae = 0` is an IDENTITY, not an achievement: Mode A prices AT `srVol`,
so the vol it reports back is the vol it was handed. It becomes a real measurement
only under Mode B. Never cite it as vol accuracy. This warning outlives every
regeneration of the numbers above — it is a property of Mode A's construction, not of
any particular sweep.

## Two error conventions — never unify

- **symmetric** `|m-o| / max(|m|,|o|,floor)` — the RATCHET BASELINE and the gated
  no-regression criterion, because it is the loss the scale selection minimises and
  it is bounded with no smallest-scale gradient.
- **standard relative** `|m-o| / max(|o|,floor)` — published beside it only so the
  floor stays comparable to the charter's "greeks within 1% rel" target. NEVER gated.

Bounded rule: a symmetric metric may regress only while `candidate <= baseline * 1.01`,
and every permitted regression is PUBLISHED in `accepted_regressions`. Cross-checked in
BOTH directions by five layers in three languages.

Accepted regressions at `54024add`: **NONE**. Every symmetric metric is at or below
its baseline and `accepted_regressions` is committed empty. This supersedes iter-000's
single accepted regression (`mode_a_vega_rel` 0.081233446188804986 ->
0.081468501930500911, `pct_of_baseline` 0.002893583280335071), which belonged to the
escrow-era floor the regeneration reset.

## Speed

**ONE pin, not two.** `speed.metric_id = rel_avx2_rows_per_second`, measured by
`convention_speed_measure` on a quiet host, rel-avx2, 264,026 rows:

- baseline: **9958.75 rows/s** (`9958.7451327843`)
- pin: **8962 rows/s** = `floor(baseline * 0.90)`

The pin is DERIVED, never copied — a pin equal to the baseline turns
re-measurement into a coin flip on run-to-run noise, so the 10% margin is part of
the contract and the validator rejects any pin above `baseline * 0.95`.

**And that is the whole speed record: a pin, with nothing measured against it
yet.** The scorecard has no third field — no `measured`, no `current` — and
`iter-000.json` is the only scorecard in `bench/oracle/scorecards/`. So the
Targets table above reports Speed as `n/a`, not YES: the number in its Current
column IS the baseline, and `baseline >= floor(baseline * 0.90)` is arithmetic,
not a gate. The first genuine re-measurement turns that cell into a verdict; it
is not one now. Anyone taking that measurement should record it in the scorecard
first and let this file follow, in the direction this whole section is meant to
flow.

`diagnostic_speed` is NOT the pin and is not performance evidence: rel-avx2 full
attribution, 2646.7 rows/s over 105.0 s, committed with `citable: false`.

Both objects are copied from `atx-vol/bench/oracle/scorecards/iter-000.json` —
the same file the Ratchet-baseline row above names — and they agree cell for cell
with the Speed-pin section of `atx-vol/bench/oracle/CONVENTIONS.md`. Re-derive:

```
python -c "import json;d=json.load(open('atx-vol/bench/oracle/scorecards/iter-000.json'));print(d['speed'],d['diagnostic_speed'])"
```

This section previously published a **second, invented pin** — baseline
3469.4698564618907 / pin 3122, a "re-measured 3857.54 rows/s", and a "dev preset,
~770 rows/s" diagnostic — attributed to iter-000. None of those four numbers
appears in the scorecard or in `CONVENTIONS.md`; 3469.47 was an escrow-era
convention-sweep figure, never a ratchet pin, and the scorecard carries exactly
one `speed` object. Recorded rather than silently deleted, because a dashboard
publishing numbers that exist in no source of record is the exact failure this
file's regeneration was meant to end.

## Open leads, ranked

1. **The standard-relative column is where the remaining error lives.** At
   `54024add` theta is 0.0639 symmetric against **7.3116** standard-relative, and
   rho / phi / delta-decay show the same shape (0.0217 vs 0.6926, 0.0245 vs 0.9976,
   0.0903 vs 0.8085). A two-orders-of-magnitude spread between the two conventions
   on exactly the `per_day` + `BUS_252` metrics is the fingerprint of a basis or
   scale error the symmetric loss partly absorbs, not of a small numerical residual.
   Still the highest information-per-unit-work lead on the board — and now the only
   one of this shape left, since the input-model resolution took price with it.
2. **Volga is the one greek the discrete-dividend tree did NOT win** (0.0831
   symmetric / 0.1875 standard-relative). Every other metric improved when the map
   moved to `discrete_dividend_tree__rate__sdiv_yield`; volga did not. See the
   lattice-volga ledger entries at `e9e6a306`.
3. **`secant_252` is plumbed but unfalsifiable.** Both time-decay arms tie on every
   key, so `analytic_derivative` won on the field-by-field identity tie-break rather
   than on evidence (ledger at `635f8bd8`). The decay axis is therefore RESOLVED in
   the map but not DEMONSTRATED.
4. **Mode B blocked on a schema gap.** `OracleRow` has no date/bucket field and
   `run_oracle_bench_core` flattens all partitions, so per `underlier x expiry x
   bucket` fitting cannot group correctly until partition identity reaches the row.
5. **`calcEngine` is absent from our data.** SpiderRock's two-tier
   `{FastHybrid, NumericLow, NumericStd, NumericMax}` selection is unobservable
   here, which is an irreducible reproduction floor of unknown size.

## Oracle suspects

`oracle_suspect_candidates` is EMPTY and `market_evidence_status` is
`not_evaluated_no_nbbo_gate`. No cell has been excluded from the ratchet, because no
NBBO gate has run. The oracle is the north star, not truth; that list stays honest by
staying empty until evidence fills it.

## Mode B — the first REAL vol measurement

PROVENANCE: every number in this section was measured BEFORE the `54024add`
convention regeneration and has not been re-measured against the resolved map. Read
it as the last Mode B measurement, not as the current one.

`mode_b_vol_mae = 442.79 bp` against a 10 bp target. Unlike Mode A's 0 bp, this is a
measurement and not an identity.

SAFETY, load-bearing: `american_implied_vol` screens only IMMEDIATE intrinsic and
returns `Ok(kIvMin)` — a SUCCESS — for a quote below the true floor. Taken at face
value it publishes 0.005 as a measured volatility for every dead deep-wing quote. The
correct bracket is the zero-vol American price = max(immediate intrinsic, DISCOUNTED
FORWARD intrinsic); the forward leg is larger for a call OTM on spot but ITM on the
forward, the everyday case on hard-to-borrow names. Mode B brackets itself and the
gate validates row-accounting closure, so a run that fits 2% and refuses the rest can
no longer publish eleven flattering numbers and pass.

Refusals, 36,900 of 299,798: below-lo-bound 35,477 | round-trip-failed 1,299 |
vega-below-floor 109 | above-up-bound 15 | at-floor 0. `at-floor = 0` proves nothing
reaches the library clamp.

`mode_b_price_mae = 35.52` is NOT our pricing improving. Mode B re-prices the mid it
inverted, so the metric is near an identity for `|mid - srPrc|` — a property of
SpiderRock's smoothing. Never cite it as our accuracy.

## Next

**The price leg answered itself, and it was the INPUTS.** The escrow-era plan on this
line was a QuantLib Andersen-Lake cross-check to split "our pricer is wrong" from
"our inputs/conventions are wrong". That question is settled without it: the closed
staged sweep at `54024add` moved pooled Mode A price MAE 40.54 -> 8.66 ticks (and
376.06 -> 8.6633 against the floor this dashboard used to publish) purely by changing
the INPUT MODEL to the discrete-dividend lattice with the reconstructed cash schedule
— same pricer throughout. The external cross-check is therefore NOT in flight and is
not the next step; the ~10x price-leg outlier argument it was meant to adjudicate was
computed off the 376-tick floor and does not survive it.

**What is genuinely next** is lead 1: theta / rho / phi / delta-decay sit at a few
percent symmetric and up to 7.3x standard-relative, which is a basis or scale error
rather than a residual, and it is now the largest identified error on the board. Any
re-derivation of Mode B's price-leg arithmetic must start from 8.6633 ticks, not from
376.06 — the Mode B figures below (442.79 bp, 35.52 ticks) predate the regeneration
and have NOT been re-measured against the resolved map.

BLOCKER for any ratchet iteration: the ready-state Measure/Improve path is still
RETIRED behind `READY_BROKER_MIGRATION_REQUIRED` and `vol-sprint` is a fail-closed
stub. Migration lane in flight. Reaching `ready` does NOT mean the loop runs.
