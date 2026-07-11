#!/usr/bin/env python3
"""Download and fingerprint authoritative OCC Equity Special Settlements reports."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import sys
import urllib.request


BASE_URL = "https://marketdata.theocc.com/ess-reports?reportDate="
DATE_RE = re.compile(r"^20\d{2}-\d{2}-\d{2}$")


def parse_dates(values: list[str]) -> list[str]:
    dates: set[str] = set()
    for value in values:
        for raw in value.split(","):
            date = raw.strip()
            if not DATE_RE.fullmatch(date):
                raise ValueError(f"invalid ISO date: {date!r}")
            dates.add(date)
    if not dates:
        raise ValueError("at least one date is required")
    return sorted(dates)


def validate_report(payload: bytes, date: str) -> None:
    if not payload:
        raise ValueError(f"empty OCC ESS response for {date}")
    text = payload.decode("ascii", errors="strict")
    expected = f"ACTIVITY DATE {date[5:7]}/{date[8:10]}/{date[2:4]}"
    if "NON-STANDARD SETTLEMENTS" not in text or expected not in text:
        raise ValueError(f"OCC ESS response does not match {date}")
    if not any(line.startswith("0706") for line in text.splitlines()):
        raise ValueError(f"OCC ESS response has no settlement records for {date}")


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "atx-vol-occ-ess/1"})
    with urllib.request.urlopen(request, timeout=60) as response:
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status} for {url}")
        return response.read()


def publish_report(out_dir: Path, date: str, payload: bytes) -> tuple[Path, str]:
    validate_report(payload, date)
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"{date}.txt"
    pending = target.with_suffix(target.suffix + ".pending")
    pending.write_bytes(payload)
    os.replace(pending, target)
    return target, hashlib.sha256(payload).hexdigest()


def download(dates: list[str], out_dir: Path) -> Path:
    rows: list[tuple[str, str, int, str, str]] = []
    for date in dates:
        url = BASE_URL + date.replace("-", "")
        target = out_dir / f"{date}.txt"
        if target.is_file():
            payload = target.read_bytes()
            validate_report(payload, date)
            digest = hashlib.sha256(payload).hexdigest()
            status = "existing"
        else:
            payload = fetch(url)
            target, digest = publish_report(out_dir, date, payload)
            status = "downloaded"
        rows.append((date, target.name, len(payload), digest, status))
        print(f"{status} {date} bytes={len(payload)} sha256={digest}")

    manifest = out_dir / "manifest.tsv"
    pending = manifest.with_suffix(manifest.suffix + ".pending")
    lines = ["date\tpath\tbytes\tsha256\tstatus"]
    lines.extend("\t".join(map(str, row)) for row in rows)
    pending.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
    os.replace(pending, manifest)
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dates", action="append", required=True,
                        help="ISO date or comma-separated ISO dates; repeatable")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        dates = parse_dates(args.dates)
        manifest = download(dates, args.out)
    except (OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
