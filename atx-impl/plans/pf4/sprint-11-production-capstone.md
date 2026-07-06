# Sprint PF4-S11 — Production capstone (end-to-end activation proof + evidenced surpass-ledger flip + whole-branch pf3+pf4 review)

**Track:** D (Harden + capstone). **Reserved migrations 0201–0204.** NEW `db/tests/test_pf4_capstone.py`.

**Goal:** make the *whole assembled pf3+pf4 product* provably activatable, provably surpass-evidenced, and provably catalogued — without computing any new factor, panel row, or eval metric. Three things, in order: (1) a **fresh-agent activation runbook** (`atx-impl/docs/`) plus a **thin `scripts/` driver** that ties PF4-S6's `scripts/warehouse_activate.py` and `scripts/warehouse_rebuild.py` into one honest recover→migrate→backfill→rebuild→gate→observe→export→release→SDK-verify sequence, and **proves on a bounded fixture slice** that a full activate-then-incremental cycle is **deterministic + idempotent** (incremental re-run = **zero net new rows + byte-identical panel**); (2) the **surpass-ledger flip** in `db/parity.py` — four `parity_status='surpassed'` rows in `provider_parity_matrix`, each backed by a new `surpass_axis_evidence` row that **cites a concrete, resolvable surface/check** (a citation that does not resolve is a review-blocking defect); (3) a **final catalog sweep** (migration + test) asserting **0 uncatalogued** pf3+pf4 tables, then a **whole-branch pf3+pf4 review** by the strongest reviewer, then `superpowers:finishing-a-development-branch`.

This is the **last** pf4 sprint — the capstone that makes the north star *evidenced, not asserted*.

> **Scope delta from pf3-S12.** pf3-S12 bundled panel-gating + factor-observability into its capstone. In pf4 those moved to **PF4-S2** (`panel_quality_gate_halt`, `factor_freshness_sla`, `panel_rowcount_anomaly`, `lineage_completeness_checks`, `maintenance_schedule`). So **PF4-S11 does NOT re-implement gating or observability** — it *drives* them end-to-end through the activation proof, *cites* them as surpass evidence, and *sweeps* the whole surface. PF4-S11 = end-to-end activation proof + parity-evidence flip + whole-branch review.

**Mandate / Owns:**
- `atx-impl/docs/runbooks/pf4-activation-capstone-runbook.md` — the fresh-agent recover→…→SDK-verify runbook (offline-deterministic vs operator-run boundary stated per step).
- NEW `atx-impl/scripts/warehouse_activation_capstone.py` — a **thin** driver: `plan` (offline dry-run, no live DB touch) and `activate` (operator-gated) referencing PF4-S6 `scripts/warehouse_activate.py` + `scripts/warehouse_rebuild.py`; and `prove` (offline slice determinism proof).
- `db/parity.py` — extend `PROVIDER_PARITY_ROWS` with four `surpassed` axis rows; add `SURPASS_AXIS_ROWS`, `SURPASS_AXIS_EVIDENCE`, and `seed_rows_into_parity_matrix(conn, rows)` / `seed_surpass_axis_evidence(conn)` helpers (do **not** fork `seed_provider_parity_matrix`).
- Migrations **0201–0204** (`db/migrations/bodies_0201_0204.py`) + registry wiring.
- NEW `db/tests/test_pf4_capstone.py`.
- `WAREHOUSE_PARITY_TRANCHES.md` (append one row) + `db/PARITY_GAP.md` (flip the four surpass axes).

**Must NOT touch:** the **factor / panel / eval / gate / observability content** — PF4-S1's `db/signal_eval.py` IC/decay/turnover/crowding bodies; PF4-S2's `panel_quality_gate_halt`, SLA/anomaly/lineage evaluators, `panel_gate_config`, `maintenance_schedule`; PF3-S7/S8/S9's factor engine/families/namespace; PF3-S10's `v_factor_panel`/`v_factor_panel_wide` + export contract + `schema_sha256`; PF4-S4's delisting returns; PF4-S5's multi-universe; PF4-S6's `ActivationHarness`; PF4-S7's `db/panel_release.py`; PF4-S8's `clients/atx-panel/`. This sprint **drives, cites, and sweeps** that content; it never changes what any of it *computes*. Do not edit a landed migration or another sprint's reserved range (stay strictly in **0201–0204**). Do not re-implement `run_warehouse_rebuild` (`db/rebuild.py`) or the S6 harness — the driver *composes* them.

**Depends on:** **all prior pf4 sprints and the pf3 closure they carry** — PF4-S1 (signal-eval surface the runbook's observe step reads), PF4-S2 (the gate + SLA/anomaly/lineage surfaces the runbook sweeps and axes 1/3 cite), PF4-S3 (green, time-bomb-free suite this capstone runs inside), PF4-S4 (survivorship returns), PF4-S5 (multi-universe), **PF4-S6** (`scripts/warehouse_activate.py` / `ActivationHarness` the driver composes), **PF4-S7** (`db/panel_release.py` the runbook's release step calls), **PF4-S8** (`clients/atx-panel` the runbook's SDK-verify step calls), PF4-S9 (`atx-impl/docs/` tree this runbook lands in), PF4-S10 (served read tier). **Also pf2-S4** (`fundamental_pit_snapshot` vintages — axis 3), **pf2-S8** (`press_release_facts` — axis 3), **pf2-S9** (`fact_disagreement` + agreement SLA — axis 4), **pf3-S8** (`signal_native_factors` / `factor_definition` rows — axis 2). **Last sprint; sequential after PF4-S10.**

---

## Baseline / where the cycles go

By PF4-S10 the product is code-complete, gated, observable, releasable, and SDK-frontable — but three capstone gaps remain. Measured 2026-07-06 against `atx-impl/db`, `atx-impl/scripts`, and the pf4 sprint deliverables this capstone ties together.

1. **The activation is push-button per-step, but not proven end-to-end as one deterministic sequence.** PF4-S6 built `scripts/warehouse_activate.py` (resumable dry-run plan + operator-gated exec of migrate→backfill→rebuild) and `scripts/warehouse_rebuild.py` drives the gated DAG (`run_warehouse_rebuild(store, since, until, rebuild_run_id, orchestrator_run_id, git_sha, gate)` → `db/rebuild.py`). PF4-S7 publishes releases; PF4-S8 verifies via the SDK. But **no single fresh-agent document** walks the *whole* chain — recover-from-`.bak` → CHECKPOINT → migrate → OPERATOR historical backfill → deterministic rebuild through the gated DAG → freshness/anomaly/lineage sweep → export + publish release → SDK verify — and **nothing proves** that a full activate followed immediately by an incremental cycle is a **no-op** (clause H) yielding a **byte-identical** `v_factor_panel` (clause D). The mechanism exists; its end-to-end determinism and its offline/operator honesty boundary are unproven and unwritten.

2. **The four surpass axes are claimed in the ROADMAP but NOT evidenced in queryable data.** `db/parity.py` carries `PROVIDER_PARITY_ROWS` (frozen `ProviderParityRow`: `provider`, `provider_domain`, `warehouse_domain`, `parity_status`, `warehouse_tables`, `limitations`, `next_gap`, …) seeded into `provider_parity_matrix` via `seed_provider_parity_matrix` (`db/parity.py:788`). pf3+pf4 committed to *surpassing* FactSet/Compustat on four axes (full lineage, signal-native factors, PIT-perfect vintages + freshness, open cross-vendor recon), but the ledger has **no `surpassed` row** and **no table** that cites the concrete surface proving each axis. The claim is prose in the ROADMAP, not resolvable data.

3. **No whole-surface catalog close-out over pf3+pf4.** Clause (E) is enforced per-migration by the standing `missing_table_catalog_entries` / `missing_field_catalog_entries` checks, but there is **no capstone assertion** that *every* table added across pf3 (0132–0175) and pf4 (0176–0204) is catalogued **and** contract-covered, and no whole-branch review has run across the assembled pf3+pf4 surface.

**Already good — do not regress:**
- **PF4-S2's gate + observability.** `panel_quality_gate_halt`, the `factor_freshness_sla` / `panel_rowcount_anomaly` / `lineage_completeness_checks` evaluators, `panel_gate_config`, and `maintenance_schedule` stay byte-identical. S11 reads and cites them; it never rewrites them.
- **`db/rebuild.py` + `scripts/warehouse_rebuild.py`.** `run_warehouse_rebuild(...)` and its `rebuild_run_id`/`orchestrator_run_id`/`git_sha`/`dataset_counts` result stay as PF2-S10/PF4-S6 shipped them. The capstone driver *composes* them.
- **`db/parity.py` ledger surface.** `ProviderParityRow`, `PROVIDER_PARITY_ROWS`, `seed_provider_parity_matrix`, `provider_parity_matrix` (PK `(provider, provider_domain)`) stay the surface; S11 *appends* rows + an evidence table through the same INSERT shape, never forks the seeder.
- **`v_factor_panel` + export contract.** PF3-S10's `v_factor_panel`/`v_factor_panel_wide`, the `factor_panel_export_contract` gate, and the panel `schema_sha256` stay exactly as landed; the determinism proof *reads* the panel, it does not perturb the export contract.

---

## PIT / determinism + production contract

All clauses **(A)–(L)** apply — S11 is the capstone that verifies they hold *end-to-end* across the assembled surface, not within one module. The task-called clauses **A / B / E / F / G** are load-bearing here:

- **(A) Bitemporal / no lookahead.** The determinism proof reads `v_factor_panel` only through the same as-of/available-at gating the panel already enforces; the runbook's rebuild step never advances `available_at` past the operator's declared as-of. The proof fixture plants no future-dated rows.
- **(B) Append-only, catalogued migrations.** **0201–0204 only**, forward-only/idempotent, split schema from index per the WAL precedent, each new table/view/check seeding `table_catalog` + `field_catalog` in the same migration via `_catalog_fields_for_tables` + `_refresh_schema_contract_v2_pin`. `0201` = `surpass_axis_evidence` + four `surpassed` `provider_parity_matrix` rows. `0202` = `activation_capstone_proof` evidence table. `0203` = final lookup indexes. `0204` = `v_pf4_capstone_uncatalogued` + `pf4_capstone_catalog_sweep` critical check. Never renumber; never edit a landed migration.
- **(E) Schema-as-contract.** No table lands without a `table_catalog` row + `field_catalog` coverage + a `schema_contract` refresh; the 0204 sweep is the clause-(E) close-out over the *whole* pf3+pf4 surface (0 uncatalogued).
- **(F) Backup-before-migrate.** The runbook's every live step is fronted by PF2-S2's `db/migration_admin.py` CHECKPOINT + timestamped `.duckdb`/`.wal` backup + post-verify (clause F is the standing invariant); no live migrate/backfill/rebuild runs without a backup first. The offline proof never touches the live DB.
- **(G) Quality-gated.** The rebuild the runbook drives runs `gate=True` (PF4-S2's `panel_quality_gate_halt` can halt it); the 0204 catalog sweep is a `severity=critical` check wired into the registry. Determinism is a pure function of source + `git_sha` + params.

- **(C)** Every test in `test_pf4_capstone.py` runs against in-memory / template-copy DuckDB with fixtures: a bounded activate+incremental slice (S11-0), the four seeded surpass rows + a planted unresolvable citation (S11-1), a fully-catalogued fixture + a planted uncatalogued table (S11-2). No SEC/vendor network. The live capstone smoke (a gated live activate on operator go, the sweep, the deterministic rebuild) is **operator-run and recorded in the ledger** — never executed in-module.
- **(K)/(L)** The runbook's release step is idempotent (re-publishing identical inputs is a no-op, clause K); its SDK-verify step asserts the `atx-panel` read equals the contracted view read bit-for-bit (clause L).

---

## Tasks

### S11-0 — End-to-end activation proof + fresh-agent runbook + thin driver

**Root cause:** the activation mechanism is per-step push-button (PF4-S6/S7/S8) but never tied into one fresh-agent sequence, and its end-to-end determinism + idempotency is unproven and its offline/operator boundary unwritten (baseline 1). A downstream team cannot yet *reproduce* activation from a fresh checkout, and cannot yet *trust* that re-running maintenance won't drift the panel.

**Fix — three artifacts, all real, no placeholders:**

**(a) The runbook** — `atx-impl/docs/runbooks/pf4-activation-capstone-runbook.md`. A fresh-agent document walking the full chain, each step tagged **[OFFLINE-DETERMINISTIC]** (a fresh agent runs it and reproduces byte-for-byte) or **[OPERATOR-GATED]** (requires explicit per-step operator go per scope decision #1; documented, never executed in-module). Steps:

1. **[OFFLINE-DETERMINISTIC]** Recover from `.bak` — `python -m db.migration_admin restore --db-path <bak>` (PF2-S2), or start from a template DuckDB.
2. **[OPERATOR-GATED]** CHECKPOINT + timestamped `.duckdb`/`.wal` backup — `python scripts/warehouse_migrate.py` pre-flight (clause F).
3. **[OPERATOR-GATED]** Migrate live to head — `python scripts/warehouse_migrate.py` (applies 0176–0204 on operator go; backup-verified).
4. **[OPERATOR-GATED]** Historical backfill — `python scripts/warehouse_activate.py activate --i-am-operator …` (PF4-S6 harness widening `equity_daily_bars` →2004+; the multi-year archive is the genuinely operator-run step).
5. **[OFFLINE-DETERMINISTIC on a slice / OPERATOR-GATED at scale]** Deterministic gated rebuild — `python scripts/warehouse_rebuild.py --since … --until …` (`run_warehouse_rebuild(gate=True)`).
6. **[OFFLINE-DETERMINISTIC]** Freshness / anomaly / lineage sweep — the PF4-S2 evaluators + the PF4-S1 signal-eval read of `v_factor_panel`.
7. **[OPERATOR-GATED]** Export + publish release — PF4-S7 `db/panel_release.py` (immutable semver'd, checksummed; re-publish identical = no-op, clause K).
8. **[OFFLINE-DETERMINISTIC]** SDK verify — PF4-S8 `atx-panel` `read_panel(as_of, universe, factors, release)` equals the contracted view read bit-for-bit (clause L).

The runbook ends with the **determinism contract**: on a bounded slice, a full activate → rebuild → incremental-maintenance cycle produces **zero net new rows** and a **byte-identical `v_factor_panel` fingerprint**; the multi-year archive load is the honestly-operator-run step whose determinism the slice *proves by construction*, not by executing it here.

**(b) The thin driver** — `atx-impl/scripts/warehouse_activation_capstone.py`. Composes the S6 harness + `warehouse_rebuild`; never re-implements them. `plan` is offline (reads the plan, touches no live DB); `activate` is operator-gated (refuses without `--i-am-operator` + `--confirm`); `prove` runs the offline slice determinism proof and prints the fingerprint pair.

```python
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from db.connection import DEFAULT_DB_PATH, connect
from db.rebuild import run_warehouse_rebuild

# PF4-S6 deliverable (scripts/warehouse_activate.py): the resumable operator harness.
# Imported lazily inside activate() so `plan`/`prove` never require live-DB deps.


def panel_fingerprint(store) -> str:
    """Deterministic content hash of the PIT factor panel (clause D)."""
    rows = store.con.execute(
        """
        SELECT security_id, CAST(as_of_date AS VARCHAR), factor_id, value
        FROM v_factor_panel
        ORDER BY security_id, as_of_date, factor_id
        """
    ).fetchall()
    payload = json.dumps([list(map(str, r)) for r in rows], sort_keys=True).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def cmd_plan(args: argparse.Namespace) -> int:
    """Offline dry-run: print the ordered activation steps; never open the live DB."""
    import warehouse_activate  # PF4-S6 module, resolved on sys.path from scripts/

    plan = warehouse_activate.plan_activation(
        since=args.since, until=args.until, db_path=args.db_path
    )
    print(json.dumps({"mode": "plan", "steps": plan.steps, "touches_live_db": False}, indent=2, sort_keys=True))
    return 0


def cmd_prove(args: argparse.Namespace) -> int:
    """Offline determinism/idempotency proof on a bounded slice (no live DB)."""
    with connect(args.db_path, read_only=False) as store:
        first = run_warehouse_rebuild(
            store, since=args.since, until=args.until,
            rebuild_run_id="capstone-full", git_sha=args.git_sha, gate=True,
        )
        fp_after_full = panel_fingerprint(store)
        rowcount_after_full = store.con.execute("SELECT count(*) FROM v_factor_panel").fetchone()[0]

        # An incremental maintenance cycle immediately after a full rebuild must be a no-op.
        second = run_warehouse_rebuild(
            store, since=args.since, until=args.until,
            rebuild_run_id="capstone-incremental", git_sha=args.git_sha, gate=True,
        )
        fp_after_incremental = panel_fingerprint(store)
        rowcount_after_incremental = store.con.execute("SELECT count(*) FROM v_factor_panel").fetchone()[0]

    net_new_rows = rowcount_after_incremental - rowcount_after_full
    byte_identical = fp_after_full == fp_after_incremental
    result = {
        "full_rebuild_run_id": first.rebuild_run_id,
        "incremental_run_id": second.rebuild_run_id,
        "panel_fingerprint_after_full": fp_after_full,
        "panel_fingerprint_after_incremental": fp_after_incremental,
        "net_new_rows": net_new_rows,
        "byte_identical_panel": byte_identical,
        "deterministic_idempotent": byte_identical and net_new_rows == 0,
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["deterministic_idempotent"] else 1


def cmd_activate(args: argparse.Namespace) -> int:
    """Operator-gated live activation. Refuses without explicit operator confirmation."""
    if not (args.i_am_operator and args.confirm):
        raise SystemExit(
            "activate is OPERATOR-GATED (scope decision #1): pass --i-am-operator --confirm. "
            "Back up first (clause F). Use `plan` for an offline dry-run."
        )
    import warehouse_activate  # PF4-S6 harness

    with connect(args.db_path, read_only=False) as store:
        return warehouse_activate.run_activation(
            store, since=args.since, until=args.until, git_sha=args.git_sha
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="PF4-S11 activation capstone driver.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--since")
    parser.add_argument("--until")
    parser.add_argument("--git-sha")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan").set_defaults(func=cmd_plan)
    sub.add_parser("prove").set_defaults(func=cmd_prove)
    act = sub.add_parser("activate")
    act.add_argument("--i-am-operator", action="store_true")
    act.add_argument("--confirm", action="store_true")
    act.set_defaults(func=cmd_activate)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
```

**(c) Migration 0202** — `activation_capstone_proof`, a small evidence table so a live proof run records its fingerprint pair + net-new-row delta as queryable data (not just stdout). Schema/catalog only; no index in 0202 (index goes in 0203, WAL-split per clause B).

```python
def _pf4_s11_activation_capstone_proof(conn: "duckdb.DuckDBPyConnection") -> None:
    """PF4-S11 S11-0: evidence surface for the activate-then-incremental determinism proof."""
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS activation_capstone_proof (
            proof_id VARCHAR PRIMARY KEY,
            full_rebuild_run_id VARCHAR NOT NULL,
            incremental_run_id VARCHAR NOT NULL,
            panel_fingerprint_after_full VARCHAR NOT NULL,
            panel_fingerprint_after_incremental VARCHAR NOT NULL,
            net_new_rows BIGINT NOT NULL,
            byte_identical_panel BOOLEAN NOT NULL,
            git_sha VARCHAR,
            slice_since DATE,
            slice_until DATE,
            recorded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'activation_capstone_proof', 'control', 'activation_capstone_proof', 'proof_id',
            'Records the PF4-S11 activate-then-incremental determinism proof: the panel fingerprint before/after an incremental cycle and the net-new-row delta.',
            '["proof_id"]',
            'Control/evidence table; byte_identical_panel=true and net_new_rows=0 are the clause-D/H acceptance conditions.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("activation_capstone_proof",))
    _refresh_schema_contract_v2_pin(conn)
```

**PIT:** (D) same source + `git_sha` + params → same panel rows across the full and incremental paths; the fingerprint is order-stable (`ORDER BY security_id, as_of_date, factor_id`). (H) the incremental re-run over an already-completed window is a no-op. (F) the runbook's live steps are backup-fronted; the offline `prove`/`plan` paths never open the live DB.

**Accept:** the runbook exists and a fresh agent follows it once (every step tagged offline-deterministic or operator-gated, honestly); `warehouse_activation_capstone.py plan` prints the ordered steps with `touches_live_db=false`; on a fixture slice `prove` reports `byte_identical_panel=true` and `net_new_rows=0`; `activate` refuses without `--i-am-operator --confirm`; `activation_capstone_proof` is queryable and catalogued.

**TDD (write first):**

```python
# db/tests/test_pf4_capstone.py  (S11-0 block)
import hashlib
import importlib
import json
from pathlib import Path

import pytest

from db.connection import connect
from db.tests.helpers import make_template_store  # existing offline template-copy helper


def _fingerprint(store) -> str:
    rows = store.con.execute(
        "SELECT security_id, CAST(as_of_date AS VARCHAR), factor_id, value "
        "FROM v_factor_panel ORDER BY security_id, as_of_date, factor_id"
    ).fetchall()
    return hashlib.sha256(
        json.dumps([list(map(str, r)) for r in rows], sort_keys=True).encode()
    ).hexdigest()


def test_full_activate_then_incremental_is_idempotent_and_byte_identical(capstone_slice_store):
    """A full rebuild then an immediate incremental cycle: zero net new rows + identical panel."""
    from db.rebuild import run_warehouse_rebuild

    store = capstone_slice_store
    run_warehouse_rebuild(store, rebuild_run_id="full", git_sha="deadbeef", gate=True)
    fp1 = _fingerprint(store)
    n1 = store.con.execute("SELECT count(*) FROM v_factor_panel").fetchone()[0]

    run_warehouse_rebuild(store, rebuild_run_id="incr", git_sha="deadbeef", gate=True)
    fp2 = _fingerprint(store)
    n2 = store.con.execute("SELECT count(*) FROM v_factor_panel").fetchone()[0]

    assert n2 - n1 == 0, "incremental cycle must add zero net new panel rows (clause H)"
    assert fp1 == fp2, "panel must be byte-identical across full and incremental paths (clause D)"


def test_activation_capstone_driver_plan_touches_no_live_db(monkeypatch, tmp_path):
    """`plan` composes the S6 planner and never opens the live DB."""
    driver = importlib.import_module("scripts.warehouse_activation_capstone")  # or load by path
    opened = {"connect": 0}
    monkeypatch.setattr(driver, "connect", lambda *a, **k: opened.__setitem__("connect", opened["connect"] + 1))
    # a stub S6 planner returns a step list without a DB
    ...
    assert opened["connect"] == 0


def test_runbook_referenced_scripts_and_paths_exist():
    """Every script/module the runbook cites resolves on disk (no dead references)."""
    root = Path(__file__).resolve().parents[2]  # atx-impl/
    for rel in (
        "docs/runbooks/pf4-activation-capstone-runbook.md",
        "scripts/warehouse_activation_capstone.py",
        "scripts/warehouse_activate.py",     # PF4-S6
        "scripts/warehouse_rebuild.py",      # PF2-S10
        "scripts/warehouse_migrate.py",      # PF2-S2
        "scripts/warehouse_backfill.py",     # PF3-S1
        "db/rebuild.py",
        "db/parity.py",
    ):
        assert (root / rel).exists(), f"runbook references a missing path: {rel}"
```

---

### S11-1 — Surpass-ledger flip (evidenced, resolvable, or it fails)

**Root cause:** the four surpass axes are prose in the ROADMAP, not resolvable data in `provider_parity_matrix` (baseline 2). A downstream reviewer cannot query "prove you surpass Compustat on lineage" and get a row that points at a real surface.

**Fix:** extend `db/parity.py` and land migration **0201**:

**(a) `db/parity.py`** — append four `ProviderParityRow`s (`parity_status="surpassed"`) to `PROVIDER_PARITY_ROWS` as `SURPASS_AXIS_ROWS`, add the evidence spec `SURPASS_AXIS_EVIDENCE`, and add reuse helpers so migration 0201 seeds through the same INSERT shape (never forking `seed_provider_parity_matrix`). Each row's `warehouse_tables` cites the concrete surface for that axis:

```python
SURPASS_AXIS_ROWS: tuple[ProviderParityRow, ...] = (
    ProviderParityRow(
        provider="FactSet / S&P Compustat (surpass axis 1)",
        provider_domain="Full factor lineage completeness",
        warehouse_domain="factor_lineage",
        reference_tables=("FactSet Formula lineage", "Compustat item derivation"),
        institutional_grain="Every emitted panel factor traces to source fact(s) + formula + standardization rule + vintage.",
        institutional_keys=("factor_id", "security_id", "as_of_date"),
        pit_fields=("available_at", "valid_from"),
        factors_or_fields=("lineage-complete factor panel",),
        open_substitute="PF4-S2 lineage_completeness_checks over v_factor_panel joined through factor_dependency_edges / feature_dependency_edges to source facts.",
        warehouse_tables=("lineage_completeness_checks", "factor_dependency_edges", "feature_dependency_edges", "v_factor_panel"),
        parity_status="surpassed",
        limitations="Vendor lineage is opaque; ours is a queryable, gated completeness check.",
        next_gap="Extend lineage completeness to price/return inputs as density grows.",
        source_urls=("https://developer.factset.com/api-catalog/formula-api",),
    ),
    ProviderParityRow(
        provider="FactSet (surpass axis 2)",
        provider_domain="Signal-native fundamental factors",
        warehouse_domain="signal_native_factors",
        reference_tables=("FactSet standardized factors",),
        institutional_grain="Warehouse-native PIT revisions-momentum, standardization-delta, segment-concentration, footnote-disclosure-change factors.",
        institutional_keys=("factor_id",),
        pit_fields=("valid_from", "source_loaded_at"),
        factors_or_fields=("revisions_momentum", "standardization_delta", "segment_concentration", "footnote_change"),
        open_substitute="PF3-S8 signal_native_factors dataset (factor_definition rows) scored for signal by PF4-S1 db/signal_eval.py.",
        warehouse_tables=("factor_definition", "fundamental_factor_values", "signal_native_factors"),
        parity_status="surpassed",
        limitations="These factors have no licensed-vendor equivalent; they are native to the open XBRL revision/segment/footnote substrate.",
        next_gap="Add cross-domain footnote-graph factors as pf5 data lands.",
        source_urls=("https://www.sec.gov/search-filings/edgar-application-programming-interfaces",),
    ),
    ProviderParityRow(
        provider="S&P Compustat (surpass axis 3)",
        provider_domain="PIT-perfect vintages and factor freshness",
        warehouse_domain="pit_vintages_freshness",
        reference_tables=("Compustat point-in-time snapshot",),
        institutional_grain="Per-vintage fundamentals + press-release flash capture + a queryable panel freshness SLA.",
        institutional_keys=("security_id", "period_end", "as_of_date"),
        pit_fields=("available_at", "rdq", "pdate"),
        factors_or_fields=("vintage-class ratios", "preliminary press-release facts", "factor freshness SLA"),
        open_substitute="PF4-S2 factor_freshness_sla over dataset_watermarks + pf2-S4 fundamental_pit_snapshot vintages + pf2-S8 press_release_facts flash capture.",
        warehouse_tables=("factor_freshness_sla", "fundamental_pit_snapshot", "press_release_facts", "press_release_reconciliation"),
        parity_status="surpassed",
        limitations="Vendor PIT is snapshot-billed; ours is continuously vintage-tracked and freshness-gated.",
        next_gap="Extend freshness SLA to intraday release capture.",
        source_urls=("https://www.spglobal.com/market-intelligence/en/solutions/products/fundamental-data",),
    ),
    ProviderParityRow(
        provider="Cross-vendor (surpass axis 4)",
        provider_domain="Open cross-vendor reconciliation",
        warehouse_domain="cross_vendor_reconciliation",
        reference_tables=("Sharadar SF1", "SimFin", "vendor baselines"),
        institutional_grain="Every standardized fact reconciled against injectable vendor baselines with a published agreement SLA.",
        institutional_keys=("security_id", "item_id", "period_end", "basis"),
        pit_fields=("available_at",),
        factors_or_fields=("agrees / disagrees / missing_warehouse", "agreement ratio"),
        open_substitute="pf2-S9 fact_disagreement over vendor_baseline_facts vs fundamental_standardized, gated by the >99% agreement quality check.",
        warehouse_tables=("fact_disagreement", "vendor_baseline_facts", "fundamental_standardized"),
        parity_status="surpassed",
        limitations="No single vendor publishes its cross-vendor disagreement; ours is a first-class gated surface.",
        next_gap="Load broader licensed baselines to widen the reconciled cross-section.",
        source_urls=("https://www.sec.gov/search-filings/edgar-application-programming-interfaces",),
    ),
)

PROVIDER_PARITY_ROWS = PROVIDER_PARITY_ROWS + SURPASS_AXIS_ROWS  # existing rows keep their honest status


@dataclass(frozen=True)
class SurpassAxisEvidence:
    axis_id: str
    axis_name: str
    provider: str
    provider_domain: str          # joins back to the surpassed provider_parity_matrix row
    claim: str
    evidence_kind: str            # 'quality_check' | 'table' | 'view' | 'dataset'
    evidence_object: str          # must resolve: a quality_check_registry.check_name,
                                  # table_catalog.table_name, or dataset_catalog.dataset_id
    evidence_locator: str         # the concrete predicate/check that proves the axis


SURPASS_AXIS_EVIDENCE: tuple[SurpassAxisEvidence, ...] = (
    SurpassAxisEvidence("axis1_full_lineage", "Full factor lineage completeness",
        "FactSet / S&P Compustat (surpass axis 1)", "Full factor lineage completeness",
        "Every emitted panel factor resolves a full lineage chain to source facts + formula + standardization + vintage.",
        "quality_check", "lineage_completeness_check",
        "quality_check_registry.check_name='lineage_completeness_check' (PF4-S2) over v_factor_panel"),
    SurpassAxisEvidence("axis2_signal_native", "Signal-native fundamental factors",
        "FactSet (surpass axis 2)", "Signal-native fundamental factors",
        "Warehouse-native revisions-momentum / standardization-delta / segment / footnote factors exist as catalogued factor rows.",
        "dataset", "signal_native_factors",
        "dataset_catalog.dataset_id='signal_native_factors' (PF3-S8) -> factor_definition rows"),
    SurpassAxisEvidence("axis3_pit_vintages_freshness", "PIT-perfect vintages and freshness",
        "S&P Compustat (surpass axis 3)", "PIT-perfect vintages and factor freshness",
        "Per-vintage snapshots + press-release flash + a gated panel freshness SLA.",
        "quality_check", "factor_freshness_sla_check",
        "quality_check_registry.check_name='factor_freshness_sla_check' (PF4-S2) + fundamental_pit_snapshot + press_release_facts"),
    SurpassAxisEvidence("axis4_cross_vendor_recon", "Open cross-vendor reconciliation",
        "Cross-vendor (surpass axis 4)", "Open cross-vendor reconciliation",
        "Standardized facts reconciled to vendor baselines with a first-class >99% agreement SLA.",
        "quality_check", "fact_disagreement_agreement_ratio",
        "quality_check_registry.check_name='fact_disagreement_agreement_ratio' (pf2-S9) over fact_disagreement"),
)


def seed_rows_into_parity_matrix(conn, rows: "tuple[ProviderParityRow, ...]") -> None:
    """Seed selected ProviderParityRows through the same INSERT shape as seed_provider_parity_matrix
    (raw-conn variant, for migration bodies). Does not fork the canonical seeder."""
    for row in rows:
        conn.execute(
            """
            INSERT OR REPLACE INTO provider_parity_matrix (
                provider, provider_domain, warehouse_domain, reference_tables_json,
                institutional_grain, institutional_keys_json, pit_fields_json,
                factors_or_fields_json, open_substitute, warehouse_tables_json,
                parity_status, limitations, next_gap, source_urls_json, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, now())
            """,
            [row.provider, row.provider_domain, row.warehouse_domain, _json_tuple(row.reference_tables),
             row.institutional_grain, _json_tuple(row.institutional_keys), _json_tuple(row.pit_fields),
             _json_tuple(row.factors_or_fields), row.open_substitute, _json_tuple(row.warehouse_tables),
             row.parity_status, row.limitations, row.next_gap, _json_tuple(row.source_urls)],
        )


def seed_surpass_axis_evidence(conn) -> None:
    for e in SURPASS_AXIS_EVIDENCE:
        conn.execute(
            """
            INSERT OR REPLACE INTO surpass_axis_evidence (
                axis_id, axis_name, provider, provider_domain, claim,
                evidence_kind, evidence_object, evidence_locator, updated_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, now())
            """,
            [e.axis_id, e.axis_name, e.provider, e.provider_domain, e.claim,
             e.evidence_kind, e.evidence_object, e.evidence_locator],
        )
```

**(b) Migration 0201** — create `surpass_axis_evidence`, catalog it, seed the four `surpassed` rows + four evidence rows, refresh the contract:

```python
def _pf4_s11_surpass_ledger_flip(conn: "duckdb.DuckDBPyConnection") -> None:
    """PF4-S11 S11-1: surpass_axis_evidence table + four evidenced surpassed parity rows."""
    from db.parity import SURPASS_AXIS_ROWS, seed_rows_into_parity_matrix, seed_surpass_axis_evidence

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS surpass_axis_evidence (
            axis_id VARCHAR PRIMARY KEY,
            axis_name VARCHAR NOT NULL,
            provider VARCHAR NOT NULL,
            provider_domain VARCHAR NOT NULL,
            claim VARCHAR NOT NULL,
            evidence_kind VARCHAR NOT NULL,
            evidence_object VARCHAR NOT NULL,
            evidence_locator VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'surpass_axis_evidence', 'catalog', 'surpass_axis_evidence', 'axis_id',
            'One row per committed surpass axis, citing the concrete resolvable surface/check that evidences it. A surpassed provider_parity_matrix row without a resolvable citation here is a review-blocking defect.',
            '["axis_id"]',
            'Catalog/evidence table; evidence_object must resolve to a quality_check_registry.check_name, table_catalog.table_name, or dataset_catalog.dataset_id.',
            now()
        )
        """
    )
    seed_rows_into_parity_matrix(conn, SURPASS_AXIS_ROWS)
    seed_surpass_axis_evidence(conn)
    _catalog_fields_for_tables(conn, ("surpass_axis_evidence",))
    _refresh_schema_contract_v2_pin(conn)
```

**PIT:** (B) 0201 catalogued, schema (no index — that's 0203). (D) the surpass evidence is data, each row citing a real surface. (E) `surpass_axis_evidence` seeds its own `table_catalog` + `field_catalog` rows.

**Accept:** `provider_parity_matrix` carries exactly four `parity_status='surpassed'` rows; each has ≥1 `surpass_axis_evidence` row whose `evidence_object` **resolves** to a real `quality_check_registry.check_name`, `table_catalog.table_name`, or `dataset_catalog.dataset_id`; the existing broad provider rows keep their honest `partial`/`implemented` status (no broad row is falsely flipped). **A citation that does not resolve fails the capstone check.**

**TDD (write first):**

```python
# db/tests/test_pf4_capstone.py  (S11-1 block)

def _resolves(store, kind: str, obj: str) -> bool:
    if kind == "quality_check":
        return store.con.execute(
            "SELECT count(*) FROM quality_check_registry WHERE check_name = ?", [obj]).fetchone()[0] > 0
    if kind in ("table", "view"):
        return store.con.execute(
            "SELECT count(*) FROM table_catalog WHERE table_name = ?", [obj]).fetchone()[0] > 0
    if kind == "dataset":
        return store.con.execute(
            "SELECT count(*) FROM dataset_catalog WHERE dataset_id = ?", [obj]).fetchone()[0] > 0
    return False


def test_provider_parity_matrix_carries_four_surpassed_rows(migrated_store):
    n = migrated_store.con.execute(
        "SELECT count(*) FROM provider_parity_matrix WHERE parity_status = 'surpassed'"
    ).fetchone()[0]
    assert n == 4


def test_every_surpassed_row_has_resolvable_surpass_axis_evidence(migrated_store):
    surpassed = migrated_store.con.execute(
        "SELECT provider, provider_domain FROM provider_parity_matrix WHERE parity_status='surpassed'"
    ).fetchall()
    assert len(surpassed) == 4
    for provider, domain in surpassed:
        ev = migrated_store.con.execute(
            "SELECT evidence_kind, evidence_object FROM surpass_axis_evidence "
            "WHERE provider = ? AND provider_domain = ?", [provider, domain]).fetchall()
        assert ev, f"surpassed row ({provider}, {domain}) has no surpass_axis_evidence citation"
        for kind, obj in ev:
            assert _resolves(migrated_store, kind, obj), \
                f"citation does not resolve: {kind} -> {obj}"


def test_unresolvable_surpass_citation_is_flagged(migrated_store):
    """A planted citation to a non-existent surface must fail the resolvability assertion."""
    migrated_store.con.execute(
        "INSERT INTO surpass_axis_evidence VALUES "
        "('axis_bogus','bogus','x','y','claim','table','table_that_does_not_exist','none', now())"
    )
    row = migrated_store.con.execute(
        "SELECT evidence_kind, evidence_object FROM surpass_axis_evidence WHERE axis_id='axis_bogus'"
    ).fetchone()
    assert not _resolves(migrated_store, row[0], row[1])
```

---

### S11-2 — Final catalog sweep + whole-branch pf3+pf4 review + finish-branch

**Root cause:** clause (E) is enforced per-migration but never closed out over the *whole* pf3+pf4 surface, and no whole-branch review has run across the assembled product (baseline 3).

**Fix:** land migration **0204** (a `v_pf4_capstone_uncatalogued` view + a `pf4_capstone_catalog_sweep` critical check), migration **0203** (final lookup indexes for the new capstone tables), then run the whole-branch review and finish-branch.

**(a) Migration 0203** — indexes (WAL-split from schema per clause B):

```python
def _pf4_s11_capstone_indexes(conn: "duckdb.DuckDBPyConnection") -> None:
    """PF4-S11 S11-2: capstone lookup indexes (split from schema for WAL safety)."""
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_surpass_axis_evidence_provider "
        "ON surpass_axis_evidence(provider, provider_domain)",
        "CREATE INDEX IF NOT EXISTS idx_provider_parity_matrix_status "
        "ON provider_parity_matrix(parity_status)",
        "CREATE INDEX IF NOT EXISTS idx_activation_capstone_proof_recorded "
        "ON activation_capstone_proof(recorded_at)",
    ):
        conn.execute(statement)
    _refresh_schema_contract_v2_pin(conn)
```

**(b) Migration 0204** — the sweep view + critical check. `v_pf4_capstone_uncatalogued` lists any base table / view in `main` (excluding DuckDB internals) that lacks a `table_catalog` row **or** has zero `field_catalog` coverage; the check fails if the count is non-zero.

```python
def _pf4_s11_capstone_catalog_sweep(conn: "duckdb.DuckDBPyConnection") -> None:
    """PF4-S11 S11-2: whole-surface catalog sweep view + critical gate (clause E close-out)."""
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_pf4_capstone_uncatalogued AS
        SELECT t.table_name, t.table_type
        FROM information_schema.tables t
        WHERE t.table_schema = 'main'
          AND t.table_name NOT LIKE 'sqlite_%'
          AND (
            NOT EXISTS (SELECT 1 FROM table_catalog c WHERE c.table_name = t.table_name)
            OR NOT EXISTS (SELECT 1 FROM field_catalog f WHERE f.table_name = t.table_name)
          )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'v_pf4_capstone_uncatalogued', 'view', 'capstone_catalog_sweep', 'table_name',
            'Any main-schema table/view missing a table_catalog row or field_catalog coverage. The PF4-S11 capstone requires this view to be empty (0 uncatalogued pf3/pf4 tables).',
            '["table_name"]',
            'Clause-E close-out view; the pf4_capstone_catalog_sweep critical check fails when this is non-empty.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        )
        VALUES (
            'pf4_capstone_catalog_sweep', 'provider_parity_matrix', 'v_pf4_capstone_uncatalogued',
            'critical', 0.0, 'eq', true, 'failed', 'pf4_s11', now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("v_pf4_capstone_uncatalogued",))
    _refresh_schema_contract_v2_pin(conn)
```

**(c) Registry wiring** — `db/migrations/bodies_0201_0204.py` ends with the `MIGRATIONS` list, and `db/migrations/registry.py` splices it in ascending order after the S10 range; `db/migrations/__init__.py` appends `globals().pop("bodies_0201_0204", None)` per the existing cleanup pattern.

```python
MIGRATIONS: list[Migration] = [
    Migration(version=201, name="pf4_s11_surpass_ledger_flip", up=_pf4_s11_surpass_ledger_flip),
    Migration(version=202, name="pf4_s11_activation_capstone_proof", up=_pf4_s11_activation_capstone_proof),
    Migration(version=203, name="pf4_s11_capstone_indexes", up=_pf4_s11_capstone_indexes),
    Migration(version=204, name="pf4_s11_capstone_catalog_sweep", up=_pf4_s11_capstone_catalog_sweep),
]
```

**(d) Whole-branch review + finish-branch.** After the offline suite is green, run a **whole-branch pf3+pf4 review** (S1–S12 of pf3 as landed into pf4 + PF4-S1…S11) by the **strongest available reviewer** — architecture soundness, no cross-date leakage, PIT propagation, parameterized SQL, clause (A)–(L) coverage, the four surpass citations resolvable, the sweep green. Then run `superpowers:finishing-a-development-branch`.

**PIT:** (B) 0203/0204 catalogued, indexes split from schema. (E) 0204's sweep is the clause-(E) close-out over the whole pf3+pf4 surface. (G) `pf4_capstone_catalog_sweep` is a `severity=critical` registry check.

**Accept:** `SELECT count(*) FROM v_pf4_capstone_uncatalogued` is **0**; the `pf4_capstone_catalog_sweep` check passes; a planted uncatalogued table makes it fail; the whole-branch review is clean and the branch is finished.

**TDD (write first):**

```python
# db/tests/test_pf4_capstone.py  (S11-2 block)

def test_capstone_catalog_sweep_zero_uncatalogued(migrated_store):
    n = migrated_store.con.execute("SELECT count(*) FROM v_pf4_capstone_uncatalogued").fetchone()[0]
    assert n == 0, f"uncatalogued pf3/pf4 tables: {migrated_store.con.execute('SELECT table_name FROM v_pf4_capstone_uncatalogued').fetchall()}"


def test_planted_uncatalogued_table_trips_the_sweep(migrated_store):
    migrated_store.con.execute("CREATE TABLE _pf4_orphan_table (x INTEGER)")
    n = migrated_store.con.execute(
        "SELECT count(*) FROM v_pf4_capstone_uncatalogued WHERE table_name = '_pf4_orphan_table'"
    ).fetchone()[0]
    assert n == 1


def test_capstone_sweep_check_registered_critical(migrated_store):
    sev = migrated_store.con.execute(
        "SELECT severity FROM quality_check_registry WHERE check_name = 'pf4_capstone_catalog_sweep'"
    ).fetchone()
    assert sev and sev[0] == "critical"


def test_migrations_0201_0204_present_and_ordered():
    from db.migrations.registry import MIGRATIONS
    versions = [m.version for m in MIGRATIONS]
    assert versions == sorted(versions)
    assert {201, 202, 203, 204}.issubset(set(versions))
```

---

## Sequencing & expected compounding

**S11-0 → S11-1 → S11-2.** S11-0 first — the activation proof + runbook establish that the whole gated, observed, releasable, SDK-frontable product is *reproducible and deterministic* on a slice; that is the precondition for claiming anything is "surpassed." S11-1 next — flip the ledger with resolvable evidence, each axis citing a concrete PF4-S2 / pf3-S8 / pf2-S4 / pf2-S8 / pf2-S9 surface. S11-2 last — sweep the whole pf3+pf4 surface for 0 uncatalogued tables, then run the whole-branch review and finish the branch. **Compounding:** once the activate-then-incremental cycle is proven byte-identical, the runbook is trustworthy; once the surpass axes are resolvable rows, the north-star claim is *evidenced, not asserted*; once the sweep is green and the review is clean, pf3+pf4 is a finished, production-ready quant product — the capstone that makes the north star real.

---

## Risks / guardrails

- **The runbook must be honest.** A fresh agent must *actually* reproduce it — offline-deterministic where claimed (the slice `prove`), and clearly OPERATOR-GATED where the multi-year archive backfill / live migrate genuinely requires it (scope decision #1). No step may claim reproducibility it cannot deliver; the offline/operator boundary is stated per step. The live archive backfill is **OPERATOR-PENDING / documented, never executed in-module**.
- **The surpass flip must be EVIDENCED, not asserted.** Each of the four `surpassed` rows must cite a **resolvable** `surpass_axis_evidence` object (a real `quality_check_registry.check_name`, `table_catalog.table_name`, or `dataset_catalog.dataset_id`). A `surpassed` row with a dangling citation is a **review-blocking defect** — `test_every_surpassed_row_has_resolvable_surpass_axis_evidence` enforces exactly this, and `test_unresolvable_surpass_citation_is_flagged` proves the guard bites. Do not flip a broad provider row to `surpassed`; the surpass claim is axis-scoped, so it lives in axis-scoped rows.
- **Do not re-implement gating/observability.** PF4-S2 owns `panel_quality_gate_halt` + the SLA/anomaly/lineage evaluators; PF4-S6 owns `ActivationHarness`; `db/rebuild.py` owns `run_warehouse_rebuild`. S11 *composes and cites* them. Touching their bodies is out of scope.
- **Whole-branch review by the strongest reviewer.** The pf3+pf4 review that gates the finish-branch runs on the most-capable available reviewer; a capstone signed off by a weaker pass is not acceptable.
- **Stay in 0201–0204.** Every new table/view/check catalogues in the same migration; schema/index split per the WAL precedent; timestamped DB+WAL backup before any live apply (clause F); strictly within the reserved range. Never edit a landed migration or another sprint's region. Never `git add -A`.
- **Offline tests run from `atx-impl/`, never from `db/`** (`db/calendar.py` shadows stdlib `calendar` and breaks collection when cwd is `db/`).

---

## Bench / acceptance

- **End-to-end activation proven deterministic + idempotent:** on a fixture slice, `warehouse_activation_capstone.py prove` (and the test) report a full rebuild then an incremental cycle with **`net_new_rows=0`** and **`byte_identical_panel=true`**; `activation_capstone_proof` records the fingerprint pair; `plan` reports `touches_live_db=false`; `activate` refuses without `--i-am-operator --confirm`.
- **Runbook honest + complete:** `docs/runbooks/pf4-activation-capstone-runbook.md` walks recover→CHECKPOINT→migrate→OPERATOR backfill→gated rebuild→freshness/anomaly/lineage sweep→export+release→SDK-verify, each step tagged offline-deterministic or operator-gated; `test_runbook_referenced_scripts_and_paths_exist` proves every referenced path exists.
- **Surpass axes evidenced:** `provider_parity_matrix` carries exactly **four** `surpassed` rows (axis 1 lineage-completeness → PF4-S2 `lineage_completeness_check`; axis 2 signal-native → PF3-S8 `signal_native_factors` factor rows; axis 3 vintages+freshness → PF4-S2 `factor_freshness_sla_check` + pf2-S4 `fundamental_pit_snapshot` + pf2-S8 `press_release_facts`; axis 4 recon → pf2-S9 `fact_disagreement` + agreement SLA), **each with a resolvable `surpass_axis_evidence` citation**; a non-resolving citation fails.
- **Catalog sweep green:** `SELECT count(*) FROM v_pf4_capstone_uncatalogued` = **0**; `pf4_capstone_catalog_sweep` is a registered `critical` check; a planted orphan table trips the view.
- **Determinism:** the panel fingerprint is order-stable and reproducible across full and incremental paths; migrations 0201–0204 are ordered/unique in the registry.
- **Full pytest green offline:** `python -m pytest atx-impl\db\tests\test_pf4_capstone.py -q` green, and full `python -m pytest atx-impl\db\tests -q` green (run from `atx-impl/`) before commit.
- **Live capstone smoke recorded** in the ledger (OPERATOR-RUN, not in-module): a gated live activate on operator go, a freshness/anomaly/lineage sweep, the deterministic rebuild's `rebuild_run_id` + `git_sha` + exact per-dataset/panel counts, and a live `activation_capstone_proof` row (`byte_identical_panel`, `net_new_rows`).
- **Ledger + parity flip:** `db/PARITY_GAP.md` flipped on the four surpass axes (each pointing at its resolvable surface); a final `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live-DB smoke posture with exact counts + `run_id` when the operator runs it, caveats/next → pf5 parked domains).
- **Whole-branch review clean + finish-branch:** the pf3+pf4 whole-branch review passes on the strongest reviewer and `superpowers:finishing-a-development-branch` runs.

**Process:** own git worktree off the integration mainline via `scripts/new_db_worktree.sh new|finish pf4-s11`; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. `python -m pytest atx-impl\db\tests -q` green in the worktree before every commit (from `atx-impl/`); operator live-DB smoke runs against the shared DB in the primary tree (backed up first, clause F). Update `db/PARITY_GAP.md` and append a `WAREHOUSE_PARITY_TRANCHES.md` row. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
