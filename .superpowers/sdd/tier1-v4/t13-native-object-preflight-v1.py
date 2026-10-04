"""Root guarded original-object availability audit. Never fetch; retain negatives."""
import datetime as dt
import gzip
import hashlib
import json
import os
import re
from pathlib import Path
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C, events_sources as E, notes_fetch as N


def main():
    if not os.environ.get("ATX_GUARD_JOB"):
        raise RuntimeError("external root guard required")
    out = Path(__file__).with_name("task13-native-object-preflight-v1.json")
    if out.exists():
        raise RuntimeError("no output overwrite")
    expected = "a76d19d7458481fcd67de4f7397617234f34b98843b2449c291628564690bf64"
    catalog = E.catalog_path()
    if C.sha256_file(catalog) != expected:
        raise RuntimeError("catalog admission mismatch")
    chosen, scanned = [], 0
    for batch in pq.ParquetFile(catalog).iter_batches(batch_size=256):
        for row in batch.to_pylist():
            scanned += 1
            if scanned > 60000:
                raise RuntimeError("metadata row bound exceeded")
            if row.get("form") not in {"8-K", "8-K/A"} or not row.get("filing_date") or row["filing_date"] < dt.date(2020, 1, 1):
                continue
            key = (str(row["filing_date"]), row["cik"], row["accession"], str(row.get("source_doc")), str(row.get("doc_source")))
            chosen.append((key, row))
            chosen.sort(key=lambda x: x[0])
            del chosen[8:]
    pins, matches = {}, {}
    paths = [E.EARNINGS_RELEASE_DIR / "units-done.jsonl", E.RAW_DIR / "fetch-ledger.jsonl",
             N.RAW_DIR / "receipts.jsonl", N.ARCHIVE_RECEIPTS / "sec_notes-receipts.jsonl"]
    for path in paths:
        if not path.exists():
            matches[str(path)] = {"status": "MISSING_EXACT_LEDGER", "rows": []}
            continue
        if path.stat().st_size > 64 * 1024 * 1024:
            raise RuntimeError("ledger64MiBbound")
        pins[str(path)] = C.sha256_file(path)
        hits = []
        with path.open("rb") as stream:
            for line in range(1, 200001):
                raw = stream.readline(1048577)
                if not raw:
                    break
                if len(raw) > 1048576:
                    raise RuntimeError("ledger line bound")
                record = json.loads(raw)
                relevant = any(record.get("accession") == row["accession"] or record.get("url") == row.get("url") and row.get("url") for _, row in chosen)
                relevant |= any(k in str(record.get("url", "")) or record.get("key") == k for k in ("2019q4", "2020q1"))
                if relevant:
                    kept = {k: record.get(k) for k in ("accession", "cik", "document_sha256", "exhibit", "outcome", "unit", "url", "sha256", "status", "fetched_at", "done_at", "key", "phase", "bytes")}
                    hits.append({"line": line, "line_sha256": hashlib.sha256(raw).hexdigest(), "record": kept})
                    if len(hits) > 128:
                        raise RuntimeError("receipt match bound")
            else:
                raise RuntimeError("ledger record bound")
        matches[str(path)] = {"status": "PINNED_RECEIPT_METADATA_ONLY", "sha256": pins[str(path)], "rows": hits}
    notes = []
    for key in ("2019q4", "2020q1"):
        path = N.RAW_DIR / f"{key}_notes.zip"
        notes.append({"path": str(path), "status": "PRESENT_STAT_ONLY_UNQUALIFIED" if path.exists() else "MISSING_ORIGINAL_ZIP",
                      "bytes": path.stat().st_size if path.exists() else None, "original_zip_read": False})
    results, total, snippets = [], 0, []
    limit = 2 * 1024 * 1024
    for _, row in chosen:
        result = {"catalog_row": row, "status": "UNQUALIFIED"}
        source = row["doc_source"]
        root = E.EARNINGS_RELEASE_DIR if source == "landed_v2" else E.RAW_DIR if source == "sec_events" else None
        if root is None:
            result["status"] = "UNSUPPORTED_SOURCE"
            results.append(result)
            continue
        ledger = root / ("units-done.jsonl" if source == "landed_v2" else "fetch-ledger.jsonl")
        receipts = [hit for hit in matches[str(ledger)]["rows"] if (hit["record"].get("document_sha256") or hit["record"].get("sha256")) == row["doc_sha256"] and (hit["record"].get("accession") == row["accession"] or hit["record"].get("url") == row.get("url") and row.get("url"))]
        if not receipts:
            result["status"] = "NO_MATCHING_ORIGINAL_RECEIPT"
            results.append(result)
            continue
        sha = row["doc_sha256"]
        candidates = [root / "objects" / sha[:2] / (sha + ".gz"), root / "objects" / sha[:2] / sha]
        path = next((p for p in candidates if p.exists()), None)
        if path is None or path.stat().st_size > limit:
            result["status"] = "MISSING_OR_STORED_OVER_2MIB"
            results.append(result)
            continue
        stored = C.sha256_file(path)
        with (gzip.open if path.suffix == ".gz" else open)(path, "rb") as stream:
            blob = stream.read(limit + 1)
        total += path.stat().st_size + len(blob)
        if total > 24 * 1024 * 1024:
            raise RuntimeError("aggregate24MiBbound")
        if len(blob) > limit:
            result["status"] = "DECODED_OVER_2MIB"
            results.append(result)
            continue
        if hashlib.sha256(blob).hexdigest() != sha or C.sha256_file(path) != stored:
            raise RuntimeError("original/storage identity mismatch")
        text = blob.decode("utf-8", errors="replace")
        patterns = {"sgml_acceptance": r"<ACCEPTANCE-DATETIME>", "inline_dei": r"dei:(?:TradingSymbol|Security12bTitle|EntityCentralIndexKey)",
                    "declaration": r"declar\w*.{0,120}dividend", "accepted_index": r"Accepted</div>"}
        presence = {}
        for name, pattern in patterns.items():
            found = re.search(pattern, text, re.I | re.S)
            presence[name] = bool(found)
            if found and len(snippets) < 16:
                snippets.append({"accession": row["accession"], "kind": name, "context": text[max(0, found.start()-80):found.end()+240]})
        result.update(status="ORIGINAL_MATCH_ONLY_UNQUALIFIED", path=str(path), stored_bytes=path.stat().st_size,
                      original_bytes=len(blob), stored_sha256=stored, original_sha256=sha, presence=presence,
                      receipt_lines=[hit["line"] for hit in receipts])
        results.append(result)
    if C.sha256_file(catalog) != expected or any(C.sha256_file(Path(path)) != sha for path, sha in pins.items()):
        raise RuntimeError("catalog/receipt changed after use")
    payload = {"status": "BOUNDED_ORIGINAL_AVAILABILITY_ONLY", "catalog_sha256": expected, "catalog_rows": scanned,
               "notes_original_zip_paths": notes, "receipts": matches, "sample_outcomes": results,
               "context_snippets": snippets, "stored_plus_decoded_bytes": total, "http_attempts": 0,
               "qualification": "Availability/native-format leads only, not publication/class/currency/cadence/history/denominator/gate certification."}
    encoded = json.dumps(payload, default=str, indent=2).encode()
    if len(encoded) > 128 * 1024:
        raise RuntimeError("output128KiBbound")
    with out.open("xb") as stream:
        stream.write(encoded)
    print(json.dumps({"output": str(out), "sample_count": len(results), "http_attempts": 0}))


if __name__ == "__main__":
    main()
