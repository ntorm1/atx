"""research_fields_gold (lane XDATA task GOLD): a synthetic gold-pipeline build root only, never atx-db's.

The world: an alpha-panel build root with a ``gold/`` stage (year files 2018 and 2019 holding keys, cik, groups, a
control ``ctl_hl_spread_21``, an admitted score ``g_mom_12_1`` and a label column ``fwd_ret_21`` that must never be
read) and a ``characteristics/`` stage (``iv_term_slope`` and a label column ``label_h21``). Each 2019 file ends with a
row group dated on or after the seal (pruned by its statistics), holds one duplicated key and a line off the role;
a partition dated in the seal year holds garbage bytes (opening it would fail). The role is the NYSE sessions
2018-07-02 .. 2019-04-30 for three lines."""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_research_fields as tool
import prepare_research_fields_xdata as xreg
import research_fields_gold as gold
import research_fields_sec as sec
import research_window as rw

EPOCH = dt.date(1970, 1, 1)
DAY_NS = 86_400_000_000_000
TOOLS = Path(__file__).resolve().parent
ROLE_FIRST, ROLE_LAST = dt.date(2018, 7, 2), dt.date(2019, 4, 30)
PANEL_FIRST, PANEL_LAST = dt.date(2018, 6, 1), dt.date(2019, 5, 31)
ROLE_IDS = [11, 22, 33]
PANEL_IDS = [11, 22, 33, 44]                  # 44 is off the role
DUP = (dt.date(2019, 2, 4), 22)
GAPS = {(dt.date(2018, 9, 4), 11), (dt.date(2019, 1, 15), 33)}
FIELDS = list(xreg.FIELDS_GOLD_DRAFT)
CUT = 100


def day(d):
    return (d - EPOCH).days


def sessions(first, last):
    return [EPOCH + dt.timedelta(days=int(x)) for x in sec.nyse_sessions(first, last)]


def sealed_day():
    return rw.SEAL + dt.timedelta(days=3)


def panel_rows(seed=4, mutate_from=None):
    """{stage: {year: rows}}, values of every (session, line) a deterministic function of the seed."""
    rng = np.random.default_rng(seed)
    out = {"gold": {}, "characteristics": {}}
    for d in sessions(PANEL_FIRST, PANEL_LAST):
        for sid in PANEL_IDS:
            hl, mom, fwd, slope, lab = (float(x) for x in rng.normal(0.0, 1.0, 5))
            if (d, sid) in GAPS:
                continue
            if mutate_from is not None and d >= mutate_from:
                hl, slope = hl * 3.0 + 1.0, slope * 3.0 - 1.0
            g = out["gold"].setdefault(d.year, [])
            g.append({"session_date": d, "security_id": sid, "cik": 1000 + sid, "grp_ff49": 7.0, "grp_ff12": 3.0,
                      "ctl_hl_spread_21": abs(hl) / 100, "g_mom_12_1": mom, "fwd_ret_21": fwd})
            c = out["characteristics"].setdefault(d.year, [])
            c.append({"session_date": d, "security_id": sid, "iv_term_slope": slope / 10, "label_h21": lab})
            if (d, sid) == DUP:
                g.append({**g[-1], "ctl_hl_spread_21": 0.5})
                c.append({**c[-1], "iv_term_slope": 0.5})
    for stage, cols in (("gold", {"ctl_hl_spread_21": 0.01, "g_mom_12_1": 0.0, "fwd_ret_21": 9.0}),
                        ("characteristics", {"iv_term_slope": 9.0, "label_h21": 9.0})):
        for k in range(3):                     # rows on or after the seal inside the 2019 file (their own row group)
            row = {"session_date": sealed_day(), "security_id": PANEL_IDS[k], **cols}
            if stage == "gold":
                row.update(cik=1, grp_ff49=1.0, grp_ff12=1.0)
            out[stage][2019].append(row)
    return out


GOLD_TYPES = {"session_date": pa.date32(), "security_id": pa.int64(), "cik": pa.int64(), "grp_ff49": pa.float64(),
              "grp_ff12": pa.float64(), "ctl_hl_spread_21": pa.float32(), "g_mom_12_1": pa.float32(),
              "fwd_ret_21": pa.float32()}
CHAR_TYPES = {"session_date": pa.date32(), "security_id": pa.int64(), "iv_term_slope": pa.float32(),
              "label_h21": pa.float32()}


def write_root(root: Path, rows, schema_override=None, status="complete"):
    """The stage files and manifests; returns {stage: manifest sha256}."""
    pins = {}
    for stage, types, schema in (("gold", GOLD_TYPES, "atx.alpha-panel.gold/v1"),
                                 ("characteristics", CHAR_TYPES, "atx.alpha-panel.characteristics/v2")):
        files = {}
        for year, rs in sorted(rows[stage].items()):
            rel = f"year={year}/{stage}.parquet"
            path = root / stage / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            table = pa.table({k: pa.array([r[k] for r in rs], t) for k, t in types.items()})
            pq.write_table(table, path, row_group_size=len(rs) - 3 if year == 2019 else 512)
            blob = path.read_bytes()
            files[rel] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
        junk = root / stage / f"year={rw.SEAL.year}" / f"{stage}.parquet"   # opening it would fail
        junk.parent.mkdir(parents=True, exist_ok=True)
        junk.write_bytes(b"not a parquet file")
        files[f"year={rw.SEAL.year}/{stage}.parquet"] = {"bytes": junk.stat().st_size,
                                                          "sha256": hashlib.sha256(junk.read_bytes()).hexdigest()}
        m = {"schema": (schema_override or {}).get(stage, schema), "status": status, "stage": stage, "files": files}
        blob = json.dumps(m, sort_keys=True).encode("utf-8")
        (root / stage / "manifest.json").write_bytes(blob)
        pins[stage] = hashlib.sha256(blob).hexdigest()
    return pins


def write_role(root: Path):
    root.mkdir(parents=True)
    days = [day(d) for d in sessions(ROLE_FIRST, ROLE_LAST)]
    blobs = {"sessions.i64": np.array([d * DAY_NS for d in days], dtype="<i8").tobytes(),
             "ids.u64": np.array(ROLE_IDS, dtype="<u8").tobytes(),
             "member.u8": np.ones((len(days), len(ROLE_IDS)), dtype="u1").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": len(days),
                "instruments": len(ROLE_IDS), "instrument_namespace": "spiderrock.securityID", "score_begin": 0,
                "score_end": len(days), "files": files, "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return days, hashlib.sha256((root / "manifest.json").read_bytes()).hexdigest()


@contextlib.contextmanager
def registered():
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        xreg.register(vars(tool))
        yield


class Case:
    def __init__(self, base: Path, rows, name="w", **kw):
        self.base, self.rows = base, rows
        self.root = base / f"{name}-panel"
        self.pins = write_root(self.root, rows, **kw)
        self.role = base / f"{name}-role"
        self.days, self.role_sha = write_role(self.role)

    def options(self, **over):
        o = {"gold_panel_root": self.root, "gold_sha256": self.pins["gold"],
             "gold_characteristics_sha256": self.pins["characteristics"]}
        o.update(over)
        return o

    def run(self, out, fields, **kw):
        kw.setdefault("module_options", self.options())
        with contextlib.redirect_stdout(io.StringIO()):
            return tool.run(self.role, self.role_sha, self.base / out, fields, **kw)

    def field(self, out, name):
        return np.fromfile(self.base / out / f"{name}.f64", dtype="<f8").reshape(len(self.days), len(ROLE_IDS))


def oracle(rows, days, stage, column, lag=1):
    """Row t = the value dated on the session before t (NaN if absent or duplicated)."""
    before = [int(x) for x in sec.nyse_sessions(EPOCH + dt.timedelta(days=days[0] - 14),
                                                EPOCH + dt.timedelta(days=days[0] - 1))][-1]
    prev = ([before] + days[:-1]) if lag else days
    seen = {}
    for rs in rows[stage].values():
        for r in rs:
            key = (day(r["session_date"]), r["security_id"])
            seen.setdefault(key, []).append(float(np.float32(r[column])))
    out = np.full((len(days), len(ROLE_IDS)), np.nan)
    for t, d in enumerate(prev):
        for j, sid in enumerate(ROLE_IDS):
            v = seen.get((d, sid), [])
            if len(v) == 1:
                out[t, j] = v[0]
    return out


class GoldFields(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self._reg = registered()
        self._reg.__enter__()

    def tearDown(self):
        self._reg.__exit__(None, None, None)
        self._tmp.cleanup()

    def test_values_match_definitions(self):
        rows = panel_rows()
        case = Case(self.base, rows)
        m = case.run("f", FIELDS)
        for name, stage, column in (("gp_hl_spread_21", "gold", "ctl_hl_spread_21"),
                                    ("gp_iv_term_slope", "characteristics", "iv_term_slope")):
            want = oracle(rows, case.days, stage, column)
            got = case.field("f", name)
            np.testing.assert_array_equal(np.isnan(got), np.isnan(want), name)
            np.testing.assert_array_equal(got[np.isfinite(got)], want[np.isfinite(want)], name)
            self.assertGreater(int(np.isfinite(got).sum()), 600)
        e = {x["name"]: x for x in m["fields"]}
        for name in FIELDS:
            self.assertTrue(e[name]["point_in_time"])
            self.assertEqual(e[name]["producer"]["module"], "research_fields_gold.py")
            self.assertEqual(e[name]["formula_sha256"], tool.formula_id(name, tool.spec_definition(name, 1)))
            self.assertEqual(e[name]["duplicate_keys_quarantined"], 1)
        st = m["source_checks"]["gold"]["stages"]
        for stage in ("gold", "characteristics"):
            self.assertEqual(st[stage]["partitions_read"], [f"year=2018/{stage}.parquet", f"year=2019/{stage}.parquet"])
            self.assertEqual(st[stage]["row_groups_pruned_on_or_after_seal"], 1)
            self.assertEqual(st[stage]["rows_in_pruned_row_groups"], 3)
            self.assertEqual(st[stage]["manifest_sha256"], case.pins[stage])
        self.assertEqual(m["seal"]["exclusive_end"], rw.SEAL_DATE)

    def test_point_in_time_probe(self):
        """Every panel value dated on or after session CUT moves: rows 0..CUT are bit-identical, later rows move; with
        the same-session clock (LAG_SESSIONS 0) row CUT moves, so the probe fails on that leak."""
        cut_date = sessions(ROLE_FIRST, ROLE_LAST)[CUT]

        def probe(tag):
            a, b = Case(self.base, panel_rows(), tag + "a"), Case(self.base, panel_rows(mutate_from=cut_date), tag + "b")
            a.run(tag + "fa", FIELDS)
            b.run(tag + "fb", FIELDS)
            res = []
            for name in FIELDS:
                x, y = a.field(tag + "fa", name), b.field(tag + "fb", name)
                res.append((x[:CUT + 1].tobytes() == y[:CUT + 1].tobytes(), x[CUT + 1:].tobytes() != y[CUT + 1:].tobytes()))
            return res
        self.assertEqual(probe("ok"), [(True, True)] * len(FIELDS))
        with mock.patch.object(gold, "LAG_SESSIONS", 0):
            leaky = probe("leak")
        self.assertTrue(all(not same for same, _ in leaky))

    def test_label_columns_and_stages_refused(self):
        for column in ("fwd_ret_21", "label_h21", "ic_mean", "holdout_ic", "ret_forward_5", "oos_score", "target",
                       "lead_ret"):
            with self.assertRaisesRegex(ValueError, "label / forward / IC / holdout"):
                gold.declare("gp_" + column, "gold", column, "u", "d", [], "f")
        for stage in ("labels", "labels_holdout", "validation", "panel"):
            with self.assertRaisesRegex(ValueError, "not a gold-pipeline feature stage"):
                gold.declare("gp_x", stage, "x", "u", "d", [], "f")
        with self.assertRaises(gold.GoldSelectionColumn):
            gold.declare("gp_g_mom_12_1", "gold", "g_mom_12_1", "u", "d", [], "f")
        with self.assertRaises(gold.GoldSelectionColumn):
            gold.declare("gp_c_all", "gold", "c_all", "u", "d", [], "f")
        self.assertNotIn("gp_g_mom_12_1", gold.FIELDS)
        for name, spec in gold.FIELDS.items():             # nothing declared reads a label or a score
            self.assertIsNone(gold.refused_reason(spec["stage"], spec["column"]), name)
            self.assertFalse(spec["column"].startswith(("g_", "c_")), name)
        rows = panel_rows()
        # a labels-schema manifest in a stage directory, an unfinished stage, a wrong pin: refused before output
        bad = Case(self.base, rows, "bad", schema_override={"gold": "atx.alpha-panel.labels/v1"})
        with self.assertRaisesRegex(ValueError, "not a complete atx.alpha-panel.gold/v1"):
            bad.run("x1", ["gp_hl_spread_21"])
        building = Case(self.base, rows, "building", status="building")
        with self.assertRaisesRegex(ValueError, "not a complete"):
            building.run("x2", FIELDS)
        case = Case(self.base, rows)
        with self.assertRaisesRegex(ValueError, "differs from its pin"):
            case.run("x3", FIELDS, module_options=case.options(gold_sha256="0" * 64))
        with self.assertRaisesRegex(ValueError, "need --gold-panel-root"):
            case.run("x4", FIELDS, module_options={})
        with self.assertRaisesRegex(ValueError, "needs --gold-characteristics-sha256"):
            case.run("x5", ["gp_iv_term_slope"], module_options=case.options(gold_characteristics_sha256=None))
        for out in ("x1", "x2", "x3", "x4", "x5"):
            self.assertFalse((self.base / out).exists(), out)
        # a stage file that differs from its manifest entry: refused, nothing published
        f = case.root / "gold" / "year=2019" / "gold.parquet"
        f.write_bytes(f.read_bytes()[:-8] + b"\0" * 8)
        with self.assertRaisesRegex(ValueError, "differs from its manifest entry"):
            case.run("x6", ["gp_hl_spread_21"])
        self.assertFalse((self.base / "x6" / "manifest.json").exists())

    def test_sealed_rows_and_partitions_refused(self):
        rows = panel_rows()
        case = Case(self.base, rows)
        h = tool.FIELD_MODULES[-1].h
        man = gold.stage_manifest(h, case.root, "gold", case.pins["gold"])
        # the pushed-down filter: a read reaching past the seal returns no row on or after it
        data, st = gold.read_stage_columns(h, case.root, "gold", man, ["ctl_hl_spread_21"], day(PANEL_FIRST),
                                           day(sealed_day()) + 10, tool.Budget(700, 600))
        self.assertLess(int(data["ctl_hl_spread_21"][0].max()), day(rw.SEAL))
        self.assertEqual(st["rows_in_pruned_row_groups"], 3)
        self.assertEqual(st["sealed_partitions_never_opened"], 1)    # the garbage seal-year file is never opened
        # a row on or after the seal in a read result is refused, never dropped
        with self.assertRaises(rw.SealError):
            gold.assert_sealed(h, np.array([day(dt.date(2019, 5, 1)), day(rw.SEAL)]), "probe")
        gold.assert_sealed(h, np.array([day(rw.SEAL) - 1]), "probe")
        parts, sealed = gold.stage_partitions(man["manifest"]["files"], 2000, 2100)
        self.assertEqual(([y for y, _, _ in parts], sealed), ([2018, 2019], 1))
        with mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1)):      # builder seal is not research_window's
            with self.assertRaisesRegex(rw.SealError, "not research_window's"):
                case.run("x1", FIELDS)

    def test_repository_window_fresh_interpreter(self):
        """Under the repository window (seal 2024-01-01 from research_window.json) the 2024 partition is never
        opened and a 2024 row group is pruned; the payloads equal this harness's."""
        rows = panel_rows()
        for stage in rows:                       # rows dated in 2024 in the 2019 file and a 2024 partition
            rows[stage][2019] = [{**r, "session_date": dt.date(2024, 3, 1)} if r["session_date"] == sealed_day() else r
                                 for r in rows[stage][2019]]
        case = Case(self.base, rows)
        for stage in ("gold", "characteristics"):
            junk = case.root / stage / "year=2024" / f"{stage}.parquet"
            junk.parent.mkdir(parents=True, exist_ok=True)
            junk.write_bytes(b"garbage")
            m = json.loads((case.root / stage / "manifest.json").read_text(encoding="utf-8"))
            m["files"]["year=2024/" + junk.name] = {"bytes": 7, "sha256": hashlib.sha256(b"garbage").hexdigest()}
            blob = json.dumps(m, sort_keys=True).encode("utf-8")
            (case.root / stage / "manifest.json").write_bytes(blob)
            case.pins[stage] = hashlib.sha256(blob).hexdigest()
        case.run("here", FIELDS)
        opts = {"gold_panel_root": str(case.root), "gold_sha256": case.pins["gold"],
                "gold_characteristics_sha256": case.pins["characteristics"]}
        script = (
            "import json, sys, contextlib, io\n"
            f"sys.path.insert(0, {str(TOOLS)!r})\n"
            "from pathlib import Path\n"
            "import prepare_research_fields as tool, prepare_research_fields_xdata as xreg, research_window as rw\n"
            "xreg.register(vars(tool))\n"
            f"opts = {opts!r}\n"
            "opts['gold_panel_root'] = Path(opts['gold_panel_root'])\n"
            "with contextlib.redirect_stdout(io.StringIO()):\n"
            f"    m = tool.run(Path({str(case.role)!r}), {case.role_sha!r}, Path({str(self.base / 'there')!r}), "
            f"{FIELDS!r}, module_options=opts)\n"
            "st = m['source_checks']['gold']['stages']\n"
            "print(json.dumps({'seal': rw.SEAL_DATE, 'sealed': {k: v['sealed_partitions_never_opened'] for k, v in "
            "st.items()}, 'pruned': {k: v['rows_in_pruned_row_groups'] for k, v in st.items()}}))\n")
        out = subprocess.run([sys.executable, "-c", script], cwd=str(self.base), capture_output=True, text=True,
                             timeout=300)
        self.assertEqual(out.returncode, 0, out.stderr[-2000:])
        got = json.loads(out.stdout.strip().splitlines()[-1])
        self.assertEqual(got["seal"], rw.load()["seal_begin"])
        self.assertEqual(got["sealed"], {"gold": 2, "characteristics": 2})        # 2024 and the harness seal year
        self.assertEqual(got["pruned"], {"gold": 3, "characteristics": 3})
        for name in FIELDS:
            self.assertEqual((self.base / "there" / f"{name}.f64").read_bytes(),
                             (self.base / "here" / f"{name}.f64").read_bytes(), name)

    def test_reuse(self):
        case = Case(self.base, panel_rows())
        full = case.run("full", FIELDS)
        again = case.run("again", FIELDS, reuse=self.base / "full")
        self.assertEqual((again["reuse"]["reused"], again["reuse"]["computed"]), (FIELDS, []))
        self.assertEqual(again["files"], full["files"])
        for name in FIELDS:
            rec = next(x for x in again["fields"] if x["name"] == name)["reused_from"]
            stage = gold.FIELDS[name]["stage"]
            self.assertEqual(rec["inputs"], {"stage": stage, "stage_manifest_sha256": case.pins[stage],
                                             "session_calendar": gold.price.session_calendar()})
        # a republished gold stage (new manifest pin) recomputes its field only
        m = json.loads((case.root / "gold" / "manifest.json").read_text(encoding="utf-8"))
        m["note"] = "republished"
        blob = json.dumps(m, sort_keys=True).encode("utf-8")
        (case.root / "gold" / "manifest.json").write_bytes(blob)
        pin = hashlib.sha256(blob).hexdigest()
        one = case.run("one", FIELDS, reuse=self.base / "full", module_options=case.options(gold_sha256=pin))
        self.assertEqual((one["reuse"]["reused"], one["reuse"]["computed"]), (["gp_iv_term_slope"], ["gp_hl_spread_21"]))
        self.assertEqual(one["files"], full["files"])
        for name in gold.FIELDS:
            self.assertEqual(gold.producer_group(name), "gold_cols")
            self.assertIs(gold.field_spec(name), gold.FIELDS[name])

    def test_registration_is_opt_in(self):
        self._reg.__exit__(None, None, None)
        try:
            self.assertFalse(set(FIELDS) & set(tool.ALL_FIELDS))
            self.assertEqual(tuple(gold.FIELDS), xreg.FIELDS_GOLD_DRAFT)
        finally:
            self._reg = registered()
            self._reg.__enter__()


if __name__ == "__main__":
    unittest.main()
