"""GATE agent 3, FIELDS-SUB step (3) beside the verdict (decides nothing): every difference between the re-based R-2 NAV
and the v8 NAV of record, classified. Prints file names, JSON leaf paths, the KIND of each differing JSON value (sha256
hex / other string / number / bool / absent) and, for CSVs, the differing-cell count per column; never a value.

Classes:
  ALLOWED      the dispatch's allowed list: summary.json /producer/* and /scenarios/<i>/ann_mean_gross (new side only)
  FIELDS       the fields manifest's SHA-256 the substitution writes by construction (/financing_fields/manifest_sha256,
               /source_bindings/research_fields_manifest_sha256)
  DERIVED      a sha256 leaf naming another output file of the same dir (/recipe_sha256, /scenarios/<i>/daily_csv_sha256,
               /v7/files/*, /v7/extras, v7_extras.json /files/*): checked = the SHA-256 of that file on its own side
  M1D-C1       a JSON leaf M1d's written P9-B0 list names for a v8-era NAV reference against a P9 exe:
               summary.json /v7/extras (changed); integration-log.md:251-259 (the list), :336-347 (the result)
  C1-COST      a CSV cell of a cost column (impact_cost_dollars, unrationed_cost_dollars, trade_cost_dollars,
               trade_cost_return) of a daily file of L1-L4 or of capacity/daily_K: the class M1d's P9-B0 list recorded
               for every v8-era NAV reference against a P9 exe (C1 NAV change; integration-log.md:251-259, :345)
  OTHER        anything else
usage: python r2_classify.py REF_DIR NEW_DIR OUT_TXT   exit 0 iff no OTHER
"""
from __future__ import annotations

import csv
import hashlib
import json
import re
import sys
from pathlib import Path

HEX = re.compile(r"[0-9a-f]{64}")
COST = {"impact_cost_dollars", "unrationed_cost_dollars", "trade_cost_dollars", "trade_cost_return"}
FIELDS = {"/financing_fields/manifest_sha256", "/source_bindings/research_fields_manifest_sha256"}


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def leaves(v, p=""):
    if isinstance(v, dict):
        for k, x in v.items():
            yield from leaves(x, f"{p}/{k}")
    elif isinstance(v, list):
        for i, x in enumerate(v):
            yield from leaves(x, f"{p}/{i}")
    else:
        yield p, v


def kind(v) -> str:
    if v is None:
        return "absent"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        return "sha256" if HEX.fullmatch(v) else "string"
    return type(v).__name__


def derived_target(name: str, path: str, doc: dict, root: Path) -> Path | None:
    """The output file a sha256 leaf names, when the document says which."""
    m = re.fullmatch(r"/scenarios/(\d+)/daily_csv_sha256", path)
    if m:
        sc = doc["scenarios"][int(m.group(1))]
        for key in ("daily_csv", "daily_file", "daily"):
            if isinstance(sc.get(key), str):
                return root / Path(name).parent / sc[key]
        nm = sc.get("name") or sc.get("scenario")
        if isinstance(nm, str):
            return root / Path(name).parent / f"daily_{nm}.csv"
    if path == "/recipe_sha256":
        return root / Path(name).parent / "recipe.json"
    if path == "/v7/extras":
        return root / "v7_extras.json"
    m = re.fullmatch(r"/(?:v7/)?files/(.+)", path)
    if m:
        return root / m.group(1)
    return None


def main(ref: Path, new: Path, out: Path) -> int:
    lines: list[str] = []
    say = lines.append
    counts: dict[str, int] = {}
    rf = {p.relative_to(ref).as_posix() for p in ref.rglob("*") if p.is_file()}
    nf = {p.relative_to(new).as_posix() for p in new.rglob("*") if p.is_file()}
    say(f"reference {ref.as_posix()} ({len(rf)} files) new {new.as_posix()} ({len(nf)} files); one side only: "
        f"{sorted(rf ^ nf) or 'none'}")
    for f in sorted(rf ^ nf):
        counts["OTHER"] = counts.get("OTHER", 0) + 1
    for name in sorted(rf & nf):
        a, b = ref / name, new / name
        if sha(a) == sha(b):
            say(f"  same   {name}")
            continue
        if name.endswith(".json"):
            da, db = json.loads(a.read_text(encoding="utf-8")), json.loads(b.read_text(encoding="utf-8"))
            la, lb = dict(leaves(da)), dict(leaves(db))
            for p in sorted(k for k in set(la) | set(lb) if la.get(k, "<absent>") != lb.get(k, "<absent>")):
                va, vb = la.get(p), lb.get(p)
                if name == "summary.json" and (p.startswith("/producer/") or (
                        re.fullmatch(r"/scenarios/\d+/ann_mean_gross", p) and p not in la)):
                    cls = "ALLOWED"
                elif p in FIELDS:
                    cls = "FIELDS"
                elif name == "summary.json" and p == "/v7/extras":
                    cls = "M1D-C1"
                else:
                    cls = "OTHER"
                    ta, tb = derived_target(name, p, da, ref), derived_target(name, p, db, new)
                    if kind(vb) == "sha256" and kind(va) in ("sha256", "absent") and tb is not None:
                        def digest(t: Path) -> str:
                            if p.endswith("/recipe_sha256"):   # the compact-JSON digest of recipe.json
                                return hashlib.sha256(json.dumps(json.loads(t.read_text(encoding="utf-8")),
                                                                 separators=(",", ":")).encode()).hexdigest()
                            return sha(t)
                        ok = tb.is_file() and digest(tb) == vb and (va is None or (
                            ta is not None and ta.is_file() and digest(ta) == va))
                        cls = "DERIVED" if ok else "OTHER"
                        p += (f" ({'added; ' if va is None else ''}names {tb.relative_to(new).as_posix()}: = its "
                              f"digest on {'the new side' if va is None else 'each side'} {ok})")
                counts[cls] = counts.get(cls, 0) + 1
                say(f"  {cls:8s} {name} {p} [{kind(va)} -> {kind(vb)}]")
        elif name.endswith(".csv"):
            ra = list(csv.reader(a.open(newline="")))
            rb = list(csv.reader(b.open(newline="")))
            cols: dict[str, int] = {}
            rows = set()
            for r, (x, y) in enumerate(zip(ra[1:], rb[1:])):
                for j, (u, v) in enumerate(zip(x, y)):
                    if u != v:
                        cols[ra[0][j] if j < len(ra[0]) else f"col{j}"] = cols.get(ra[0][j] if j < len(ra[0]) else
                                                                                     f"col{j}", 0) + 1
                        rows.add(r)
            daily = Path(name).name.startswith("daily_") and "linear-6bps" not in name
            shape = ra[:1] == rb[:1] and len(ra) == len(rb)
            cls = "C1-COST" if daily and shape and cols and set(cols) <= COST else "OTHER"
            counts[cls] = counts.get(cls, 0) + 1
            say(f"  {cls:8s} {name}: header equal {ra[:1] == rb[:1]}; rows {len(ra) - 1} vs {len(rb) - 1}; rows "
                f"differing {len(rows)}; differing cells by column {cols}")
        else:
            counts["OTHER"] = counts.get("OTHER", 0) + 1
            say(f"  OTHER    {name} (binary)")
    say(f"classes: {counts}")
    say(f"VERDICT (classification only): {'no OTHER' if not counts.get('OTHER') else 'OTHER present'}")
    text = "\n".join(lines) + "\n"
    out.write_text(text, encoding="utf-8", newline="\n")
    print(text, end="")
    return 0 if not counts.get("OTHER") else 1


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])))
