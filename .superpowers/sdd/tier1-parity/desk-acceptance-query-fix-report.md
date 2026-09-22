# Acceptance-query review repairs

Root fixed both Important findings in the single static review. There were no
Critical findings; no further review or repeated runtime validation is required
by the agreed process.

1. The frozen executed `quarterly-eps-acceptance.sql` and its JSON result are
   unchanged. New `quarterly-eps-acceptance-v2.sql` determines publication
   status only from the selected derived state: absent, invalid/unavailable,
   null, nonfinite, or valid. The separate raw status retains missing-input,
   zero-denominator and nonfinite diagnostics. A release-sourced Q4 can now
   report a valid published value while direct Company Facts inputs are absent.
2. The CF1 readout pins both `atx_custom_price_liquidity_v1` and
   `tbltickerhistory3_10y` against each build/evaluation manifest, in addition
   to existing version/hash/run/date/horizon pins. It returns the actual four
   provider-name fields. All eight hypotheses and three splits remain present.

These are read-only reporting edits. Archive8 remains the sole runtime writer;
no test, import or database query was launched for the repairs. The v2 EPS and
CF1 queries are prepared, not executed. Root will run v2 after materialization
and CF1 readout after the existing evaluator, with new immutable evidence
outputs under the memory guard. No full suite or production source changes.
