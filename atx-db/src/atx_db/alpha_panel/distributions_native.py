"""Bounded original-byte replay with independently supplied admission identities.

Admission is an external expectation, not a self-generated parse receipt. Hash
matching proves byte identity; it never promotes class/clock assertions to source truth.
"""
from __future__ import annotations
import gzip
import hashlib
import json
from pathlib import Path
from typing import Any
from . import common as C

LIMIT = 2 * 1024 * 1024
MODULES = ("distributions_native", "distributions", "distributions_qualification", "distributions_ancillary", "events_buyback", "events_sources", "events_common", "guidance", "common")


def code_pins() -> dict[str, str]:
    from . import distributions_qualification as Q
    Q.assert_runtime()
    import lxml.html
    import lxml.etree
    import pyarrow
    import pyarrow.lib
    import pyarrow._parquet
    import duckdb
    import _duckdb
    paths = [Path(__file__).parent / f"{name}.py" for name in MODULES]
    paths += [Path(module.__file__) for module in (lxml.html, lxml.etree, pyarrow, pyarrow.lib, pyarrow._parquet, duckdb, _duckdb)]
    from . import distributions_qualification as Q
    paths += Q.timezone_paths()
    return {**{str(path): C.sha256_file(path) for path in paths}, **Q.loaded_code_pins()}


def recheck(pins: dict[str, str]) -> None:
    from . import distributions_qualification as Q
    Q.assert_runtime()
    loaded = Q.loaded_code_pins()
    for path, digest in pins.items():
        actual = loaded.get(path) if path.startswith("runtime:") else C.sha256_file(Path(path))
        if actual != digest:
            raise ValueError(f"admitted input/code changed: {path}")


def admission(path: Path, expected_sha256: str) -> tuple[dict[str, Any], dict[str, str]]:
    if not expected_sha256 or C.sha256_file(path) != expected_sha256:
        raise ValueError("independent admission SHA mismatch")
    record = json.loads(path.read_text(encoding="utf-8"))
    if set(record) != {"schema", "inputs", "code_sha256", "population"} or record["schema"] not in {"atx.native-admission/v1", "atx.native-admission/v2"}:
        raise ValueError("abbreviated or unknown admission schema")
    required = {"catalog", "source_catalog", "calendar", "class", "clock", "fundamentals", "identity"}
    if record["schema"] == "atx.native-admission/v2":
        required.add("prices")
    if set(record["inputs"]) != required:
        raise ValueError("complete independent input roles required")
    pins = {str(path.resolve()): expected_sha256}
    for item in record["inputs"].values():
        if set(item) != {"path", "sha256"}:
            raise ValueError("exact path/hash expected for every admitted input")
        pins[str(Path(item["path"]).resolve())] = item["sha256"]
    actual = code_pins()
    if record["code_sha256"] != actual:
        raise ValueError("independently expected complete reader/parser code mismatch")
    pins.update(actual)
    recheck(pins)
    for role, filename in (("fundamentals", "events.parquet"), ("identity", "link_table.parquet")):
        manifest_path = Path(record["inputs"][role]["path"])
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        files = manifest.get("files", {})
        if filename not in files or not isinstance(files[filename], dict) or "sha256" not in files[filename]:
            raise ValueError("independently admitted fundamental/identity file lineage incomplete")
        pins[str((manifest_path.parent / filename).resolve())] = files[filename]["sha256"]
    recheck(pins)
    if set(record["population"]) != {"catalog_rows", "selection", "eligible_source_rows", "partition_index", "partition_count"} or record["population"]["selection"] != "all_eligible_source_rows_hash_partition":
        raise ValueError("full admitted catalog population required")
    return record, pins


def native_document(doc: dict[str, Any], pins: dict[str, str]) -> tuple[bytes, str]:
    receipt = doc.get("receipt")
    if not isinstance(receipt, dict) or set(receipt) != {"path", "sha256", "line", "line_sha256"}:
        raise ValueError("independently admitted original source receipt required")
    ledger = Path(receipt["path"])
    if ledger.stat().st_size > 32 * 1024 * 1024 or C.sha256_file(ledger) != receipt["sha256"]:
        raise ValueError("source receipt ledger bound/hash mismatch")
    pins[str(ledger.resolve())] = receipt["sha256"]
    if not isinstance(receipt["line"], int) or not 1 <= receipt["line"] <= 1000000:
        raise ValueError("bounded positive receipt line required")
    with ledger.open("rb") as stream:
        for _ in range(receipt["line"]):
            raw = stream.readline(65537)
            if not raw or len(raw) > 65536:
                raise ValueError("receipt line missing/oversize")
    if hashlib.sha256(raw).hexdigest() != receipt["line_sha256"]:
        raise ValueError("independent native receipt line mismatch")
    source_receipt = json.loads(raw)
    if (source_receipt.get("document_sha256") or source_receipt.get("sha256")) != doc["source_sha256"]:
        raise ValueError("receipt original byte identity mismatch")
    if source_receipt.get("url"):
        if source_receipt["url"] != doc["source_url"] or source_receipt.get("status") != 200:
            raise ValueError("native receipt URI/status mismatch")
    elif source_receipt.get("accession") != doc["accession"] or source_receipt.get("exhibit") != doc["source_doc"] or source_receipt.get("outcome") != "document_ok":
        raise ValueError("native source accession/exhibit receipt mismatch")
    actual_floor = source_receipt.get("fetched_at") or source_receipt.get("done_at")
    if not actual_floor or doc["fetched_at"] != actual_floor:
        raise ValueError("conservative clock must match native receipt")
    path = Path(doc["source_path"])
    if path.stat().st_size > LIMIT or C.sha256_file(path) != doc["stored_sha256"]:
        raise ValueError("native stored bytes exceed bound or hash mismatch")
    pins[str(path.resolve())] = doc["stored_sha256"]
    if doc["storage_encoding"] == "gzip":
        with gzip.open(path, "rb") as stream:
            blob = stream.read(LIMIT + 1)
    elif doc["storage_encoding"] == "plain":
        blob = path.read_bytes()
    else:
        raise ValueError("unknown native storage encoding")
    digest = hashlib.sha256(blob).hexdigest()
    if len(blob) > LIMIT or digest != doc["source_sha256"]:
        raise ValueError("native original-byte bound/hash mismatch")
    from .events_sources import document_text
    text = document_text(blob, doc["source_doc"])
    return blob, text


def replay_inputs(path: Path, expected_sha256: str) -> tuple[list[tuple[dict[str, Any], bytes, str]], dict[str, str], dict[str, Any]]:
    record, pins = admission(path, expected_sha256)
    catalog = Path(record["inputs"]["catalog"]["path"])
    output = []
    total = 0
    seen = set()
    import pyarrow.parquet as pq
    import datetime as dt
    population = record["population"]
    count, index = population["partition_count"], population["partition_index"]
    if not isinstance(count, int) or not isinstance(index, int) or not 0 <= index < count <= 100000:
        raise ValueError("invalid independent native population partition")
    expected_keys = set()
    all_keys = set()
    parent = Path(record["inputs"]["source_catalog"]["path"])
    if parent.stat().st_size > 8 * 1024 * 1024:
        raise ValueError("bounded parent source metadata file exceeds 8MiB")
    metadata_rows = 0
    for batch in pq.ParquetFile(parent).iter_batches(batch_size=256):
        for row in batch.to_pylist():
            metadata_rows += 1
            if metadata_rows > 100000:
                raise ValueError("parent source metadata exceeds100000rows")
            if row.get("form") not in {"8-K", "8-K/A", "10-Q", "10-Q/A"} or not row.get("filing_date") or row["filing_date"] < dt.date(2020, 1, 1):
                continue
            key = (row["accession"], row["doc_sha256"], row["source_doc"])
            if not all(key) or key in all_keys:
                raise ValueError("duplicate/null parent source metadata key")
            all_keys.add(key)
            partition = int(hashlib.sha256(json.dumps(key, separators=(",", ":")).encode()).hexdigest(), 16) % count
            if partition == index:
                expected_keys.add(key)
    if len(all_keys) != population["eligible_source_rows"]:
        raise ValueError("full eligible parent source denominator mismatch")
    with catalog.open(encoding="utf-8") as stream:
        while True:
            line = stream.readline(65537)
            if not line:
                break
            if len(line) > 65536 or len(output) >= 100:
                raise ValueError("bounded native replay requires catalog partition <=100 documents")
            doc = json.loads(line)
            key = (doc["accession"], doc["source_sha256"], doc["source_doc"])
            if not all(key) or key in seen:
                raise ValueError("duplicate/conflicting native catalog key")
            seen.add(key)
            blob, text = native_document(doc, pins)
            total += len(blob) + len(text.encode("utf-8"))
            if total > 8 * 1024 * 1024:
                raise ValueError("native replay total exceeds 8MiB; partition catalog")
            output.append((doc, blob, text))
    if len(output) != record["population"]["catalog_rows"]:
        raise ValueError("admitted full row population mismatch")
    if seen != expected_keys:
        raise ValueError("missing/extra deterministic eligible source population")
    sessions = json.loads(Path(record["inputs"]["calendar"]["path"]).read_text(encoding="utf-8"))
    sessions = [dt.date.fromisoformat(s) for s in sessions]
    if len(sessions) < 2 or sessions != sorted(set(sessions)):
        raise ValueError("invalid admitted exchange calendar")
    recheck(pins)
    return output, pins, record


def validate_keys(rows: list[dict[str, Any]], columns: list[str]) -> None:
    seen = set()
    for row in rows:
        key = tuple(row.get(name) for name in columns)
        if any(value is None or value == "" for value in key) or key in seen:
            raise ValueError("null/duplicate published event key")
        seen.add(key)
