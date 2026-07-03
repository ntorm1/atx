# XBRL DQC SQL Subset

PF-S7 S7-2 adds an offline, SQL-native subset of XBRL US Data Quality Committee
checks to `xbrl_validation_results` under `rule_family = 'dqc'`.

Official anchors used for the subset documentation:

- XBRL US rules guidance: https://xbrl.us/home/priorities/data-quality/rules-guidance/
- DQC_0015 Negative Values: https://xbrl.us/data-rule/dqc_0015/
- DQC_0053 Excluded Members from an Axis: https://xbrl.us/data-rule/dqc_0053/

The controller snapshot for this sprint recorded approved plugin version 30.0.0
(June 2026) and 196 approved DQC rules. Tests do not use the network.

## Ported

- `DQC_0015`: curated non-negative US GAAP concept subset only. The in-code list
  covers common monetary/share-count concepts already present in local fixtures
  and cached facts, such as `Assets`, cash, accounts receivable, inventory, PP&E,
  liabilities, revenues, and `CommonStockSharesOutstanding`. A negative numeric
  fact for one of these concepts emits a failed row.
- `DQC_0053`: SQL-expressible excluded member-axis subset. The check flags a
  filing dimension when local `xbrl_dimension_edges` directly marks the
  axis-member relation `usable = false`, or when it matches the tiny curated
  fallback disallowed pair list in `xbrl_validation.py`.

## Skipped

- Full DQC_0015 member exclusions and the complete official element catalog.
  Those require the full DQC rule catalog and XBRL dimensional semantics beyond
  this safe SQL subset.
- Full DQC_0053 dimension-domain-member closure. The local normalized dimension
  surface stores direct definition-linkbase edges, but this subset does not infer
  every transitive domain/member relationship or target-role chain.
- The other approved DQC families. They need Arelle/full plugin semantics,
  richer rule parameters, or rule-specific taxonomy interpretation and are not
  claimed by S7-2.
