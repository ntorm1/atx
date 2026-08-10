#!/usr/bin/env python3
"""Corpus batch runner for bev_label_factory (atx-vol Task 4).

Fans one bev_label_factory driver invocation out per (run x tenor) pair in a
JSON manifest, sequentially (the driver is already internally threaded via
its own --threads). Pure stdlib: no pandas, no third-party deps -- see
atx-vol/scripts/README.md for the tier note.

CLI:
    python bev_corpus_run.py --manifest run.json --exe <path> --out-dir <dir> [--dry-run]

Manifest schema:
    {
      "defaults": {"delta_lo": 0.05, "delta_hi": 0.95, "threads": 0},
      "tenor_days": [30, 60, 90, 180],
      "runs": [
        {"db": "C:/atx-data/surface-db-r2/spy-2019", "uid": "SPY",
         "entry_start": "2019-01-02", "entry_end": "2019-12-31",
         "dividends": "C:/atx-data/div/spy.tsv", "events": ""}
      ]
    }

"defaults.delta_lo"/"defaults.delta_hi" are required (the driver rejects a
missing/invalid delta window); "defaults.threads" is optional and defaults
to 0 (== the driver's own "auto worker count" default). Each run requires
db/uid/entry_start/entry_end/dividends; "events" is optional and, when
absent or empty, is omitted from the command entirely (the driver treats an
absent --events and an empty one as both "no calendar", but this runner only
ever emits the flag when there is a real path to pass).

Output naming: one driver invocation per (run, tenor) writes
`<out-dir>/<uid>_<entry_start>_<tenor>d.tsv`, with per-invocation stdout/
stderr captured to the sibling `.log` file. `<out-dir>/manifest_out.json`
summarizes every invocation's argv, exit code, and parsed `# key=value` meta
header (e.g. n_rows_written). The process exit code is nonzero if any
invocation failed.

--exe handling: the real driver is a native binary, but tests (and any CI
smoke check) exercise this runner against a small Python stub instead of a
built executable. Rather than requiring callers to hand-quote
`--exe "<python> stub.py"` (fragile on Windows), a `--exe` value ending in
`.py` is automatically run as `[sys.executable, exe, ...]`; any other value
is treated as a single ready-to-exec path and used as-is. This is the one
piece of "magic" in this module -- documented here and in README.md.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_TOP_LEVEL_REQUIRED_KEYS = ("defaults", "tenor_days", "runs")
_DEFAULTS_REQUIRED_KEYS = ("delta_lo", "delta_hi")
_RUN_REQUIRED_KEYS = ("db", "uid", "entry_start", "entry_end", "dividends")


class ManifestError(ValueError):
    """Raised when the manifest JSON is missing a required field."""


def validate_manifest(manifest: dict[str, Any]) -> None:
    for key in _TOP_LEVEL_REQUIRED_KEYS:
        if key not in manifest:
            raise ManifestError(f"manifest missing required key: '{key}'")
    defaults = manifest["defaults"]
    for key in _DEFAULTS_REQUIRED_KEYS:
        if key not in defaults:
            raise ManifestError(f"manifest.defaults missing required key: '{key}'")
    if not manifest["tenor_days"]:
        raise ManifestError("manifest.tenor_days must be a non-empty list")
    if not manifest["runs"]:
        raise ManifestError("manifest.runs must be a non-empty list")
    for i, run in enumerate(manifest["runs"]):
        for key in _RUN_REQUIRED_KEYS:
            if key not in run:
                raise ManifestError(f"manifest.runs[{i}] missing required key: '{key}'")


def load_manifest(path: Path) -> dict[str, Any]:
    manifest = json.loads(Path(path).read_text(encoding="utf-8"))
    validate_manifest(manifest)
    return manifest


def resolve_exe(exe: str) -> list[str]:
    """A `.py` --exe value execs under sys.executable; anything else as-is."""
    if exe.lower().endswith(".py"):
        return [sys.executable, exe]
    return [exe]


def build_command(exe: str, defaults: dict[str, Any], run: dict[str, Any], tenor_days: int, out_path: Path) -> list[str]:
    cmd = resolve_exe(exe) + [
        "--db", str(run["db"]),
        "--uid", str(run["uid"]),
        "--entry-start", str(run["entry_start"]),
        "--entry-end", str(run["entry_end"]),
        "--tenor-days", str(tenor_days),
        "--delta-lo", str(defaults["delta_lo"]),
        "--delta-hi", str(defaults["delta_hi"]),
        "--dividends", str(run["dividends"]),
    ]
    events = run.get("events", "")
    if events:
        cmd += ["--events", str(events)]
    cmd += ["--out", str(out_path)]
    cmd += ["--threads", str(defaults.get("threads", 0))]
    return cmd


@dataclass
class Invocation:
    uid: str
    entry_start: str
    tenor_days: int
    argv: list[str]
    out_path: Path
    log_path: Path


def build_invocations(manifest: dict[str, Any], exe: str, out_dir: Path) -> list[Invocation]:
    defaults = manifest["defaults"]
    invocations: list[Invocation] = []
    for run in manifest["runs"]:
        uid = str(run["uid"])
        entry_start = str(run["entry_start"])
        for tenor_days in manifest["tenor_days"]:
            out_path = out_dir / f"{uid}_{entry_start}_{tenor_days}d.tsv"
            log_path = out_path.with_suffix(".log")
            argv = build_command(exe, defaults, run, tenor_days, out_path)
            invocations.append(Invocation(uid, entry_start, tenor_days, argv, out_path, log_path))
    return invocations


def parse_meta_header(text: str) -> dict[str, str]:
    """Parse leading `# key=value` lines, stopping at the first non-'#' line."""
    meta: dict[str, str] = {}
    for line in text.splitlines():
        if not line.startswith("#"):
            break
        stripped = line[1:].strip()
        if "=" not in stripped:
            continue
        key, _, value = stripped.partition("=")
        meta[key.strip()] = value.strip()
    return meta


def run_invocation(inv: Invocation) -> dict[str, Any]:
    inv.out_path.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(inv.argv, capture_output=True, text=True, check=False)
    inv.log_path.write_text(
        "argv: " + json.dumps(inv.argv) + "\n"
        f"returncode: {completed.returncode}\n"
        "--- stdout ---\n" + completed.stdout +
        "\n--- stderr ---\n" + completed.stderr + "\n",
        encoding="utf-8",
    )
    meta: dict[str, str] = {}
    if completed.returncode == 0 and inv.out_path.exists():
        meta = parse_meta_header(inv.out_path.read_text(encoding="utf-8"))
    return {
        "uid": inv.uid,
        "entry_start": inv.entry_start,
        "tenor_days": inv.tenor_days,
        "out": str(inv.out_path),
        "log": str(inv.log_path),
        "argv": inv.argv,
        "returncode": completed.returncode,
        "meta": meta,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fan bev_label_factory out across a manifest's (run x tenor) grid."
    )
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        manifest = load_manifest(args.manifest)
    except ManifestError as exc:
        print(f"bev_corpus_run: {exc}", file=sys.stderr)
        return 2

    invocations = build_invocations(manifest, args.exe, args.out_dir)

    if args.dry_run:
        for inv in invocations:
            print(json.dumps(inv.argv))
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    results = [run_invocation(inv) for inv in invocations]
    ok = all(r["returncode"] == 0 for r in results)
    manifest_out = {"ok": ok, "invocations": results}
    (args.out_dir / "manifest_out.json").write_text(
        json.dumps(manifest_out, indent=2, sort_keys=True), encoding="utf-8"
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
