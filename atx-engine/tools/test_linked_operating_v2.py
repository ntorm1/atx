"""linked-operating-v2 (platform v7 W5b): v1 output byte identity, the v2 class rule, delisting attributes and the
optional delisting-return application, on the synthetic role of test_prepare_recent_research."""
import contextlib
import datetime as dt
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import prepare_recent_research as tool
import test_prepare_recent_research as base

# Normalised v1 output of LinkedOperatingUniverse's fixture computed with prepare_recent_research.py at the W5b base
# commit 9da38ad8 (before linked-operating-v2 existed): environment-dependent pins and paths are replaced by tokens
# and universe.inputs.code (the code identities) is removed.
V1_GOLDEN_MANIFEST = "1e008e0a7d9b3e6019827d3215b470e0ffe3fd64e9267de429cb9fdfe5bc948e"
V1_GOLDEN_MEMBER = "adeab0af11f850b5c18ea0d8127c5c828e701bf213b5f3530ae51113cdc7a3e7"
STOP = 250          # line 3 has no vendor row after days[STOP]
DELISTING_SCHEMA = pa.schema([
    ("security_id", pa.int64()), ("last_session", pa.date32()), ("continued", pa.bool_()), ("cause", pa.string()),
    ("cause_basis", pa.string()), ("dlret", pa.decimal128(3, 2)), ("dlret_if_performance", pa.decimal128(3, 2)),
    ("available_at", pa.timestamp("us")), ("exchange", pa.string()), ("ever_member", pa.bool_())])


def normalized_v1(case, m):
    root = case.base
    tokens = {case.role_sha: "<role-sha>", case.bridge_sha: "<bridge-sha>", case.sic_sha: "<sic-sha>",
              base.sha((root / "bridge/links.parquet").read_bytes()): "<bridge-file-sha>",
              base.sha((root / "events/sic_events.parquet").read_bytes()): "<sic-file-sha>",
              base.sha((root / "source.parquet").read_bytes()): "<source-sha>",
              base.sha((root / "cache/manifest.json").read_bytes()): "<projection-sha>"}
    roots = sorted({str(root), str(root.resolve())}, key=len, reverse=True)
    doc = json.loads(json.dumps(m))
    doc["universe"]["inputs"].pop("code")

    def walk(x):
        if isinstance(x, dict):
            return {k: walk(v) for k, v in x.items()}
        if isinstance(x, list):
            return [walk(v) for v in x]
        if isinstance(x, str):
            for r in roots:
                x = x.replace(r, "<BASE>")
            return tokens.get(x, x)
        return x
    blob = json.dumps(walk(doc), sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest(), base.sha((root / "lo" / "member.u8").read_bytes())


class V1Identity(unittest.TestCase):
    def test_v1_output_is_byte_identical_to_the_base_commit(self):
        case = base.LinkedOperatingUniverse("test_restricts_members_point_in_time_and_copies_payloads")
        case.setUp()
        try:
            m = case.restrict()
            self.assertEqual(normalized_v1(case, m), (V1_GOLDEN_MANIFEST, V1_GOLDEN_MEMBER))
            self.assertNotIn("delisting", m["universe"])
        finally:
            case.tearDown()


def v2_bridge_rows(days):
    rows = base.bridge_rows(days)
    status = {0: "common", 1: "common", 2: "A", 3: "common", 4: "B", 5: "U"}   # 3: class A letter; 5: a unit
    for k, r in enumerate(rows):
        r["class_status"] = status[k]
    return rows


def delisting_rows(days, last):
    d = lambda i: dt.date.fromisoformat(days[i])  # noqa: E731
    row = lambda sid, i, cont, cause, r, perf, avail=None: {  # noqa: E731
        "security_id": sid, "last_session": d(i), "continued": cont, "cause": cause,
        "cause_basis": "item_3.01_or_form_25" if cause == "performance" else None,
        "dlret": None if r is None else __import__("decimal").Decimal(r),
        "dlret_if_performance": __import__("decimal").Decimal(perf),
        "available_at": avail or base.at(days[min(i + 1, len(days) - 1)], 22), "exchange": "XNAS", "ever_member": True}
    return [row(3, STOP, False, "performance", "-0.55", "-0.55"),       # applied on days[STOP + 1]
            row(4, 300, False, "mna", "0.00", "-0.30"),                   # still trades after: skipped
            row(1, 150, False, "unknown", None, "-0.30"),                  # no imputed return: skipped
            row(5, last, False, "non_common", "0.00", "-0.30"),            # the role's last session: skipped
            row(2, 100, True, "mna", "0.00", "-0.30"),                     # a continuation, not a termination
            row(99, 100, False, "mna", "0.00", "-0.30"),                   # not a role line
            row(1, 210, False, "mna", "0.00", "-0.30", dt.datetime(2025, 2, 1)),   # sealed
            row(6, 200, False, "mna", "0.00", "-0.30")]                     # line 6 is not on the role axis


class LinkedOperatingV2(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self._quiet = contextlib.redirect_stdout(io.StringIO())
        self._quiet.__enter__()
        source = self.base / "source.parquet"
        self.days = base.fixture(source)
        table = pq.read_table(source)
        stop = np.datetime64(self.days[STOP])
        drop = (table.column("securityID").to_numpy() == 3) & \
               (table.column("tradingDate").to_numpy(zero_copy_only=False).astype("datetime64[D]") > stop)
        pq.write_table(table.filter(pa.array(~drop)), source, row_group_size=1000)
        tool.prepare_cache(source, self.base / "cache", self.days[0], "2021-01-01", tool.Limits(30))
        tool.create_role(self.base / "cache", self.base / "role", self.days[0], self.days[400], "2021-01-01",
                         tool.Limits(30), top_n=5)
        self.role_sha = base.sha((self.base / "role/manifest.json").read_bytes())
        self.nd = json.loads((self.base / "role/manifest.json").read_bytes())["dates"]
        self.bridge_sha = base.write_listed(self.base / "bridge", "links.parquet",
                                            pa.Table.from_pylist(v2_bridge_rows(self.days), schema=base.BRIDGE_SCHEMA),
                                            "atx.identity-bridge/v1", rehearsal_identity=True)
        self.sic_sha = base.write_listed(self.base / "events", "sic_events.parquet", base.sic_rows(self.days),
                                         "atx.fundamental-events/v1")
        self.del_sha = base.write_listed(self.base / "delisting", "events.parquet",
                                         pa.Table.from_pylist(delisting_rows(self.days, self.nd - 1),
                                                              schema=DELISTING_SCHEMA),
                                         "atx.alpha-panel.delisting/v1", rule="delisting-rule-r-v1")

    def tearDown(self):
        self._quiet.__exit__(None, None, None)
        self._tmp.cleanup()

    def restrict(self, out, universe="linked-operating-v2", **kw):
        args = dict(bridge=self.base / "bridge", bridge_sha256=self.bridge_sha, sic_events=self.base / "events",
                    sic_events_sha256=self.sic_sha, universe=universe)
        if universe == "linked-operating-v2":
            args.update(delisting=self.base / "delisting", delisting_sha256=self.del_sha)
        args.update(kw)
        return tool.restrict_role(self.base / "role", self.role_sha, self.base / out, tool.Limits(60), **args)

    def arrays(self, directory):
        m = json.loads((self.base / directory / "manifest.json").read_bytes())
        shape = (m["dates"], m["instruments"])
        ids = np.fromfile(self.base / directory / "ids.u64", dtype="<u8")
        out = {"ids": ids, "manifest": m}
        for name, dtype in (("member.u8", "u1"), ("present.u8", "u1"), ("close.f64", "<f8"), ("raw_close.f64", "<f8"),
                            ("volume.f64", "<f8")):
            out[name] = np.fromfile(self.base / directory / name, dtype=dtype).reshape(shape)
        return out

    def test_class_rule(self):
        self.assertEqual(tool.v2_class_ok(["common", "A", "B", "C", "V", "U", "W", "R", "", None, "unknown", "AB"]).tolist(),
                         [True, True, True, True, True, False, False, False, False, False, False, False])

    def test_v2_membership_and_delisting_attributes(self):
        m = self.restrict("v2")
        v1 = self.restrict("v1", universe="linked-operating-v1")
        b, a, one = self.arrays("role"), self.arrays("v2"), self.arrays("v1")
        col = {int(x): k for k, x in enumerate(b["ids"])}
        for name in ("close.f64", "raw_close.f64", "volume.f64", "present.u8", "sessions.i64", "ids.u64"):
            self.assertEqual((self.base / "v2" / name).read_bytes(), (self.base / "role" / name).read_bytes(), name)
        u = m["universe"]
        self.assertEqual((u["id"], u["class_status_required"]), ("linked-operating-v2", tool.V2_CLASS_RULE))
        # line 3's class-A rows pass under v2 (SIC visible from t=6); under v1 its "A" is not "common"
        c3 = col[3]
        self.assertTrue(a["member.u8"][6:STOP + 1, c3][b["member.u8"][6:STOP + 1, c3] != 0].all())
        self.assertFalse(one["member.u8"][:200, c3].any())                   # v1: common only from t=200
        # line 5's unit class fails: class_not_common from its link (t=311) on
        self.assertFalse(a["member.u8"][:, col[5]].any())
        self.assertEqual(u["dropped_by_reason"]["class_not_common"],
                         int(b["member.u8"][311:, col[5]].astype(np.int64).sum()))
        base_cells = int(b["member.u8"].astype(np.int64).sum())
        self.assertEqual(sum(u["dropped_by_reason"].values()), base_cells - int(a["member.u8"].astype(np.int64).sum()))
        self.assertEqual(u["kept_member_counts"], a["member.u8"].astype(np.int64).sum(axis=1).tolist())
        d = u["delisting"]
        self.assertEqual(d["counts"], {"rows_total": 8, "rows_on_role_in_window": 6,
                                       "rows_available_on_or_after_2025_dropped": 1, "continued_not_terminations": 1,
                                       "terminations": 4})
        ev = {e["security_id"]: e for e in d["events"]}
        self.assertEqual(sorted(ev), [1, 3, 4, 5])
        self.assertEqual((ev[3]["delist_code"], ev[3]["delist_return"], ev[3]["termination_session"]),
                         ("performance", -0.55, self.days[STOP + 1]))
        self.assertIsNone(ev[1]["delist_return"])
        self.assertEqual(ev[1]["delist_return_if_performance"], -0.3)
        self.assertIsNone(ev[5]["termination_session"])
        self.assertEqual(ev[3]["available_at"], self.days[STOP + 1] + "T22:00:00Z")
        self.assertFalse(any(e["returns_applied"] for e in d["events"]) or d["returns_applied"])
        self.assertNotIn("applied", d)
        self.assertEqual(u["inputs"]["delisting"]["manifest_sha256"], self.del_sha)
        self.assertEqual(d["by_cause"]["performance"]["kept_member_at_last_session"], 1)

    def test_delisting_returns_on_the_termination_session(self):
        m = self.restrict("ret", delisting_returns=True)
        plain = self.restrict("plain")
        b, r, p = self.arrays("role"), self.arrays("ret"), self.arrays("plain")
        c3, t = {int(x): k for k, x in enumerate(b["ids"])}[3], STOP + 1
        self.assertEqual((b["present.u8"][t, c3], p["member.u8"][t, c3]), (0, 1))   # a member with no price before
        self.assertAlmostEqual(r["close.f64"][t, c3] / r["close.f64"][t - 1, c3] - 1, -0.55, places=12)
        self.assertAlmostEqual(r["raw_close.f64"][t, c3] / r["raw_close.f64"][t - 1, c3] - 1, -0.55, places=12)
        self.assertEqual((r["present.u8"][t, c3], r["volume.f64"][t, c3], r["member.u8"][t, c3]), (1, 0.0, 0))
        for name in ("close.f64", "raw_close.f64", "volume.f64", "present.u8", "member.u8"):
            diff = ~((r[name] == p[name]) | (np.isnan(r[name]) & np.isnan(p[name])) if name.endswith("f64")
                     else r[name] == p[name])
            self.assertEqual(np.argwhere(diff).tolist(), [[t, c3]], name)   # exactly one cell changes
        u = m["universe"]
        self.assertEqual(u["delisting"]["applied"], {"terminations": 1, "members_cleared_on_termination_session": 1,
                                                     "skipped": {"no_delist_return": 1,
                                                                 "last_session_not_a_role_session": 0,
                                                                 "last_role_session": 1,
                                                                 "not_present_at_last_session": 0,
                                                                 "present_after_last_session": 1}})
        self.assertEqual(u["kept_member_counts"], r["member.u8"].astype(np.int64).sum(axis=1).tolist())
        self.assertEqual(m["score_member_counts"], u["kept_member_counts"][m["score_begin"]:])
        for name in ("close.f64", "present.u8"):
            self.assertEqual(m["files"][name]["sha256"], base.sha((self.base / "ret" / name).read_bytes()))
        self.assertTrue({e["security_id"]: e for e in u["delisting"]["events"]}[3]["returns_applied"])

    def test_refusals_and_cli(self):
        with self.assertRaisesRegex(ValueError, "--delisting"):
            self.restrict("x1", delisting=None, delisting_sha256=None)
        with self.assertRaisesRegex(ValueError, "--delisting-returns"):
            self.restrict("x2", universe="linked-operating-v1", delisting_returns=True)
        with self.assertRaisesRegex(ValueError, "delisting manifest SHA-256"):
            self.restrict("x3", delisting_sha256="0" * 64)
        for out in ("x1", "x2", "x3"):
            self.assertFalse((self.base / out).exists())
        common = ["role", "--out", str(self.base / "cli"), "--base-role", str(self.base / "role"),
                  "--base-role-sha256", self.role_sha, "--identity-bridge", str(self.base / "bridge"),
                  "--identity-bridge-sha256", self.bridge_sha, "--sic-events", str(self.base / "events"),
                  "--sic-events-sha256", self.sic_sha]
        for argv in (common + ["--universe", "linked-operating-v2"],
                     # v8 F-0: v1 accepts the stage (test_lo1_delisting) but still needs it for the returns
                     common + ["--universe", "linked-operating-v1", "--delisting-returns",
                               str(self.base / "delisting")],
                     common + ["--universe", "linked-operating-v2", "--delisting", str(self.base / "delisting"),
                               "--delisting-sha256", self.del_sha, "--delisting-returns", str(self.base / "events")]):
            with patch.object(sys, "argv", ["prepare_recent_research.py", *argv]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tool.main()
        self.assertFalse((self.base / "cli").exists())
        argv = common + ["--universe", "linked-operating-v2", "--delisting", str(self.base / "delisting"),
                         "--delisting-sha256", self.del_sha, "--delisting-returns",
                         str(self.base / "delisting" / "events.parquet")]
        with patch.object(sys, "argv", ["prepare_recent_research.py", *argv]):
            tool.main()
        m = json.loads((self.base / "cli" / "manifest.json").read_bytes())
        self.assertTrue(m["universe"]["delisting"]["returns_applied"])


if __name__ == "__main__":
    unittest.main()
