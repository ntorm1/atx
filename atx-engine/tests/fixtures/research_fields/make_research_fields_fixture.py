"""Synthetic fixture of the research field-builder identity test (platform core migration, slice 1; lane YARCH).

Writes synthetic inputs, runs the EXISTING Python builder (``atx-engine/tools/prepare_research_fields.py``, function
``run``) on them for ``si_shares``, ``si_dtc`` (FINRA as-of, publication-lagged) and ``vol_126`` (role volume mean),
and keeps its outputs. The C++ library ``atx-engine-research-fields`` reads the same inputs in the gtest
``ResearchFieldsFixture.*`` and must reproduce every payload byte and every coverage number.

  python make_research_fields_fixture.py [--out DIR]     (default: this directory)

Layout under DIR:
  role/       atx.recent-research-role/v1 subset: manifest.json, sessions.i64, ids.u64, member.u8, present.u8,
              volume.f64 (every session is in 2021-2022, inside TRAIN; nothing real is read)
  finra/      dissemination_schedule.csv, asof/si_shares.csv (LF), asof/si_dtc.csv (CRLF), asof/manifest.json
  expected/   si_shares.f64, si_dtc.f64, vol_126.f64 (the Python builder's bytes) and manifest.normalized.json: the
              Python manifest with absolute paths under DIR written as "<fixture>/<posix path>" and the Python code
              identity keys (code_sha256*, code_git_blob_sha1, producer) dropped, so a builder edit that leaves every
              field byte unchanged leaves the fixture unchanged.

One synthetic FINRA row is dated 2024-02-07: it is the reader-side seal probe (dropped and counted by both sides).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile

import numpy as np

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[2] / "tools"          # atx-engine/tools
DAY_NS = 86_400_000_000_000
IDS = [11, 22, 33, 44, 55, 66, 77, 88]
N_SESSIONS = 300
FIRST = dt.date(2021, 1, 4)
FIELDS = ["si_shares", "si_dtc", "vol_126"]
SEALED_DISSEMINATION = "2024-02-07"
CODE_KEYS = {"code_sha256", "code_sha256_lf", "code_git_blob_sha1", "producer"}


def sessions() -> list[dt.date]:
    out, d = [], FIRST
    while len(out) < N_SESSIONS:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def role_arrays(nd: int):
    """member, present, volume (date-major) with every vol_126 exclusion path exercised."""
    rng = np.random.default_rng(20261002)
    n = len(IDS)
    member = np.ones((nd, n), dtype="u1")
    member[:50, 7] = 0                                   # 88: not a member early
    member[rng.random((nd, n)) < 0.05] = 0               # scattered non-member cells
    present = np.ones((nd, n), dtype="u1")
    volume = np.exp(rng.uniform(np.log(1e2), np.log(1e9), (nd, n)))   # wide range: summation order matters
    volume[:, 2] = np.round(volume[:, 2])
    present[rng.random(nd) < 0.6, 2] = 0                 # 33: present ~40% -> often fewer than 63 sessions
    present[:, 4] = 0                                    # 55: never present
    present[rng.random(nd) < 0.1, 3] = 0                 # 44: absent days with a finite volume (excluded)
    bad = rng.random(nd) < 0.08
    volume[bad, 1] = np.nan                              # 22: present, NaN volume
    volume[rng.random(nd) < 0.05, 1] = -5.0              # 22: negative volume (excluded)
    volume[rng.random(nd) < 0.03, 1] = -0.0              # 22: negative zero (kept: >= 0)
    volume[rng.random(nd) < 0.03, 1] = 0.0
    volume[150, 5] = np.inf                              # 66: infinite volume (excluded)
    volume[present == 0] = np.where(rng.random(int((present == 0).sum())) < 0.5, np.nan, 7.0)
    return member, present, volume


def write_role(root: Path) -> str:
    root.mkdir(parents=True)
    days = sessions()
    member, present, volume = role_arrays(len(days))
    blobs = {"sessions.i64": np.array([(d - dt.date(1970, 1, 1)).days * DAY_NS for d in days], dtype="<i8").tobytes(),
             "ids.u64": np.array(IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "present.u8": present.tobytes(), "volume.f64": volume.astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": sha(blob)}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": len(days),
                "instruments": len(IDS), "instrument_namespace": "spiderrock.securityID", "score_begin": 130,
                "score_end": len(days), "source_sha256": "0" * 64, "files": files,
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    blob = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    (root / "manifest.json").write_bytes(blob)
    return sha(blob)


def dissemination_dates() -> list[str]:
    """Synthetic twice-monthly dissemination dates, 2020-12 .. 2022-03, plus the sealed probe date."""
    out = []
    for year, month in [(2020, 12)] + [(y, m) for y in (2021, 2022) for m in range(1, 13)]:
        if (year, month) > (2022, 3):
            break
        for day in (9, 24):
            out.append(dt.date(year, month, day).isoformat())
    return out + [SEALED_DISSEMINATION]


SI_SHARES = [  # (security_id, available_at, value): unsorted on purpose
    (22, "2021-01-09", "500000"), (11, "2021-01-24", "1250000.5"), (11, "2020-12-24", "1000000"),
    (11, "2021-02-09", "nan"), (11, "2021-02-24", "1300000"), (11, "2021-06-09", "1e6"),
    (11, "2021-09-24", ".5"), (11, SEALED_DISSEMINATION, "999"),
    (22, "2021-03-09", ""), (22, "2021-03-24", "750000"), (22, "2022-01-24", "-0"),
    (33, "2021-04-09", "42"), (33, "2021-04-24", "43.25"), (33, "2021-12-09", "1.5e2"),
    (44, "2021-05-24", "123456789"), (44, "2021-07-09", "2.5E-1"),
    (77, "2020-12-09", "3"), (77, "2021-11-24", "4"), (77, "2022-02-09", "5"),
    (99, "2021-01-24", "1"),                             # not on the role axis: ignored, counted
]
SI_DTC = [
    (11, "2021-01-09", "1.00"), (11, "2021-05-24", "2.75"), (22, "2021-06-24", "1.5"),
    (44, "2021-08-09", "NaN"), (88, "2021-10-09", "3.125"), (88, "2022-01-09", "1"),
]


def write_finra(root: Path) -> None:
    (root / "asof").mkdir(parents=True)
    lines = ["settlement_date,due_date,dissemination_date,source,third_column_label,in_download,source_url"]
    for d in dissemination_dates():
        settle = (dt.date.fromisoformat(d) - dt.timedelta(days=9)).isoformat()
        lines.append(f"{settle},x,{d},official,Publication Date,True,https://example.invalid/{d}")
    (root / "dissemination_schedule.csv").write_bytes(("\n".join(lines) + "\n").encode())
    outputs = {}
    for name, rows, eol in (("si_shares", SI_SHARES, "\n"), ("si_dtc", SI_DTC, "\r\n")):
        blob = ("security_id,available_at,value" + eol + "".join(f"{a},{b},{c}{eol}" for a, b, c in rows)).encode()
        (root / "asof" / f"{name}.csv").write_bytes(blob)
        outputs[name] = {"sha256": sha(blob)}
    (root / "asof" / "manifest.json").write_bytes((json.dumps({"outputs": outputs}, indent=2) + "\n").encode())


def normalized(value, root: str):
    """The manifest with absolute paths under the fixture root made relative and the code identity keys dropped."""
    if isinstance(value, dict):
        return {k: normalized(v, root) for k, v in value.items() if k not in CODE_KEYS}
    if isinstance(value, list):
        return [normalized(v, root) for v in value]
    if isinstance(value, str) and value.lower().startswith(root.lower()):
        return "<fixture>/" + Path(value[len(root):].lstrip("\\/")).as_posix()
    return value


def build(out: Path) -> None:
    """Write the inputs under ``out`` and the Python builder's outputs under ``out/expected``."""
    sys.path.insert(0, str(TOOLS))
    import prepare_research_fields as builder   # the EXISTING Python builder
    for sub in ("role", "finra", "expected"):
        if (out / sub).exists():
            shutil.rmtree(out / sub)
    role_sha = write_role(out / "role")
    write_finra(out / "finra")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp) / "fields"
        manifest = builder.run(out / "role", role_sha, work, FIELDS, finra=out / "finra", module_options={})
        (out / "expected").mkdir()
        for name in FIELDS:
            shutil.copyfile(work / f"{name}.f64", out / "expected" / f"{name}.f64")
    root = str((out).resolve())
    text = json.dumps(normalized(manifest, root), indent=2, sort_keys=True, allow_nan=False) + "\n"
    (out / "expected" / "manifest.normalized.json").write_bytes(text.encode())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--out", type=Path, default=HERE)
    a = ap.parse_args(argv)
    build(a.out.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
