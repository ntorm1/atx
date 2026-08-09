# Fundamental signal improvement loop 05: survivorship and industry controls

Date: 2026-08-09

## Research question

Can the operating-profitability leader survive two research-design controls that are required
before the signal is production-certified?

1. Forward returns must include a terminal return when a security delists inside the holding
   window.
2. The factor must be testable within point-in-time industry groups so its result is not merely an
   industry allocation.

## Primary-source basis

- Shumway documents that omitted delisting returns create a material bias in CRSP-based return
  studies: <https://www.tylergshumway.org/Shumway-DelistingBiasCRSP-1997.pdf>.
- Beaver, McNichols, and Price show that accounting-anomaly inference is sensitive to whether
  delisting firm-years and returns are included:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=949601>.
- Ball, Gerakos, Linnainmaa, and Nikolaev use CRSP delisting returns and impute -30% only when a
  missing return is **performance-related**; they also exclude one-digit SIC 6 financial firms:
  <https://www.ivey.uwo.ca/media/3775325/gerakos.pdf>.
- The SEC states that its submissions bulk ZIP contains the public EDGAR filing history for all
  filers and is rebuilt nightly:
  <https://www.sec.gov/search-filings/edgar-application-programming-interfaces>.
- FF12 is the warehouse's governed coarse industry control. Its SIC definitions follow the Ken
  French Data Library:
  <https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_12_ind_port.html>.

The -30% rule is therefore not valid for a generic Nasdaq delete whose reason could be a merger,
transfer, or performance failure. The implementation now refuses that imputation without
performance-related reason evidence.

## Live warehouse audit

Before this loop:

| Surface | Rows | Finding |
|---|---:|---|
| `listing_status_intervals` | 12,944 | Only two explicit inactive intervals |
| `nasdaq_listing_events` | 27 | Two deletes, both effective 2026-06-26 |
| `delisting_events` | 0 | Builder existed but had not been run |
| `delisting_return_observations` | 0 | No observed public/vendor DLRET |
| `delisting_terminal_returns` | 0 | No observed or policy terminal return |
| `forward_returns_survivorship_safe` | 0 | Cannot be built honestly without terminal evidence |
| `entity_classification` | 0 | Taxonomy/classifier existed but was not activated |

Price-history endpoints cannot be treated as delistings. Of 9,346 securities with bars, 8,411 end
before the global last bar, and 6,355 end in 2015. That distribution is coverage architecture, not
credible evidence of 8,411 economic delistings.

## Build

### SEC classification activation

Downloaded the official SEC nightly `submissions.zip` snapshot:

- bytes: 1,556,554,492
- SHA-256: `96b089278d81eae11e6887700a737dc7c77478b3ca87d71da4bb214927904f14`
- lineage: recorded in `raw_source_files` for `entity_classification`

Loaded:

| Surface | Result |
|---|---:|
| SIC taxonomy nodes | 493 after on-demand leaves |
| FF12 taxonomy nodes | 12 |
| SIC classifications | 6,961 securities |
| FF12 classifications | 6,961 securities |
| NAICS classifications | 6,886 securities |
| Governed-universe names with FF12 | 956 / 1,081 |
| Open `(security, taxonomy)` duplicates | 0 |
| Industry-template routes | 27,130 |

All entity classifications have `valid_from=2026-08-09` and corresponding knowledge timestamps.
That makes them valid for current/prospective features, but correctly unavailable to earlier
backtest dates. The reference-classification CLI now cascades into industry-template routing so a
single operator command activates the whole layer.

### Evaluation controls

The evaluator now provides:

- `--return-target adjusted_prices|survivorship_safe`;
- a source-partitioned, formation-key-scoped loader for
  `forward_returns_survivorship_safe`;
- `--neutralize-taxonomy FAMA_FRENCH_12` using within-group percentile ranks;
- effective-date, `as_of_date`, and `available_at` predicates on the classification join;
- minimum group-size and usable-coverage gates that fail closed;
- complete method provenance in `factor_eval_manifest.params_json`.

Generic `NASDAQ_DELETE` events no longer permit the -30% imputation. The live event build now
contains two high-confidence public delete events (PRA and SDM) with unobserved returns and no
fabricated terminal values.

## Analysis

Strict robustness runs for `profitability_operating_profitability` produced:

| Run | Outcome |
|---|---|
| Survivorship-safe target | Rejected: target table has no observed/policy terminal returns |
| PIT FF12-neutral | Rejected: historical usable coverage is 0.00%, below the 80% gate |

The adjusted-price baseline was rerun with explicit target lineage:

| Horizon | Mean rank IC | HAC t-stat | Top-minus-bottom spread |
|---:|---:|---:|---:|
| 21d | 0.02061 | 2.83 | 0.294% |
| 63d | 0.03054 | 3.00 | 0.890% |
| 126d | 0.04350 | 3.45 | 1.282% |
| 252d | 0.05603 | 3.46 | 2.068% |

Run id: `loop5-op-adjusted-baseline`.

## Decision

Operating profitability remains the leading **provisional** signal, but this loop does not promote
it to survivorship- or industry-robust status. The correct production decision is `not evaluable`,
not a made-up robustness pass.

Prospective FF12-neutral evaluation can begin immediately after 2026-08-09. Historical
certification needs either filing-time SIC extracted from individual EDGAR filing headers or a
licensed point-in-time classification history. Survivorship certification needs observed terminal
returns or sufficient public corporate-action consideration to compute a governed policy return.

## Next loop

Build a signal whose input coverage and current production use do not depend on the unresolved
terminal-return history, while continuing to accumulate prospective classifications and explicit
listing-event evidence. The next candidate is change in shares outstanding / net issuance, which
has a distinct financing-behavior channel and can be constructed from the existing SEC share-count
facts plus split normalization.
