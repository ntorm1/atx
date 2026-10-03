# Task 3 fix round 1 re-review

Reviewed the supplied per-commit package `review-T3-r1.diff` (791767cd, dc68f3bf, 2d2c5af5, d8f28ad6), reviewer/resume contracts, task brief, GC1–GC9, gap-analysis §2 item 2, recovered findings, and report including final attribution. No git reconstruction, full suite, or real-data build was run. Only an offline fixture was executed for the specific clock doubt below.

### Spec Compliance — ❌ Needs fixes

Original Important findings:

1. **NOT ADDRESSED fully — historical comparative vintages.** `_vintages` now emits older-quarter and annual-period rows and preserves their base fiscal identity, but its per-accession minimum clock silently loses later-clock components of the same comparative filing. See Important I1 below. This is an internal reconstruction defect, outside the T3-a upstream routing.
2. **ADDRESSED — annual/TTM contamination and fyr resets.** `compustat_std.py:432` limits FY calendar sources to annual forms and ignores FY-labelled 10-Qs; `:601` requires the base period's annual `dform`. Later 10-Q annual comparatives keep annual base values/calendar rather than adopting the current quarter's TTM. The new fixture assertions distinguish both cases.
3. **ADDRESSED — misleading MIBT/NIADJ names.** `compustat_std.py:211` replaces these with `tempeq` and `nicon`; code, tests, dataset documentation and registry stub explicitly call them extensions and do not claim verified Compustat equivalents.

T3-b is implemented: `compustat_std.py:924` checks matched primary 8-K acceptance ET date against actual accepted filing clock only when provenance is `fsds_accepted_utc`, otherwise `filed`. The same-date metric controls the unchanged .95 gate and strict share remains reported. It does not alter published item availability. Reported rdq same-date .9015–.9459 and strict .8500–.9008 remain FAIL.

T3-a routes demonstrated upstream cases only; sum4 and balance remain FAIL, with no threshold waiver. Final attribution JSON and its SQL support 554 base-pass/restatement-fail periods: 552 USD and 2 FX, complete visible history, stage matches already-failing stock history, zero complete passing-history cases made to fail in that subset. This evidence does not establish that every possible reconstruction is correct or excuse I1. Retained base FX conversion and unmapped catalog/share/debt history are expressly documented limitations.

Changed package paths do not overlap foreign-dirty-files.txt and do not edit shared registries. Build cap 1.0 GiB and 32 buckets respect GC4. Report discloses the earlier memory failure and final publication; inspected final build/validate/cutoff stderr has bucket progress only, no warnings. Original acceptance criteria remain 5 PASS / 3 FAIL with measured values and guarded commands.

⚠ Cannot independently verify real-data gate outcomes from the diff alone; inspected saved report/attribution evidence rather than rerunning them. Coverage on Task 2 universes remains an explicitly recorded dependency qualification.

### Strengths

Annual-form protection and extension naming directly resolve the corresponding recovered findings. Derived equity now moves by actual published component changes, including a component first observed in history; the fixture explicitly exercises base MIB 50 → 60 and revised TEQ 470. Attribution separates demonstrated upstream balance deterioration from internal component bookkeeping. Validation joins and cutoff keys now include datadate, preventing comparative-row collisions.

### Issues — Critical / Important / Minor

Critical: 0. Important: 1. Minor: 0.

**I1 Important — later-clock components in a comparative accession are permanently discarded (`atx-db/src/atx_db/alpha_panel/compustat_std.py:616`, `:632`, `:640`).** `vk` groups by `(cik, period_end, accession)` and uses `min(h.available_at)` as its sole restatement clock. All component selection is then bounded by that minimum. If the same accession supplies one item at t1 and another at t2 > t1, the second item never reaches any row even after t2. Keeping the first item's clock conservative does not make that lost later value faithful.

Focused offline reproduction used the existing `World` fixture via importlib and `pytest.MonkeyPatch`, executing `atx-db/.venv/Scripts/python.exe -` with `PYTHONPATH=C:/atx/atx-db/src`. Base Q1 at 2021-05-01 has sale 100 and assets 1000. Q2 accession restates that Q1 sale to 90 at 2021-08-01 and assets to 1010 at 2021-08-02. Build output for Q1 is exactly:

```text
q1  2021-05-01  saleq=100  atq=1000
q2  2021-08-01  saleq= 90  atq=1000
```

There is no later row and latest assets remain 1000 instead of 1010. The fixture completed successfully in approximately 2.5 seconds; it operated solely on temporary synthetic parquet inputs. Existing cutoff equality cannot detect this omission because both publication and rebuild use the same minimum-clock logic. The amended cutoff fixture itself contains a same-accession delayed CFO input but asserts row equality, not eventual visibility of that delayed value.

Fix: define a conservative row policy that includes every supplied component at its legitimate clock. With the current one-row-per-accession keys, delay the combined row to the maximum required input/filing clock, with cutoff handling that does not emit a permanently partial row when later components are hidden. Alternatively introduce separately keyed clock vintages if the contract is explicitly adjusted. Add behavioral assertions for before/after t2 visibility and full-vs-cutoff rebuilds. Do not backdate the later component or route this internal loss to Task 5.

### Assessment — Task quality: Needs fixes

Original findings: 2 ADDRESSED, 1 NOT ADDRESSED fully. Spec and quality both need fixes for the demonstrated internal vintage-clock omission. The annual/naming corrections and demonstrated upstream attribution can stand; the unchanged three failed acceptance gates remain explicit handoffs, not waived passes.
