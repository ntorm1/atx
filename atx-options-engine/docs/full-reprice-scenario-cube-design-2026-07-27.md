# Segmented full-reprice options risk evidence

Date: 2026-07-27
Status: revision-2 implementation contract

## Decision

The risk producer operates on one decision-time active catalog at a time. The
catalog is:

```text
new-trade candidates
UNION filled positions
UNION working orders
UNION pending cancels
```

It is not the union of every contract seen over the backtest horizon.
Consequently, retained risk memory is `O(active contracts * scenarios)` for a
segment rather than `O(historical union contracts * dates * scenarios)`.
Inactive historical contracts cannot make a later decision fail merely because
their old risk row is absent. A contract with economic state must remain in the
active catalog until that state is resolved.

Each input row is a decision-only `OptionScenarioActiveContract` and carries an
explicit bitmask for candidate, filled-position, working-order, and
pending-cancel state. The compiler rejects zero-role, duplicate, out-of-order,
or mixed-decision catalogs. A PIT lifecycle-source attestation carries
observation/availability clocks, persisted identity, and expected counts for
each role; all four counts must match the supplied union. It does not accept
`OptionResearchPanel`, so execution timestamps and ex-post outcome labels
cannot cross the live risk API. The lifecycle owner remains responsible for
the truth of the attested source artifact.

This is the first vertical slice of the partitioned point-in-time evidence
plane. Session continuation across separately compiled segments and immutable
segment persistence are follow-on work.

## Why the existing scenario grid is not the producer

`atx-vol::scenario_grid` is a book-level analysis tool. It:

- returns portfolio totals instead of per-contract evidence;
- uses Taylor reconstruction inside measured radii;
- may fall back to Taylor when an exact lane fails; and
- excludes failed base lanes.

Those are useful exploratory semantics, but not authoritative pretrade risk
semantics. `compile_option_scenario_cube` instead produces
shocked-minus-base dollars for every active long contract. A missing surface,
bad lineage, stale clock, unsupported model corner, or nonfinite solve aborts
the entire segment. There is no skip and no Taylor fallback.

## Revision-2 scenario semantics

The versioned surface dynamics are `FrozenStickyStrike`.

For contract `(K, expiry, side, multiplier)` at decision time `t`:

```text
T0     = (expiry - t) / (365.25 days)
base   = surface.resolve(K, T0)
S'     = S * (1 + simple_spot_return)
T'     = max(0, (expiry - t - horizon_ns) / (365.25 days))
raw_sigma' = base.sigma + absolute_vol_level_shift
sigma'     = max(minimum_implied_vol, raw_sigma')
r'     = base.rate + absolute_rate_shift
q'     = base.q_eff
PnL    = (full_price(S', K, T', sigma', r', q') - base_full_price)
         * multiplier
```

`T' == 0` uses exact spot intrinsic rather than a synthetic positive-tenor
clamp. American contracts retain the archive's selected American method and
resolved method options. European contracts use Black-76 with the forward implied by shocked
spot and the frozen carry. The base and scenario routes are consistent within
each exercise style.

The manifest requires exactly one shock for every
`(scenario_id, active underlier_uid)`. This supports coherent simultaneous
index/component moves without letting the low-level producer invent
constituents, betas, correlations, or basis offsets.

Every live-tenor lane for which `raw_sigma'` binds the floor is counted.
The default policy permits zero hits and fails the whole build otherwise.
Callers may set an explicit bounded allowance; the actual count is retained in
the build report and the allowance is bound into the risk digest. These are
stress marks, not independently calibrated surfaces. Revision 2 does not claim
that a transformed cross-strike or calendar surface has passed the admission
oracle.

## Cash Greek units

Every row is for one listed contract:

```text
delta_cash = delta_spot * S * multiplier
gamma_cash = gamma_spot * S^2 * multiplier
vega_01    = vega * 0.01 * multiplier
theta_day  = theta_calendar / 365.25 * multiplier
vanna      = vanna_spot * S * 0.01 * multiplier
volga      = volga * 0.01^2 * multiplier
premium    = abs(PIT market mark) * multiplier
```

Gamma and volga retain the raw second derivative; a PnL expansion applies its
own one-half coefficient.

## Lineage and deterministic construction

The compiler requires:

- a canonical, one-decision active catalog whose rows have explicit lifecycle
  roles and no ex-post fields, plus a PIT lifecycle-source attestation whose
  role counts match the catalog;
- surface observation time no later than externally observed archive
  availability, and availability no later than decision time;
- scenario-manifest observation no later than availability, availability no
  later than decision, and effective time no later than decision;
- a nonzero explicit maximum surface age;
- the exact loaded surface archive identity; and
- healthy, nonlegacy, risk-purpose surface provenance with nonzero validation
  identity and source/served generations for every referenced underlier; and
- separate quote-event, quote-availability, and execution-source lineage for
  the market mark used by premium limits;
- a positive maximum quote age, a tradable quote status, and a definition
  available no later than the quote event.

It canonicalizes scenarios by ID and shocks by `(scenario_id, underlier_uid)`.
Scenario tasks write disjoint cells and results are byte-identical across input
permutations and worker counts.

Two SHA-256 values are retained in `OptionRiskPanelProvenance`:

- the scenario-manifest digest covers schema/version, dynamics, volatility
  floor, manifest clocks and artifact identity, scenarios, and shocks;
- the risk-snapshot digest covers model/convention versions, clocks, age policy,
  volatility-floor allowance, lifecycle roles and attestation, accepted audit
  status, definition/quote clocks, surface and quote identities, generated
  contract rows, and every PnL cell.

The panel's existing 64-bit definition hash remains a regression fingerprint,
not cryptographic provenance.

## External methodology boundaries

The initial house grid is not represented as regulatory margin:

- [FINRA Rule 4210](https://www.finra.org/rules-guidance/rulebooks/finra-rules/4210)
  defines approved-model and valuation-point requirements. A house scenario
  grid must not be called TIMS-equivalent.
- [CME SPAN methodology](https://www.cmegroup.com/clearing/files/span-methodology.pdf)
  defines price/volatility scan arrays, special extreme cells, spread charges,
  and short-option minimums. Merely reproducing price/vol shocks does not
  reproduce SPAN.
- [OCC margin methodology](https://www.theocc.com/risk-management/margin-methodology)
  uses portfolio simulation, dependence, concentration, and Expected Shortfall.
  This deterministic pretrade envelope is not STANS.
- Derman's original
  [volatility-regime paper](https://emanuelderman.com/wp-content/uploads/1999/03/risk-regimes_of_volatility.pdf)
  motivates testing both sticky-strike and sticky-delta behavior. Revision 2
  implements only the explicitly versioned frozen sticky-strike lane.
- Gatheral and Jacquier's
  [arbitrage-free SVI paper](https://arxiv.org/abs/1204.0646) motivates
  revalidating surface shape after future skew/curvature shocks rather than
  assuming an arbitrary transformed smile remains admissible.

## Explicit revision-2 limitations

- no sticky-forward-moneyness, skew, curvature, or term-structure shock;
- no dividend, borrow, FX, or currency-conversion shock curve;
- no constituent membership, weight, beta, or correlation projection;
- no adjusted deliverables;
- no scenario probabilities, Monte Carlo, VaR, or Expected Shortfall;
- no immutable on-disk segment store yet;
- no execution-session continuation across changing catalogs yet.

Those features require new manifest/storage schema versions. They must not be
silently inferred inside this producer or retrofitted by reinterpreting
revision-2 fields.
