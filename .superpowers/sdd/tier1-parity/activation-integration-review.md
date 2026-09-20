# AR1/AR2 production integration: independent controller review

Reviewed by the Codex controller, independently of the implementer, in one source
pass on 2026-09-20. The stable implementation includes activation.py, jobs.py,
production_panels.py, the bounded universe.py writer, migration0312 and its
registrations, capped migration connections, reconciliation planning, optional
quality checked_at, focused tests and the production runbook.

Disposition: **no Critical or Important findings**. The implementer reported
passing focused tests; this review did not rerun tests or access the warehouse.
Accepted implementation commit: `e5a9941b`. The companion
activation-integration-report.md records125 unique passing focused checks and
one default slow skip. No full-suite run or production validation is inferred.

Confirmed behavior:

- Updated native Parquet is passed to the reviewed loader, with no mandatory ZIP
  extraction. Source diagnostics and provenance survive into stage detail.
- All SEC submission forms are selected with50-CIK flush batches. Query defaults
  are1GB/one thread, including the reconciliation planning connection and both
  governed migration connections before schema work. Reconciliation partitions
  remain sequential.
- Legacy liquidity decisions and interval compression stay in SQL, preserving
  the existing interval/threshold rules. Current unversioned classification is
  gated by its recorded availability; unknown historical classification is
  explicitly uncertified. The same-day price revision is selected after the
  decision cutoff and all used clocks contribute to decision availability.
- The actual monthly cohort drives retained parents and23 projections, in
  complete-peer calendar-year partitions. Empty scope does not become an
  all-security parent query. Empty and insufficient outputs are diagnostic;
  preserving old rows on an empty cohort is not claimed as coverage continuity.
- Shared scheduler/activation adapters build terminal returns and code
  reconciliation, observed calendar, adjusted-price survivorship labels and
  annual cohort/item coverage in prerequisite order. Missing listing history
  remains explicit.0312 describes adjusted price and named terminal policies.
- Final provider/quality results remain visible, with the quality record clock
  derived from explicit as-of date. A completed measurement stage is not a
  successful parity gate or an instruction to flip schema conditions.

Operational limits remain to be measured on production: full-scale SQL/index
memory, retained-parent input volumes, actual annual cohort completeness,
identity/listing history, and economic adjustment/vintage quality. The guarded
reload and run5 will measure these; fixture evidence does not certify them.
No additional test loop or speculative cleanup is requested.
