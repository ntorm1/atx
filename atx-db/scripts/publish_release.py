"""Operator entry point for a scheduled full-universe publication.

Publishes through ``atx_db.cli publish-release`` (read-only pending-migration preflight,
bounded session, the one publication code path), then reads the manifest it wrote and
reports the release eligibility. Exit codes: 0 = ``eligible``;
``atx_db.publication.CANDIDATE_EXIT_CODE`` (3) = written, but only a ``candidate`` (the
gates that did not pass are printed); 1 = error (nothing published); 2 = usage error.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from atx_db import cli
from atx_db.publication import MANIFEST_NAME, read_release_manifest, release_exit_code


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    code = cli.main(["publish-release", *args])
    if code != 0:
        return code
    # cli.main validated both (required) flags; the release directory is <out-dir>/<release-id>.
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--release-id", required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    located, _ = parser.parse_known_args(args)
    manifest = read_release_manifest(located.out_dir.resolve() / located.release_id / MANIFEST_NAME)
    eligibility = manifest.get("eligibility")
    gates = manifest.get("gates")
    gate_status = (
        {name: gate.get("status") if isinstance(gate, dict) else None for name, gate in gates.items()}
        if isinstance(gates, dict)
        else {}
    )
    print(
        json.dumps(
            {
                "release_id": manifest.get("release_id"),
                "eligibility": eligibility,
                "gates_not_passed": manifest.get("gates_not_passed"),
                "gate_status": gate_status,
            },
            sort_keys=True,
        )
    )
    return release_exit_code(eligibility)


if __name__ == "__main__":
    raise SystemExit(main())
