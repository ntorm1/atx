# Reported-quarter EPS source: static review

**Disposition: reject pending the Critical fixes.**  This was a source-only,
static review of the preserved 0320 draft; no draft or production code was
executed, imported, modified, or committed.  Source evidence is not an
accepted core/standardized/derived EPS result; the separately owned 0321
resolver still has to establish that state.

## Critical

1. **The production Chevron exhibit is rejected, so the proposed generic parser
   does not meet the actual target case.**  `_HtmlTableCollector` records only
   physical cells and ignores `colspan`/`rowspan` (draft
   `press_release.py:233-267`).  `_period_header_column` then treats every
   current-year header as a flat column (`366-382`).  Attachment 1 has a
   two-level `Three Months Ended December 31` / `Year Ended December 31` header
   with both 2025 columns; it therefore has no unique 2025 column.  The
   candidate row is also just `- Diluted`; its required `per share` parent and
   `Net Income (Loss) Attributable to Chevron Corporation` ancestor are on
   preceding rows, while the extractor examines only `row[0]`
   (`441-460`).  The official release reports 1.39 in that current quarterly
   cell and 1.84 in the prior-year comparable, while its summary contains the
   adjusted values as well.  Add a span-expanded header grid and inherited row
   label hierarchy; select the leaf that belongs to the explicit quarterly
   period group, reject annual/YTD leaves, and make adjusted/basic/continuing
   exclusions at the selected row lineage.  Add a network-free fixture of this
   exact structural shape asserting 1.39, current-quarter column selection,
   and rejection of the adjusted/basic/annual siblings.

2. **An accepted terminal receipt can be committed without its fact.**  The
   accepted branch writes `sec_earnings_release_receipts` at `701`, then calls
   `_write_press_release_facts_frame` at `702-710`; that helper starts its own
   transaction (`1177-1200`).  A failure after the receipt write leaves an
   `accepted` record, and `_terminal_sec_receipt_exists` (`529-537`) permanently
   skips that accession on resume.  Put accepted receipt insertion and fact
   insertion in one outer transaction (with a non-nesting fact writer), or
   make the terminal-skip predicate prove the referenced immutable fact exists
   and has the matching receipt/SHA.  Cover the injected fact-write failure,
   rollback, and successful resume.  Rejected and fetch-failed outcomes may
   remain independently durable, but `fetch_failed` must remain retryable.

3. **13/14-week candidates are admitted without the exact period-start
   evidence that the bridge requires.**  `_duration_evidence` only returns a
   label (`385-393`), and extraction records only `period_end` plus that label
   (`453-460`, `667`, `690-695`).  No code obtains an explicit start boundary
   or a qualified week-period identity.  This contradicts the source contract:
   month-based start may be derived only for an explicit three-month period,
   whereas 13/14-week periods must carry explicit start/period qualification.
   Either parse and persist both explicitly labeled week boundaries and the
   exact duration, or reject all week-based tables until that evidence exists.
   Add 13- and 14-week pass/fail fixtures, including a 53-week issuer case.

## Important

1. **0320 is not connected to a governed production path.**  The draft adds
   only `scripts/refresh_sec_earnings_release_facts.py`.  Its mirror does not
   add a `SecEarningsReleaseDataset`/option factory to `jobs.py`, a seeded job,
   or an activation stage; current activation moves directly from
   `submissions_load` to `companyfacts_load` and then `statement_points`.
   Existing `press_release_facts` job wiring accepts only `PressReleaseOptions`
   (source-file injection), so it cannot invoke this SEC loader.  Add a
   governed dataset and job after `sec_submissions`, with explicit cache,
   history/scope, byte, timeout, and project-only UA parameters; add an
   activation stage after `submissions_load` and before statement materializes.
   Ledger the source run and make its receipt/fact transaction observable.
   This does not authorize treating the source stage as a completed EPS metric.

2. **The document cache can durably turn an interrupted or corrupt write into
   evidence.**  A fresh response is written directly to its final cache path
   (`624-629`); a process interruption can leave a truncated file.  On the next
   run `is_file()` accepts it (`619-623`), then it is parsed and terminally
   rejected or accepted without a refetch.  Write to a same-directory temporary
   file, fsync as appropriate, validate the byte cap and SHA, then atomically
   replace.  Persist the content SHA in cache metadata/name and on a cache hit
   verify it against any prior receipt before parsing; remove/re-fetch an
   invalid cache entry.  Test interrupted/truncated cache recovery.

3. **The draft has no regression proof for the additive bulk metadata field or
   resume invariants.**  The new `acceptance_datetime_raw` is populated in
   `_normalize` (`151-190`), but the preserved bulk tests do not assert its
   raw offset-bearing value, legacy-null behavior, archive hash/prefix result,
   or a failed-run resume after the added column.  Add fixtures that replay the
   same pinned archive through a prefix resume and prove the old receipt/scope,
   CIK order, and source hash remain unchanged while raw strings are retained
   for newly loaded rows.

## Minor

1. The receipt check permits a `fetch_failed` row with no rejection reason or
   retrieval diagnostics, although the implementation currently supplies one.
   Tighten the constraint or validation so every non-accepted outcome has a
   reason and its candidate/index identity.

## Confirmed static properties

- Candidate discovery is CIK/accession keyset-paged from all loaded 8-K Item
  2.02 history; no two-year ceiling appears in the source path.
- Index/document reads have configured byte caps and request timeouts.  The
  inherited SEC session provides five retries, retry backoff, and global
  0.11-second request pacing.  The source defaults to the required project
  user agent and uses no issuer-specific rule or LLM.
- The raw offset-bearing acceptance string is additive to submissions; naïve
  timestamps remain `timestamp_zone_unknown`.  With a filed date, eligibility
  is conservatively `max(qualified acceptance, filed date + 46h)`, not an
  exact dissemination claim.

The Chevron structural observations are from the official SEC EX-99.1:
https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/a12312025ex9918-k.htm
