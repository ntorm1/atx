"""End-to-end DB viewer gate using isolated DuckDB files and alternate HTTP/gRPC ports.

Requires Python DuckDB 1.5.x solely to construct test fixtures. Every server request
uses the real HTTP gateway and native gRPC provider. The operator's data is never opened.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import socket
import tempfile
import time
from urllib.parse import urlencode


REPO = Path(__file__).resolve().parents[1]
BASE_HARNESS = runpy.run_path(str(REPO / "scripts/test-atx-workspace.py"))["Harness"]


def create_fixture(temporary: Path) -> dict:
    import duckdb

    if not duckdb.__version__.startswith("1.5."):
        raise RuntimeError(f"Fixture requires DuckDB 1.5.x, got {duckdb.__version__}")
    database = temporary / "fixture.duckdb"
    lake = temporary / "approved-lake"
    lake.mkdir()
    secret = temporary / "outside-allowlist.txt"
    secret.write_text("DO-NOT-EXPOSE-OUTSIDE-LAKE", encoding="utf-8")
    con = duckdb.connect(str(database), config={"threads": 1, "memory_limit": "128MB"})
    try:
        con.execute((REPO / "scripts/fixtures/atx-database.sql").read_text(encoding="utf-8"))
        parquet = (lake / "observations.parquet").as_posix().replace("'", "''")
        con.execute(f"COPY research.observations TO '{parquet}' (FORMAT PARQUET)")
        con.execute(f"CREATE VIEW research.lake_view AS SELECT * FROM read_parquet('{parquet}')")
        outside = secret.as_posix().replace("'", "''")
        con.execute(f"CREATE VIEW research.denied_view AS SELECT * FROM read_text('{outside}')")
        con.execute("CHECKPOINT")
    finally:
        con.close()
    locked = temporary / "writer-owned.duckdb"
    con = duckdb.connect(str(locked), config={"threads": 1, "memory_limit": "128MB"})
    try:
        con.execute("CREATE TABLE writer_evidence AS SELECT 7 AS value")
        con.execute("CHECKPOINT")
    finally:
        con.close()
    return {"database": database, "lake": lake, "secret": secret,
            "locked_database": locked,
            "sha256": hashlib.sha256(database.read_bytes()).hexdigest()}


class DatabaseHarness(BASE_HARNESS):
    def __init__(self, *args, fixture: dict, **kwargs):
        super().__init__(*args, **kwargs)
        self.fixture = fixture

    def start_native(self) -> None:
        self.native = self.spawn([
            str(self.server), "--listen", f"127.0.0.1:{self.grpc_port}",
            "--db", str(self.temporary / "workspace.db"),
            "--repo-root", str(self.temporary), "--artifact-root", str(self.temporary),
            "--data-source", f"fixture={self.fixture['database']}",
            "--data-source", f"writer-owned={self.fixture['locked_database']}",
            "--data-root", str(self.fixture["lake"]), "--workers", "2", "--capacity", "8",
        ] + self.tls_args, "native.log", self.repo)

    def query(self, sql: str, max_rows: int = 10, *, expected: int = 200) -> dict:
        return self.request("/api/data/query", {
            "source_id": "fixture", "sql": sql, "max_rows": max_rows,
        }, expected=expected)

    def preview(self, table: dict, **options) -> dict:
        parameters = {"source_id": "fixture", "catalog": table["catalog"],
                      "schema": table["schema"], "table": table["name"], **options}
        return self.request("/api/data/preview?" + urlencode(parameters))

    @staticmethod
    def texts(document: dict) -> list[list[str | None]]:
        return [[None if cell["is_null"] else cell["text"] for cell in row["cells"]]
                for row in document["rows"]]

    def verify_values(self, tables: dict[str, dict]) -> None:
        result = self.query("SELECT * FROM research.observations ORDER BY id DESC", 3)
        rows = self.texts(result)
        assert rows[0] == ["9223372036854775807", "12345678901234567890123456.123456789012",
                           "2022-01-03", "2022-01-03 22:00:00", 'A"B', None, ""], rows
        assert rows[2][0] == "-9223372036854775808", rows
        assert rows[2][1] == "-0.000000000001", rows
        assert rows[2][4:7] == ["NULL", "NULL", ""], rows
        assert "DECIMAL" in result["columns"][1]["type"], result
        assert not result["has_more"] and not result["truncated"], result
        quoted = self.preview(tables['quoted"table'], limit=1, offset=0)
        assert self.texts(quoted) == [["quoted identifier survives"]], quoted
        lake = self.preview(tables["lake_view"], limit=2, offset=0)
        assert len(lake["rows"]) == 2 and lake["has_more"], lake
        page = self.query("SELECT id FROM research.observations ORDER BY id", 2)
        assert self.texts(page) == [["-9223372036854775808"], ["0"]], page
        assert page["has_more"], page
        offset = self.preview(tables["observations"], limit=1, offset=2)
        assert len(offset["rows"]) == 1 and not offset["has_more"], offset
        assert self.texts(self.query("WITH q AS (SELECT 42 AS n) SELECT n FROM q")) == [["42"]]
        assert self.texts(self.query("SELECT ';not-a-statement' AS literal; -- terminator")) == \
            [[";not-a-statement"]]
        assert self.texts(self.query("SELECT 'A' || chr(0) || 'B' AS embedded_nul")) == [["A\0B"]]
        unicode = self.query("SELECT repeat(chr(128512), 5000) AS unicode_cell")
        unicode_cell = unicode["rows"][0]["cells"][0]
        assert unicode_cell["truncated"] and len(unicode_cell["text"].encode("utf-8")) <= 16384
        assert set(unicode_cell["text"]) == {chr(128512)}, unicode_cell
        large = self.query("SELECT repeat('x', 20000) AS large_cell")
        cell = large["rows"][0]["cells"][0]
        assert cell["truncated"] and len(cell["text"]) <= 16384, large

    def verify_rejections(self, fixture: dict, capabilities: dict, table: dict) -> None:
        self.request("/api/data/sources", auth=False, expected=401)
        self.request("/api/data/catalog?source_id=not-configured", expected=404)
        self.query("SELEC invalid", expected=400)
        self.query("SELECT 1\0; SELECT 2", expected=400)
        self.query("SELECT 1; SELECT 2", expected=400)
        self.query("SELECT 1; /* trailing */ DELETE FROM research.observations", expected=400)
        outside = fixture["secret"].as_posix().replace("'", "''")
        exported = (fixture["lake"] / "forbidden-export.csv").as_posix().replace("'", "''")
        blocked = [
            "DELETE FROM research.observations", "CREATE TABLE forbidden(i INTEGER)",
            "INSERT INTO research.observations(id) VALUES (1)",
            "UPDATE research.observations SET id=1", "SET enable_external_access=true",
            "EXPLAIN DELETE FROM research.observations",
            "PRAGMA enable_external_access=true", "INSTALL httpfs", "LOAD httpfs",
            f"COPY research.observations TO '{exported}'",
            f"ATTACH '{outside}' AS injected", f"SELECT * FROM read_text('{outside}')",
            "SELECT * FROM research.denied_view",
            "SELECT * FROM read_csv('http://127.0.0.1:1/denied.csv')",
        ]
        for sql in blocked:
            self.query(sql, expected=400)
        assert not Path(exported).exists(), "COPY escaped the read-only query gate"
        self.query("SELECT 1", int(capabilities["max_rows"]) + 1, expected=400)
        self.query("SELECT 1 --" + "x" * int(capabilities["max_query_bytes"]), expected=400)
        self.request("/api/data/preview?" + urlencode({
            "source_id": "fixture", "catalog": table["catalog"], "schema": table["schema"],
            "table": table["name"],
            "limit": 1, "offset": int(capabilities["max_offset"]) + 1,
        }), expected=400)
        self.request("/api/data/query", {"source_id": "fixture", "sql": "SELECT 1",
                                            "max_rows": -1}, expected=400)
        self.request("/api/data/query", {"source_id": "fixture", "sql": "SELECT 1",
                                            "max_rows": 1.5}, expected=400)

    def verify_writer_lock(self, fixture: dict) -> None:
        import duckdb

        writer = duckdb.connect(str(fixture["locked_database"]),
                                config={"threads": 1, "memory_limit": "128MB"})
        try:
            self.request("/api/data/query", {
                "source_id": "writer-owned", "sql": "SELECT value FROM writer_evidence",
                "max_rows": 1,
            }, expected=503)
            assert writer.execute("SELECT value FROM writer_evidence").fetchone() == (7,), \
                "The viewer disrupted the independent writer"
            # A busy source must not block another provider operation.
            assert self.texts(self.query("SELECT 9 AS value")) == [["9"]]
        finally:
            writer.close()
        available = self.request("/api/data/query", {
            "source_id": "writer-owned", "sql": "SELECT value FROM writer_evidence",
            "max_rows": 1,
        })
        assert self.texts(available) == [["7"]], available

    def verify_disconnect_cancellation(self) -> None:
        # Constant-memory CPU work remains inside the native five-second budget.
        # Closing both owned HTTP sockets must free the two provider slots sooner.
        payload = json.dumps({"source_id": "fixture", "max_rows": 1,
                              "sql": "SELECT sum(hash(i)) FROM range(1000000000) t(i)"}).encode()
        headers = (f"POST /api/data/query HTTP/1.1\r\nHost: 127.0.0.1:{self.http_port}\r\n"
                   f"Authorization: Bearer {self.token}\r\nContent-Type: application/json\r\n"
                   f"Content-Length: {len(payload)}\r\nConnection: close\r\n\r\n").encode()
        with ExitStack() as connections:
            sockets = [connections.enter_context(socket.create_connection(
                ("127.0.0.1", self.http_port), timeout=5)) for _ in range(2)]
            for connection in sockets:
                connection.sendall(headers + payload)
            time.sleep(0.2)
            for connection in sockets:
                connection.shutdown(socket.SHUT_RDWR)
        deadline = time.monotonic() + 2
        while True:
            try:
                assert self.texts(self.query("SELECT 123 AS value")) == [["123"]]
                break
            except AssertionError as error:
                # Only a still-occupied provider slot is eligible for brief retry.
                details = error.args[0] if error.args else None
                if not isinstance(details, tuple) or details[1] != 429:
                    raise
                if time.monotonic() >= deadline:
                    raise AssertionError("Disconnected requests retained native provider slots") from error
                time.sleep(0.05)

    def verify(self, fixture: dict) -> dict:
        sources = self.request("/api/data/sources")["sources"]
        source = next(item for item in sources if item["id"] == "fixture")
        assert source["provider"] == "duckdb" and source["read_only"], source
        capabilities = source["capabilities"]
        assert capabilities["browse"] and capabilities["query"], capabilities
        assert 0 < int(capabilities["max_rows"]) <= 1000, capabilities
        catalog = self.request("/api/data/catalog?source_id=fixture")
        assert not catalog["truncated"], catalog
        tables = {item["name"]: item for item in catalog["tables"]}
        assert {"observations", "observation_view", 'quoted"table', "lake_view"} <= tables.keys()
        self.verify_values(tables)
        self.verify_rejections(fixture, capabilities, tables["observations"])
        self.verify_writer_lock(fixture)
        self.verify_disconnect_cancellation()
        assert len(self.query("SELECT id FROM research.observations")["rows"]) == 3
        assert hashlib.sha256(fixture["database"].read_bytes()).hexdigest() == fixture["sha256"], \
            "The read-only viewer changed its input database"
        return {"status": "passed", "checks": [
            "real HTTP-to-gRPC DuckDB provider", "authenticated curated source IDs",
            "catalog and quoted identifiers", "BIGINT/DECIMAL precision and NULL distinction",
            "embedded NUL and UTF-8 truncation through HTTP/gRPC",
            "bounded pagination and has_more", "approved Parquet lake views",
            "single SELECT/CTE query restriction", "DML/DDL/extension/setting refusal",
            "external file and stored-view refusal", "row/query/offset limits",
            "large-cell truncation", "independent writer lock refusal and recovery",
            "HTTP disconnect cancellation frees native provider slots",
            "unchanged database SHA-256",
        ], "grpc_transport": "verified TLS" if self.tls else "loopback plaintext"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--tls", action="store_true")
    args = parser.parse_args()
    server = (args.server or REPO / "build-server/bin" /
              ("atx-server.exe" if os.name == "nt" else "atx-server")).resolve()
    if not server.is_file():
        parser.error(f"Build atx-server first: {server}")
    (REPO / ".cache").mkdir(exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="database-integration-", dir=REPO / ".cache"))
    success = False
    try:
        fixture = create_fixture(temporary)
        with ExitStack() as stack:
            harness = DatabaseHarness(REPO, server, temporary, stack, args.tls, fixture=fixture)
            harness.start()
            print(json.dumps(harness.verify(fixture), indent=2))
            success = True
    finally:
        (temporary / "tls-key.pem").unlink(missing_ok=True)
        if success and not args.keep:
            target, cache = temporary.resolve(strict=True), (REPO / ".cache").resolve(strict=True)
            if target.parent != cache or not target.name.startswith("database-integration-"):
                raise RuntimeError(f"Refusing cleanup outside test cache: {target}")
            shutil.rmtree(target)
        else:
            print(f"Database integration evidence: {temporary}")


if __name__ == "__main__":
    main()
