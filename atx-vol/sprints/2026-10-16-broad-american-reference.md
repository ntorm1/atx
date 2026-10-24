# Broader independent American reference panel

The earlier 24-contract ATM comparison omitted funded, separate_clocks and
high_vol fixtures. This extension selects those three existing synthetic cases,
both American sides, and each available strike nearest .8,1,1.2 times spot:
79.375,100,118.75. The choice uses contract/model parameters only. It reads no
vendor labels and changes no production settings.

The narrow input derives from immutable sinh-v3 audit SHA256
`28d6aeabf3cc985b952ad62d8d4f35dca546d196ffae3757ce17da583a481739`.
Input `markov-audit-20261016-broad-reference-input-v1.json` has SHA256
`98ec610606a926501a8845174deaf73278b4349119c54f894f45e3085299071f`.
All artifacts below reside in `C:/atx-cache/quote-benchmark/`.

Reference tool: integrated `american_reference_audit.py`, isolated pinned
QuantLib1.44 environment, base800 time/space and3 refinement levels, absolute
tolerance .0001, unchanged relative tolerance. Its durable per-contract journal
completed18 references:15 observed stable,3 unresolved. All three unresolved
were high_vol (call79.375, put100 and118.75). Original output:

- `american-reference-20261016-broad-v1.json`, SHA256
  `279fb6f50009ed9597755d4a44d0fd694b601c8d4f84baffe03b3bafa3f44171`.

The same input filtered to high_vol was refined at base1600 time/space and3
levels, with identical tolerances. All six high_vol contracts were observed
stable. This explicit six-row replacement retains the earlier reference history:

- `american-reference-20261016-high-vol-refined-v2.json`, SHA256
  `fc38a3bfc1c28d6b40bede38ba15f44df19ec01a87941ddb39aa251e162f9b28`.

Observed refinement stability is evidence, not a rigorous error bound. No local
volatility calibration or continuous-American accuracy guarantee is inferred.

## Exact paired comparisons

All18 contract keys match the audit without name-only joins. The comparison
tools keep the full requested audit universes, including refused cases, and use
only observed-stable references for their primary error summaries.

| Profile | Stable priced references | American price MAE | Maximum absolute error |
| --- | ---: | ---: | ---: |
| default65 |18/18|.028841080502|.106942367779|
| adaptive129 upper4S |18/18|.001986835263|.010837906962|
| adaptive129 upper8S |18/18|.000372908262|.001131695962|
| adaptive129 upper16S |18/18|.000717406588|.003445068067|
| matched-spacing upper8S |18/18|.000460057963|.001592687112|
| matched-spacing upper16S |18/18|.000463768421|.001598019361|
| adaptive129 upper4S time2 |18/18|.002288384681|.011821827955|
| adaptive129 upper4S time4 |12/18|.000526512796|.001127245289|

Time4 excludes the high_vol references because that model exceeds unchanged
resource limits. Its aggregate is not comparable to the18-row entries without
using the saved exact-pair summaries. Time2 also refuses near_cash outside this
reference set, so full-audit coverage is88.8889%; time4 is83.3333%. Every other
listed new profile retains full requested coverage. More temporal refinement
does not monotonically improve these total errors: spatial/time cancellation
and boundary truncation must be assessed separately.

The new matched-spacing profiles use bounded ascending node searches, not a
monotonicity assumption. Their high_vol state counts164/196 preserve both
near-spot gaps of the129-state upper4S grid. Wider boundaries reduce upper mass
while fixed counts coarsen the center. The separate spatial/time controls show
why a universal rule to increase either domain or time resolution is not yet
justified. No grid policy was tuned to srPrc/srVol.

Comparison receipts (explicit refined-reference replacement):

- `common-broad-sinh-v3-compare-20261016-v2.json`, SHA256
  `11ab57def8ebe7eaf11db3e7d2da2ffe5d73cd6031d8a893750b1042d6f79591`;
  46,656 rows across18 grids.
- `common-broad-controls-compare-20261016-v2.json`, SHA256
  `b8197a3a2e72d2f8de189d5fa97265a542a3f27dbd930275c644fac98e856ec7`;
  12,960 rows across5 grids. This receipt uses controls-v1, SHA256
  `e514f9539e91f1d28e40813068f32b521efd32dee85f833e9adff45747ff758c`.
  The subsequent controls-v2 artifact has identical numerical results; its
  corrected clock metadata and exact-source receipt are documented in
  `2026-10-16-audit-spatial-time-controls.md`. Neither artifact is overwritten.

The first full common-model parquet fit is still running independently of these
synthetic controls. Its vendor benchmark is a separate claim and denominator.
