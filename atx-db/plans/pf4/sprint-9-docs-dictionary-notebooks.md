# Sprint PF4-S9 — Data dictionary + docs + notebooks (generated-from-contract; fresh-agent runbook; runnable quickstarts)

**Goal:** deliver everything a downstream quant team needs to **understand and adopt** the product — as
**living, gated artifacts**, not prose that rots. Three coupled deliverables land together: (1) a
**deterministic data-dictionary generator** (`scripts/gen_data_dictionary.py` → `atx-impl/docs/data_dictionary.md`)
that reads the **real contracts** — `db/panel_contract.py` (the export shape), the factor catalog surfaces
(`db/factors/catalog.py`, `fundamental_families.py`, `cross_domain.py`), and the PF4-S1 `db/signal_eval.py`
scores **if available** — and emits, per factor, `id / family / domain / definition / formula / unit / sign /
scale / direction / lookback / lineage summary` plus `mean rank-IC / IC-decay / crowding / breadth` when the
signal-eval surface is populated; (2) an **activation + consumption RUNBOOK** (`atx-impl/docs/RUNBOOK.md`) a
fresh agent can follow end-to-end, honest about which steps are **offline-deterministic** and which are
**operator-run**; and (3) three **runnable quickstart notebooks** (`atx-impl/docs/notebooks/`) that execute
against a **bounded committed fixture slice** with **pinned outputs**. This sprint is where the product stops
being "code + a schema" and becomes a **documented, self-verifying product**: the dictionary is
**byte-stable** and **drift-gated** (regenerate → identical, or the gate fails); a factor with **no**
dictionary entry **fails the coverage gate**; every path the runbook names **must exist**; and every notebook
**must execute** on the fixture without error. **No migrations — docs only.** Reserved migrations: **none**.

**Mandate / Owns:** NEW `atx-impl/docs/` tree — the dictionary generator `atx-impl/scripts/gen_data_dictionary.py`,
its rendered `atx-impl/docs/data_dictionary.md`, the `atx-impl/docs/RUNBOOK.md`, the
`atx-impl/docs/notebooks/` quickstarts (+ their shared fixture-slice helper), a small `atx-impl/docs/README.md`
index, and the single gate module `atx-impl/db/tests/test_data_dictionary.py`.

**Must NOT touch:** the panel contract (`db/panel_contract.py`), the factor catalog / families / cross-domain
surfaces (`db/factors/*`), the signal-eval surface (`db/signal_eval.py` — PF4-S1), the panel read/export path
(`db/factor_panel.py`), the release engine (`db/panel_release.py` — PF4-S7), the SDK (`clients/atx-panel/` —
PF4-S8), or any migration. S9 is a **pure read-and-render consumer** of these contracts: it must not author,
amend, or re-pin any contract, `schema_sha256`, or public-API snapshot. If a referenced surface's landed name
differs from this plan, S9 **reconciles to the landed name** (ROADMAP §"reconciles to the landed name") — it
never edits the surface.

**Depends on:** PF4-S1 (`db/signal_eval.py` scores — the IC/decay/crowding/breadth columns, read
**if available**), PF4-S2 (`db/observability.py` freshness/anomaly/lineage surfaces the runbook's sweep step
cites), PF4-S6 (`scripts/warehouse_activate.py` — the operator activation harness the runbook drives),
PF4-S7 (`db/panel_release.py` — the release the notebooks pin), PF4-S8 (`clients/atx-panel/` — the SDK the
notebooks import and the runbook verifies through), and the always-present pf3 surfaces (`panel_contract`,
factor catalog, `factor_panel`). Per ROADMAP sequencing S9 runs **after** S7→S8; the runbook's
path-existence gate is the mechanism that **enforces** those predecessors actually landed.

---

## Baseline / where the cycles go

The product is code-complete through the SDK but **undocumented and unadoptable by an outsider**. Measured
2026-07-06 against `atx-impl/`.

1. **The contracts are machine-readable but not human-readable.** `db/panel_contract.py::PANEL_CONTRACT` is 8
   typed `PanelColumnSpec` rows with `unit/sign/scale` and a stable `PANEL_CONTRACT_SHA256`; the factor
   namespace is ~21 fundamental seed factors (`db/seeds/factor_definitions.csv` via
   `fundamental_families.factor_seed_definitions()`) **plus** ~32 cross-domain factors
   (`cross_domain.cross_domain_factor_definitions()` over `CROSS_DOMAIN_SPECS`) **plus** the legacy
   price/alpha namespace (`catalog.legacy_factor_definitions()`) — each a `FactorDefinition` carrying
   `family / description / expression / input_ids_json / direction / lookback_days / unit / sign / scale /
   available_at_policy / declared_in`. **Nothing renders these into a single dictionary a quant can read**, and
   nothing guarantees such a dictionary stays in sync when a factor is added or a unit changes.

2. **There is no signal metadata surfaced next to each factor.** PF4-S1's `db/signal_eval.py` scores every
   factor (rank-IC over `{1,5,10,21,63}`, IC-decay, decile spread, turnover, crowding, breadth), but those
   scores live only in the eval tables — a consumer choosing factors cannot see "mean rank-IC / decay /
   crowding" **beside the definition**. The dictionary is the join surface, and it must degrade honestly when
   the eval surface is empty (data-empty warehouse) rather than fabricate numbers.

3. **There is no adoption path from a fresh checkout.** The activation sequence
   (recover-from-`.bak` → migrate → operator backfill → gated rebuild → freshness/anomaly/lineage sweep →
   export → release → SDK-verify) exists as **scattered scripts** (`scripts/warehouse_migrate.py`,
   `warehouse_backfill.py`, `warehouse_rebuild.py`, `warehouse_jobs.py`, `query_asof.py`, plus the PF4-S6/S7/S8
   surfaces) with **no single ordered runbook** that says what to run, in what order, which steps mutate the
   live 14 GB DB (operator-gated), and which are deterministic offline.

4. **There are no runnable examples.** No notebook shows a consumer loading a pinned release, computing factor
   IC + a decile long-short backtest, or walking a survivorship-safe universe cross-section. Prose examples go
   stale silently; **an executed example on a pinned fixture cannot**.

**Already good — reuse, do not reinvent:**
- **The hash-a-contract-and-compare discipline.** `panel_contract.panel_contract_sha256()` and
  `schema_contract.schema_contract_sha256()` are the exact "declare a shape, hash it, gate on drift" pattern —
  the dictionary's drift gate reuses that idea (regenerate → byte-compare), not a parallel scheme.
- **The pure-frame definition surfaces.** `factor_definitions_frame()`, `factor_seed_frame()`,
  `cross_domain_definition_frame()` already emit deterministic, stable-sorted DataFrames of the factor
  contract — the generator reads **these**, so the dictionary is generated from the same source of truth the
  engine seeds from, never a hand-copied list.
- **The consumer read path.** `factor_panel.read_panel_asof()` / `describe_factor_panel()` are the PIT read the
  notebooks demonstrate; the quickstarts call them (against the fixture store), never a bespoke query.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal/no-lookahead, **(C)** offline tests, **(D)** determinism/provenance, **(J)**
semantic contract apply; **(D) is the load-bearing clause for this sprint** — the generator is a pure,
byte-deterministic transform.

- **(D)** `render_data_dictionary(...)` is **pure**: `(factor definitions, panel contract, optional signal-eval
  summary) → Markdown str`. Deterministic — stable-sorted by `(family, domain, factor_id)`, no wall-clock, no
  UUID/`run_id`, no absolute path, no environment string, no dict-ordering nondeterminism; identical inputs →
  byte-identical output. The rendered `data_dictionary.md` is written with `\n` newlines, UTF-8, no trailing
  whitespace, exactly one terminal newline. The **drift gate** re-invokes the pure renderer and asserts equality
  with the committed bytes — the file cannot drift from the contract without failing CI.
- **(J)** The dictionary reproduces each factor's declared `unit / sign / scale` **verbatim from its
  `FactorDefinition`**, and each panel column's `unit / sign / scale` **verbatim from its `PanelColumnSpec`** —
  it never re-derives or "corrects" them. It **reconciles vocabularies honestly**: the factor catalog signs
  (`signed / nonnegative / bounded`) and the panel/schema-contract `SIGN_VALUES`
  (`signed / non_negative / non_positive / unit_interval / bounded`) differ by spelling — a deterministic
  `_canonical_sign()` maps `nonnegative → non_negative` and the dictionary prints both the raw and canonical
  form with a legend, so the two layers reconcile without either being silently rewritten.
- **(A)/(C)** The signal-eval summary is read through an **injectable adapter** (`load_signal_eval_summary(store)`)
  that queries the PF4-S1 surface when present and returns an **empty mapping** otherwise; when empty, IC/decay/
  crowding/breadth cells render a fixed `—` sentinel (not `0`, not a guess). Notebooks run against a **committed
  in-memory fixture slice** — no live DB, no network. No step in the test path opens the 14 GB DB.
- **No migrations.** Reserved range: **none**. S9 adds zero tables/views and touches no `schema_sha256`.

---

## Tasks

### S9-0 — Deterministic data-dictionary generator + drift gate

**Root cause:** the factor and panel contracts are machine-readable Python but there is no human-readable,
**always-in-sync** dictionary; a consumer cannot see definition + unit/sign/scale + signal in one place, and
any hand-written doc would silently drift from `PANEL_CONTRACT` / the catalog.

**Fix:** add `atx-impl/scripts/gen_data_dictionary.py` with a **pure** core and a thin CLI:

- **Collection (pure, deterministic).** `collect_factor_entries() -> list[FactorEntry]` unions the three
  definition surfaces — `fundamental_families.factor_seed_definitions()`,
  `cross_domain.cross_domain_factor_definitions()`, and `catalog.legacy_factor_definitions()` — de-duplicated by
  `factor_id` (a duplicate id across surfaces is a hard error, mirroring `catalog.validate_catalog`'s
  uniqueness rule), and stable-sorted by `(family, domain, factor_id)`. Each `FactorEntry` carries
  `factor_id, factor_name, family, domain, description, expression (formula), input_ids (lineage summary from
  input_ids_json), direction, lookback_days, unit, sign (raw + canonical), scale, is_point_in_time_safe,
  available_at_policy, declared_in, source`.
- **Signal join (optional, honest).** `load_signal_eval_summary(store=None) -> dict[str, SignalStats]` returns
  per-`factor_id` `mean_rank_ic, ic_ir, ic_decay_halflife, decile_spread, turnover, crowding, breadth` from the
  PF4-S1 surface **if present**; otherwise `{}`. Missing stats render the `—` sentinel.
- **Render (pure).** `render_data_dictionary(entries, panel_contract=PANEL_CONTRACT, signal=None) -> str`
  emits Markdown with: a header stamping `PANEL_CONTRACT_SHA256`, the factor count, and the sign/unit/scale
  **legend** (from `schema_contract.SIGN_VALUES` + the catalog vocab); a **Panel schema** section (the 8
  `PanelColumnSpec` rows: `name / data_type / unit / sign / scale / is_panel_key`); and a **Factor dictionary**
  section grouped by family, one row per factor with the fields above + the signal columns.
- **CLI.** `python -m scripts.gen_data_dictionary [--check] [--out atx-impl/docs/data_dictionary.md]` — default
  writes the file (byte-canonical); `--check` regenerates in memory and exits non-zero on any diff (the same
  assertion the gate runs), so the generator is self-checking.

Write the byte-canonical `atx-impl/docs/data_dictionary.md` as the committed artifact.

**TDD (write first, in `db/tests/test_data_dictionary.py`):**
- `test_generator_is_byte_deterministic` — `render_data_dictionary(...)` called twice on the same inputs is
  byte-identical, and equals `data_dictionary.md` on disk (drift gate).
- `test_generator_check_flag_matches_committed_file` — the `--check` path exits 0 against the committed file.
- `test_signal_columns_degrade_to_sentinel` — with an empty signal summary, IC/decay/crowding cells are the `—`
  sentinel (never `0`/`nan`); with a planted 2-factor summary, exactly those two rows show numbers.
- `test_sign_vocabulary_reconciles` — every catalog sign maps through `_canonical_sign()` into `SIGN_VALUES`;
  an unmapped sign raises.

**PIT:** (D) pure byte-stable render; (J) unit/sign/scale copied verbatim + reconciled, not re-derived.

**Accept:** the generator regenerates the committed `data_dictionary.md` **byte-for-byte**; the panel-schema
section matches `PANEL_CONTRACT` exactly and stamps `PANEL_CONTRACT_SHA256`; signal columns are present-and-real
when the eval surface is populated and a fixed sentinel when it is not.

### S9-1 — Every-panel-factor coverage gate

**Root cause:** a dictionary that silently omits a factor is worse than none — a consumer trusts it as
complete. Coverage must be **enforced**, not assumed, in both directions (no missing factor, no orphan entry).

**Fix:** in `db/tests/test_data_dictionary.py`, add the coverage gate that binds the dictionary to the **live
factor namespace** two ways:
- **Definition coverage.** Every `factor_id` in the unioned definition surfaces
  (`factor_seed_definitions() ∪ cross_domain_factor_definitions() ∪ legacy_factor_definitions()`) has exactly
  one dictionary entry, and every dictionary entry maps back to a real definition (no orphan rows).
- **Fixture-panel coverage.** Build the bounded fixture panel (the S9-2 slice) via
  `assemble_factor_panel_long(...)`; every distinct `factor_id` emitted into that panel has a dictionary entry.
  A deliberately-removed entry turns the gate **red**.

Parse the dictionary's factor rows back out of the Markdown with a small stable reader
(`parse_dictionary_factor_ids(text) -> set[str]`) so the gate reads the **artifact**, not just the in-memory
renderer — catching a stale committed file, not only a stale function.

**TDD:**
- `test_dictionary_covers_every_definition_factor` — set-equality between definition `factor_id`s and parsed
  dictionary `factor_id`s.
- `test_missing_entry_fails_gate` — dropping one factor from the render makes the coverage assertion fail
  (proves the gate bites).
- `test_no_orphan_dictionary_entries` — every parsed id resolves to a definition.

**PIT:** (D) deterministic set comparison; (J) coverage keyed on the contract's `factor_id`.

**Accept:** the coverage gate is green on the full namespace and **red** when any single factor lacks an entry
or any entry lacks a definition.

### S9-2 — Runbook + referenced-path-existence gate

**Root cause:** the activation/consumption sequence is scattered across scripts and sprints with no single
ordered, honest runbook; and any runbook that names a script which does not exist is a lie that a fresh agent
discovers only at the failing step.

**Fix:** author `atx-impl/docs/RUNBOOK.md` — a fresh-agent, copy-pasteable, **ordered** activation +
consumption guide. Each step is tagged **`[OFFLINE-DETERMINISTIC]`** or **`[OPERATOR — live DB, backup first]`**
and names the exact command + repo-relative path:
1. **Recover from `.bak` (clause F)** — restore `db/atx_impl.duckdb` from the latest timestamped `*.bak` + WAL
   split; verify with `scripts/verify_quant_warehouse.py`. `[OPERATOR]`
2. **Migrate** — `python scripts/warehouse_migrate.py --db-path db/atx_impl.duckdb` through the pf4 range
   (`…0204`); CHECKPOINT + timestamped backup first. `[OPERATOR]`
3. **Operator-gated backfill** — `python scripts/warehouse_activate.py --plan` (dry-run, archives the plan
   without touching the live DB) then `--execute` on explicit go, widening `equity_daily_bars`. `[OPERATOR]`
   (PF4-S6.)
4. **Rebuild through the gated DAG** — `python scripts/warehouse_rebuild.py` / `scripts/warehouse_jobs.py`
   through the factor-panel gate. `[OFFLINE-DETERMINISTIC]` on a slice; `[OPERATOR]` full.
5. **Freshness / anomaly / lineage sweep** — the PF4-S2 `db/observability.py` factor surfaces + the
   `panel_quality_gate_halt` orchestrator gate. `[OFFLINE-DETERMINISTIC]`
6. **Export + publish release** — `python -m db.factor_panel export` then publish an immutable semver release
   via `db/panel_release.py` (PF4-S7). `[OFFLINE-DETERMINISTIC]` on a slice.
7. **Verify via the SDK** — `pip install -e clients/atx-panel`, then `atx_panel.read_panel(as_of=…, release=…)`
   and assert it matches the view read (PF4-S8 clause-L parity). `[OFFLINE-DETERMINISTIC]`

Add `atx-impl/docs/README.md` as a one-screen index linking the dictionary, runbook, and notebooks.

**TDD:**
- `test_runbook_referenced_paths_exist` — extract every backtick-quoted repo-relative path (scripts, modules,
  packages) from `RUNBOOK.md` with a stable extractor and assert each exists on disk; a typo'd or
  not-yet-landed path fails (this is the guard that S1/S2/S6/S7/S8 landed first).
- `test_runbook_steps_are_tagged` — every numbered step carries exactly one `[OFFLINE-DETERMINISTIC]` or
  `[OPERATOR …]` tag (honesty gate).
- `test_readme_links_resolve` — every relative link in `docs/README.md` resolves to a file in the docs tree.

**PIT:** (C) the gate reads the repo tree offline; no step in the test path mutates a live DB.

**Accept:** every path the runbook names exists; every step is tagged offline-vs-operator; the index links
resolve. Adding a reference to a nonexistent script turns the gate red.

### S9-3 — Runnable quickstart notebooks + execution gate

**Root cause:** prose examples rot; only an **executed** example on a pinned fixture stays honest. A consumer
needs three worked flows — load a release, score + backtest a factor, and walk a survivorship-safe universe —
that provably run.

**Fix:** ship three quickstarts under `atx-impl/docs/notebooks/`, each an importable `.py` with a
`main(store) -> dict` entry point (plus an optional `.ipynb` mirror), plus a shared
`atx-impl/docs/notebooks/_fixture.py` that builds a **bounded in-memory DuckDB slice** from committed CSV
fixtures (reusing the `db/tests` factor-panel fixtures) — no live DB, no network:
1. `quickstart_01_load_release.py` — load a pinned release via the `atx-panel` SDK (falling back to
   `factor_panel.read_panel_asof` against the fixture store when the SDK/release is absent) → a **PIT
   cross-section**; return `{row_count, factor_count, as_of_date}`.
2. `quickstart_02_factor_ic_backtest.py` — compute rank-IC and a **decile long-short** backtest from
   `signal_eval` outputs when present, else self-contained from the fixture panel + forward returns; return
   `{mean_rank_ic, decile_spread_sign, n_dates}`.
3. `quickstart_03_universe_survivorship.py` — walk a universe as-of cross-section with **survivorship-safe**
   (delisting-return-aware) forward returns; return `{n_members, n_delisted_handled}`.

Each `main` is **deterministic** on the fixture (fixed seed / no wall-clock) with **pinned** return values the
test asserts.

**TDD:**
- `test_quickstart_01_executes` / `_02_` / `_03_` — import the module, run `main(fixture_store())`, assert it
  returns the pinned dict without raising.
- `test_notebooks_are_offline` — the fixture builder opens only an in-memory store and reads only committed
  fixture files (no `atx_impl.duckdb`, no network import).
- `test_quickstart_02_decile_spread_sign_is_stable` — the monotone-factor fixture yields a positive decile
  spread (a real signal check, not just "ran").

**PIT:** (A) the IC/backtest joins gate forward returns on `available_at ≤ as_of`; (C)/(D) fixture-only,
pinned, deterministic.

**Accept:** all three quickstarts execute end-to-end on the fixture slice and return their pinned outputs; the
offline gate proves no live-DB/network dependency; the monotone fixture produces the expected positive spread.

---

## Sequencing & expected compounding

**S9-0 → S9-1 → S9-2 → S9-3.** S9-0 lays the deterministic generator + the committed dictionary (the artifact
everything else asserts against). S9-1 binds that artifact to the live factor namespace so it can never silently
omit a factor. S9-2 writes the adoption path and gates it on real, existing paths — the mechanism that proves
the S1/S2/S6/S7/S8 predecessors actually landed. S9-3 makes the consumption story **executable** on a pinned
fixture. Compounding: the dictionary is the **join surface** between the factor contract and PF4-S1's signal
scores — the "every factor scored for signal, with a generated data dictionary" half of the north star — and
the runbook + notebooks are what let a fresh quant team `pip install atx-panel`, pin a release, and pull a
scored PIT panel **without reading the source**. This is the last Track-C product-surface sprint before the
served read tier (PF4-S10) and the capstone (PF4-S11), which cites this runbook as the activation script.

## Risks / guardrails

- **The dictionary must be generated, never hand-edited.** If someone edits `data_dictionary.md` by hand, the
  drift gate must fail. Never "fix" the file by editing it — fix the generator and regenerate. The committed
  file is an **output**, not a source.
- **Honest signal columns.** When the eval surface is empty, IC/decay/crowding render the `—` sentinel — never
  `0`, never a fabricated number. A consumer must be able to tell "not yet evaluated" from "evaluated as zero".
- **Coverage is bidirectional.** No missing factor **and** no orphan entry. A dictionary that lists a factor the
  catalog no longer defines is as wrong as one that omits a live factor.
- **Runbook paths must exist.** The path-existence gate is the honesty contract; do not weaken it to "most paths
  exist". If a predecessor sprint has not landed, the runbook step for it must not be referenced yet (or the
  gate is red) — reconcile to the landed name, never paper over a missing script.
- **Notebooks are offline + pinned.** No live DB, no network, no wall-clock, no random seed drift. A notebook
  that only "runs" but returns unpinned output is not a gate.
- **Stay in lane.** Read-and-render only. Do not author/amend any contract, `schema_sha256`, public-API
  snapshot, or migration. Zero schema change; reserved migrations: none.

## Bench / acceptance

- `python -m scripts.gen_data_dictionary` regenerates `atx-impl/docs/data_dictionary.md` **byte-for-byte**; the
  `--check` flag exits 0 against the committed file and non-zero on any drift.
- The dictionary covers **every** panel factor (definition-set equality + fixture-panel coverage); a removed
  entry fails the gate; there are no orphan entries.
- The panel-schema section matches `PANEL_CONTRACT` exactly and stamps `PANEL_CONTRACT_SHA256`; signal columns
  are real when `signal_eval` is populated and the `—` sentinel when it is not.
- `atx-impl/docs/RUNBOOK.md` names only paths that exist; every numbered step is tagged offline-vs-operator; the
  `docs/README.md` index links resolve.
- All three quickstart notebooks execute end-to-end on the bounded fixture slice and return their pinned
  outputs; the offline gate proves no live-DB/network dependency.
- `python -m pytest atx-impl\db\tests\test_data_dictionary.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green before commit (run from `atx-impl/`, never from `db/`).
- No live-DB smoke required (docs-only, zero migrations) — but the runbook is recorded as the operator's
  activation script, and the capstone (PF4-S11) executes it on a slice.
- `PARITY_GAP.md` updated (the product now ships a generated, drift-gated data dictionary + a fresh-agent
  activation/consumption runbook + runnable quickstart notebooks — the "understandable + adoptable" gap closed);
  a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains = docs/dictionary/notebooks,
  verification commands, "no live smoke — docs-only, 0 migrations", caveats/next → PF4-S10 served read tier).

**Process:** own git worktree off the integration mainline, merged at sprint end via
`atx-impl/scripts/new_db_worktree.sh new|finish pf4-s9`; controller `superpowers:subagent-driven-development`
(fresh implementer + reviewer per task; TDD + verification-before-completion). Offline tests run from
`atx-impl/` (`db/calendar.py` shadows stdlib `calendar` if cwd is `db/`). Never `git add -A` (stage explicit
paths); never push unless asked. Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
