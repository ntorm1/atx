"""era_data_audit.py (platform v8 H-1 step 3) on the tiny_world fixture split in two eras.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_era_data_audit.py

No real data. tiny_world (scripts/tests/fixtures/tiny_world.py) is built in a temporary root, then cut into two era
roles on its session axis (rows 0..519 scoring 384..519, rows 136..655 scoring 520..655), each with its fields. A
return field (mkt_ret), a delisting block carrying return values and fields value statistics are added so the test can
prove, with an audit hook on every file open, that the audit opens no return and reports no value statistic.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

import numpy as np
import pytest

import era_data_audit as EA

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "tests" / "fixtures"))
import tiny_world as TW  # noqa: E402

DAY = 86_400_000_000_000
SENTINELS = (0.3712345, -0.4412345, 123.456789)   # values the audit must never report
OPENED: list[str] | None = None


def _hook(event, args):
    if OPENED is not None and event == "open" and args and isinstance(args[0], (str, bytes, Path)):
        OPENED.append(str(args[0]).replace("\\", "/"))


sys.addaudithook(_hook)


def opened_by(call):
    global OPENED
    OPENED = []
    try:
        result = call()
    finally:
        seen, OPENED = OPENED, None
    return result, seen


# ------------------------------------------------------------------ the tiny_world eras
def write_payloads(d: Path, arrays: dict) -> dict:
    d.mkdir(parents=True)
    out = {}
    for name, arr in arrays.items():
        data = np.ascontiguousarray(arr).tobytes()
        (d / name).write_bytes(data)
        out[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return out


def write_json(path: Path, doc: dict) -> str:
    data = (json.dumps(doc, indent=2, sort_keys=True) + "\n").encode()
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def era_dirs(world: Path, dst: Path, lo: int, hi: int, *, member_exit=(), delisting=None, coverage_for=()):
    """An era role (rows lo..hi-1 of tiny_world's role, score_begin 384) and its fields dir (tiny_world's fields plus
    mkt_ret). Returns (role manifest, role sha, fields manifest, fields sha, arrays)."""
    role = json.loads((world / "role" / "manifest.json").read_bytes())
    d, n = role["dates"], role["instruments"]
    load = lambda name, dtype: np.fromfile(world / "role" / name, dtype=dtype)  # noqa: E731  (the fixture's own)
    arrays = {"sessions.i64": load("sessions.i64", "<i8")[lo:hi], "ids.u64": load("ids.u64", "<u8")}
    for name, dtype in (("close.f64", "<f8"), ("raw_close.f64", "<f8"), ("volume.f64", "<f8"), ("present.u8", "u1"),
                        ("member.u8", "u1")):
        arrays[name] = load(name, dtype).reshape(d, n)[lo:hi].copy()
    for j, row in member_exit:
        arrays["member.u8"][row:, j] = 0
    files = write_payloads(dst / "role", arrays)
    s = arrays["sessions.i64"]
    manifest = dict(role, dates=hi - lo, score_begin=TW.SCORE_BEGIN, score_end=hi - lo,
                    score_start_ns=int(s[TW.SCORE_BEGIN]), score_end_ns=int(s[-1]) + DAY, files=files)
    manifest.pop("score_member_counts", None)
    if delisting is not None:
        manifest["universe"] = {"id": "tiny-v1", "delisting": delisting}
    role_sha = write_json(dst / "role" / "manifest.json", manifest)
    fields_doc = json.loads((world / "fields" / "manifest.json").read_bytes())
    payloads = {f"{r['name']}.f64": np.fromfile(world / "fields" / r["file"], dtype="<f8").reshape(d, n)[lo:hi].copy()
                for r in fields_doc["fields"]}
    payloads["mkt_ret.f64"] = np.full((hi - lo, n), 0.001)
    ffiles = write_payloads(dst / "fields", payloads)
    rows = [dict(r, sha256=ffiles[r["file"]]["sha256"], shape=[hi - lo, n]) for r in fields_doc["fields"]]
    rows.append(dict(rows[0], name="mkt_ret", file="mkt_ret.f64", sha256=ffiles["mkt_ret.f64"]["sha256"]))
    member = arrays["member.u8"] != 0
    years = np.array([int(EA.iso(x)[:4]) for x in s])
    for r in rows:
        if r["name"] in coverage_for:
            finite = np.isfinite(payloads[r["file"]]) & member
            r["coverage"] = {"per_year": {str(y): {"member_cells": int(member[years == y].sum()),
                                                   "finite_member_cells": int(finite[years == y].sum()),
                                                   "finite_member_frac": 1.0} for y in sorted(set(years.tolist()))},
                             "member_finite_min": SENTINELS[1], "member_finite_max": SENTINELS[2],
                             "member_finite_mean": SENTINELS[0]}
    fdoc = dict(fields_doc, role=dict(fields_doc["role"], manifest_sha256=role_sha, dates=hi - lo), fields=rows,
                files=ffiles)
    fields_sha = write_json(dst / "fields" / "manifest.json", fdoc)
    return dst / "role" / "manifest.json", role_sha, dst / "fields" / "manifest.json", fields_sha, arrays, payloads


@pytest.fixture(scope="module")
def eras(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("audit")
    TW.build(tmp / "tw", bin_dir=tmp / "bin")
    ids = np.fromfile(tmp / "tw" / "role" / "ids.u64", dtype="<u8")
    delisting = {"rule": "test", "counts": {"terminations": 3},
                 "by_cause": {"merger": {"terminations": 2, "kept_member_at_last_session": 1, "with_return": 2},
                              "dropped": {"terminations": 1, "kept_member_at_last_session": 1, "with_return": 1}},
                 "events": [{"security_id": int(ids[3]), "delist_code": "merger", "kept_member_at_last_session": True,
                             "delist_return": SENTINELS[0], "delist_return_if_performance": SENTINELS[1]},
                            {"security_id": int(ids[5]), "delist_code": "merger", "kept_member_at_last_session": False,
                             "delist_return": SENTINELS[1]},
                            {"security_id": int(ids[9]), "delist_code": "dropped", "kept_member_at_last_session": True,
                             "delist_return": SENTINELS[2]}]}
    e1 = era_dirs(tmp / "tw", tmp / "e1", 0, 520, member_exit=((3, 300), (5, 410)), delisting=delisting)
    e2 = era_dirs(tmp / "tw", tmp / "e2", 136, TW.DATES, coverage_for=("shares_out",))
    library = json.loads((tmp / "tw" / TW.LIBRARY).read_bytes())["candidates"]
    library.append({"id": "reads_a_return", "dsl": "rank(mkt_ret)"})
    plan = {"candidates": [{"id": "planted_a", "required_lookback": 20, "extra_fields": ["si_shares", "shares_out"]},
                           {"id": "planted_b", "required_lookback": 0, "extra_fields": ["tiny_signal"]}]}
    (tmp / "plan.json").write_text(json.dumps(plan))
    return {"tmp": tmp, "e1": e1, "e2": e2, "library": library, "plan": tmp / "plan.json"}


def run_audit(t):
    return EA.audit([EA.Era("E1", *t["e1"][:4]), EA.Era("E2", *t["e2"][:4])], t["library"], EA.load_plan(t["plan"]))


# ------------------------------------------------------------------ the tests
def test_the_audit_opens_no_return(eras):
    doc, seen = opened_by(lambda: run_audit(eras))
    names = [Path(p).name for p in seen]
    for never in ("close.f64", "raw_close.f64", "mkt_ret.f64"):
        assert never not in names, never
    for read in ("member.u8", "present.u8", "sessions.i64", "shares_out.f64", "tiny_signal.f64"):
        assert read in names, read                              # the hook sees the audit's opens
    body = json.dumps(doc["eras"])
    for key in EA.VALUE_KEYS + EA.DELIST_VALUE_KEYS:
        assert key not in body, key
    for value in SENTINELS:
        assert repr(value) not in body, value
    assert all(e["skipped_return_fields"] == ["mkt_ret"] for e in doc["eras"])


def test_coverage_by_year_computed_or_from_the_manifest(eras):
    doc = run_audit(eras)
    e1, e2 = doc["eras"]
    _, _, _, _, arrays, payloads = eras["e1"]
    member = arrays["member.u8"] != 0
    years = np.array([int(EA.iso(s)[:4]) for s in arrays["sessions.i64"]])
    got = e1["coverage"]["si_shares"]
    assert got["source"] == "computed"
    for y, v in got["per_year"].items():
        m = member[years == int(y)]
        f = np.isfinite(payloads["si_shares.f64"][years == int(y)]) & m
        assert (v["member_cells"], v["finite_member_cells"]) == (int(m.sum()), int(f.sum())), y
    assert set(got["per_year"]) == {str(y) for y in sorted(set(years.tolist()))}
    fields = json.loads(eras["e2"][2].read_bytes())
    want = next(r for r in fields["fields"] if r["name"] == "shares_out")["coverage"]["per_year"]
    assert e2["coverage"]["shares_out"] == {"source": "manifest", "per_year": want}
    assert e2["coverage"]["tiny_signal"]["source"] == "computed"


def test_first_valid_per_field_and_per_candidate(eras):
    e1, e2 = run_audit(eras)["eras"]
    s1 = eras["e1"][4]["sessions.i64"]
    s2 = eras["e2"][4]["sessions.i64"]
    warm = TW.MEMBER_WARMUP
    assert e1["first_valid"]["fields"]["shares_out"] == EA.iso(s1[warm]) == e1["first_valid"]["fields"]["close"]
    assert e2["first_valid"]["fields"]["tiny_signal"] == EA.iso(s2[0])      # every row of era 2 is a member row
    c = e1["first_valid"]["candidates"]
    assert c["planted_a"] == {"fields": ["shares_out", "si_shares"], "missing_fields": [], "required_lookback": 20,
                              "first_valid": EA.iso(s1[warm + 20])}
    assert c["noise_c"]["first_valid"] == EA.iso(s1[warm]) and c["noise_c"]["required_lookback"] is None
    assert c["reads_a_return"] == {"fields": ["mkt_ret"], "missing_fields": ["mkt_ret"], "required_lookback": None,
                                   "first_valid": None}


def test_delisting_share_and_mask_exits(eras):
    e1, e2 = run_audit(eras)["eras"]
    d = e1["delisting"]
    assert (d["terminations"], d["kept_member_at_last_session"], d["share_kept"]) == (3, 2, round(2 / 3, 6))
    assert d["mask_exits"] == {"names": 2, "delisted": 2, "share_delisted": 1.0}
    assert d["by_cause"]["merger"] == {"terminations": 2, "kept_member_at_last_session": 1}
    assert e2["delisting"]["terminations"] is None and e2["delisting"]["mask_exits"] == {"names": 0}


def test_cli_text_and_json(eras, tmp_path, capsys):
    t = eras
    lib = t["tmp"] / "tw" / TW.LIBRARY
    argv = ["--era", "E1", *map(str, t["e1"][:4]), "--era", "E2", *map(str, t["e2"][:4]), "--library", str(lib),
            "--library-sha256", hashlib.sha256(lib.read_bytes()).hexdigest(), "--plan", str(t["plan"]),
            "--json", str(tmp_path / "a.json")]
    code, seen = opened_by(lambda: EA.main(argv))
    assert code == 0 and not {"close.f64", "raw_close.f64", "mkt_ret.f64"} & {Path(p).name for p in seen}
    out = capsys.readouterr().out
    assert "== era E1:" in out and "skipped (return token, never opened): mkt_ret" in out
    assert "candidate planted_a: first valid" in out
    doc = json.loads((tmp_path / "a.json").read_text())
    assert [e["id"] for e in doc["eras"]] == ["E1", "E2"] and doc["schema"] == EA.SCHEMA


def test_refusals(eras, tmp_path, capsys):
    t = eras
    role_path, role_sha, fields_path, fields_sha = t["e1"][:4]
    with pytest.raises(EA.AuditError, match="SHA-256 differs"):
        EA.Era("E1", role_path, "0" * 64, fields_path, fields_sha)
    with pytest.raises(EA.AuditError, match="bound to its role"):
        EA.Era("E2", t["e2"][0], t["e2"][1], fields_path, fields_sha)     # era 1's fields on era 2's role
    # a role reaching the seal is refused on its manifest, before any payload is opened
    doc = json.loads(role_path.read_bytes())
    doc["score_end_ns"] = EA.rw.SEAL_NS + DAY
    (tmp_path / "sealed").mkdir()
    sealed_sha = write_json(tmp_path / "sealed" / "manifest.json", doc)
    code, seen = opened_by(lambda: EA.main(["--era", "E9", str(tmp_path / "sealed" / "manifest.json"), sealed_sha,
                                            str(fields_path), fields_sha]))
    assert code == 2 and "seal" in capsys.readouterr().err
    assert [Path(p).name for p in seen if Path(p).suffix not in (".py", ".pyc")] == ["manifest.json"]
    with pytest.raises(EA.AuditError, match="never opened"):
        EA.Era("E1", *t["e1"][:4]).payload("close.f64", "<f8", 1)


@pytest.mark.parametrize("name, is_return", [
    ("mkt_ret", True), ("ret21", True), ("dlret", True), ("stock_returns", True), ("fwd_1d", True),
    ("forward_return", True), ("ret", True), ("reversal", False), ("retail_flow", False), ("tiny_signal", False),
    ("shares_out", False), ("sv_ratio126", False)])
def test_return_tokens(name, is_return):
    assert EA.is_return_field(name) is is_return
