"""M2d root item (iii), part 2: D2 flag-absent identity on the fitter's synthetic fixture (aim_world as in
test_composition_engine_fit.World: two themes, five admitted members; registry absent). Synthetic, 0 trials.

  build TOOLS_DIR ROOT            write the fixture under ROOT/fx with TOOLS_DIR's test helpers; ROOT/base-argv.json
  run   TOOLS_DIR ROOT TAG        run every case flag-absent with TOOLS_DIR's fitter into ROOT/TAG-<case>/
  cmp   ROOT OUT_JSON             compare ROOT/old-<case> with ROOT/new-<case> (SHA-256 + JSON path; no value printed)
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

CASES = {
    # the P9 lineage parent's form (Y-F0: ew-theme-std-v1 + theme-erc-v1)
    "std-erc": ["--orientation", "prior", "--composition", "ew-theme-std-v1", "--theme-erc", "theme-erc-v1"],
    "std-erc-tsmom": ["--orientation", "prior", "--composition", "ew-theme-std-v1", "--theme-erc", "theme-erc-v1",
                      "--theme-tsmom", "theme-tsmom-v1"],
    "shrink": ["--orientation", "prior", "--composition", "ic-shrink-v1"],
    "shrink-erc": ["--orientation", "prior", "--composition", "ic-shrink-v1", "--theme-erc", "theme-erc-v1"],
    "shrink-tsmom": ["--orientation", "prior", "--composition", "ic-shrink-v1", "--theme-tsmom", "theme-tsmom-v1"],
    "std": ["--orientation", "prior", "--composition", "ew-theme-std-v1"],
    "std-aim-resid": ["--orientation", "prior", "--composition", "ew-theme-std-aim-v1", "--theme-resid",
                      "theme-resid-v1"],
    # an R-11 child: the "std" parent plus --theme-resid, checked against that parent's file
    "std-resid-child": ["--orientation", "prior", "--composition", "ew-theme-std-v1", "--theme-resid",
                        "theme-resid-v1", "--theme-resid-parent", "@PARENT:std"],
    "ew-theme": ["--orientation", "prior", "--composition", "ew-theme-v1"],
}
OUTPUT = "@OUTPUT"


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def build(tools: Path, root: Path) -> int:
    sys.path.insert(0, str(tools))
    import test_fit_composition_weights as tfw
    panel, signals, ids, extra = tfw.aim_world()
    extra = dict(extra)
    extra["flip"] = dict(extra["flip"], theme="value")
    extra["medium_half"] = dict(extra["medium_half"], tier="C+")
    fx = tfw.Fixture(root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                     candidate_extra=extra)
    (root / "base-argv.json").write_text(json.dumps(fx.argv(Path(OUTPUT), "v4-prior-v1")), encoding="utf-8")
    return 0


def run(tools: Path, root: Path, tag: str) -> int:
    sys.path.insert(0, str(tools))
    import composition_rules as cr
    import fit_composition_weights as fcw
    cr.REGISTRY_PATH = root / "no-registry.json"       # as World / EngineFitEndToEnd: the declared tiers score
    base = json.loads((root / "base-argv.json").read_text(encoding="utf-8"))
    codes = {}
    for name, extra in CASES.items():
        out = root / f"{tag}-{name}"
        argv = [str(out) if a == OUTPUT else a for a in base]
        flags = []
        for a in extra:
            if a.startswith("@PARENT:"):
                parent = root / f"{tag}-{a.split(':', 1)[1]}" / "composition_weights.json"
                flags += [str(parent), "--theme-resid-parent-sha256", sha(parent)]
            else:
                flags.append(a)
        code, _ = fcw.fit(fcw.parse_args(argv + flags))
        codes[name] = code
    (root / f"{tag}-codes.json").write_text(json.dumps(codes, indent=1), encoding="utf-8")
    print(tag, codes)
    return 0


def flatten(doc, prefix="$"):
    out = {}
    if isinstance(doc, dict):
        for k, v in doc.items():
            out.update(flatten(v, f"{prefix}.{k}"))
        if not doc:
            out[prefix] = "{}"
    elif isinstance(doc, list):
        for i, v in enumerate(doc):
            out.update(flatten(v, f"{prefix}[{i}]"))
        if not doc:
            out[prefix] = "[]"
    else:
        out[prefix] = json.dumps(doc, sort_keys=True)
    return out


def cmp(root: Path, out_json: Path) -> int:
    report = {"codes": {t: json.loads((root / f"{t}-codes.json").read_text()) for t in ("old", "new")}, "cases": {}}
    for name in CASES:
        old, new = root / f"old-{name}", root / f"new-{name}"
        fo = {p.name for p in old.iterdir() if p.is_file()} if old.is_dir() else set()
        fn = {p.name for p in new.iterdir() if p.is_file()} if new.is_dir() else set()
        row = {"only_old": sorted(fo - fn), "only_new": sorted(fn - fo), "files": {}}
        for f in sorted(fo & fn):
            a, b = old / f, new / f
            r = {"identical": sha(a) == sha(b)}
            if not r["identical"] and f.endswith(".json"):
                xa, xb = flatten(json.loads(a.read_bytes())), flatten(json.loads(b.read_bytes()))
                r["paths_only_old"] = sorted(set(xa) - set(xb))
                r["paths_only_new"] = sorted(set(xb) - set(xa))
                r["paths_value_differs"] = sorted(k for k in set(xa) & set(xb) if xa[k] != xb[k])
            elif not r["identical"]:
                r["note"] = "non-JSON file differs"
            row["files"][f] = r
        report["cases"][name] = row
    out_json.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "build":
        raise SystemExit(build(Path(sys.argv[2]), Path(sys.argv[3])))
    if mode == "run":
        raise SystemExit(run(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]))
    raise SystemExit(cmp(Path(sys.argv[2]), Path(sys.argv[3])))
