"""Bounded physical publication for the eight core fundamentals outputs."""

from __future__ import annotations

from collections.abc import Callable, Iterator, Mapping, Sequence
from contextlib import contextmanager, suppress
from dataclasses import dataclass
import hashlib
import json
import re
from pathlib import Path

from ._bulk_publication import _identifier, _validate_shadow_contract, publish_validated_shadows
from .connection import DuckDBStore


@dataclass(frozen=True)
class BucketedTable:
    """A complete physical output and its exact retained-row policy."""

    name: str
    order_by: tuple[str, ...]
    natural_key: tuple[str, ...]
    replace_where: str = "TRUE"
    fc1_date: str | None = None
    bucket_key: str = "security_id"


def _bucket_json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)


def _bucket_sha(value: object) -> str:
    return hashlib.sha256(_bucket_json(value).encode()).hexdigest()


def _bucket_fingerprint(conn, relation: str, params=()) -> list[int]:
    # Deliberately a multiset diagnostic, not a cryptographic content digest.
    row = conn.execute(
        f"SELECT count(*),coalesce(sum(hash(t)::HUGEINT),0),coalesce(bit_xor(hash(t)),0) "
        f"FROM {relation} t", params,
    ).fetchone()
    return [int(value) for value in row]


def _bucket_catalog(conn, name: str) -> dict:
    row = conn.execute(
        "SELECT sql,comment FROM duckdb_tables() WHERE database_name=current_database() "
        "AND schema_name=current_schema() AND table_name=? AND NOT temporary", [name],
    ).fetchone()
    if row is None:
        raise ValueError(f"missing publication input/output table: {name}")
    indexes = conn.execute(
        "SELECT index_name,sql FROM duckdb_indexes() WHERE database_name=current_database() "
        "AND schema_name=current_schema() AND table_name=? ORDER BY index_name", [name],
    ).fetchall()
    return {"ddl": row[0], "comment": row[1],
            "columns": [list(value) for value in conn.execute(f"DESCRIBE {_identifier(name)}").fetchall()],
            "indexes": [list(value) for value in indexes]}


def security_bucket_sql(bucket: int, buckets: int, column: str = "security_id") -> str:
    """The shared whole-security partition, including NULL when retaining rows."""
    if not 0 <= bucket < buckets or buckets < 1:
        raise ValueError("invalid security bucket")
    if not re.fullmatch(r"(?:[A-Za-z_][A-Za-z_0-9]*\.)?[A-Za-z_][A-Za-z_0-9]*", column):
        raise ValueError("invalid bucket column")
    return f"hash({column}) % {buckets} = {bucket}"


class BucketContext:
    """One transaction's measured writes, including every temporary row surface.

    Builders may only write through this context. Read queries and temporary
    views do not materialize rows. Every INSERT is counted before execution;
    defaults are projected explicitly so the receipt covers every live column.
    """

    MAX_WRITES = 2_000_000

    def __init__(self, conn, state: dict, bucket: int, buckets: int, tables: Mapping[str, BucketedTable]):
        self.conn, self.state = conn, state
        self.bucket, self.buckets, self.tables = bucket, buckets, tables
        self.writes = 1  # The runner's coupled build_batches INSERT.
        self.outputs: list[dict] = []
        self.temporary_rows = 0
        self.temporary_tables: list[str] = []
        self.temporary_views: list[str] = []

    def charge(self, rows: int) -> None:
        if rows < 0 or self.writes + rows > self.MAX_WRITES:
            raise ValueError("fundamentals aggregate transaction writes exceed 2M; use a finer frozen plan")
        self.writes += rows

    def materialize(self, name: str, select: str, params=()) -> int:
        name_sql = _identifier(name)
        rows = int(self.conn.execute(f"SELECT count(*) FROM ({select}) q", params).fetchone()[0])
        self.charge(rows)
        self.conn.execute(f"CREATE TEMP TABLE {name_sql} AS {select}", params)
        self.temporary_tables.append(name)
        self.temporary_rows += rows
        return rows

    def view(self, name: str, select: str) -> None:
        self.conn.execute(f"CREATE TEMP VIEW {_identifier(name)} AS {select}")
        self.temporary_views.append(name)

    def cleanup(self) -> None:
        for name in reversed(self.temporary_views):
            self.conn.execute(f"DROP VIEW {_identifier(name)}")
        for name in reversed(self.temporary_tables):
            self.conn.execute(f"DROP TABLE {_identifier(name)}")

    def temporary_executor(self, allowed_names: Sequence[str]):
        """Narrow adapter for reviewed pure CREATE-TEMP-AS query helpers.

        It accepts no other mutation. Each helper SELECT is counted before
        materialization and charged to this same batch transaction.
        """
        context = self
        allowed = set(allowed_names)

        class TemporaryConnection:
            def execute(self, sql, params=()):
                match = re.fullmatch(
                    r"\s*CREATE\s+(?:OR\s+REPLACE\s+)?TEMP\s+TABLE\s+([A-Za-z_][A-Za-z_0-9]*)\s+AS\s+(.+?)\s*;?\s*",
                    sql, re.IGNORECASE | re.DOTALL,
                )
                if not match or match[1] not in allowed:
                    raise ValueError("fiscal helper attempted an undeclared statement or temporary output")
                context.materialize(match[1], match[2], params)
                return context.conn

        return TemporaryConnection()

    def insert(self, sql: str, params=()) -> int:
        match = re.fullmatch(
            r'\s*INSERT\s+INTO\s+("?[A-Za-z_][A-Za-z_0-9]*"?)\s*\(([^()]*)\)\s*(.+?)\s*;?\s*',
            sql, re.IGNORECASE | re.DOTALL,
        )
        if not match:
            raise ValueError("bucket builder must emit one INSERT with an explicit column list")
        target, columns_text, select = match.groups()
        target = target.strip('"')
        table = target.removesuffix("__next")
        if target != table + "__next" or table not in self.tables:
            raise ValueError("bucket builder attempted to mutate a non-shadow table")
        columns = [value.strip().strip('"') for value in columns_text.split(",")]
        if len(set(columns)) != len(columns):
            raise ValueError("duplicate bucket INSERT columns")
        for column in columns:
            _identifier(column)
        catalog = self.state["catalogs"][table]
        all_columns = [row[0] for row in catalog["columns"]]
        if set(columns) - set(all_columns):
            raise ValueError("bucket output has columns outside the frozen catalog")
        expressions = []
        for name, dtype, nullable, _, default, _ in catalog["columns"]:
            if name in columns:
                expression = f"incoming.{_identifier(name)}"
            elif default is not None and re.fullmatch(r"\(?now\(\)\)?", default, re.IGNORECASE):
                clock = self.state["operational_clocks"].get(table, self.state["created_at"])
                expression = "TIMESTAMP '" + str(clock).replace("'", "''") + "'"
            elif default is not None:
                expression = str(default)
            elif nullable == "YES":
                expression = f"CAST(NULL AS {dtype})"
            else:
                raise ValueError(f"missing required bucket column: {table}.{name}")
            expressions.append(expression + " AS " + _identifier(name))
        names = ",".join(_identifier(name) for name in columns)
        projection = f"SELECT {','.join(expressions)} FROM ({select}) incoming({names})"
        expected = _bucket_fingerprint(self.conn, f"({projection})", params)
        self.charge(expected[0])
        definition = self.tables[table]
        key = ",".join(_identifier(name) for name in definition.natural_key)
        nulls = " OR ".join(f"{_identifier(name)} IS NULL" for name in definition.natural_key)
        if self.conn.execute(
            f"SELECT 1 FROM ({projection}) q GROUP BY {key} HAVING count(*)>1 OR {nulls} LIMIT 1", params,
        ).fetchone():
            raise ValueError(f"invalid natural key in fundamentals bucket: {table}")
        order = ",".join(_identifier(name) for name in definition.order_by)
        count = self.conn.execute(
            f"INSERT INTO {_identifier(target)} SELECT * FROM ({projection}) ordered" +
            (f" ORDER BY {order}" if order else ""), params,
        ).fetchone()[0]
        if int(count) != expected[0]:
            raise ValueError("fundamentals bucket INSERT count differs from measured query")
        self.outputs.append({"table": table, "fingerprint": expected})
        return int(count)


def bucketed_publication_stage(
    *, stage_name: str, tables: Sequence[BucketedTable], bucket_count: int,
    build_bucket_sql: Callable[[int, int], Sequence[str]] | None = None,
    partition_keys_asserted: Sequence[str] = ("security_id",),
    carry_over_sql: Mapping[str, str] | None = None,
    memory_limit: str = "384MB", threads: int = 1,
    sources: Sequence[str] = (), bucket_sources: Sequence[str] = (),
    initial_phases: Sequence[tuple[str, Callable[[BucketContext], None]]] = (),
    phases: Sequence[tuple[str, Callable[[BucketContext], None]]] = (),
    final_phases: Sequence[tuple[str, Callable[[BucketContext], None]]] = (),
    code_dependencies: Sequence[str] = (), max_source_rows: int = 50_000,
):
    """Build complete INSERT-only shadows in independently committed buckets.

    Phase order is part of the frozen plan. In particular, ALL revision buckets
    precede the reported-EPS ownership reduction used by statement buckets.
    ``carry_over_sql`` supplies SELECTs for exact rows outside replacement scope;
    each SELECT must expose the complete physical live columns and security_id.
    Auxiliary builders charge temporary/output rows through BucketContext.
    """
    from .batch_runner import BatchResult, BatchSpec, BatchStage, mark_published, run_record, store_for

    if tuple(partition_keys_asserted) != ("security_id",) or threads != 1:
        raise ValueError("fundamentals stages require whole-security partitions and one thread")
    definitions = {table.name: table for table in tables}
    if not definitions or len(definitions) != len(tables) or bucket_count < 1 or max_source_rows < 1:
        raise ValueError("invalid fundamentals stage definition")
    for table in tables:
        for name in (table.name, *table.order_by, *table.natural_key):
            _identifier(name)
        for name in table.bucket_key.split(","):
            _identifier(name.strip())
        if not table.natural_key:
            raise ValueError("fundamentals outputs need explicit natural keys")
    inputs = tuple(sorted(set(sources) | set(definitions)))
    state_table = _identifier(f"_{stage_name}_state")
    carry = dict(carry_over_sql or {})
    if set(carry) - set(definitions):
        raise ValueError("unknown retained fundamentals output")
    callbacks = list(initial_phases) + list(phases)
    if build_bucket_sql is not None:
        def build_default(context):
            for sql in build_bucket_sql(context.bucket, context.buckets):
                context.insert(sql)
        callbacks.append(("build", build_default))
    callbacks += list(final_phases)
    callback_map = dict(callbacks)
    if len(callback_map) != len(callbacks) or not callbacks:
        raise ValueError("fundamentals phase names must be nonempty and unique")

    def request(conn, run_key):
        record = run_record(conn, stage_name, run_key)
        if record is None:
            raise ValueError("fundamentals stage requires an open ledger")
        return record["spec"]["spec"]

    def state_for(conn, run_key):
        rows = conn.execute(f"SELECT state_json FROM {state_table} WHERE run_key=?", [run_key]).fetchall()
        record = run_record(conn, stage_name, run_key)
        if len(rows) != 1:
            raise ValueError("fundamentals frozen state missing or duplicated")
        state = json.loads(rows[0][0])
        if _bucket_sha(state) != record["spec"]["plan"][0]["payload"]["state_sha256"]:
            raise ValueError("fundamentals frozen state differs from ledger")
        return state

    def plan(conn, run_key):
        spec = request(conn, run_key)
        clocks = spec.get("diagnostic_operational_clocks", {})
        diagnostic_options = clocks or spec.get("diagnostic_calculation_date") or spec.get("diagnostic_coverage_loaded_at")
        if diagnostic_options and spec.get("purpose") != "diagnostic_scratch":
            raise ValueError("operational clock overrides require a diagnostic scratch contract")
        if spec.get("purpose") == "diagnostic_scratch":
            path = conn.execute("SELECT path FROM duckdb_databases() WHERE database_name=current_database()").fetchone()[0]
            root = Path("C:/atx/atx-db/data/staging/fundamentals").resolve()
            if not path or not Path(path).resolve().is_relative_to(root):
                raise ValueError("diagnostic fundamentals stages require their isolated scratch directory")
        if set(clocks) - set(definitions) or any(not isinstance(value, str) for value in clocks.values()):
            raise ValueError("invalid per-table operational clock map")
        catalogs = {name: _bucket_catalog(conn, name) for name in inputs}
        fingerprints = {name: _bucket_fingerprint(conn, _identifier(name)) for name in inputs}
        buckets = bucket_count
        while True:
            largest = 0
            for name in bucket_sources:
                _identifier(name)
                row = conn.execute(
                    f"SELECT coalesce(max(n),0) FROM (SELECT count(*) n FROM {_identifier(name)} "
                    f"WHERE security_id IS NOT NULL GROUP BY hash(security_id)%{buckets}) q"
                ).fetchone()
                largest = max(largest, int(row[0]))
            for name, select in carry.items():
                row = conn.execute(
                    f"SELECT coalesce(max(n),0) FROM (SELECT count(*) n FROM ({select}) retained "
                    f"GROUP BY hash({definitions[name].bucket_key})%{buckets}) q"
                ).fetchone()
                largest = max(largest, int(row[0]))
            if largest <= max_source_rows:
                break
            if buckets >= 65_536:
                raise ValueError("a whole-security source exceeds the bounded plan; no issuer splitting")
            buckets *= 2
        clock = conn.execute("SELECT created_at FROM build_runs WHERE stage=? AND run_key=?",
                             [stage_name, run_key]).fetchone()[0]
        calculation_date = spec.get("diagnostic_calculation_date", str(clock.date()))
        import datetime as dt
        calculation_date = str(dt.date.fromisoformat(calculation_date))
        state = {"catalogs": catalogs, "source_fingerprints": fingerprints, "buckets": buckets,
                 "created_at": str(clock), "operational_clocks": clocks,
                 "calculation_date": calculation_date,
                 "request": spec, "run_key": run_key,
                 "fingerprint_basis": "noncryptographic all-column multiset count/SUM/XOR",
                 "max_source_rows": max_source_rows, "largest_source_bucket": largest}
        conn.execute(f"CREATE TABLE IF NOT EXISTS {state_table}(run_key VARCHAR NOT NULL,state_json VARCHAR NOT NULL)")
        if conn.execute(f"SELECT 1 FROM {state_table} WHERE run_key=?", [run_key]).fetchone():
            raise ValueError("fundamentals state already has an owner")
        conn.execute(f"INSERT INTO {state_table} VALUES (?,?)", [run_key, _bucket_json(state)])
        payloads = []
        for name in carry:
            for bucket in range(buckets):
                payloads.append({"kind": "retained", "table": name, "bucket": bucket})
        final_names = {name for name, _ in (*initial_phases, *final_phases)}
        for name, _ in callbacks:
            for bucket in range(1 if name in final_names else buckets):
                payloads.append({"kind": "phase", "phase": name, "bucket": bucket})
        return [BatchSpec(index, input_sha256=_bucket_sha(payload),
                          payload={**payload, "state_sha256": _bucket_sha(state)})
                for index, payload in enumerate(payloads)]

    def prepare(conn, run_key):
        state = json.loads(conn.execute(f"SELECT state_json FROM {state_table} WHERE run_key=?", [run_key]).fetchone()[0])
        for name in definitions:
            target = name + "__next"
            if conn.execute("SELECT 1 FROM information_schema.tables WHERE table_name=?", [target]).fetchone():
                raise ValueError(f"fundamentals shadow already has an owner: {target}")
            ddl = state["catalogs"][name]["ddl"]
            conn.execute(f"CREATE TABLE {_identifier(target)} " + ddl[ddl.index("("):])

    def build(conn, batch, run_key):
        state = state_for(conn, run_key)
        payload = {key: value for key, value in batch.payload.items() if key != "state_sha256"}
        if batch.input_sha256 != _bucket_sha(payload):
            raise ValueError("fundamentals batch payload differs")
        for name, catalog in state["catalogs"].items():
            if _bucket_catalog(conn, name) != catalog:
                raise ValueError(f"fundamentals catalog drift: {name}")
        context = BucketContext(conn, state, int(payload["bucket"]), state["buckets"], definitions)
        try:
            if payload["kind"] == "retained":
                name = payload["table"]
                columns = ",".join(_identifier(row[0]) for row in state["catalogs"][name]["columns"])
                context.insert(f"INSERT INTO {_identifier(name+'__next')} ({columns}) "
                               f"SELECT * FROM ({carry[name]}) retained WHERE " +
                               f"hash({definitions[name].bucket_key}) % {context.buckets} = {context.bucket}")
            else:
                callback_map[payload["phase"]](context)
        except BaseException:
            # Runner rolls back the entire transaction; preserve the original
            # failure even if a DuckDB error invalidated temporary cleanup.
            with suppress(Exception):
                context.cleanup()
            raise
        context.cleanup()
        note = {"outputs": context.outputs, "aggregate_writes": context.writes,
                "temporary_rows": context.temporary_rows, "payload_sha256": batch.input_sha256}
        return BatchResult(0, sum(item["fingerprint"][0] for item in context.outputs), _bucket_json(note))

    def finalize(conn, run_key):
        state = state_for(conn, run_key)
        record = run_record(conn, stage_name, run_key)
        plans = record["spec"]["plan"]
        ledger = conn.execute("SELECT batch_id,rows_out,note FROM build_batches WHERE stage=? AND run_key=? ORDER BY batch_id",
                              [stage_name, run_key]).fetchall()
        if [row[0] for row in ledger] != list(range(len(plans))):
            raise ValueError("fundamentals ledger is incomplete or duplicated")
        totals = {name: [0, 0, 0] for name in definitions}
        for (_, rows_out, raw), planned in zip(ledger, plans, strict=True):
            note = json.loads(raw)
            if note["payload_sha256"] != planned["input_sha256"] or note["aggregate_writes"] > BucketContext.MAX_WRITES:
                raise ValueError("fundamentals batch proof differs")
            if rows_out != sum(item["fingerprint"][0] for item in note["outputs"]):
                raise ValueError("fundamentals ledger output count differs")
            for item in note["outputs"]:
                current, values = totals[item["table"]], item["fingerprint"]
                current[0] += values[0]
                current[1] += values[1]
                current[2] ^= values[2]
        for name in inputs:
            if (_bucket_catalog(conn, name) != state["catalogs"][name] or
                    _bucket_fingerprint(conn, _identifier(name)) != state["source_fingerprints"][name]):
                raise ValueError(f"fundamentals frozen live/source state changed: {name}")
        index_writes = 1  # Coupled published run metadata.
        for name, definition in definitions.items():
            target = _identifier(name + "__next")
            if _bucket_fingerprint(conn, target) != totals[name]:
                raise ValueError(f"fundamentals staged all-column proof differs: {name}")
            keys = ",".join(_identifier(key) for key in definition.natural_key)
            nulls = " OR ".join(f"{_identifier(key)} IS NULL" for key in definition.natural_key)
            if conn.execute(f"SELECT 1 FROM {target} GROUP BY {keys} HAVING count(*)>1 OR {nulls} LIMIT 1").fetchone():
                raise ValueError(f"fundamentals global natural-key validation failed: {name}")
            columns = {row[0] for row in state["catalogs"][name]["columns"]}
            filed = definition.fc1_date or ("filed_date" if "filed_date" in columns else None)
            if filed is not None and filed not in columns:
                raise ValueError(f"missing declared FC1 basis: {name}.{filed}")
            if filed and "available_at" in columns and conn.execute(
                f"SELECT 1 FROM {target} WHERE isfinite({filed}) AND available_at < "
                f"CAST({filed} AS TIMESTAMP)+INTERVAL 46 HOUR LIMIT 1"
            ).fetchone():
                raise ValueError(f"fundamentals FC1 floor failed: {name}")
            index_writes += totals[name][0] * len(state["catalogs"][name]["indexes"])
        if index_writes > BucketContext.MAX_WRITES:
            raise ValueError("exact index recreation exceeds final aggregate write bound")
        receipt = {"stage": stage_name, "run_key": run_key, "buckets": state["buckets"],
                   "outputs": totals, "fingerprint_basis": state["fingerprint_basis"],
                   "final_index_and_metadata_writes": index_writes,
                   "max_aggregate_batch_writes": max(json.loads(row[2])["aggregate_writes"] for row in ledger)}
        store = store_for(conn)
        with store.transaction():
            for name in definitions:
                _validate_shadow_contract(store, live_table=name, shadow_table=name + "__next")
            for name in definitions:
                conn.execute(f"DROP TABLE {_identifier(name)}")
                conn.execute(f"ALTER TABLE {_identifier(name+'__next')} RENAME TO {_identifier(name)}")
                comment = state["catalogs"][name]["comment"]
                value = "NULL" if comment is None else "'" + comment.replace("'", "''") + "'"
                conn.execute(f"COMMENT ON TABLE {_identifier(name)} IS {value}")
                for _, sql in state["catalogs"][name]["indexes"]:
                    conn.execute(sql)
                if _bucket_catalog(conn, name) != state["catalogs"][name]:
                    raise ValueError(f"fundamentals published catalog differs: {name}")
            mark_published(conn, stage_name, run_key, receipt)
        return receipt

    def discard(conn, run_key):
        request(conn, run_key)
        for name in definitions:
            conn.execute(f"DROP TABLE IF EXISTS {_identifier(name+'__next')}")

    return BatchStage(stage_name, plan, prepare, build, finalize, memory_limit=memory_limit,
                      threads=threads, discard=discard, code_dependencies=tuple(code_dependencies))

_KEYS = {
    "fundamental_fact_revisions": "fact_revision_id",
    "fundamental_statement_points": "statement_point_id",
    "fundamental_periods": "fundamental_period_id",
    "fundamental_ttm_points": "ttm_point_id",
    "fundamental_calendar_map": "calendar_map_id",
    "fundamental_calendar_ttm": "calendar_ttm_id",
    "fundamental_standardized": "standardized_id",
    "fundamental_standardization_exception": "exception_id",
}
_BATCH_ROWS = 50_000
_REOPEN_BATCHES = 4
_PREFIX_BOUNDARIES = tuple(f"{prefix:02x}" for prefix in range(1, 256))
_OWNER = "atx_db._fundamental_publication:v1"


def check_publication_session(
    store: DuckDBStore, *, owned_registrations: tuple[str, ...] = ()
) -> None:
    """Fail before mutation if a persistent caller would lose temporary state."""
    if (
        str(store.path).startswith(":memory:")
        or not store.path.is_file()
        or store.analytical_memory_limit is None
        or store.analytical_threads is None
    ):
        return
    temporary = store.con.execute("""
        SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal
        UNION ALL
        SELECT view_name FROM duckdb_views() WHERE temporary AND NOT internal
    """).fetchall()
    unrelated = sorted({str(row[0]) for row in temporary} - set(owned_registrations))
    if unrelated:
        raise RuntimeError(
            "fundamentals publication requires a persistent session without caller "
            f"temporary relations; retained unchanged: {unrelated}"
        )


def _recycle(store: DuckDBStore) -> None:
    if (
        str(store.path).startswith(":memory:")
        or not store.path.is_file()
        or store.analytical_memory_limit is None
        or store.analytical_threads is None
    ):
        return
    check_publication_session(store)
    store.close()
    store.reopen()


def _assert_owned_or_absent(store: DuckDBStore, name: str) -> None:
    rows = store.con.execute("""
        SELECT comment, temporary FROM duckdb_tables()
        WHERE table_name = ? AND (
            (database_name = current_database() AND schema_name = current_schema())
            OR temporary
        )
    """, [name]).fetchall()
    if any(row[0] != _OWNER or row[1] for row in rows):
        raise RuntimeError(f"fundamentals publication reserved table is not owned: {name}")
    # A view collision must also refuse before any stale-stage cleanup.
    if store.con.execute("""
        SELECT 1 FROM duckdb_views()
        WHERE view_name = ? AND (
            (database_name = current_database() AND schema_name = current_schema())
            OR temporary
        )
    """, [name]).fetchone() is not None:
        raise RuntimeError(f"fundamentals publication reserved name is a view: {name}")


def _mark_owned(store: DuckDBStore, name: str) -> None:
    store.con.execute(f"COMMENT ON TABLE {_identifier(name)} IS '{_OWNER}'")


def _build_shadow(store: DuckDBStore, table: str) -> None:
    key = _KEYS[table]
    stage, ordered, shadow = (f"{table}_bulk_{suffix}" for suffix in ("stage", "ordered", "next"))
    # Validate before keyset pagination: duplicate boundary keys must neither
    # exceed a batch bound nor cause a later page to silently skip rows.
    if store.con.execute(f"""
        SELECT {key} FROM {stage} GROUP BY {key}
        HAVING {key} IS NULL OR count(*) <> 1 LIMIT 1
    """).fetchone() is not None:
        raise RuntimeError(f"fundamentals publication stage has invalid keys: {table}")
    expected_row = store.con.execute(f"SELECT count(*) FROM {stage}").fetchone()
    assert expected_row is not None
    expected = int(expected_row[0])
    # One spillable sort makes subsequent ordered ranges prune disk row groups.
    with store.transaction():
        store.con.execute(f"CREATE TABLE {ordered} AS SELECT * FROM {stage} ORDER BY {key}")
        _mark_owned(store, ordered)
        store.con.execute(f"DROP TABLE {stage}")
        store.con.execute(f"ALTER TABLE {ordered} RENAME TO {stage}")
    ddl_row = store.con.execute("""
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = ?
    """, [table]).fetchone()
    if ddl_row is None or not ddl_row[0]:
        raise RuntimeError(f"missing physical fundamentals table: {table}")
    ddl = str(ddl_row[0])
    # DuckDB's catalog DDL retains column order, defaults, NOT NULL, CHECK,
    # PRIMARY KEY and UNIQUE constraints, including migrated appended columns.
    with store.transaction():
        store.con.execute(f"CREATE TABLE {shadow} " + ddl[ddl.index("("):])
        _mark_owned(store, shadow)
    _validate_shadow_contract(store, live_table=table, shadow_table=shadow)
    _recycle(store)
    batches = 0
    copied = 0
    prefix_lower: str | None = None
    # Prefix upper bounds prevent every page from scanning the whole future
    # suffix. Open first/last intervals also retain IDs outside the SHA domain.
    for prefix_upper in (*_PREFIX_BOUNDARIES, None):
        lower: str | None = None
        while True:
            clauses: list[str] = []
            params: list[str] = []
            if lower is not None:
                clauses.append(f"{key} > ?")
                params.append(lower)
            elif prefix_lower is not None:
                clauses.append(f"{key} >= ?")
                params.append(prefix_lower)
            if prefix_upper is not None:
                clauses.append(f"{key} < ?")
                params.append(prefix_upper)
            predicate = " AND ".join(clauses) or "TRUE"
            boundary = store.con.execute(f"""
                SELECT max({key}), count(*) FROM (
                    SELECT {key} FROM {stage} WHERE {predicate}
                    ORDER BY {key} LIMIT ?
                ) batch
            """, [*params, _BATCH_ROWS]).fetchone()
            assert boundary is not None
            if boundary[0] is None:
                break
            upper = str(boundary[0])
            store.con.execute(f"""
                INSERT INTO {shadow}
                SELECT * FROM {stage} WHERE {predicate} AND {key} <= ? ORDER BY {key}
            """, [*params, upper])
            copied += int(boundary[1])
            lower = upper
            batches += 1
            if batches % _REOPEN_BATCHES == 0:
                _recycle(store)
        prefix_lower = prefix_upper
    actual_row = store.con.execute(f"SELECT count(*) FROM {shadow}").fetchone()
    if actual_row is None or int(actual_row[0]) != expected or copied != expected:
        raise RuntimeError(f"fundamentals shadow row count differs from complete stage: {table}")
    _validate_shadow_contract(store, live_table=table, shadow_table=shadow)
    _recycle(store)


@contextmanager
def fundamental_publication(
    store: DuckDBStore,
    tables: tuple[str, ...],
    *,
    replace_where: str = "TRUE",
    replace_params: Sequence[object] = (),
    owned_registrations: tuple[str, ...] = (),
    before_build: Callable[[], None] | None = None,
    before_swap: Callable[[], None] | None = None,
) -> Iterator[None]:
    """Fill unindexed persistent stages, then publish complete constrained tables.

    Callers insert into ``<table>_bulk_stage``. Retained rows are copied with
    their original physical values. Only this helper's marked artifacts are
    discarded on retry/failure; live tables change solely in the final swap.
    """
    if not tables or len(set(tables)) != len(tables) or any(table not in _KEYS for table in tables):
        raise ValueError("unsupported or duplicate fundamentals publication table")
    check_publication_session(store, owned_registrations=owned_registrations)
    artifacts = tuple(f"{table}_bulk_{suffix}" for table in tables for suffix in ("stage", "ordered", "next"))
    for name in artifacts:
        _assert_owned_or_absent(store, name)
    for table in tables:
        if store.con.execute("""
            SELECT 1 FROM information_schema.tables
            WHERE table_name = ?
        """, [f"{table}_bulk_previous"]).fetchone() is not None:
            raise RuntimeError(f"fundamentals publication previous-table name already exists: {table}")
        if store.con.execute("""
            SELECT index_name FROM duckdb_indexes()
            WHERE database_name = current_database() AND schema_name = current_schema()
              AND table_name = ?
        """, [table]).fetchone() is not None:
            raise RuntimeError(f"fundamentals publication requires governed index migration 0318: {table}")
    work_started = False
    try:
        with store.transaction():
            work_started = True
            for name in artifacts:
                store.con.execute(f"DROP TABLE IF EXISTS {name}")
            for table in tables:
                declarations = []
                for column in store.con.execute(f"DESCRIBE {table}").fetchall():
                    name, data_type, nullable, _, default, _ = column
                    declaration = f"{_identifier(str(name))} {data_type}"
                    if default is not None:
                        declaration += f" DEFAULT {default}"
                    if nullable == "NO":
                        declaration += " NOT NULL"
                    declarations.append(declaration)
                stage = f"{table}_bulk_stage"
                store.con.execute(f"CREATE TABLE {stage} ({', '.join(declarations)})")
                _mark_owned(store, stage)
                store.con.execute(
                    f"INSERT INTO {stage} SELECT * FROM {table} WHERE ({replace_where}) IS NOT TRUE",
                    list(replace_params),
                )
            yield
        if before_build is not None:
            before_build()
        for relation in owned_registrations:
            with suppress(Exception):
                store.con.unregister(relation)
        check_publication_session(store)
        for table in tables:
            _build_shadow(store, table)

        def complete_publication() -> None:
            # Ownership comments belong to private artifacts, not public tables.
            # Restore the public description within the same atomic transaction.
            for table in tables:
                row = store.con.execute("""
                    SELECT comment FROM duckdb_tables()
                    WHERE database_name = current_database() AND schema_name = current_schema()
                      AND table_name = ?
                """, [table]).fetchone()
                assert row is not None
                comment = "NULL" if row[0] is None else "'" + str(row[0]).replace("'", "''") + "'"
                store.con.execute(f"COMMENT ON TABLE {table}_bulk_next IS {comment}")
            if before_swap is not None:
                before_swap()

        publish_validated_shadows(
            store,
            tables=tuple((table, f"{table}_bulk_next") for table in tables),
            before_swap=complete_publication,
        )
    except BaseException:
        if work_started:
            with suppress(Exception):
                store.con.execute("ROLLBACK")
        raise
    finally:
        # An invalidated DB may reject cleanup. Preserve the original error;
        # marked leftovers are safe for a subsequent explicit retry to remove.
        for name in (artifacts if work_started else ()):
            with suppress(Exception):
                _assert_owned_or_absent(store, name)
                store.con.execute(f"DROP TABLE IF EXISTS {name}")
