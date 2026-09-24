# IQ2: exact selected-issuer lineage on accounting reads

Concrete finding: issuer-derived-lineage-audit.md. A visible A fact revision
can nominate an accounting owner that also holds B standardized source leaves
with an earlier usable clock but no B fact revision visible at the content
cutoff. Current API and issuer_derived_asof select by owner and can label B's
numeric metric as A. Correct this generically for all canonical metrics, not
CVX or EPS only. The case is synthetic, not measured production corruption.

Own atx-db/src/atx_db/api/service.py, asof/fundamentals.py, derived_lineage.py
(add a public reader/helper as appropriate without weakening DL1), relevant
issuer tests and a new tiny focused test if needed, docs/ISSUER_CONTENT_QUERY.md,
and only the issuer-derived description/version in api/catalog.py if needed.
No new migration or registry/activation/jobs edits. Parent owns dictionary,
public API snapshot coordination, desk SQL and controller ledgers. API/asof
packages must import public root helpers, not cross-package private modules.

Requirements:
- Preserve existing exact-CIK behavior for CIK-bearing schemas, separate
  issuer_lookup_as_of/content_as_of, first_reported/latest semantics and
  non-tradable source-owner metadata. Retain coarse visible fact-owner
  discovery/collision diagnostics; do not infer historical market identity.
- Rank the visible whole derived revision BEFORE lineage/value screening
  and period filtering. Never resurrect an older valid row after a latest
  unqualified or NULL state. Select internal derived_value_id regardless of
  projected fields, then qualify actual selected leaves with DL1 against
  requested CIK, canonical source and approved definition closure hashes.
- Prove operands at the selected root's own event (decision_cutoff=None),
  after SQL bounds root visibility by content_as_of. In first_reported mode
  a historically valid first state may legitimately expire before the query
  cutoff; do not mistakenly require it to be current at content_as_of.
- Numeric rows require qualified selected ownership. NULL/invalid events
  with fully verified requested-CIK ownership may remain NULL rows; invalid
  numeric/dependency states cannot leak a number. For missing/legacy/foreign
  or unverified ownership, exclude numeric output and emit bounded explicit
  diagnostic metadata (DataFrame attrs for asof), including period/state ID,
  fixed reason/status and counts. Do not return a foreign numeric value or
  invent CIK proof for missing operands. Never echo arbitrary source JSON.
- Pre-0323 schema/legacy NULL refs must produce explicit unavailable-lineage
  diagnostics or a clear controlled query error, never fall back to owner-only
  numeric certification. Do not reconstruct exact refs from inputs_hash.
- Qualify bounded pages before applying the public output limit. Excluded
  candidates must not silently hide later eligible quarters. Stable tie order,
  bounded candidate scan/diagnostic size and explicit truncation/limit status
  are mandatory. Respect DL1 per-root and aggregate batch node/byte caps; split
  a batch if only its aggregate bound exceeds, never raise the bound. As-of
  SELECT* may now include large JSON refs: bound output bytes as well as rows
  before Python fetch; fail explicitly rather than exhaust memory or silently
  claim a complete result. No whole-universe or whole-table Python fetch.
- Reuse one generic implementation for API and asof consumer qualification;
  do not duplicate approximate ownership SQL. A public helper may compute
  canonical publisher definition hashes through root-level imports and check
  registered definitions. Do not couple serving to private research helpers.
- No automatic DB initialize/migrate, writes, or global process settings.

Focused proof must cover the producer-consistent A-owner/B-leaf gap; a valid
same-CIK numeric chain; legacy/unverified rows; NULL invalidation without old
value revival; first_reported root/dependencies valid at their event but expired
by content cutoff; definition/source/clock mismatch; enough rejected rows
before valid output to exercise limit-after-qualification; API and asof parity.
Use tiny isolated256MB/one-thread fixtures, no full schema initialization.
One focused batch and scoped Ruff under exact1.5GiB process-tree guard only
after root grants sole runtime. No full suite. Fresh Codex implementer, one
independent review; Important fixed on report, Critical rereview only.
No stash/reset/restore/checkout/clean. Preserve four unrelated EOL modifications.
Write issuer-selected-lineage-result.md and wait for root integration/commit
coordination. Prescribed coauthor trailer, explicit paths only.
