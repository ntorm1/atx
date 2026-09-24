"""Guarded operator probe for the bounded SEC submissions directory reader.

Run with the project interpreter after the sole heavy-work slot is granted.
This hashes/indexes the full retained archive and visits every sorted main name,
but reads only a small, explicitly bounded selection of payloads. It never opens
an ordinary ZipFile on the complete archive or connects to the warehouse.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import logging
import os
import sqlite3
import time
from collections import deque
from pathlib import Path
from typing import Any

from atx_db._submissions_archive import SubmissionsArchive, _one_info

_ROOT = Path(__file__).resolve().parents[3]
_SAMPLE_COUNT = 5
_MAX_EXPLICIT_MEMBERS = 8
_MAX_PAYLOAD_LIMIT = 32 * 1024 * 1024


def _stat(path: Path) -> dict[str, int]:
    value = path.stat()
    return {
        "device": value.st_dev, "inode": value.st_ino,
        "bytes": value.st_size, "mtime_ns": value.st_mtime_ns,
    }


def _existing_sidecars(path: Path) -> dict[str, dict[str, int]]:
    # There are normally zero or one version/hash sidecars. Bound even this
    # operator bookkeeping rather than retaining arbitrary directory listings.
    result = {}
    for candidate in path.parent.glob(f"{path.name}.directory-v*.sqlite3"):
        if len(result) >= 64:
            raise ValueError("More than 64 source directory sidecars; inspect cache before probing")
        result[str(candidate.resolve())] = _stat(candidate)
    return result


def _main_inventory(archive: SubmissionsArchive) -> dict[str, Any]:
    digest = hashlib.sha256()
    first: list[str] = []
    last: deque[str] = deque(maxlen=_SAMPLE_COUNT)
    count = 0
    previous = ""
    for name in archive.main_members():
        if name <= previous:
            raise ValueError("Main directory traversal is not strictly ordered and unique")
        encoded = name.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)
        if len(first) < _SAMPLE_COUNT:
            first.append(name)
        last.append(name)
        previous = name
        count += 1
        if count % 100000 == 0:
            logging.info("Full main-name traversal: %d/%d", count, archive.main_member_count)
    if count != archive.main_member_count:
        raise ValueError("Main directory cursor count differs from indexed denominator")
    return {
        "count": count,
        "ordered_names_sha256": digest.hexdigest(),
        "digest_encoding": "each UTF-8 name prefixed by its four-byte unsigned big-endian byte length",
        "first": first, "last": list(last),
    }


def _selected_payloads(
    archive: SubmissionsArchive, names: list[str], maximum_bytes: int,
) -> list[dict[str, Any]]:
    results = []
    # The reader intentionally exposes only read(name); inspect one tiny cached
    # central record to enforce the probe's declared-size ceiling BEFORE asking
    # it to allocate a payload. Stdlib interprets that one record, including ZIP64.
    index = sqlite3.connect(f"{archive.index_path.resolve().as_uri()}?mode=ro", uri=True)
    try:
        index.execute("PRAGMA cache_size=-256")
        index.execute("PRAGMA mmap_size=0")
        index.execute("PRAGMA temp_store=FILE")
        first_history = index.execute("SELECT name FROM members WHERE kind=2 ORDER BY name LIMIT 1").fetchone()
        if first_history is not None and first_history[0] not in names:
            names = [*names, first_history[0]]
        for name in names:
            row = index.execute("SELECT record FROM members WHERE name=?", (name,)).fetchone()
            if row is None:
                results.append({"name": name, "status": "absent"})
                continue
            info = _one_info(row[0])
            if info.file_size > maximum_bytes or info.compress_size > maximum_bytes:
                results.append({
                    "name": name, "status": "skipped_payload_size_limit",
                    "declared_bytes": info.file_size, "compressed_bytes": info.compress_size,
                })
                continue
            payload = archive.read(name)
            results.append({
                "name": name, "status": "read_with_standard_zip_integrity_checks",
                "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
            })
            del payload
    finally:
        # sqlite3's context manager commits/rolls back but does not close.
        index.close()
    return results


def _pass(path: Path, names: list[str], maximum_bytes: int) -> dict[str, Any]:
    before = _existing_sidecars(path)
    started = time.monotonic()
    with SubmissionsArchive(path) as archive:
        opened = time.monotonic()
        index_path = str(archive.index_path.resolve())
        index_stat = _stat(archive.index_path)
        main = _main_inventory(archive)
        # At most two automatic main samples, one history sample (inside the
        # payload helper), plus at most eight explicitly requested names.
        selected = list(dict.fromkeys([*main["first"][:1], *main["last"][-1:], *names]))
        payloads = _selected_payloads(archive, selected, maximum_bytes)
        archive.assert_unchanged()
        result = {
            "source_sha256": archive.sha256,
            "directory_entries": archive.entry_count,
            "unique_member_names": archive.unique_member_count,
            "duplicate_name_entries": archive.entry_count - archive.unique_member_count,
            "unique_main_members": archive.main_member_count,
            "unique_history_members": archive.history_member_count,
            "unique_other_members": archive.unique_member_count - archive.main_member_count - archive.history_member_count,
            "declared_payload_compressed_bytes": archive.compressed_bytes,
            "declared_payload_expanded_bytes": archive.expanded_bytes,
            "main_traversal": main,
            "selected_payloads": payloads,
            "index": {
                "path": index_path, "stat": index_stat,
                "existed_before_open": index_path in before,
                "file_identity_unchanged_from_before_open": before.get(index_path) == index_stat,
                "observation": "existing_file_reused" if before.get(index_path) == index_stat else "created_or_rebuilt",
                "digest_receipt_exists": archive.index_path.with_suffix(".sqlite3.sha256").is_file(),
                "validation": "reader verified full index SHA against receipt and source SHA/stat/version",
            },
            "open_seconds": opened - started,
        }
    result["total_seconds"] = time.monotonic() - started
    return result


def _prior_comparison(prior: dict[str, Any], path: Path, stat: dict[str, int], observed: dict[str, Any]) -> dict[str, Any]:
    checks = {
        "source_path": Path(prior["source"]).resolve() == path.resolve(),
        "archive_bytes": prior["archive_bytes"] == stat["bytes"],
        "directory_entries": prior["members"] == observed["directory_entries"],
        "main_members": prior["main_cik_members"] == observed["unique_main_members"],
        "history_members": prior["history_members"] == observed["unique_history_members"],
        "other_members": prior["other_members"] == observed["unique_other_members"],
        "declared_expanded_bytes": prior["declared_uncompressed_bytes"] == observed["declared_payload_expanded_bytes"],
        "no_duplicate_names_for_raw_to_unique_count_comparison": observed["duplicate_name_entries"] == 0,
    }
    if "source_sha256" in prior:
        checks["source_sha256"] = prior["source_sha256"] == observed["source_sha256"]
    return {
        "prior_observed_at_utc": prior.get("observed_at_utc"),
        "checks": checks,
        "all_recorded_comparisons_match": all(checks.values()),
        "prior_source_sha256_available": "source_sha256" in prior,
        "identity_limit": (
            "Prior inventory has no source SHA; matching path, byte size and denominators alone do not prove byte identity."
            if "source_sha256" not in prior else "Source SHA compared with the prior inventory."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=_ROOT / "atx-db/data/cache/submissions.zip")
    parser.add_argument("--prior-inventory", type=Path, default=Path(__file__).with_name("submissions-archive-inventory.json"))
    parser.add_argument("--output", type=Path, required=True, help="Fresh JSON receipt; existing files are refused")
    parser.add_argument("--member", action="append", default=[], help="Selected payload name, at most eight explicit names")
    parser.add_argument("--max-payload-bytes", type=int, default=8 * 1024 * 1024)
    args = parser.parse_args()
    if len(args.member) > _MAX_EXPLICIT_MEMBERS or not 1 <= args.max_payload_bytes <= _MAX_PAYLOAD_LIMIT:
        parser.error("Use at most eight --member values and a payload ceiling between 1 and 33554432 bytes")
    if args.output.exists():
        parser.error("The output receipt already exists; choose a fresh path")
    if args.output.resolve() in {args.archive.resolve(), args.prior_inventory.resolve()}:
        parser.error("The output must be separate from source and prior evidence")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    result: dict[str, Any] = {
        "started_at_utc": dt.datetime.now(dt.UTC).isoformat(),
        "source": str(args.archive.resolve()),
        "prior_inventory": str(args.prior_inventory.resolve()),
        "scope": "Complete ZIP directory and all sorted main names; bounded selected payloads only; no warehouse access",
        "all_payloads_validated": False,
        "production_eligibility_established": False,
        "maximum_selected_payload_bytes_each": args.max_payload_bytes,
        "native_peak_memory": "Use the enclosing run_memory_guarded.py receipt; this script does not estimate native peak",
    }
    exit_code = 2
    try:
        prior = json.loads(args.prior_inventory.read_text(encoding="utf-8"))
        result["source_stat_before"] = _stat(args.archive)
        first = _pass(args.archive, args.member, args.max_payload_bytes)
        result["first_open"] = first
        second = _pass(args.archive, args.member, args.max_payload_bytes)
        result["reuse_open"] = second
        result["source_stat_after"] = _stat(args.archive)
        comparison = _prior_comparison(prior, args.archive, result["source_stat_before"], first)
        result["prior_comparison"] = comparison
        parity_keys = (
            "source_sha256", "directory_entries", "unique_member_names", "unique_main_members",
            "unique_history_members", "unique_other_members", "declared_payload_expanded_bytes",
            "declared_payload_compressed_bytes", "main_traversal", "selected_payloads",
        )
        result["reuse_parity"] = {key: first[key] == second[key] for key in parity_keys}
        result["source_stat_unchanged"] = result["source_stat_before"] == result["source_stat_after"]
        passed = (
            comparison["all_recorded_comparisons_match"]
            and all(result["reuse_parity"].values())
            and result["source_stat_unchanged"]
            and second["index"]["file_identity_unchanged_from_before_open"]
            and all(item["status"] == "read_with_standard_zip_integrity_checks" for item in first["selected_payloads"])
        )
        result["status"] = "directory_and_selected_payload_probe_passed" if passed else "probe_disagreement_or_incomplete_selected_reads"
        exit_code = 0 if passed else 2
    except Exception as exc:
        result["status"] = "probe_failed"
        result["error_type"] = type(exc).__name__
        result["error"] = str(exc)[:512]
    result["finished_at_utc"] = dt.datetime.now(dt.UTC).isoformat()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    print(json.dumps({"status": result["status"], "receipt": str(args.output.resolve()), "exit_code": exit_code}), flush=True)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
