# Sprint PF3-S10 — Backtest-ready factor panel (catalogued PIT views + Parquet/Arrow export)

**Goal:** materialize the unified factor namespace assembled across PF3-S8 (fundamental families) and
PF3-S9 (cross-domain integration) as a genuinely **BACKTEST-READY panel** — not a research view but a
governed product surface. Two coupled artifacts land together: `v_factor_panel` catalogued **PIT views**
(a long factor stream and a wide as-of cross-section) **and** a partitioned **Parquet/Arrow** export to the
lake that reuses `lake.py`'s `_schema_sha256` + `_manifest.json` discipline. The panel is
**schema-contracted** (it enforces the PF3-S2 `panel_contract` shape/unit/sign declaration),
**universe-filtered** (PF3-S4 membership applied strictly as-of), and **engine-agnostic** (delivered as
DuckDB views + columnar files so any backtester consumes it PIT-safely with **ZERO lookahead**, never
coupled to one engine's native binary). This sprint is where clause **(I)** stops being a design promise
and becomes an **enforced export boundary**: a row keyed `(security_id, as_of_date)` may only carry inputs
whose `available_at ≤ as_of_date`. Reserved migrations **0164–0167**.

**Mandate / Owns:** NEW `db/factor_panel.py` (the panel assembler + exporter + as-of consumer read path),
the `v_factor_panel` catalogued views (long + wide), the lake Parquet/Arrow export registration and its
schema-hash/manifest wiring, and `db/tests/test_factor_panel.py`.

**Must NOT touch:** the factor engine and factor families (`db/factors/` — PF3-S7/S8/S9) — S10 **consumes**
their landed namespace and never redefines a factor; the signal-evaluation surface (`db/signal_eval.py` —
PF3-S11 **scores** the exported panel, it is not built here); and the PF3-S2 `panel_contract` **definition**
— S10 **enforces** the contract at the export boundary, it does not author or amend it. Do not edit any
landed migration (≤ 0163) or another sprint's reserved region.

**Depends on:** PF3-S9 (the unified factor namespace with consistent keys/units — the panel's content),
PF3-S4 (PIT universe-membership as-of — the row filter), PF3-S2 (the `panel_contract` shape/unit/sign
declaration — the enforced schema), and `lake.py`'s export machinery (`_schema_sha256`,
`LakehouseExporter.export_objects`, `DEFAULT_EXPORT_OBJECTS`, `_manifest.json`, `lake_export_runs` /
`lake_export_files`). Sequential **after** PF3-S9; PF3-S11 follows and reads what S10 exports.

---

## Baseline / where the cycles go

The factor content exists after S9, and the lake export discipline exists from pf1 — but there is no single
governed surface that is *both* the unified panel *and* a contract-enforced, lookahead-gated export.
Measured 2026-07-04 against `atx-impl/db`.

1. **Factor values live in per-family / per-domain surfaces, plus a price-centric panel.** After S8/S9 the
   fundamental families and the cross-domain integration each land their own factor rows, and pf1's
   `v_alpha_daily_panel` (a member of `lake.py::DEFAULT_EXPORT_OBJECTS`) is a **price/alpha-centric** wide
   panel, not the unified factor namespace. There is **no single** universe-filtered, contract-enforced,
   backtest-ready wide/long **factor** panel — no `v_factor_panel` a backtester can point at and trust.

2. **The lake export pattern exists but has no factor-panel object.** `LakehouseExporter.export_objects`
   (`lake.py:202`) walks `DEFAULT_EXPORT_OBJECTS`, computes `_object_schema` from `duckdb_columns()`, stamps
   `_schema_sha256` (`lake.py:168`), writes `<object>/part-00000.parquet` (`lake.py:242`) + a
   `<object>/_manifest.json` (`lake.py:256`), and records `lake_export_runs` / `lake_export_files`. The
   machinery is proven — `alpha_signal_values` and `v_alpha_daily_panel` already export through it — but
   **no `v_factor_panel` object is registered**, so the flagship deliverable has no export path.

3. **There is no export-boundary lookahead gate.** Nothing today re-asserts, *at the moment of export*, that
   every emitted row uses only `available_at ≤ as_of_date` inputs and as-of universe membership. Clause (A)
   is enforced inside the as-of readers, but the panel is the surface an *external* engine trusts blindly —
   and no adversarial check stands at that boundary. Clause (I) is declared in the contract but **not yet
   enforced anywhere**.

4. **The downstream engine is binary-format and options-shaped.** `atx-engine` is a C++ options pipeline
   consuming `.seg`/`.bin`; it is not an equity factor consumer. So the panel must be delivered as
   engine-**AGNOSTIC** catalogued views + Parquet/Arrow — never as a native binary — so any backtester
   (including a future atx-engine equity mode) reads it PIT-safely without coupling PF3 to one engine.

**Already good — do not regress:**
- **`lake.py`'s schema-hash + manifest export discipline.** `_schema_sha256` over the sorted introspected
  schema, the per-object `part-00000.parquet` + `_manifest.json` layout, and the `expected_schema_sha256`
  re-check on validation are the exact contract-and-alert shape S10 reuses — not a parallel one.
- **The export-boundary guard precedent.** `quality.py::_export_scan_internal_cusip_sql(DEFAULT_EXPORT_OBJECTS)`
  (`quality.py:780`) already proves the pattern of a check that scans *every export object* for a forbidden
  leak (an `internal_cusip` column); S10's lookahead gate is the same shape aimed at future-dated inputs.
- **The catalogued-view precedent.** pf1-S4-3's `v_formula_registry` (catalogued with its own
  `table_catalog` / `field_catalog` rows + a bitemporal as-of reader) is the template `v_factor_panel` follows.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal/no-lookahead, **(B)** append-only catalogued migrations, **(C)** offline tests,
**(D)** determinism/provenance, **(E)** schema-as-contract, **(I)** panel PIT-safety, and **(J)** semantic
contract all apply — and **(I) is ENFORCED by this sprint** at the export boundary (it was defined, not
enforced, upstream).

- **(I)** The exported panel is point-in-time *by construction and by gate*: a row keyed
  `(security_id, as_of_date)` carries only inputs with `available_at ≤ as_of_date`; cross-sectional values
  rank only within the as-of cross-section; universe membership is resolved as-of. A lookahead-detection
  test **fails the export** when any row violates this. This is the clause's first enforcement.
- **(E)/(J)** The panel matches the PF3-S2 `panel_contract` exactly — declared columns, units, sign, scale —
  and its `_schema_sha256` is registered as the object's `expected_schema_sha256`; drift on either fails.
- **(A)/(D)** The assembler is a pure as-of read (deterministic long DataFrame out); same inputs + as-of →
  same rows + same schema hash.
- **(B)** Migrations **0164–0167** only, schema/view split from index/catalog per precedent, each seeding
  `table_catalog` + `field_catalog` in the same migration; timestamped DB+WAL backup before any live apply:
  - **0164** — `v_factor_panel` long + wide views + their catalog rows.
  - **0165** — lake export objects registered + panel `expected_schema_sha256` registration.
  - **0166** — panel-contract enforcement metadata + the lookahead-gate check registration.
  - **0167** — indexes + the consumer-facing catalog/read-path surface.

---

## Tasks

### S10-0 — `v_factor_panel` PIT views (long + wide, universe-filtered)

**Root cause:** the unified factor namespace (S9) has no single catalogued PIT surface a backtester can
consume; factor values remain scattered across family/domain surfaces and the only wide panel is
price-centric (`v_alpha_daily_panel`).

**Fix:** migration **0164** adds `v_factor_panel` as two catalogued views over the S9 namespace: a **long**
shape `(security_id, as_of_date, factor_id, value)` and a **wide** as-of cross-section
(`security_id × factor_id` per `as_of_date`), both **inner-joined to PF3-S4 universe membership resolved
as-of** so non-members never appear. Catalogue each with its own `table_catalog` / `field_catalog` rows
exactly like `v_formula_registry`. The assembler in `db/factor_panel.py` produces the long frame as a pure
as-of read gating on `available_at ≤ as_of_date`.

**PIT:** (A) as-of read, no lookahead; (I) universe applied as-of; (B) 0164 views catalogued.

**Accept:** both views resolve for a fixture as-of date; a security outside as-of membership is absent; the
long and wide shapes are consistent (wide is a pivot of long); a row whose input `available_at` is after
`as_of_date` never appears.

### S10-1 — Parquet/Arrow lake export (partitioned, schema-hashed, manifested)

**Root cause:** `DEFAULT_EXPORT_OBJECTS` has no factor-panel object, so the flagship deliverable cannot be
materialized to the lake with the same hash/manifest guarantees every other surface carries.

**Fix:** register the panel as a lake export object (migration **0165**) and export it through the existing
`LakehouseExporter` discipline — **partitioned by `as_of_date`** (one part per date rather than a single
`part-00000.parquet`), each partition carrying a `_manifest.json` and the export run recorded in
`lake_export_runs` / `lake_export_files`. Compute the panel schema via the same introspection `_object_schema`
uses, stamp `_schema_sha256`, and **register it as the object's `expected_schema_sha256`** so a later schema
change is caught on validation exactly like every other export. Emit Arrow alongside Parquet (or document
the Arrow read path over the Parquet) so a non-DuckDB consumer has a zero-copy columnar entry point.

**PIT:** (E) schema-hash registered + drift-checked; (B) 0165 object registration catalogued; (D) same
inputs → same bytes/hash per partition.

**Accept:** `export_objects` writes date-partitioned Parquet + per-partition manifests; the panel's
`schema_sha256` equals the registered `expected_schema_sha256`; a planted schema change trips the mismatch;
row counts round-trip through `read_parquet`.

### S10-2 — Panel-contract enforcement + lookahead gate *(the clause-(I) export gate)*

**Root cause:** clause (I) is declared but enforced nowhere, and the PF3-S2 `panel_contract` shape/units are
not checked at the surface an external engine trusts. The export boundary is the last line against lookahead.

**Fix:** enforce the PF3-S2 `panel_contract` — every panel column present with its declared unit/sign/scale,
no undeclared columns — and register a **LOOKAHEAD-DETECTION** check (migration **0166**) modelled on
`_export_scan_internal_cusip_sql`: it scans the assembled panel and **fails** if any row references an input
whose `available_at > as_of_date`, or a `(security_id, as_of_date)` outside as-of universe membership. Author
it `severity=critical` and wire it so the export **raises rather than writes** on violation. Provide the
adversarial fixture: a planted future-dated input must turn the gate **red** and abort the export.

**PIT:** (I) enforced at export — the gate is the boundary; (J) unit/sign contract checked; (C) offline
fixtures with a planted future-dated input.

**Accept:** contract conformance green on a clean panel, red on a unit/sign/undeclared-column violation; the
lookahead gate green on the clean panel and **red (export aborts)** on a planted future-dated input and on a
planted non-member row.

### S10-3 — Consumer read path + CLI + catalog

**Root cause:** even a correct export is unusable if there is no documented, PIT-safe way for a backtester to
load a cross-section; and the panel needs a catalogued as-of surface like every other governed table.

**Fix:** ship a documented consumer read path in `db/factor_panel.py` — `read_panel_asof(as_of_date, …)`
returning the PIT cross-section from the views (or the Parquet partitions) with the same
`available_at ≤ as_of_date` gate — and a thin CLI `python -m db.factor_panel export --as-of …` (plus a
`read`/`describe` subcommand). Migration **0167** adds panel indexes and the consumer-facing catalog rows so
the panel is queryable as-of like `warehouse_catalog_asof`. Document the engine-agnostic contract: view
schema, Parquet/Arrow layout, partition key, and the zero-lookahead guarantee a consumer may rely on.

**PIT:** (A) read path gates on availability; (B) 0167 indexes/catalog; (D) deterministic cross-section.

**Accept:** `read_panel_asof` returns a PIT cross-section a backtester consumes with zero lookahead; the CLI
exports and reads a slice; the panel carries its own catalog rows; the documented contract matches the
exported schema hash.

---

## Sequencing & expected compounding

**S10-0 → S10-1 → S10-2 → S10-3.** S10-0 lays the catalogued PIT views (the content surface everything else
reads). S10-1 materializes them to date-partitioned Parquet/Arrow under the proven `lake.py` hash/manifest
discipline. S10-2 then bolts the contract-enforcement + lookahead gate onto that export boundary — it must
land after there is an export to gate, and before the consumer path is documented as trustworthy. S10-3
exposes the read path + CLI + catalog last. Compounding: the exported panel is **the PF3 product
deliverable** — precisely the "pipes directly into a quant backtesting engine that mines for signal" surface
the northstar promises — and it is the **direct input to PF3-S11**, which scores each exported factor's
IC / decay / turnover / crowding over exactly this panel.

## Risks / guardrails

- **The export boundary is the last line against lookahead.** The gate must be **adversarial**: plant a
  future-dated input → the export must **FAIL**, not warn. A permissive gate is worse than none because
  downstream trusts the panel blindly.
- **Schema-hash drift must alert.** Reuse `lake.py::_schema_sha256` + `expected_schema_sha256` — do not build
  a parallel hash. A silent panel-schema change would corrupt every downstream backtest.
- **Universe must be applied AS-OF.** Filtering on *today's* membership reintroduces survivorship bias;
  membership is resolved at each row's `as_of_date` via the PF3-S4 surface.
- **Engine-agnostic — no coupling to `.seg`/`.bin`.** The panel ships as views + Parquet/Arrow only; no
  atx-engine native binary, no options-pipeline assumptions leak into the factor panel.
- **Stay in lane.** Do not touch `db/factors/` (consume it), `db/signal_eval.py` (S11), or the S2
  `panel_contract` definition (enforce it). Strictly migrations **0164–0167**; DB+WAL backup before any live
  apply.

## Bench / acceptance

- `v_factor_panel` long + wide views resolve, universe-filtered as-of, and export to date-partitioned
  Parquet/Arrow through `LakehouseExporter`.
- The PF3-S2 `panel_contract` is enforced at export (unit/sign/scale/columns); a violation fails.
- The **lookahead gate is green** on a clean panel and **red** on a planted future-dated input and a planted
  non-member row — the export aborts rather than writes.
- A consumer (`read_panel_asof` / the documented Parquet-Arrow path) loads a PIT cross-section with **zero
  lookahead**.
- The panel `schema_sha256` is registered as `expected_schema_sha256` and re-checked on validation.
- `python -m pytest atx-impl\db\tests\test_factor_panel.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green before commit.
- **Live proof-slice smoke** recorded in the ledger: panel row counts (long + wide), partition count,
  distinct `factor_id` / `security_id`, the panel `schema_sha256`, export `run_id`, and the gate result.
- `PARITY_GAP.md` updated (clause I now enforced at the export boundary; the backtest export contract
  landed); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live
  smoke with exact counts + run_id, caveats/next → PF3-S11 signal evaluation).

**Process:** own git worktree off `main`, merged at sprint end via
`atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller `superpowers:subagent-driven-development`
(fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage
explicit paths); never push unless asked. Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
