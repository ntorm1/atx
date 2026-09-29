"""``python -m atx_db.lake {list,verify,catalog,plan,run} ...`` (see docs/LAKE.md)."""

from __future__ import annotations

import sys


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    cmd = args.pop(0) if args else "list"
    if cmd == "verify":
        from .verify import main as run
        return run(args)
    if cmd == "catalog":
        from .catalog import main as run
        return run(args)
    if cmd in ("plan", "run"):
        from .orchestrate import main as run
        return run(["--dry-run" if cmd == "plan" else "--run", *args])
    if cmd == "list":
        from . import registry
        for s in registry.load():
            flag = " (planned)" if s.planned else ""
            print(f"{s.name:<32} {s.lane:<5} {s.manifest:<48} inputs={','.join(s.inputs) or '-'}{flag}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
