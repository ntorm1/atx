"""Synthetic fixture of the holdings-field identity tests (P9 lane A4, migration slice 5).

Writes lane T2's re-dated synthetic holdings stages and role (the writers of ``atx-engine/tools/
test_research_fields_holdings.py``, imported, so the data has one source) and runs the EXISTING Python builder
(``prepare_research_fields.run`` with its late module ``research_fields_holdings``) on them for the test module's recipe
(``si_shares``, ``shares_out`` and every holdings field). The C++ library reads the same committed inputs in the gtests
``ResearchFieldsHoldings*`` and must reproduce every payload byte and coverage number of the fields it ports.

  python make_holdings_fixture.py [--out DIR]     (default: ./  i.e. this directory)

Layout under DIR (every file pinned: the role by its manifest, each stage file by its stage manifest, the expected
payloads by the gtests):
  role/                 atx.recent-research-role/v1 of the test module (5 lines, the weekdays 2023-05-01 .. 2023-12-29);
                        its source_sha256 is the SHA-256 of the builder-only TickerHistory3 input (not kept)
  thirteenf/ ftd/ regsho_threshold/ security_master/ short_volume_ext/
                        the atx-db alpha-panel v1 stages exactly as the test module writes them (manifest.json + parquet)
  inputs/shares_out.f64 this run's shares_out payload (the input of ftd_shares_ratio21 and the 13F ownership fields)
  expected/             the Python builder's holdings payloads and manifest.normalized.json (paths under DIR made
                        relative, the Python code identity and runtime versions dropped, as make_research_fields_fixture)

The builder's other inputs (TickerHistory3, FINRA, lake) are written to a temporary directory for the run and not kept.
Only synthetic data is read. Rows dated on or after the research seal are the test module's synthetic seal probes.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[2] / "tools"                       # atx-engine/tools
FIXTURES = HERE.parent
STAGES = ("thirteenf", "ftd", "regsho_threshold", "security_master", "short_volume_ext")
KEPT = ("role", *STAGES, "inputs", "expected")


def _modules():
    for path in (str(TOOLS), str(FIXTURES / "research_fields")):
        if path not in sys.path:
            sys.path.insert(0, path)
    import make_research_fields_fixture as base_fx       # normalized()
    import prepare_research_fields as builder            # the EXISTING Python builder
    import research_fields_holdings as hold
    import research_window as rw
    import test_prepare_research_fields as base          # the role / TickerHistory3 / FINRA / lake writers
    import test_research_fields_holdings as t2           # lane T2's re-dated holdings stages
    return base_fx, builder, hold, rw, base, t2


def fields() -> list:
    """The recipe the fixture runs: the test module's RECIPE (si_shares, shares_out, every holdings field)."""
    return list(_modules()[5].RECIPE)


def holdings_fields() -> list:
    return list(_modules()[2].HOLD_FIELDS)


def build(out: Path) -> dict:
    """Write the inputs and the Python builder's outputs under ``out`` (its KEPT entries are replaced); the manifest."""
    base_fx, builder, hold, rw, base, t2 = _modules()
    if rw.WINDOW_ID != rw.current()["WINDOW_ID"] or builder.SEAL != rw.current()["SEAL"]:
        raise RuntimeError(f"the fixture is generated under the repository research window, not {rw.WINDOW_ID}")
    out.mkdir(parents=True, exist_ok=True)
    for sub in KEPT:
        if (out / sub).exists():
            shutil.rmtree(out / sub)
    writers = {"thirteenf": t2.write_thirteenf, "ftd": t2.write_ftd, "regsho_threshold": t2.write_regsho,
               "security_master": t2.write_security_master, "short_volume_ext": t2.write_svx}
    stages = {}
    for key in STAGES:
        stages[key] = out / key
        stages[key + "_sha256"] = writers[key](out / key)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        th = work / "TickerHistory3.parquet"
        base.write_tickerhistory(th)
        base.write_finra(work / "finra")
        base.write_lake(work / "lake")
        role_sha = base.write_role(out / "role", base.sha(th), t2.SESSIONS)
        with contextlib.redirect_stdout(io.StringIO()):
            manifest = builder.run(out / "role", role_sha, work / "fields", t2.RECIPE, finra=work / "finra",
                                   tickerhistory=th, lake=work / "lake", **stages)
        (out / "inputs").mkdir()
        shutil.copyfile(work / "fields" / "shares_out.f64", out / "inputs" / "shares_out.f64")
        (out / "expected").mkdir()
        for name in hold.HOLD_FIELDS:
            shutil.copyfile(work / "fields" / f"{name}.f64", out / "expected" / f"{name}.f64")
        roots = sorted((str(out.resolve()), str(work.resolve())), key=len, reverse=True)
        doc = manifest
        for root in roots:   # the temporary inputs' paths become <fixture>/... too (they are not kept)
            doc = base_fx.normalized(doc, root)
    text = json.dumps(doc, indent=2, sort_keys=True, allow_nan=False) + "\n"
    (out / "expected" / "manifest.normalized.json").write_bytes(text.encode())
    return manifest


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--out", type=Path, default=HERE)
    a = ap.parse_args(argv)
    build(a.out.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
