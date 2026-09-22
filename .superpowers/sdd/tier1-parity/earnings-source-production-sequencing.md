# Earnings-source production sequencing — 2026-09-22

The read-only queue measurement at 23:53:57 UTC found **426,151 unique 8-K
Item 2.02 candidates**, spanning filing years 2004–2026, in the currently
retained submissions. This is an incomplete denominator because the full
submissions archive resume is still pending. No candidate has a filing date
after the fixed 2026-09-20 snapshot. The measurement made no network requests
and peaked at 0.928 GiB under the 2 GiB guard.

Evidence: `earnings-source-queue-before-submissions-resume.json`,
`measure_earnings_source_queue.py`, and `earnings-source-queue1-memory.json`.
The measured queue is not a count of supported earnings tables, requests
already made, or missing EPS observations. The current loader attempts an
index for each nonterminal candidate and may fetch an exhibit afterward.

## Production ruling

The user's priority is a usable full-universe fundamentals warehouse. Do not
put an unmeasured, all-history, per-filing exhibit backfill in front of the
first full CompanyFacts-based statement, ratio, growth and market build.

1. Finish the verified full CompanyFacts and full submissions archive resumes.
2. Run the governed earnings source for CIK `0000093410`, through the fixed
   snapshot, as the explicit CVX end-to-end acceptance scope. Preserve its
   actual scope in the run options and evidence. This is not full-universe
   earnings-release coverage and cannot certify a provider SLO.
3. Run the full-universe `activation-run5` suffix from `statement_points` with
   `--force`, then execute the EPS and institutional desk acceptance SQL.
4. Use measured item gaps to schedule additional earnings-source waves and
   rerun affected downstream materialization. Full historical source coverage
   remains open; never report the acceptance wave as completion of that work.

Prefer bulk source delivery for the historical backfill. SEC's official
[Accessing EDGAR Data](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data)
documentation, checked on 2026-09-22, identifies daily tar/gzip archives under
`/Archives/edgar/Feed/` and concatenated submissions under
`/Archives/edgar/Oldloads/`. It also explains that later corrections/removals
are not reflected in prior daily archives. Their byte volume and compatibility
with our receipt/provenance contract are not yet measured; no archive download
or new adapter was started. Do not silently substitute a corrected archive
for a certified historical vintage.

This changes execution order, not the Tier-1 objective or any coverage gate.
It supersedes the earlier instruction to exhaust the entire earnings-release
candidate queue before the first downstream build. Keep the existing memory
limits, dummy SEC contact, immutable evidence and measured-threshold rules.
