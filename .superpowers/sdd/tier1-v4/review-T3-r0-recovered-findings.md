# Task 3 round 0 findings recovered from ledger

The original review text was not persisted. These are the binding recorded findings, not a claim to reproduce unavailable verbatim text.

1. Important: vintage model: restated older quarters/annual comparatives not emitted as fundq/funda rows (internal cause of sum4 FAIL).
2. Important: funda leaks 10-Q rows (1,364; 483 >=2020) with TTM values, fyr resets from 10-Qs DEI-labelled FY (192 rows/101 issuers).
3. Important: `mibt` holds temp equity under Compustat MIBT name; `niadj` unverified.

Ruling T3-b: rdq gate metric = 8-K 2.02 accepted on or before the same ET calendar date as the row's filing; strict share must also be reported. Do not backdate announcement-clock values.

Ruling T3-a amended: fix internal vintage issue in round 1; residual upstream v10 Q4/sum4 and mezzanine balance issues route to Task 5. rdq residual routes to Task 9. These failures remain explicitly reported.
