# Production measurement independent static review

Reviewed `atx-db/scripts/measure_tier1_readiness.py` and
`.superpowers/sdd/tier1-parity/production-measurement-report.md` against the
declared schemas, migrations, activation writers, dataset runner, annual
coverage gate, and quality-result writer contracts.

**Critical findings: none. Important findings: none.** This is a static review;
DuckDB binding, execution and production measurements remain controller-owned
verification. No imports, tests, database connections, live scans, or probes were
executed by this reviewer. No implementation files were changed.

- Selected surface columns match their schema declarations; optional columns
  are inspected before use. Required missing relations/columns remain
  `unmeasured`, and SQL failures are not disguised as empty evidence.
- Variable results are limited in SQL and fetched with a bounded `fetchmany`.
  Surface data crosses into Python as aggregates; the documented memory limit
  caveat correctly distinguishes result size from scan/hash/window work.
- The annual gate matches `item_coverage.coverage_gate_count_sql`: all completed
  FY2015+ years, complete 3000-member denominators, consistent coverage arithmetic,
  and 90% for each counted item. Missing years and empty evidence cannot satisfy
  the 110-item threshold.
- Activation execution status, observation-date freshness, numeric coverage,
  stored provider condition, and stored quality outcomes remain separate. The
  report does not turn successful execution, empty records, disabled/skipped
  checks, or same-day quality observations into release certification.
- Direct activation output IDs and known suffixes match the activation writers.
  Dataset UUIDs are linked through the original requested ID preserved in
  `dataset_runs.params_json`, succeeded status, and the activation time interval,
  matching `Dataset.run` behavior. Quality results correctly remain unverified
  against a rebuild because their table has no run ID.
- Terminal gaps select the latest eligible terminal revision before evaluating
  its value. Listing, universe and identity counts retain their explicit
  historical-completeness limitations.
- The script opens DuckDB read-only, executes read queries in one transaction,
  and refuses existing report outputs. It does not import the application,
  migrate, activate, run DQC, or send messages/network requests.

Nonblocking wording caveat: `measure_tier1_readiness.py:60` and `:560` claim
contact/credential redaction more broadly than `detail()` implements at `:85`.
Truncated or malformed diagnostic JSON remains a string, so key-based redaction
does not apply to that string; email replacement still applies. A narrow claim
such as “email addresses are redacted; sensitive keys are redacted in successfully
parsed structured details” is sufficient, or unsafe raw diagnostics may be
omitted. No actual credential exposure was established in this source-only
review, and no general-purpose sanitization framework is requested.

One review pass completed. Any Important corrections may be accepted on the
implementer's report; only a Critical finding would require another review.
