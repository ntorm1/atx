# FP1: bounded publication of the full forward-return panel

Prepared by root, 2026-09-20. This is a production capacity task supporting the
requested full-universe forward-return labels and custom-feature decile evidence.

## Evidence and contract

Current delisting.refresh_survivorship_safe_forward_returns uses spillable SQL,
but puts five full-universe horizon inserts and the source deletion in one
transaction. Migration0186 defines a SHA256-string primary key;0187 adds two
nonunique indexes. The measured price source has31,959,271 rows. Therefore the
publication transaction grows with the entire expanded panel (potentially about
160million rows), regardless of SQL operator spilling. This stage has not yet
run at production scale. Its failure is not claimed as measured.

The analogous31million-row price publication already failed at COMMIT under
both1GB and2GB query limits. Ordered-prefix construction of a persistent,
primary-key-constrained shadow with checkpoint/reopen and atomic publication
solved that actual failure. Existing _bulk_publication and equity_price_metrics
contain the reviewed pattern. Reuse established machinery where it fits.

Preserve the full existing forward-return contract: all five configured horizons,
complete source scope, deterministic IDs, physical table and primary key,
NOTNULL/defaults, public views, adjusted/raw option behavior, calendar endpoints,
PIT revision selection and clocks, terminal stitching, source isolation, empty
refresh semantics and rollback. Do not reduce the population, labels, date range
or precision, weaken gates, enlarge memory budgets, or modify historical migrations.

## Bounded implementation

Prepare a patch and new files only under forward-publication-draft/. Do not edit
live atx-db source while the healthy companyfacts writer is active. Root will
integrate after the writer ends, then run focused validation and one fresh review.

Move full-panel result construction into persistent staging using bounded commits.
Build a constrained publication shadow in deterministic key prefixes, retaining
other sources unchanged. Checkpoint and reopen at bounded intervals, preserving
analytical configuration. Validate the full shadow before a short atomic swap;
the published table must retain prior contents until that swap succeeds.
Preserve dependent views and all metadata contracts. Define failure/retry staging
cleanup explicitly; never delete a live table or unrelated/caller-owned artifacts.
Small in-memory callers must remain supported without attempting an invalid
persistent reopen. Never silently drop caller-owned temporary objects.

Migration0317 is reserved in this DRAFT for removal of only the two optional
forward-return secondary indexes if required by the existing publication helper.
Preserve the primary key and all logical constraints. Do not modify registry.py,
migration facade, jobs.py or activation.py live. Include only owned0317 lines in
the draft patch. Root will serialize integration and update exact module pins.

Reuse existing SQL arithmetic/lineage tests. Add only focused capacity-publication
contract checks: a real file-backed tiny build that crosses forced prefix/reopen
boundaries, identical results and retained foreign source, plus injected failure
before/at publication proving the prior source and views survive. Include populated
migration/index-contract coverage if0317 is needed. A small fixture proves control
flow and rollback, not production memory capacity; keep that limitation explicit.

## Process and ownership

Fresh Codex implementer. Own the draft patch/new files and
forward-publication-capacity-report.md. No commits, tests, imports, collection,
lint/type checks, DB connections/probes, installs, network or runtime commands.
Root owns the sole runtime slot; archive4 continues at1GB/one thread within3GiB.
No stash/checkout/reset/restore/clean. Report exact paths, integration order,
focused selectors and any source-semantic tradeoff that needs root resolution.
