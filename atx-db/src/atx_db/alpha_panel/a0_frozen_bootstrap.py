"""Explicit frozen53 data-root compatibility, without editing the frozen archive."""
from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.util
import json
import os
import runpy
import sys
from pathlib import Path

COMMIT = "53a1807d18a32528e7ffff716d395ea438c9ca15"
MODULE = "atx_db.alpha_panel.a0_check"
CONSTANTS = ("DEFAULT_ROOT", "COMPANYFACTS_DIR", "FSDS_DIR", "IDENTITY_R4_DIR")
ENVIRONMENT = ("ATX_A0_S1", "ATX_A0_S2", "ATX_ALPHA_PANEL_ROOT", "PYTHONPATH",
               "PYTHONDONTWRITEBYTECODE", "OPENBLAS_NUM_THREADS", "ATX_GUARD_JOB")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    split = sys.argv.index("--")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter-sha256", required=True)
    parser.add_argument("--binding-pin", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args(sys.argv[1:split])
    inner = sys.argv[split + 1:]
    if len(inner) < 3 or inner[:2] != ["-m", MODULE] or inner[2] not in ("seed", "stage", "unseed"):
        raise SystemExit("compatibility supports only frozen53 seed/stage/unseed")
    binding = json.loads(args.binding_pin.read_text(encoding="utf-8"))
    adapter = Path(__file__).resolve()
    adapter_hash = digest(adapter)
    if (binding["mode"] != "frozen53-v1" or binding["commit"] != COMMIT
            or adapter_hash != args.adapter_sha256 or adapter_hash != binding["adapter_sha256"]
            or adapter != Path(binding["adapter"]).resolve()):
        raise SystemExit("compatibility adapter binding changed")

    def option(name: str) -> Path:
        if inner.count(name) != 1:
            raise SystemExit(f"exactly one {name} required")
        return Path(inner[inner.index(name) + 1]).resolve()

    code, data = option("--code-root"), option("--data-root")
    if code != Path(binding["code_root"]).resolve() or data != Path(binding["data_root"]).resolve():
        raise SystemExit("compatibility code/data root binding changed")
    arm_env = binding["arm_env"].get(str(option("--root")))
    if arm_env is None or any(os.environ.get(name) != value
                             for name, value in {**binding["frozen_env"], **arm_env}.items()):
        raise SystemExit("compatibility effective environment changed")
    if adapter.is_relative_to(code):
        raise SystemExit("adapter must remain outside the immutable archive")
    arms = json.loads(Path(binding["arms_pin"]).read_text(encoding="utf-8"))
    if arms["commit"] != COMMIT or Path(arms["code"]).resolve() != code:
        raise SystemExit("frozen arm binding changed")
    src = code / "atx-db" / "src"
    for rel, expected in arms["modules"].items():
        blob = (src / rel).read_bytes()
        actual = {"sha256": hashlib.sha256(blob).hexdigest(),
                  "sha256_lf": hashlib.sha256(blob.replace(b"\r\n", b"\n")).hexdigest()}
        if actual != expected:
            raise SystemExit(f"frozen export changed: {rel}")
    common = importlib.import_module("atx_db.alpha_panel.common")
    package = importlib.import_module("atx_db")
    target = importlib.util.find_spec(MODULE)
    if (not Path(common.__file__).resolve().is_relative_to(src.resolve())
            or not Path(package.__file__).resolve().is_relative_to(src.resolve())
            or target is None or not Path(target.origin).resolve().is_relative_to(src.resolve())):
        raise SystemExit("bootstrap module imported outside frozen export")
    package_data = Path(common.PACKAGE_ROOT) / "data"
    before = {name: str(getattr(common, name)) for name in CONSTANTS}
    for name in CONSTANTS:
        value = Path(getattr(common, name))
        if value.is_relative_to(package_data):
            setattr(common, name, data / value.relative_to(package_data))
    receipt = {"status": "bindings_verified", "binding": binding, "adapter_sha256": adapter_hash,
               "actual_argv": sys.orig_argv, "inner_argv": inner,
               "effective_env": {name: os.environ.get(name) for name in ENVIRONMENT},
               "before": before, "after": {name: str(getattr(common, name)) for name in CONSTANTS},
               "frozen_module_sha256": arms["modules"]}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    with args.receipt.open("x", encoding="utf-8") as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")
    sys.argv = [MODULE, *inner[2:]]
    runpy.run_module(MODULE, run_name="__main__", alter_sys=True)


if __name__ == "__main__":
    main()
