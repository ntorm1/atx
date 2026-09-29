"""CLI: ``python -m atx_db.licensed {load,mock,substitutes} ...`` (run ``load`` on real data under the memory guard).

* ``load <adapter> --raw DIR [--out DIR] [--no-strict]``: normalize a delivery landed under ``DIR`` (as served, with
  ``receipts.jsonl``) against the lake identity histories; writes ``<lake>/licensed_<adapter>/`` by default.
* ``mock <adapter> --raw DIR``: write the adapter's vendor-layout mock delivery (for dry runs of ``load``).
* ``substitutes``: resolve every adapter's free substitute stage in the lake (existence, columns, manifest status).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import ADAPTERS, get
from .contract import LAKE_ROOT


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m atx_db.licensed", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    ld = sub.add_parser("load")
    ld.add_argument("adapter", choices=sorted(ADAPTERS))
    ld.add_argument("--raw", type=Path, required=True)
    ld.add_argument("--out", type=Path, default=None)
    ld.add_argument("--no-strict", action="store_true")
    mk = sub.add_parser("mock")
    mk.add_argument("adapter", choices=sorted(ADAPTERS))
    mk.add_argument("--raw", type=Path, required=True)
    sub.add_parser("substitutes")
    args = ap.parse_args(argv)
    if args.cmd == "mock":
        print([p.name for p in get(args.adapter).mock(args.raw)])
        return 0
    if args.cmd == "substitutes":
        out = {}
        for name in sorted(ADAPTERS):
            s = get(name).substitute()
            out[name] = s.resolve() if s else None
        print(json.dumps(out, indent=1, default=str))
        return 0
    a = get(args.adapter)
    stage = a.load(args.raw, out_dir=args.out or (LAKE_ROOT / a.stage_name), strict=not args.no_strict)
    print(json.dumps({"stats": stage.report.stats, "rejects": stage.rejects,
                      "failures": stage.report.failures, "out": str(stage.out_dir)}, indent=1, default=str))
    return 0 if not stage.report.failures else 1


if __name__ == "__main__":
    sys.exit(main())
