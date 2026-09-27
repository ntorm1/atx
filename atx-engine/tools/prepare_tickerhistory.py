#!/usr/bin/env python3
"""Prepare a bounded, explicitly quarantined TickerHistory research input.

No third-party packages, source modifications, identity inference, or price repair.
Only a completed manifest.json authorizes consuming accepted.zip. On failure an
exclusive output directory retains *.partial files and failure.json for diagnosis.
Reruns require a new output directory; existing paths are never overwritten.
"""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import zipfile

POLICY_VERSION = "tickerhistory-qa-v1"
QA_V2_POLICY_VERSION = "tickerhistory-qa-v2"
# Recovered cp15-idgap-investigation.md, SHA256 fd4adfba...0a8c9c.
# Canonical full allowlist is ASCII dates + LF, including a final LF.
QA_V2_DATES = tuple(dt.date.fromisoformat(s) for s in (
    "2016-01-15", "2016-02-12", "2016-03-24", "2016-05-27", "2016-07-01",
    "2016-09-02", "2016-11-23", "2016-12-23", "2016-12-30", "2017-01-13",
    "2017-02-17", "2017-04-13", "2017-05-26", "2017-07-03", "2017-09-01",
    "2017-11-22", "2017-12-22", "2018-01-12", "2018-02-16"))
QA_V2_DATES_SHA256 = "0589dc9ae5c96e68d183820f4733ade7df94245d805e285e43e1bdf29c0fef60"
MAX_LINE_BYTES = 1 << 20
MAX_DATE_BYTES = 128 << 20
I64_MAX = (1 << 63) - 1
NUMBER = re.compile(rb"-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
DATE = re.compile(rb"[0-9]{4}-[0-9]{2}-[0-9]{2}\Z")
PRICES = ("open", "high", "low", "close")
REQUIRED = ("tradingDate", "securityID", "ticker_tk", "todayTicker",
            *PRICES, "volume", "shares", "cumulReturnFactor")
# QA-v2 (ruling R21-4): on explicitly listed vendor-corrupted OHLC sessions only, a row
# whose SOLE rejection reason is this is accepted with open/high/low blanked (NaN natively).
QA_V2_RESCUABLE = ["ohlc_order_violation"]
QA_V2_BLANKED = ("open", "high", "low")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def lines(stream):
    """Frame on LF with bounded buffers, retaining original line bytes."""
    pending = b""
    while True:
        block = stream.read(8 << 20)
        if not block:
            break
        framed = (pending + block).split(b"\n")
        pending = framed.pop()
        for line in framed:
            if len(line) + 1 > MAX_LINE_BYTES:
                raise ValueError("Source line exceeds byte limit")
            yield line + b"\n"
        if len(pending) > MAX_LINE_BYTES:
            raise ValueError("Source line exceeds byte limit")
    if pending:
        yield pending


def parse_date(value: bytes) -> dt.date:
    if not DATE.fullmatch(value):
        raise ValueError("Invalid YYYY-MM-DD date: " + repr(value))
    return dt.date.fromisoformat(value.decode("ascii"))


def positive_id(value: bytes) -> int | None:
    # Bound digit count after canonical leading zeros before Python integer parse.
    canonical = value.lstrip(b"0")
    if not value.isdigit() or not canonical or len(canonical) > 19:
        return None
    parsed = int(canonical)
    return parsed if parsed <= I64_MAX else None


def numeric(value: bytes) -> float | None:
    # Python float accepts whitespace/underscores that the native reader does not.
    if not NUMBER.fullmatch(value):
        return None
    parsed = float(value)
    if parsed == 0.0 and any(digit in b"123456789" for digit in value.lower().split(b"e", 1)[0]):
        return None  # Native from_chars reports out-of-range on underflow to zero.
    return parsed if math.isfinite(parsed) else None


def classify_row(values, field, sid, duplicate_positive_ids):
    """Pure QA-v1 predicates shared by preparation and read-only reconciliation.

    The caller supplies duplicate IDs for the WHOLE original date group. Reasons
    and nonrejecting flags retain the original preparation order and semantics.
    """
    reasons = []
    if sid is None:
        reasons.append("invalid_or_nonpositive_security_id")
    elif sid in duplicate_positive_ids:
        reasons.append("duplicate_positive_date_security_id")
    prices = [numeric(values[field[name]]) for name in PRICES]
    if any(value is None or value <= 0 for value in prices):
        reasons.append("invalid_or_nonpositive_ohlc")
    else:
        o, h, l, c = prices
        if l > min(o, c) or h < max(o, c) or l > h:
            reasons.append("ohlc_order_violation")
    factor = numeric(values[field["cumulReturnFactor"]])
    if factor is None or factor <= 0:
        reasons.append("invalid_or_nonpositive_cumulative_factor")
    elif all(value is not None and value > 0 for value in prices):
        products = [value * factor for value in prices]
        if any(not math.isfinite(value) or value <= 0 for value in products):
            reasons.append("invalid_adjusted_ohlc_product")
    volume = numeric(values[field["volume"]])
    if volume is None or volume < 0:
        reasons.append("invalid_or_negative_volume")
    shares_text = values[field["shares"]]
    shares_id = positive_id(shares_text)
    flags = []
    if shares_text.isdigit() and not shares_text.lstrip(b"0"):
        flags.append("zero_shares")
    elif shares_id is None:
        flags.append("invalid_negative_or_out_of_range_shares")
    if volume == 0:
        flags.append("zero_volume")
    return reasons, flags


def zip_info(member_name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(member_name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    return info


def write_json(path: Path, value) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


def prepare(source: Path, output_dir: Path, start: dt.date, end: dt.date,
            qa_v2_dates: tuple[dt.date, ...] = (), *, qa_rule: str | None = None) -> dict:
    source = source.resolve(strict=True)
    output_dir = output_dir.resolve()
    if start > end:
        raise ValueError("start-date must be at or before end-date")
    rule = qa_rule or ("qa-v2" if qa_v2_dates else "legacy-v1")
    if rule not in ("qa-v2", "legacy-v1") or (rule == "legacy-v1" and qa_v2_dates):
        raise ValueError("invalid QA rule/date combination")
    if rule == "qa-v2":
        if end >= dt.date(2020, 1, 1):
            raise ValueError("qa-v2 requires a pre-2020 window")
        expected_dates = tuple(d for d in QA_V2_DATES if start <= d <= end)
        if qa_v2_dates and tuple(sorted(qa_v2_dates)) != expected_dates:
            raise ValueError("qa-v2 dates must equal the pinned allowlist inside the window")
        qa_v2_dates = expected_dates
    if len(set(qa_v2_dates)) != len(qa_v2_dates) or any(not start <= d <= end for d in qa_v2_dates):
        raise ValueError("qa-v2 dates must be unique and inside the window")
    qa_v2_keys = {d.isoformat().encode() for d in qa_v2_dates}
    if source == output_dir or source in output_dir.parents:
        raise ValueError("Output directory cannot be inside the source file")
    # Exclusive ownership: failure artifacts cannot be mistaken for a successful rerun.
    output_dir.mkdir(parents=True, exist_ok=False)
    try:
        source_hash = sha256(source)
        source_size = source.stat().st_size
        stats = collections.Counter()
        primary = collections.Counter()
        reasons = collections.Counter()
        flags = collections.Counter()
        examples = collections.defaultdict(list)
        daily = []
        qa_v2_daily = {}
        conflict_fixture = None
        accepted_path = output_dir / "accepted.zip.partial"
        start_key, end_key = start.isoformat().encode(), end.isoformat().encode()
        with zipfile.ZipFile(source) as archive:
            members = [member for member in archive.infolist() if not member.is_dir()]
            if len(members) != 1 or "tbltickerhistory" not in members[0].filename:
                raise ValueError("Expected one tbltickerhistory data member")
            member = members[0]
            with archive.open(member) as raw, zipfile.ZipFile(
                    accepted_path, "x", compression=zipfile.ZIP_DEFLATED,
                    compresslevel=6, allowZip64=True) as accepted_zip:
                framed = iter(lines(raw))
                header_bytes = next(framed, b"")
                header = header_bytes.rstrip(b"\r\n").decode("ascii").split("\t")
                if len(set(header)) != len(header) or any(k not in header for k in REQUIRED):
                    raise ValueError("Missing or duplicate required header columns")
                if header[0] != "tradingDate":
                    raise ValueError("TickerHistory tradingDate must be the first column")
                field = {name: i for i, name in enumerate(header)}
                width = len(header)
                with accepted_zip.open(zip_info(member.filename), "w", force_zip64=True) as accepted:
                    accepted.write(header_bytes)

                    def flush(date: bytes, group: list[tuple[bytes, list[bytes], int | None]]) -> None:
                        nonlocal conflict_fixture
                        if not group:
                            return
                        keys = collections.Counter(sid for _, _, sid in group if sid is not None)
                        duplicate_keys = {sid for sid, count in keys.items() if count > 1}
                        stats["duplicate_positive_keys"] += len(duplicate_keys)
                        if conflict_fixture is None and duplicate_keys:
                            # First source-order duplicate key, all original rows, no selection by price.
                            chosen = next(sid for _, _, sid in group if sid in duplicate_keys)
                            conflicting = [original for original, _, sid in group if sid == chosen]
                            fixture_path = output_dir / "conflict_fixture.zip.partial"
                            with zipfile.ZipFile(fixture_path, "x", compression=zipfile.ZIP_DEFLATED,
                                                 compresslevel=6) as fixture:
                                with fixture.open(zip_info(member.filename), "w", force_zip64=True) as dest:
                                    dest.write(header_bytes)
                                    for original in conflicting:
                                        dest.write(original)
                            conflict_fixture = {"filename": "conflict_fixture.zip", "date": date.decode(),
                                                "security_id": chosen, "rows": len(conflicting),
                                                "purpose": "Native duplicate-key rejection fixture; NOT accepted data"}
                        accepted_count = 0
                        rescued_count = 0
                        for original, values, sid in group:
                            row_reasons, row_flags = classify_row(values, field, sid, duplicate_keys)
                            if date in qa_v2_keys and row_reasons == QA_V2_RESCUABLE:
                                # Close/volume/factor/ID valid; only the corrupt OHL range is dropped.
                                content = original.rstrip(b"\r\n")
                                blanked = list(values)
                                for name in QA_V2_BLANKED:
                                    blanked[field[name]] = b""
                                accepted.write(b"\t".join(blanked) + original[len(content):])
                                accepted_count += 1
                                rescued_count += 1
                                stats["accepted_rows"] += 1
                                stats["qa_v2_rescued_rows"] += 1
                                for flag in row_flags:
                                    flags["selected_" + flag] += 1
                                    flags["accepted_" + flag] += 1
                                continue
                            for flag in row_flags:
                                flags["selected_" + flag] += 1
                            if row_reasons:
                                stats["rejected_rows"] += 1
                                primary[row_reasons[0]] += 1
                                for reason in row_reasons:
                                    reasons[reason] += 1
                                    if len(examples[reason]) < 5:
                                        examples[reason].append({"date": date.decode(),
                                            "security_id": values[field["securityID"]].decode("ascii", "replace"),
                                            "ticker": values[field["ticker_tk"]].decode("utf-8", "replace"),
                                            "reasons": row_reasons})
                            else:
                                accepted.write(original)
                                accepted_count += 1
                                stats["accepted_rows"] += 1
                                for flag in row_flags:
                                    flags["accepted_" + flag] += 1
                        daily.append({"date": date.decode(), "selected": len(group),
                                      "accepted": accepted_count, "rejected": len(group) - accepted_count,
                                      "duplicate_positive_keys": len(duplicate_keys)})
                        if date in qa_v2_keys:
                            qa_v2_daily[date.decode()] = {"accepted_v1": accepted_count - rescued_count,
                                                          "accepted_v2_rescued": rescued_count,
                                                          "accepted": accepted_count}

                    current_date = None
                    group = []
                    group_bytes = 0
                    for original in framed:
                        stats["source_rows"] += 1
                        content = original.rstrip(b"\r\n")
                        if content.count(b"\t") + 1 != width:
                            raise ValueError(f"Bad column count on source row {stats['source_rows']}")
                        # Session keys are validated over the entire source, including outside the window.
                        date = content.split(b"\t", 1)[0]
                        if date != current_date:
                            parse_date(date)
                            if current_date is not None and date < current_date:
                                raise ValueError("Non-monotonic source tradingDate")
                            if current_date is not None:
                                flush(current_date, group)
                            current_date, group, group_bytes = date, [], 0
                        if not start_key <= date <= end_key:
                            stats["outside_window_rows"] += 1
                            continue
                        stats["selected_rows"] += 1
                        group_bytes += len(original)
                        if group_bytes > MAX_DATE_BYTES:
                            raise ValueError("Selected date exceeds bounded-memory byte limit")
                        values = content.split(b"\t")
                        group.append((original, values, positive_id(values[field["securityID"]])))
                    if current_date is not None:
                        flush(current_date, group)
        if not stats["selected_rows"] or not stats["accepted_rows"]:
            raise ValueError("Selected interval contains no accepted rows")
        if len(qa_v2_daily) != len(qa_v2_keys):
            raise ValueError("qa-v2 date has no selected source rows")
        if sha256(source) != source_hash:
            raise ValueError("Source ZIP changed during preparation")
        assert stats["selected_rows"] == stats["accepted_rows"] + stats["rejected_rows"]
        assert stats["source_rows"] == stats["selected_rows"] + stats["outside_window_rows"]
        accepted_hash = sha256(accepted_path)
        manifest = {
            "status": "complete", "policy_version": POLICY_VERSION,
            "source": {"path": str(source), "size_bytes": source_size, "sha256": source_hash,
                       "member": member.filename, "member_uncompressed_bytes": member.file_size,
                       "member_crc32": f"{member.CRC:08x}", "crc_verified": True},
            "window": {"start_inclusive": start.isoformat(), "end_inclusive": end.isoformat()},
            "columns": header, "counts": dict(stats), "daily_counts": daily,
            "primary_rejection_counts": dict(primary), "overlapping_reason_counts": dict(reasons),
            "nonrejecting_flags": dict(flags), "bounded_rejection_examples": dict(examples),
            "accepted": {"filename": "accepted.zip", "sha256": accepted_hash,
                         "size_bytes": accepted_path.stat().st_size, "rows_preserved_byte_for_byte": True},
            "conflict_fixture": conflict_fixture,
            "policy": ["All positive duplicate date/ID rows quarantined, including identical duplicates",
                       "No first/last selection, price repair, factor adjustment, or todayTicker selection",
                       "Finite positive OHLC and cumulative factor; valid OHLC ordering and adjusted products",
                       "Finite nonnegative volume; zero volume allowed and counted",
                       "Shares preserved unchanged; zero/invalid shares flagged without inference"],
            "limits": ["Does not validate economic daily-return/factor consistency or shares revisions",
                       "Archive snapshot is not original historical publication or bitemporal data",
                       "Vendor currently states 05:00 CT T+1; historical calendar/release timing is unverified",
                       "Input quality checkpoint only; no investment performance or tradability claim"],
            "bounds": {"max_line_bytes": MAX_LINE_BYTES, "max_source_bytes_per_date": MAX_DATE_BYTES},
        }
        if rule == "qa-v2":
            manifest["policy_version"] = QA_V2_POLICY_VERSION
            manifest["qa_version"] = "v2"
            manifest["qa_v2_allowlist_sha256"] = QA_V2_DATES_SHA256
            manifest["qa_v2_blanked_fields"] = list(QA_V2_BLANKED)
            manifest["qa_v2_rescuable_reasons"] = QA_V2_RESCUABLE
            manifest["qa_v2_dates"] = sorted(qa_v2_daily)
            manifest["qa_v2_daily_counts"] = qa_v2_daily
            manifest["accepted"]["rows_modified_qa_v2"] = stats["qa_v2_rescued_rows"]
            manifest["accepted"]["rows_preserved_byte_for_byte"] = stats["qa_v2_rescued_rows"] == 0
            manifest["accepted"]["unmodified_rows"] = stats["accepted_rows"] - stats["qa_v2_rescued_rows"]
            manifest["accepted"]["unmodified_rows_preserved_byte_for_byte"] = True
            manifest["policy"].append("QA-v2: on qa_v2_dates only, rows whose sole rejection reason is "
                                      "ohlc_order_violation are accepted with open/high/low blanked; "
                                      "all other rows are preserved byte-for-byte")
        if conflict_fixture is not None:
            fixture_path = output_dir / "conflict_fixture.zip.partial"
            conflict_fixture["sha256"] = sha256(fixture_path)
            conflict_fixture["size_bytes"] = fixture_path.stat().st_size
        # Publish files only after the full source and all rows have been checked.
        accepted_path.rename(output_dir / "accepted.zip")
        if conflict_fixture is not None:
            (output_dir / "conflict_fixture.zip.partial").rename(output_dir / "conflict_fixture.zip")
        write_json(output_dir / "manifest.json.partial", manifest)
        (output_dir / "manifest.json.partial").rename(output_dir / "manifest.json")
        return manifest
    except Exception as error:
        write_json(output_dir / "failure.json", {"status": "failed", "source": str(source),
                   "error": str(error), "instruction": "Do not consume partial artifacts; use a new output directory"})
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--start-date", required=True, type=dt.date.fromisoformat)
    parser.add_argument("--end-date", required=True, type=dt.date.fromisoformat)
    parser.add_argument("--qa-rule", choices=("qa-v2", "legacy-v1"), default="qa-v2")
    parser.add_argument("--qa-v2-dates", default="",
                        help="Optional exact pinned allowlist inside the window; qa-v2 derives it by default")
    args = parser.parse_args()
    qa_v2_dates = tuple(dt.date.fromisoformat(d) for d in args.qa_v2_dates.split(",") if d)
    manifest = prepare(args.source, args.out_dir, args.start_date, args.end_date, qa_v2_dates,
                       qa_rule=args.qa_rule)
    print(json.dumps({"manifest": str(args.out_dir / "manifest.json"),
                      "counts": manifest["counts"], "accepted": manifest["accepted"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
